// Correctness test for SPSCRingBuffer: a real producer thread and a real
// consumer thread, racing against each other for millions of operations,
// checked for lost, duplicated, reordered, or torn items. No test framework
// dependency, and deliberately not built on assert() either -- see CHECK()
// below -- so this stays trivial to run under ThreadSanitizer, which is the
// actual point: a memory-ordering bug in push()/pop() would very likely pass
// a single-threaded smoke test and only show up under real contention or
// under TSan's happens-before analysis.
//
// Build with TSan to get real signal here:
//   g++ -std=c++20 -O1 -g -fsanitize=thread -Icpp/include
//       cpp/tests/spsc_ring_buffer_test.cpp -o spsc_test_tsan -lpthread
//   ./spsc_test_tsan

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "agrios/spsc_ring_buffer.hpp"

namespace {

// Deliberately not assert(): NDEBUG is defined by every CMake Release-family
// build type (including RelWithDebInfo, which cpp/CMakeLists.txt defaults
// to), which silently compiles assert() down to nothing -- a test file built
// that way would report "all tests passed" while checking literally zero of
// its conditions. This bit in review while building against the CMake
// project: `ctest` reported this test passing, but the build log showed an
// "unused variable" warning on a variable only ever read inside assert()
// calls -- the tell that NDEBUG had stripped every check. CHECK() always
// evaluates its condition, in every build type.
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                              \
        }                                                                              \
    } while (0)

constexpr std::size_t kCapacity = 256;  // deliberately small to force wraparound repeatedly
constexpr std::uint64_t kItemCount = 5'000'000;

struct Item {
    std::uint64_t sequence;
};
static_assert(std::is_trivially_copyable_v<Item>);

void test_basic_empty_full() {
    agrios::SPSCRingBuffer<Item, 4> buf;
    Item out{};

    CHECK(buf.empty());
    CHECK(!buf.pop(out));  // empty: pop must fail

    CHECK(buf.push({1}));
    CHECK(buf.push({2}));
    CHECK(buf.push({3}));
    // Capacity 4 holds at most 3 items (one slot is always kept empty to
    // distinguish "full" from "empty" using only head == tail).
    CHECK(!buf.push({4}));

    CHECK(buf.pop(out) && out.sequence == 1);
    CHECK(buf.pop(out) && out.sequence == 2);
    CHECK(buf.push({4}));  // freed a slot, should succeed now
    CHECK(buf.pop(out) && out.sequence == 3);
    CHECK(buf.pop(out) && out.sequence == 4);
    CHECK(!buf.pop(out));  // drained again

    std::puts("test_basic_empty_full: OK");
}

void test_concurrent_producer_consumer() {
    static agrios::SPSCRingBuffer<Item, kCapacity> buf;

    std::thread producer([]() {
        for (std::uint64_t i = 0; i < kItemCount; ++i) {
            while (!buf.push(Item{i})) {
                std::this_thread::yield();  // buffer full, back off and retry
            }
        }
    });

    std::uint64_t received = 0;
    std::uint64_t expected_next = 0;
    bool order_violation = false;

    std::thread consumer([&]() {
        Item item{};
        while (received < kItemCount) {
            if (!buf.pop(item)) {
                std::this_thread::yield();
                continue;
            }
            if (item.sequence != expected_next) {
                order_violation = true;
            }
            ++expected_next;
            ++received;
        }
    });

    producer.join();
    consumer.join();

    CHECK(!order_violation && "items arrived out of order -- push/pop are not correctly ordered");
    CHECK(received == kItemCount && "lost or duplicated items");

    std::printf("test_concurrent_producer_consumer: OK (%llu items, capacity %zu, wrapped ~%llu times)\n",
                static_cast<unsigned long long>(kItemCount), kCapacity,
                static_cast<unsigned long long>(kItemCount / kCapacity));
}

}  // namespace

int main() {
    test_basic_empty_full();
    test_concurrent_producer_consumer();
    std::puts("all tests passed");
    return 0;
}

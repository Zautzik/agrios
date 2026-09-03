#pragma once

#include <atomic>
#include <cstddef>  // std::size_t, offsetof
#include <type_traits>

namespace agrios {

// Deliberately a fixed constant, not std::hardware_destructive_interference_
// size: this value is part of this project's cross-process/cross-language
// ABI (bridge_c_api.cpp's layout, and shared_ring_buffer.py's fixed struct
// offsets, both depend on it), and GCC explicitly warns that the standard
// library constant "can vary between compiler versions or with different
// -mtune/-mcpu flags" and recommends exactly this fix for anything ABI-
// facing. 64 bytes is correct for essentially every mainstream x86-64 and
// ARM64 target this is meant to run on (desktop/server, Raspberry Pi, Jetson).
inline constexpr std::size_t kCacheLineSize = 64;

// Single-producer / single-consumer lock-free ring buffer.
//
// Safe for exactly one writer and one reader -- two threads, or (once placed
// in POSIX shared memory via SharedRingBuffer) two separate processes. Not
// safe for more than one producer or more than one consumer: the algorithm
// relies on each index having exactly one writer.
//
// Capacity must be a power of two so index wraparound is a bitwise AND
// against (Capacity - 1) instead of a modulo.
//
// T must be trivially copyable. Instances of this buffer are designed to
// live in shared memory mapped at a different virtual address in every
// attaching process, so T cannot hold pointers, references, virtual
// functions, or anything else that isn't valid reinterpreted verbatim in
// another process's address space.
template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable to live in shared memory");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "std::atomic<size_t> must be lock-free on this platform: a "
                  "mutex-backed atomic (using futex/semaphore state private "
                  "to one process's libstdc++ instance) cannot safely be "
                  "shared across a process boundary");

   public:
    SPSCRingBuffer() noexcept : head_(0), tail_(0) {}

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // Producer side. Returns false if the buffer is full; never blocks.
    bool push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = (tail + 1) & kMask;

        // Acquire: synchronizes-with the consumer's release-store to head_ in
        // pop(). Guarantees that if we observe the consumer has freed this
        // slot, we also observe every memory effect the consumer performed
        // before freeing it -- so we never overwrite a slot the consumer
        // thinks it still owns.
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // full
        }

        buffer_[tail] = item;

        // Release: publishes the write above. Paired with the consumer's
        // acquire-load of tail_ in pop(), so once the consumer observes this
        // new tail value, it's also guaranteed to observe the item we just
        // wrote -- not a torn or stale read.
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the buffer is empty; never blocks.
    bool pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // Acquire: synchronizes-with the producer's release-store to tail_ in
        // push(). Guarantees that once we observe a new item is available,
        // we also observe the payload write that produced it.
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // empty
        }

        out = buffer_[head];

        // Release: publishes that this slot is now free. Paired with the
        // producer's acquire-load of head_ in push().
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Best-effort snapshot -- meaningful for diagnostics/monitoring, not for
    // synchronization (the result can be stale before the caller even reads
    // it, same as any concurrent queue's size()).
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // shared_ring_buffer.py parses this object's raw bytes with a fixed
    // struct.Struct offset table -- it has no way to ask the compiler where
    // these members actually landed. These three accessors exist purely so
    // that fact can be checked at compile time (see the static_asserts right
    // after this class): offsetof requires a complete type, and a class
    // template is only complete inside its own member-function bodies, not
    // in a static_assert written directly in the member list -- so the
    // checks live here and are asserted from outside, not in-line above.
    static constexpr std::size_t head_offset() noexcept { return offsetof(SPSCRingBuffer, head_); }
    static constexpr std::size_t tail_offset() noexcept { return offsetof(SPSCRingBuffer, tail_); }
    static constexpr std::size_t buffer_offset() noexcept { return offsetof(SPSCRingBuffer, buffer_); }

   private:
    static constexpr std::size_t kMask = Capacity - 1;

    // head_, tail_, and buffer_ each start on their own cache line. Without
    // this, a producer's store to tail_ would invalidate the same cache line
    // in the consumer's core if head_ (which the consumer writes) lived next
    // to it -- forcing a cross-core cache-coherency transaction on every
    // single push/pop even though the two indices are logically independent.
    // This is the textbook false-sharing failure mode for SPSC queues.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;  // consumer-owned read index
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;  // producer-owned write index
    alignas(kCacheLineSize) T buffer_[Capacity];
};

// Layout contract check (see head_offset()'s comment above): requires
// T's alignment to not exceed a cache line, which holds for every payload
// type this project uses -- see Pose6D. Checked against one concrete
// instantiation since the offsets don't depend on T's value or Capacity,
// only on the alignas(kCacheLineSize) placement above.
namespace detail {
using LayoutCheckBuffer = SPSCRingBuffer<unsigned char, 4>;
static_assert(LayoutCheckBuffer::head_offset() == 0);
static_assert(LayoutCheckBuffer::tail_offset() == kCacheLineSize);
static_assert(LayoutCheckBuffer::buffer_offset() == 2 * kCacheLineSize);
}  // namespace detail

}  // namespace agrios

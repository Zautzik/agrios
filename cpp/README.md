# Agrios bridge — lock-free SPSC ring buffer over POSIX shared memory

A single-producer/single-consumer lock-free ring buffer, built to carry 6-DoF
pose data between a Python perception process and a C++ motor-control
process across a POSIX shared-memory segment (`shm_open`/`mmap`) — no kernel
round-trip, no serialization, no lock, on the hot path between "perception
sees something" and "motor control reacts to it."

This exists to close a specific gap in the rest of the Agrios/Agroteca/
Agroscopio portfolio: none of it touches physical hardware or a real-time
control loop. This is the first piece that's meant to eventually run on real
robotics-class hardware (a Raspberry Pi or Jetson), not just a laptop.

**Requires Linux or macOS.** POSIX shared memory doesn't exist on native
Windows. Build under WSL, a Linux container, or a real Linux/macOS host —
`cmake -S cpp -B cpp/build` will refuse to configure anywhere else, on
purpose, rather than silently producing a binary that can't work.

## Layout

```
include/agrios/
  spsc_ring_buffer.hpp   the lock-free algorithm itself (template, header-only)
  pose6d.hpp             the payload struct (6D pose + monotonic timestamp)
  shared_ring_buffer.hpp POSIX shm_open/mmap wrapper: places a ring buffer in shared memory
  bridge_config.hpp      the one place capacity/shm-name are defined, shared by every caller
  bridge_c_api.h         plain-C ABI over SharedRingBuffer<Pose6D, ...> -- see below
src/
  bridge_c_api.cpp       implementation of the C API, built as libagrios_bridge.so
  motor_control_consumer.cpp   demo: the C++ "motor control" side
python/
  shared_ring_buffer.py  ctypes wrapper calling into libagrios_bridge.so
  perception_producer_demo.py  demo: the Python "perception" side
tests/
  spsc_ring_buffer_test.cpp    concurrency correctness test (see below)
```

## Why a compiled C API instead of letting Python poke the shared bytes directly

`push()`/`pop()`'s correctness depends on real `std::memory_order_acquire`/
`release` fences, not just on reading and writing the right bytes at the
right offsets. Python's `struct`/`ctypes` modules have no memory-order-aware
atomics. A hand-rolled Python-side load/store would *look* correct and
*happen to work* on x86-64, because x86's strong memory model papers over
the missing fences — and would be a real, silent correctness bug on a
weakly-ordered target like the ARM64 Jetson/Raspberry Pi boards this is
ultimately meant to run on. `bridge_c_api.cpp` compiles to a small shared
library that both the C++ demo and `shared_ring_buffer.py` (via `ctypes`)
call into, so there's exactly one implementation of the protocol — the real
one — instead of a second, Python-side reimplementation that has to be kept
in sync with the first by hand and would only be caught by testing on the
right CPU architecture.

## The memory-ordering contract

- Producer: read `tail_` (own cursor) relaxed → check `head_` **acquire**
  (must see everything the consumer did before freeing this slot) → write
  the payload → publish by storing `tail_` **release** (so the payload write
  happens-before the consumer's acquire-load of `tail_` observes it).
- Consumer: mirror image — read `head_` relaxed → check `tail_` **acquire**
  (must see the payload the producer just wrote) → read the payload →
  publish the freed slot by storing `head_` **release**.

`head_`, `tail_`, and the payload buffer each start on their own
`alignas(std::hardware_destructive_interference_size)` (64-byte) boundary,
so a store to one by its owning thread never invalidates the other thread's
cached copy of the other index — the textbook false-sharing failure mode for
SPSC queues, avoided rather than discovered later in a profiler.

The exact byte offsets this produces (`head_` at 0, `tail_` at 64, the
buffer at 128) are asserted at compile time in `spsc_ring_buffer.hpp` via
`static_assert(offsetof(...))` — not because the C++ side needs them, but
because `bridge_c_api.cpp`'s ABI needs to stay stable, and any future
reordering of the members would otherwise fail silently rather than at
build time.

## Verifying correctness

`tests/spsc_ring_buffer_test.cpp` runs a real producer thread against a real
consumer thread for 5,000,000 items through a deliberately small (256-slot)
buffer, forcing constant index wraparound, and checks for lost, duplicated,
or reordered items. A memory-ordering bug here would very plausibly pass a
single-threaded smoke test and only surface under real contention — so the
test is written to make that contention actually happen, and is meant to
also be run under ThreadSanitizer (see the comment at the top of the file)
for real happens-before-graph verification, not just a lucky pass.

## Building and running the demo

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure   # runs spsc_ring_buffer_test

# Terminal 1 — C++ motor control (creates the shared-memory segment)
./cpp/build/motor_control_consumer

# Terminal 2 — Python perception
python cpp/python/perception_producer_demo.py --count 20
```

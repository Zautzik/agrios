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
  rt_thread.hpp          PREEMPT_RT thread setup: mlockall, SCHED_FIFO, CPU affinity -- see below
src/
  bridge_c_api.cpp       implementation of the C API, built as libagrios_bridge.so
  motor_control_consumer.cpp   demo: simple, non-real-time -- verifies the bridge itself
  motor_control_rt_loop.cpp    the real-time-compliant 1 kHz control loop -- see below
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

`head_`, `tail_`, and the payload buffer each start on their own 64-byte
(`alignas(kCacheLineSize)`) boundary, so a store to one by its owning thread
never invalidates the other thread's cached copy of the other index — the
textbook false-sharing failure mode for SPSC queues, avoided rather than
discovered later in a profiler. `kCacheLineSize` is a fixed constant, not
`std::hardware_destructive_interference_size`: GCC explicitly warns that
value "can vary between compiler versions or with different -mtune/-mcpu
flags" and recommends a fixed constant for anything ABI-facing — which this
is, since `bridge_c_api.cpp` and `shared_ring_buffer.py` both depend on the
resulting byte offsets staying stable.

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

## The real-time control loop (`motor_control_rt_loop.cpp`)

`motor_control_consumer.cpp` (above) is deliberately simple — a poll loop
with a sleep and a `std::cout` per pose, built to verify the bridge itself,
not to demonstrate real-time discipline. `motor_control_rt_loop.cpp` is the
actual PREEMPT_RT / ROS 2 real-time-guideline-compliant 1 kHz loop, built on
top of the same bridge:

- **`mlockall(MCL_CURRENT | MCL_FUTURE)` + stack pre-fault**, first thing in
  the RT thread's body (`agrios::rt::configure_current_thread_realtime` in
  `rt_thread.hpp`). `mlockall` stops any page of this process from being
  swapped out; it does *not* by itself guarantee a page is populated before
  its first touch, so the thread's stack is also explicitly pre-faulted
  (touched once, before the hot loop, using a stack sized for exactly this
  via `make_realtime_thread_attr`) rather than risking a first-touch minor
  fault from an unusually deep call inside the loop.
- **`SCHED_FIFO`, priority 99**, via `pthread_setschedparam`. Worth knowing
  before copying this onto real hardware: priority 99 is the same priority
  Linux gives several of its own kernel housekeeping threads (e.g. the
  per-CPU `migration/N` threads) — a runaway or misbehaving user thread at
  99 can starve kernel-internal work that the box needs to stay healthy.
  Real PREEMPT_RT deployments commonly reserve 99 and run application
  threads at something like 90-98 for exactly this reason. This code uses
  99 because that's what was specified; know what you're asking for if you
  reuse it.
- **`pthread_setaffinity_np()`**, pinning to one CPU core. Pinning a thread
  to a core is not the same thing as isolating that core: without also
  booting the kernel with `isolcpus=`, `nohz_full=`, and `rcu_nocbs=` for
  that core, the scheduler is still free to run other tasks there between
  this thread's slices, and periodic kernel work (timer ticks, RCU
  callbacks) can still land on it. That kernel-boot-parameter configuration
  is a host-level prerequisite this code cannot set up or verify from
  inside a single process — treat "pinned" and "isolated" as two different
  claims.
- **Zero dynamic allocation and no `std::cout` inside the hot loop.** Every
  loop-body value is a stack value or a pre-allocated buffer. Telemetry
  (per-iteration jitter, pose-received flag) is pushed into a second,
  in-process `SPSCRingBuffer` — the same primitive verified above, reused
  for a thread-to-thread pairing instead of the cross-process one — and
  drained by a separate, ordinary-priority monitor thread that does all the
  actual printing. This is the same shape as ROS 2 control's
  `RealtimeBuffer`/`RealtimePublisher`: get state out of the RT loop without
  letting a logger's I/O jitter leak into the control loop's own timing.
- **Absolute-deadline scheduling**, not a relative sleep. Each iteration
  advances a `CLOCK_MONOTONIC` deadline by exactly one period and sleeps to
  that deadline with `clock_nanosleep(..., TIMER_ABSTIME, ...)`. A relative
  `sleep_for(period)` re-measured from "now" each pass would accumulate that
  pass's own execution time as drift on every iteration; anchoring to a
  fixed, monotonically-advancing deadline doesn't.
- **`-Wl,-z,now`** on this executable specifically (see `CMakeLists.txt`):
  forces eager, load-time symbol resolution instead of lazy binding, so the
  *first* call to a dynamically-linked function from inside the hot loop
  doesn't pay for on-demand dynamic-linker resolution — another possible
  source of first-touch, hard-to-reproduce latency this loop exists to
  eliminate.

### What was actually verified, and what wasn't

Built and run in the same Linux container as everything else above, granted
`--cap-add=SYS_NICE --cap-add=IPC_LOCK` (`SCHED_FIFO` and `mlockall` both
need `CAP_SYS_NICE`/`CAP_IPC_LOCK` or root, which Docker does not grant by
default even to a root user inside the container):

- **Structural correctness: verified.** `mlockall`, `pthread_setschedparam(SCHED_FIFO, 99)`,
  and `pthread_setaffinity_np` all succeeded with the capabilities granted,
  and the loop ran its requested iteration count cleanly with zero crashes.
  Re-ran *without* those capabilities to confirm the failure path actually
  fires rather than silently degrading: it correctly reported
  `pthread_setschedparam(SCHED_FIFO, 99) failed: Operation not permitted`
  and exited nonzero without ever entering the loop, exactly as designed —
  a control loop that silently continues without the real-time guarantees
  it asked for and didn't get is worse than one that refuses to run.
- **Loop-timing logic: verified.** The absolute-deadline scheduling
  correctly produced the requested iteration count in the expected wall-clock
  time, with no drift accumulation, across a 2,000-iteration (~2 s) run.
- **Latency/jitter numbers: measured, but not representative of anything
  beyond this dev machine.** That same run showed a 76 μs average and
  450 μs worst-case deadline miss, with 76/2000 iterations over a 200 μs
  overrun threshold. Those numbers are **not** a real-time performance claim
  — they were measured inside Docker Desktop on WSL2, i.e. a container on
  top of a *stock*, non-PREEMPT_RT-patched Linux kernel, itself inside a
  virtualized VM, with no core isolation configured anywhere in that stack.
  Every layer of that is exactly what PREEMPT_RT and `isolcpus` exist to
  eliminate. What's genuinely proven here is that the code's real-time
  *mechanisms* work correctly when granted the right privileges and that its
  *timing logic* doesn't drift — not that this loop meets any particular
  latency bound. Re-measuring on a real PREEMPT_RT-patched kernel with an
  actually-isolated core is a prerequisite for treating any latency number
  from this loop as meaningful.

## Building and running the demos

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure   # runs spsc_ring_buffer_test

# Terminal 1 — C++ motor control (creates the shared-memory segment)
./cpp/build/motor_control_consumer

# Terminal 2 — Python perception
python cpp/python/perception_producer_demo.py --count 20
```

The real-time loop, in place of the simple consumer above (needs
`CAP_SYS_NICE`/`CAP_IPC_LOCK` or root — e.g. `docker run --cap-add=SYS_NICE
--cap-add=IPC_LOCK ...`):

```bash
./cpp/build/motor_control_rt_loop --core 0 --priority 99 --iterations 2000
```

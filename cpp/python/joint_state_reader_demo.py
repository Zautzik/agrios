"""Demo reader: the monitoring/perception side of the joint-telemetry bridge.

Attaches to the joint-telemetry shared-memory segment that
cpp/src/motor_control_rt_loop.cpp publishes synthetic joint-angle telemetry
into, and prints the latest reading a few times. Proves the second,
independent shared-memory channel (motor control -> Python) actually works,
the same role perception_producer_demo.py plays for the pose channel
(Python -> motor control) -- see cpp/README.md.

The RT loop is the owner/creator of this segment (it must already be
running, or this will fail to attach) -- see cpp/src/motor_control_rt_loop.cpp.

Usage:
    ./motor_control_rt_loop --iterations 3000 &   # start the RT loop first
    python joint_state_reader_demo.py --count 10
"""

from __future__ import annotations

import argparse
import time

from shared_ring_buffer import DEFAULT_JOINT_SHM_NAME, JointStateBridge


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=10, help="number of readings to print")
    parser.add_argument("--interval", type=float, default=0.2, help="seconds between readings")
    parser.add_argument("--shm-name", default=DEFAULT_JOINT_SHM_NAME)
    args = parser.parse_args()

    with JointStateBridge(name=args.shm_name, owner=False) as bridge:
        print(f"joint_state_reader_demo: attached to {args.shm_name}")

        seen = 0
        misses = 0
        while seen < args.count:
            state = bridge.read_latest()  # non-blocking; never stalls the RT writer
            if state is None:
                misses += 1
                time.sleep(args.interval)
                continue

            seen += 1
            angles = ", ".join(f"{a:+.3f}" for a in state.joint_angles_rad)
            print(f"  reading #{seen}: joint_angles_rad=[{angles}] t={state.timestamp_ns}ns")
            time.sleep(args.interval)

        print(f"joint_state_reader_demo: done ({seen} readings, {misses} empty polls)")


if __name__ == "__main__":
    main()

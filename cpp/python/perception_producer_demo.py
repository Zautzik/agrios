"""Demo producer: the "Python perception" side of the bridge.

Simulates a perception pipeline publishing target poses (e.g. a detected
object's pose to move a farm robot's end effector toward) into the shared
ring buffer that cpp/src/motor_control_consumer.cpp reads from.

The C++ consumer is the owner/creator of the shared-memory segment (it must
already be running, or this will fail to attach) -- see
cpp/src/motor_control_consumer.cpp.

Usage:
    ./motor_control_consumer &          # start the consumer first
    python perception_producer_demo.py --count 20
"""

from __future__ import annotations

import argparse
import math
import time

from shared_ring_buffer import DEFAULT_SHM_NAME, Pose6D, PoseBridge


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=20, help="number of poses to publish")
    parser.add_argument("--hz", type=float, default=10.0, help="publish rate")
    parser.add_argument("--shm-name", default=DEFAULT_SHM_NAME)
    args = parser.parse_args()

    period_s = 1.0 / args.hz

    with PoseBridge(name=args.shm_name, owner=False) as bridge:
        print(f"perception_producer_demo: attached to {args.shm_name}, "
              f"publishing {args.count} poses at {args.hz} Hz")

        for i in range(args.count):
            # Synthetic circular trajectory -- stands in for a real
            # perception pipeline's detected target pose.
            angle = 2 * math.pi * (i / max(args.count, 1))
            pose = Pose6D(
                x=math.cos(angle),
                y=math.sin(angle),
                z=0.5,
                pitch=0.0,
                yaw=angle,
                roll=0.0,
                timestamp_ns=time.monotonic_ns(),
            )

            while not bridge.push(pose):
                # Ring buffer full: consumer is behind, back off briefly.
                time.sleep(period_s / 10)

            print(f"  pushed #{i + 1}: x={pose.x:.3f} y={pose.y:.3f} yaw={pose.yaw:.3f}")
            time.sleep(period_s)

        # Sentinel: timestamp_ns < 0 tells the demo consumer to shut down.
        shutdown = Pose6D(0, 0, 0, 0, 0, 0, timestamp_ns=-1)
        while not bridge.push(shutdown):
            time.sleep(period_s / 10)
        print("perception_producer_demo: sent shutdown sentinel, done")


if __name__ == "__main__":
    main()

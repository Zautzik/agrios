"""Demo producer: the "Python perception" side of the bridge.

Simulates a perception pipeline (e.g. FoundationPose/ArUco) publishing
target poses -- translation + Rodrigues rotation vector, the representation
those pipelines emit natively -- into the shared ring buffer that
cpp/src/motor_control_consumer.cpp reads from.

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
            # perception pipeline's detected target pose. rvec=(0,0,angle)
            # is a pure rotation about the Z axis by `angle` radians: the
            # Rodrigues vector for a rotation about a unit axis u by angle
            # theta is simply u*theta, and (0,0,1) is already unit length.
            angle = 2 * math.pi * (i / max(args.count, 1))
            pose = Pose6D(
                tvec=(math.cos(angle), math.sin(angle), 0.5),
                rvec=(0.0, 0.0, angle),
                timestamp_ns=time.monotonic_ns(),
            )

            while not bridge.push(pose):
                # Ring buffer full: consumer is behind, back off briefly.
                time.sleep(period_s / 10)

            tx, ty, tz = pose.tvec
            print(f"  pushed #{i + 1}: tvec=({tx:.3f}, {ty:.3f}, {tz:.3f}) rvec_z={pose.rvec[2]:.3f}")
            time.sleep(period_s)

        # Sentinel: timestamp_ns < 0 tells the demo consumer to shut down.
        shutdown = Pose6D(tvec=(0.0, 0.0, 0.0), rvec=(0.0, 0.0, 0.0), timestamp_ns=-1)
        while not bridge.push(shutdown):
            time.sleep(period_s / 10)
        print("perception_producer_demo: sent shutdown sentinel, done")


if __name__ == "__main__":
    main()

"""LangGraph tools bridging Agrios's agent to the C++ real-time motor-control
loop over the shared-memory channels in cpp/ -- see cpp/README.md for the
bridge design and NOTES.md for how it was built and verified.

Kept out of main.py rather than defined alongside the fixture/SQLAlchemy
tools there: this module's dependency footprint is fundamentally different
(POSIX shared memory via ctypes, a sibling C++ build) from the rest of the
agent, and isolating it means that footprint can't leak into the tools that
don't need it.

Both tools are scoped to what the bridge can honestly promise:
  - submit_spatial_intent's return value distinguishes "delivered to the
    ring buffer" from "no motor-control process is attached to receive it"
    -- push() succeeding just means there was room in the buffer, not that
    anything is actually reading it, and silently accepting a spatial
    command that will never be acted on is exactly the kind of thing that
    should be surfaced, not swallowed.
  - read_joint_states never blocks the real-time writer -- see
    JointStateBridge.read_latest() in cpp/python/shared_ring_buffer.py.

The underlying bridge is Linux-only (POSIX shared memory) and requires
libagrios_bridge.so to have been built (see cpp/CMakeLists.txt). On a
platform or environment where that isn't true -- including this project's
own Windows dev machine -- both tools report that plainly at call time
instead of crashing the agent, or the whole process at import time.
"""

import logging
import sys
import time
from pathlib import Path
from typing import Optional

from langchain_core.tools import tool
from pydantic import BaseModel, Field

logger = logging.getLogger(__name__)

# cpp/python isn't an installed package -- it's a sibling directory in this
# same repo, already a complete, independently-verified module (see
# NOTES.md's bridge entries). Importing it this way instead of duplicating
# its ctypes/shared-memory logic here keeps there being exactly one
# implementation of the bridge protocol -- the same reason that module
# exists instead of a second Python reimplementation in the first place.
_CPP_PYTHON_DIR = Path(__file__).parent / "cpp" / "python"
if str(_CPP_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(_CPP_PYTHON_DIR))

from shared_ring_buffer import (  # noqa: E402
    DEFAULT_JOINT_SHM_NAME,
    DEFAULT_SHM_NAME,
    JointStateBridge,
    Pose6D,
    PoseBridge,
)


class SubmitSpatialIntent(BaseModel):
    """A 6D target pose from the FoundationPose/ArUco perception pipeline,
    to be delivered to the C++ motor-control real-time loop over shared
    memory."""

    tvec: tuple[float, float, float] = Field(
        ..., description="Translation vector [tx, ty, tz] in meters"
    )
    rvec: tuple[float, float, float] = Field(
        ...,
        description=(
            "Rodrigues rotation vector [rx, ry, rz] in radians (axis * "
            "angle) -- this is what cv2.solvePnP / ArUco pose estimation "
            "return directly, not Euler angles"
        ),
    )


# Lazy, module-level singletons: opened on first use, not at import time.
# Neither the C++ motor-control process nor libagrios_bridge.so is expected
# to exist on every machine that imports this module (this project's own
# Windows dev environment included) -- failing at import time would take the
# whole agent down over a robotics-hardware dependency most of the agent
# doesn't otherwise need.
_pose_bridge: Optional[PoseBridge] = None
_joint_bridge: Optional[JointStateBridge] = None


def _get_pose_bridge() -> PoseBridge:
    global _pose_bridge
    if _pose_bridge is None:
        # owner=False: the C++ motor-control loop (or motor_control_consumer,
        # for testing) is expected to be the process that creates this
        # segment. Left unset on failure so the next call retries rather than
        # latching a permanent failure if the C++ side starts up later.
        _pose_bridge = PoseBridge(name=DEFAULT_SHM_NAME, owner=False)
    return _pose_bridge


def _get_joint_bridge() -> JointStateBridge:
    global _joint_bridge
    if _joint_bridge is None:
        _joint_bridge = JointStateBridge(name=DEFAULT_JOINT_SHM_NAME, owner=False)
    return _joint_bridge


@tool(args_schema=SubmitSpatialIntent)
def submit_spatial_intent(tvec: tuple[float, float, float], rvec: tuple[float, float, float]) -> str:
    """Sends a 6D target pose (translation + Rodrigues rotation vector) to
    the C++ motor-control real-time loop. Use when the user wants the robot
    to move to, reach for, or orient itself toward a specific spatial
    target -- not for anything about crops, weather, or farm-task logging."""
    try:
        bridge = _get_pose_bridge()
    except (FileNotFoundError, RuntimeError) as e:
        logger.error("submit_spatial_intent: bridge unavailable: %s", e)
        return f"Could not reach the motor-control bridge: {e}"

    pose = Pose6D(tvec=tvec, rvec=rvec, timestamp_ns=time.monotonic_ns())
    if not bridge.push(pose):
        return (
            "Spatial intent NOT delivered: the shared-memory ring buffer is "
            "full, which means the motor-control loop isn't draining it -- "
            "check that motor_control_rt_loop is actually running."
        )
    return f"Spatial intent delivered: tvec={tvec}, rvec={rvec}"


@tool
def read_joint_states() -> str:
    """Returns the robot's current physical joint angles, read from the
    motor-control loop's live telemetry. Use when the user asks about the
    robot's current pose, joint positions, or whether it has reached a
    target -- not for anything about crops, weather, or farm-task logging."""
    try:
        bridge = _get_joint_bridge()
    except (FileNotFoundError, RuntimeError) as e:
        logger.error("read_joint_states: bridge unavailable: %s", e)
        return f"Could not reach the joint-telemetry bridge: {e}"

    state = bridge.read_latest()
    if state is None:
        return (
            "No joint-state telemetry available yet -- check that "
            "motor_control_rt_loop is actually running and has published "
            "at least one sample."
        )
    angles = ", ".join(f"{a:.4f}" for a in state.joint_angles_rad)
    return f"Current joint angles (rad): [{angles}] (sampled {state.timestamp_ns}ns)"

"""Python side of the Agrios perception <-> motor-control shared-memory bridge.

Deliberately thin: every call here goes straight into the compiled
libagrios_bridge shared library (see cpp/src/bridge_c_api.cpp), which runs
the exact same std::atomic push()/pop() as the C++ side. This module does
not re-derive the shared-memory layout or attempt its own acquire/release
logic in Python -- struct/ctypes have no memory-order-aware atomics, and a
naive Python-side implementation would only happen to work on x86-64's
strong memory model. It would be a silent, real bug on a weakly-ordered
target like the ARM64 Jetson/Raspberry Pi boards this bridge is meant to run
motor control on eventually. Routing through the compiled shim means there
is exactly one implementation of the protocol, used by both languages.

Two independent channels, matching bridge_c_api.h:
    PoseBridge        perception -> motor control (target poses)
    JointStateBridge  motor control -> monitoring/perception (joint telemetry)

Requires libagrios_bridge.so to be built first (see cpp/CMakeLists.txt) and
discoverable via the AGRIOS_BRIDGE_LIB env var or one of the default build-
output locations this module checks.
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
from typing import NamedTuple, Optional

_HERE = Path(__file__).resolve().parent

_DEFAULT_SEARCH_PATHS = [
    Path(os.environ["AGRIOS_BRIDGE_LIB"]) if "AGRIOS_BRIDGE_LIB" in os.environ else None,
    _HERE.parent / "build" / "libagrios_bridge.so",
    _HERE.parent / "build" / "Release" / "libagrios_bridge.so",
    Path("/usr/local/lib/libagrios_bridge.so"),
]

DEFAULT_SHM_NAME = "/agrios_pose_bridge"
DEFAULT_JOINT_SHM_NAME = "/agrios_joint_telemetry"


# ---- Pose bridge: perception -> motor control ----


class Pose6D(NamedTuple):
    tvec: tuple[float, float, float]  # translation vector [tx, ty, tz], meters
    rvec: tuple[float, float, float]  # Rodrigues rotation vector [rx, ry, rz], radians
    timestamp_ns: int


class _CPose6D(ctypes.Structure):
    # Field order and types must match AgriosPose6D in bridge_c_api.h exactly
    # -- this is a raw memory-layout mirror, not a convenience wrapper.
    _fields_ = [
        ("tvec", ctypes.c_double * 3),
        ("rvec", ctypes.c_double * 3),
        ("timestamp_ns", ctypes.c_int64),
    ]


# ---- Joint telemetry: motor control -> monitoring/perception ----


class JointState6(NamedTuple):
    joint_angles_rad: tuple[float, float, float, float, float, float]
    timestamp_ns: int


class _CJointState6(ctypes.Structure):
    # Field order and types must match AgriosJointState6 in bridge_c_api.h.
    _fields_ = [
        ("joint_angles_rad", ctypes.c_double * 6),
        ("timestamp_ns", ctypes.c_int64),
    ]


def _load_library() -> ctypes.CDLL:
    for path in _DEFAULT_SEARCH_PATHS:
        if path is not None and path.exists():
            return ctypes.CDLL(str(path))
    raise FileNotFoundError(
        "libagrios_bridge.so not found. Build it first:\n"
        "  cmake -S cpp -B cpp/build && cmake --build cpp/build\n"
        "or set AGRIOS_BRIDGE_LIB to its full path. Checked: "
        + ", ".join(str(p) for p in _DEFAULT_SEARCH_PATHS if p is not None)
    )


_lib = None  # lazily loaded so importing this module doesn't require the .so to exist yet


def _lib_handle() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        lib = _load_library()
        lib.agrios_bridge_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.agrios_bridge_open.restype = ctypes.c_void_p
        lib.agrios_bridge_close.argtypes = [ctypes.c_void_p]
        lib.agrios_bridge_close.restype = None
        lib.agrios_bridge_push.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CPose6D)]
        lib.agrios_bridge_push.restype = ctypes.c_int
        lib.agrios_bridge_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CPose6D)]
        lib.agrios_bridge_pop.restype = ctypes.c_int

        lib.agrios_joint_bridge_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.agrios_joint_bridge_open.restype = ctypes.c_void_p
        lib.agrios_joint_bridge_close.argtypes = [ctypes.c_void_p]
        lib.agrios_joint_bridge_close.restype = None
        lib.agrios_joint_bridge_push.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CJointState6)]
        lib.agrios_joint_bridge_push.restype = ctypes.c_int
        lib.agrios_joint_bridge_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CJointState6)]
        lib.agrios_joint_bridge_pop.restype = ctypes.c_int
        _lib = lib
    return _lib


class PoseBridge:
    """Attaches to (or creates) the shared-memory pose ring buffer.

    Usage:
        with PoseBridge(owner=True) as bridge:   # e.g. the C++ consumer's counterpart
            bridge.push(Pose6D(tvec=(1.0, 2.0, 0.0), rvec=(0.0, 0.0, 0.0),
                                timestamp_ns=time.monotonic_ns()))

    `owner=True` creates/resets the segment -- call this from exactly one
    process (whichever starts first). Every other process attaches with
    owner=False.
    """

    def __init__(self, name: str = DEFAULT_SHM_NAME, owner: bool = False):
        self._lib = _lib_handle()
        self._handle: Optional[int] = self._lib.agrios_bridge_open(name.encode("utf-8"), int(owner))
        if not self._handle:
            raise RuntimeError(f"agrios_bridge_open({name!r}, owner={owner}) failed -- see stderr")

    def close(self) -> None:
        if self._handle:
            self._lib.agrios_bridge_close(self._handle)
            self._handle = None

    def __enter__(self) -> "PoseBridge":
        return self

    def __exit__(self, *_exc_info) -> None:
        self.close()

    def push(self, pose: Pose6D) -> bool:
        """Returns False if the ring buffer is full (never blocks)."""
        c_pose = _CPose6D((ctypes.c_double * 3)(*pose.tvec), (ctypes.c_double * 3)(*pose.rvec),
                           pose.timestamp_ns)
        return bool(self._lib.agrios_bridge_push(self._handle, ctypes.byref(c_pose)))

    def pop(self) -> Optional[Pose6D]:
        """Returns None if the ring buffer is empty (never blocks)."""
        c_pose = _CPose6D()
        if not self._lib.agrios_bridge_pop(self._handle, ctypes.byref(c_pose)):
            return None
        return Pose6D(tuple(c_pose.tvec), tuple(c_pose.rvec), c_pose.timestamp_ns)


class JointStateBridge:
    """Attaches to (or creates) the shared-memory joint-telemetry ring buffer.

    In the normal topology the motor-control RT loop is the owner (it's the
    producer); readers attach with owner=False (the default).
    """

    def __init__(self, name: str = DEFAULT_JOINT_SHM_NAME, owner: bool = False):
        self._lib = _lib_handle()
        self._handle: Optional[int] = self._lib.agrios_joint_bridge_open(name.encode("utf-8"), int(owner))
        if not self._handle:
            raise RuntimeError(f"agrios_joint_bridge_open({name!r}, owner={owner}) failed -- see stderr")

    def close(self) -> None:
        if self._handle:
            self._lib.agrios_joint_bridge_close(self._handle)
            self._handle = None

    def __enter__(self) -> "JointStateBridge":
        return self

    def __exit__(self, *_exc_info) -> None:
        self.close()

    def push(self, state: JointState6) -> bool:
        """Returns False if the ring buffer is full (never blocks)."""
        c_state = _CJointState6((ctypes.c_double * 6)(*state.joint_angles_rad), state.timestamp_ns)
        return bool(self._lib.agrios_joint_bridge_push(self._handle, ctypes.byref(c_state)))

    def pop(self) -> Optional[JointState6]:
        """Returns None if the ring buffer is empty (never blocks)."""
        c_state = _CJointState6()
        if not self._lib.agrios_joint_bridge_pop(self._handle, ctypes.byref(c_state)):
            return None
        return JointState6(tuple(c_state.joint_angles_rad), c_state.timestamp_ns)

    def read_latest(self) -> Optional[JointState6]:
        """Drains the ring buffer and returns only the freshest sample.

        This is a FIFO ring buffer, so a single pop() returns the oldest
        unread sample, not the newest -- fine for the pose bridge (every
        pose matters), wrong for "what's the current joint state" (only the
        newest reading matters; older ones are just backlog). Draining is
        still non-blocking and never stalls the writer: each pop() is
        wait-free and this loop only ever does as many of them as are
        already queued, terminating the moment pop() reports empty.
        """
        latest: Optional[JointState6] = None
        while True:
            sample = self.pop()
            if sample is None:
                return latest
            latest = sample

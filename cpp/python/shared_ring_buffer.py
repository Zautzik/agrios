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


class Pose6D(NamedTuple):
    x: float
    y: float
    z: float
    pitch: float
    yaw: float
    roll: float
    timestamp_ns: int


class _CPose6D(ctypes.Structure):
    # Field order and types must match AgriosPose6D in bridge_c_api.h exactly
    # -- this is a raw memory-layout mirror, not a convenience wrapper.
    _fields_ = [
        ("x", ctypes.c_double),
        ("y", ctypes.c_double),
        ("z", ctypes.c_double),
        ("pitch", ctypes.c_double),
        ("yaw", ctypes.c_double),
        ("roll", ctypes.c_double),
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
        _lib = _load_library()
        _lib.agrios_bridge_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
        _lib.agrios_bridge_open.restype = ctypes.c_void_p
        _lib.agrios_bridge_close.argtypes = [ctypes.c_void_p]
        _lib.agrios_bridge_close.restype = None
        _lib.agrios_bridge_push.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CPose6D)]
        _lib.agrios_bridge_push.restype = ctypes.c_int
        _lib.agrios_bridge_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CPose6D)]
        _lib.agrios_bridge_pop.restype = ctypes.c_int
    return _lib


class PoseBridge:
    """Attaches to (or creates) the shared-memory pose ring buffer.

    Usage:
        with PoseBridge(owner=True) as bridge:   # e.g. the C++ consumer's counterpart
            bridge.push(Pose6D(x=1.0, y=2.0, z=0.0, pitch=0, yaw=0, roll=0,
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
        c_pose = _CPose6D(*pose)
        return bool(self._lib.agrios_bridge_push(self._handle, ctypes.byref(c_pose)))

    def pop(self) -> Optional[Pose6D]:
        """Returns None if the ring buffer is empty (never blocks)."""
        c_pose = _CPose6D()
        if not self._lib.agrios_bridge_pop(self._handle, ctypes.byref(c_pose)):
            return None
        return Pose6D(
            c_pose.x, c_pose.y, c_pose.z, c_pose.pitch, c_pose.yaw, c_pose.roll, c_pose.timestamp_ns
        )

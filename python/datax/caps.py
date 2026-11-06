# SPDX-License-Identifier: MIT
"""Capabilities: public flags, probe-detected flags, and numeric limits."""
import ctypes as C

from ._lib import c_ull, get_lib

# Order matches enum datax_cap / enum datax_limit in datax.h.
CAP_NAMES = (
    "timed_commands", "rx_arm", "tx_arm", "stream_gate", "timestamps",
    "fastlock_hop", "fhm_hop", "mcs", "phase_coherent",
    "net_offload_nic", "net_offload_samples",
)
LIMIT_NAMES = ("tick_hz", "queue_depth", "hop_table_depth",
               "guard_margin_ms", "max_channels")


class Capabilities:
    """``"name" in dev.caps`` tests the public flags (P3a: timed_commands, fhm_hop,
    mcs, phase_coherent when the SDK can do them on this board).

    ``detected`` is the set the probe found (lowercase names), independent of
    the public flags; ``limits`` maps lowercase limit names to ints.
    """

    def __init__(self, device):
        self._dev = device

    def _handle(self):
        self._dev._check()
        return self._dev._caps_ptr

    def __contains__(self, name):
        if name not in CAP_NAMES:
            return False
        return bool(get_lib().datax_caps_has(self._handle(), CAP_NAMES.index(name)))

    @property
    def detected(self):
        self._dev._check()
        bits = get_lib().datax_internal_probe_flags(self._dev._ptr)
        return frozenset(n for i, n in enumerate(CAP_NAMES) if bits & (1 << i))

    @property
    def limits(self):
        h = self._handle()
        out = {}
        for i, n in enumerate(LIMIT_NAMES):
            v = c_ull()
            rc = get_lib().datax_caps_get_limit(h, i, C.byref(v))
            if rc == 0:
                out[n] = v.value
        return out

    def __iter__(self):
        return iter([n for n in CAP_NAMES if n in self])

    def __repr__(self):
        return f"Capabilities({sorted(self)!r}, detected={sorted(self.detected)!r})"

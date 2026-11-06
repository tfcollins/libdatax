# SPDX-License-Identifier: MIT
"""P3a multi-chip sync on the ADRV9009-ZU11EG: verification only, and why
PHASE_COHERENT stays false (hopcap-sfp image)."""
import ctypes as C

import pytest

import datax
import hwtimed as ht
import loopback as lb

pytestmark = pytest.mark.p3a


def _mcs_info(dev):
    fn = datax._lib.get_lib().datax_internal_mcs_info
    fn.restype = C.c_int
    fn.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t]
    state, reason = C.create_string_buffer(256), C.create_string_buffer(256)
    rc = fn(dev._ptr, state, len(state), reason, len(reason))
    return rc, state.value.decode(), reason.value.decode()


def test_run_mcs_verifies(dev, record):
    assert "mcs" in dev.caps, "the ZU11EG hopcap image must report the MCS capability"
    dev.run_mcs()                                  # raises EIO naming the chip/state when not done
    rc, state, reason = _mcs_info(dev)
    record("fsm_state", state)
    record("phase_coherent_rc", rc)
    record("phase_coherent_reason", reason)
    assert "opt_post_running_stage" in state, state


def test_phase_coherent_false(dev, record):
    dev.run_mcs()
    caps = list(dev.caps)
    record("caps", caps)
    assert "mcs" in caps
    assert "phase_coherent" not in caps            # MCS alone does not fix the LO phase
    rc, _, reason = _mcs_info(dev)
    assert rc == 0, reason


def test_phase_sync_attribute_absent(dev, hw_ctx, record):
    """List every attribute of the adrv9009 phys that mentions phase or sync.
    The record documents why PHASE_COHERENT is false: the only sync-like
    attribute is the JESD multichip_sync; no RF PLL phase sync control is
    exposed. (A new attribute here is news, not a failure of this test.)"""
    found, listing = {}, {}
    for phy in (ht.PHY_A, ht.PHY_B):
        names = ht.attribute_names(hw_ctx, phy)
        if names is None:
            continue
        flat = set(names["attrs"]) | set(names["debug"])
        for chan in names["channels"].values():
            flat |= set(chan)
        found[phy] = ht.names_matching(flat)
        listing[phy] = {"device_attrs": len(names["attrs"]), "debug_attrs": len(names["debug"]),
                        "channels": len(names["channels"])}
    record("phys_listed", listing)
    record("phase_or_sync_attributes", found)
    record("phase_named_attributes",
           {p: [n for n in v if "phase" in n.lower()] for p, v in found.items()})
    assert found, f"neither {ht.PHY_A} nor {ht.PHY_B} found in the context"
    assert "phase_coherent" not in dev.caps

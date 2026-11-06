# SPDX-License-Identifier: MIT
"""Locate and load libdatax.so and declare the C signatures."""
import ctypes as C
import ctypes.util
import os

from .errors import Error

c_ull = C.c_ulonglong
c_uint = C.c_uint
c_void_p = C.c_void_p

# datax_event_cb: void (*)(const struct datax_event *, void *)
EVENT_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p)


class StreamConfig(C.Structure):
    """struct datax_stream_config."""
    _fields_ = [("direction", C.c_int), ("channel_mask", C.c_uint),
                ("block_samples", C.c_size_t), ("block_count", C.c_uint),
                ("cyclic", C.c_bool)]


class BlockInfoC(C.Structure):
    """struct datax_block_info."""
    _fields_ = [("timestamp_ticks", C.c_ulonglong), ("discontinuous", C.c_bool),
                ("valid_samples", C.c_size_t)]


_lib = None


def _load():
    path = os.environ.get("DATAX_LIBRARY") or ctypes.util.find_library("datax")
    if not path:
        raise Error(2, "cannot find libdatax; set DATAX_LIBRARY to the path of libdatax.so")
    try:
        return C.CDLL(path)
    except OSError as e:
        raise Error(2, f"cannot load libdatax from {path!r} (set DATAX_LIBRARY): {e}") from e


def _sig(lib, name, restype, *argtypes):
    fn = getattr(lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)


def get_lib():
    """Return the loaded, typed library (loaded once)."""
    global _lib
    if _lib is not None:
        return _lib
    lib = _load()
    P = C.POINTER
    _sig(lib, "datax_context_create_from_uri", C.c_int, C.c_char_p, P(c_void_p))
    _sig(lib, "datax_context_create_local", C.c_int, P(c_void_p))
    _sig(lib, "datax_context_destroy", None, c_void_p)
    _sig(lib, "datax_context_get_device_count", c_uint, c_void_p)
    _sig(lib, "datax_context_get_device", C.c_int, c_void_p, c_uint, P(c_void_p))
    _sig(lib, "datax_context_get_iio_context", c_void_p, c_void_p)
    _sig(lib, "datax_context_get_last_error", C.c_char_p, c_void_p)
    _sig(lib, "datax_context_get_version", C.c_int, c_void_p, P(c_uint), P(c_uint))
    _sig(lib, "datax_context_set_event_callback", C.c_int, c_void_p, EVENT_CB, c_void_p)
    _sig(lib, "datax_device_get_name", C.c_char_p, c_void_p)
    _sig(lib, "datax_device_get_caps", c_void_p, c_void_p)
    _sig(lib, "datax_caps_has", C.c_bool, c_void_p, C.c_int)
    _sig(lib, "datax_caps_get_limit", C.c_int, c_void_p, C.c_int, P(c_ull))
    _sig(lib, "datax_device_set_frequency", C.c_int, c_void_p, c_uint, c_ull, c_ull)
    _sig(lib, "datax_device_get_frequency", C.c_int, c_void_p, c_uint, P(c_ull))
    _sig(lib, "datax_device_set_gain", C.c_int, c_void_p, c_uint, C.c_double, c_ull)
    _sig(lib, "datax_device_get_gain", C.c_int, c_void_p, c_uint, P(C.c_double))
    _sig(lib, "datax_device_set_sample_rate", C.c_int, c_void_p, c_ull)
    _sig(lib, "datax_device_set_bandwidth", C.c_int, c_void_p, c_uint, c_ull)
    _sig(lib, "datax_device_get_sample_rate", C.c_int, c_void_p, C.POINTER(c_ull))
    _sig(lib, "datax_device_get_bandwidth", C.c_int, c_void_p, c_uint, C.POINTER(c_ull))
    _sig(lib, "datax_device_stage_hop_table", C.c_int, c_void_p, P(c_ull), C.c_size_t)
    _sig(lib, "datax_device_get_time", C.c_int, c_void_p, P(c_ull), P(c_ull))
    _sig(lib, "datax_device_set_time_next_pps", C.c_int, c_void_p, c_ull)
    _sig(lib, "datax_device_run_mcs", C.c_int, c_void_p)
    _sig(lib, "datax_internal_probe_flags", c_uint, c_void_p)
    _sig(lib, "datax_internal_device_stream_info", C.c_int, c_void_p, C.c_int,
         C.c_char_p, C.c_size_t, P(C.c_int))
    _sig(lib, "datax_stream_create", C.c_int, c_void_p, P(StreamConfig), P(c_void_p))
    _sig(lib, "datax_stream_destroy", None, c_void_p)
    _sig(lib, "datax_stream_start", C.c_int, c_void_p)
    _sig(lib, "datax_stream_stop", C.c_int, c_void_p)
    _sig(lib, "datax_stream_recv", C.c_int, c_void_p, c_void_p, C.c_size_t,
         P(BlockInfoC), C.c_int)
    _sig(lib, "datax_stream_send", C.c_int, c_void_p, c_void_p, C.c_size_t, C.c_int)
    _lib = lib
    return lib

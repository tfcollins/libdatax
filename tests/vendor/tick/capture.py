# SPDX-License-Identifier: MIT
"""Shared helpers for timed data capture and transmission.

This module is the single source of truth for the arm-release path: the
capture/transmit descriptor construction, the ADI ``data_offload``
register map and the access routes that reach it, and the in-band
timestamp header codec. The example script
(``examples/timed_capture_burst.py``) and the hardware test suite
(``hardware_tests/test_timed_capture_iio.py``) both import from here so
the wire-level behaviour cannot diverge between them.

Division of labour (a hardware constraint, not a design preference)
-------------------------------------------------------------------
``axi_dmac`` has no trigger input on the AD9081 designs: ``SYNC_TRANSFER_START``
is 0, the DMA is AXI-Stream sourced so ``fifo_wr_sync`` does not exist,
and ``s_axis_xfer_req``/``m_axis_xfer_req`` are status *outputs*. The only
arm-release primitive in the datapath is the ``sync_ext`` input on the ADI
``data_offload`` blocks. So:

1. The host creates the RX or TX buffer through libiio. With the offload
   in ``HARDWARE`` sync mode this parks its state machine in the sync
   state instead of transferring.
2. A scheduled ``CAPTURE_ARM``/``TRANSMIT_ARM`` descriptor pulses the
   gate line wired to ``sync_ext``, releasing the burst on that tick.

The gate is a **one-shot pulse**, not a latched level: ``sync_ext`` is
level-sensitive, so a latched gate would release not only the intended
burst but every burst after it. The pulse width is a correctness
parameter because ``sync_ext`` crosses into the offload's device clock
through a two-stage synchronizer, so a single-cycle pulse can be dropped
entirely; the HDL floors ``args[1]`` to a CDC-safe minimum.

Board-design convention on ZCU102/AD9081, established by
``hdl/projects/ad9081_fmca_ebz/zcu102/tick_ad9081_zcu102.tcl``::

    stream_gate_en[0] -> mxfe_rx_data_offload/sync_ext   (capture release)
    stream_gate_en[1] -> mxfe_tx_data_offload/sync_ext   (transmit release)

numpy and pylibiio are imported lazily inside the functions that need
them so this module stays importable on hosts without either (the
hardware-test contract collects tests on bare CI machines).
"""

from __future__ import annotations

import os
import re
import struct
import subprocess
from collections.abc import Callable
from dataclasses import dataclass

from .descriptor import CommandDescriptor
from .enums import CommandID, Flag, TargetFamily

# ---------------------------------------------------------------------------
# Gate assignments and pulse width
# ---------------------------------------------------------------------------

#: ``stream_gate_en`` index wired to the RX data-offload ``sync_ext``.
CAPTURE_GATE_INDEX = 0
#: ``stream_gate_en`` index wired to the TX data-offload ``sync_ext``.
TRANSMIT_GATE_INDEX = 1

#: Default ``args[1]`` pulse width in scheduler ticks.
#:
#: ``sync_ext`` is resynchronized into the offload's device clock by a
#: two-stage synchronizer, so the pulse must be wide enough to be sampled
#: there. At Tick's 100 MHz timebase against the AD9081 device clock the
#: CDC floor is a small number of ticks; 16 leaves generous margin while
#: staying far below any plausible inter-burst spacing. Passing 0 asks the
#: HDL for its own default instead.
DEFAULT_ARM_PULSE_TICKS = 16

#: Sentinel meaning "let the device choose the pulse width".
DEVICE_DEFAULT_PULSE_TICKS = 0

# ---------------------------------------------------------------------------
# In-band capture timestamp header
# ---------------------------------------------------------------------------

#: First word of the header. Little-endian on the wire, so a hexdump of a
#: tagged buffer literally reads ``TICK`` at offset 0.
CAPTURE_HEADER_MAGIC = 0x4B434954
CAPTURE_HEADER_VERSION = 1
#: Fixed header size in bytes. Also the number of 32-bit words it spans
#: times four (``header_words`` = 8).
CAPTURE_HEADER_BYTES = 32
CAPTURE_HEADER_WORDS = CAPTURE_HEADER_BYTES // 4

# magic, version, header_words, flags, tick, seq, target, command, tick_hz, burst
_HEADER_STRUCT = struct.Struct("<IHBBQIHHII")

#: ``flags`` bit assignments.
CAPTURE_FLAG_VALID = 1 << 0
CAPTURE_FLAG_STALE = 1 << 1
CAPTURE_FLAG_LATE_EXECUTED = 1 << 2
_CAPTURE_FLAG_GATE_SHIFT = 4
_CAPTURE_FLAG_GATE_MASK = 0xF


class CaptureHeaderError(ValueError):
    """Raised when a buffer does not carry a well-formed capture header."""


@dataclass(frozen=True)
class CaptureHeader:
    """Decoded in-band capture timestamp header.

    ``capture_tick`` is the scheduler tick at which the arm-release gate
    fired, i.e. the instant the first sample of this burst was released.
    It is the *scheduled* tick latched by the HDL, not a sample of a
    free-running counter, so it is exact rather than approximate.
    """

    magic: int
    version: int
    header_words: int
    flags: int
    capture_tick: int
    sequence_id: int
    target_id: int
    command_id: int
    tick_hz: int
    burst_index: int

    @property
    def gate_id(self) -> int:
        return (self.flags >> _CAPTURE_FLAG_GATE_SHIFT) & _CAPTURE_FLAG_GATE_MASK

    @property
    def valid(self) -> bool:
        return bool(self.flags & CAPTURE_FLAG_VALID)

    @property
    def stale(self) -> bool:
        """True when no new tag was latched since the previous tagged burst.

        The HDL cannot tear the tag (it is loaded by a single register
        write in the source clock domain), so the pathological case is a
        *stale* value rather than a corrupt one. This flag reports it.
        """
        return bool(self.flags & CAPTURE_FLAG_STALE)

    @property
    def late_executed(self) -> bool:
        return bool(self.flags & CAPTURE_FLAG_LATE_EXECUTED)

    @property
    def capture_seconds(self) -> float:
        """``capture_tick`` converted to seconds using the header's own
        ``tick_hz``, so no extra device query is needed."""
        if self.tick_hz <= 0:
            raise CaptureHeaderError(f"header carries a non-positive tick_hz: {self.tick_hz}")
        return self.capture_tick / self.tick_hz


def build_capture_header(
    *,
    capture_tick: int,
    gate_id: int = CAPTURE_GATE_INDEX,
    sequence_id: int = 0,
    target_id: int = int(TargetFamily.CAPTURE),
    command_id: int = int(CommandID.CAPTURE_ARM),
    tick_hz: int = 0,
    burst_index: int = 0,
    valid: bool = True,
    stale: bool = False,
    late_executed: bool = False,
) -> bytes:
    """Build the 32-byte header the HDL tagger emits.

    This is the golden model for the RTL contract: the tagger testbench
    writes out the header beat it generates and a local test parses it
    back through :func:`parse_capture_header`, so RTL/userspace agreement
    is checkable in CI without a board.
    """

    if not 0 <= gate_id <= _CAPTURE_FLAG_GATE_MASK:
        raise ValueError(f"gate_id must fit in 4 bits, got {gate_id}")
    if not 0 <= capture_tick <= 0xFFFF_FFFF_FFFF_FFFF:
        raise ValueError(f"capture_tick must be a uint64, got {capture_tick}")

    flags = gate_id << _CAPTURE_FLAG_GATE_SHIFT
    if valid:
        flags |= CAPTURE_FLAG_VALID
    if stale:
        flags |= CAPTURE_FLAG_STALE
    if late_executed:
        flags |= CAPTURE_FLAG_LATE_EXECUTED

    return _HEADER_STRUCT.pack(
        CAPTURE_HEADER_MAGIC,
        CAPTURE_HEADER_VERSION,
        CAPTURE_HEADER_WORDS,
        flags,
        capture_tick,
        sequence_id,
        target_id,
        command_id,
        tick_hz,
        burst_index,
    )


def capture_header_span(beat_bytes: int) -> int:
    """Leading bytes of a tagged buffer that are header, not samples.

    The header is a fixed 32-byte blob, but the tagger overwrites whole
    AXI-Stream beats, so on a bus wider than 32 bytes the remainder of
    the first beat is zero padding that is also not sample data.

    ``beat_bytes`` must be supplied by the caller rather than assumed: it
    is ``adc_data_width / 8``, derived at block-design elaboration from
    the JESD profile (128 bit = 16 bytes on stock ZCU102/AD9081), so any
    change to the JESD mode changes it.
    """

    if beat_bytes <= 0:
        raise ValueError(f"beat_bytes must be positive, got {beat_bytes}")
    return max(CAPTURE_HEADER_BYTES, beat_bytes)


def has_capture_header(payload: bytes) -> bool:
    """True when ``payload`` begins with a plausible capture header.

    Checks the magic word and the version, so an untagged buffer whose
    samples happen to start with the magic bytes is still very unlikely
    to be misread as tagged.
    """

    if len(payload) < CAPTURE_HEADER_BYTES:
        return False
    magic, version = struct.unpack_from("<IH", payload, 0)
    return magic == CAPTURE_HEADER_MAGIC and version == CAPTURE_HEADER_VERSION


def parse_capture_header(payload: bytes, beat_bytes: int | None = None) -> CaptureHeader:
    """Decode the header at the start of ``payload``.

    Raises :class:`CaptureHeaderError` if the buffer is too short, the
    magic word is wrong, or the version is unrecognized — never a silent
    wrong answer. ``beat_bytes``, when given, is validated against the
    payload length so a mis-specified bus width is caught here.
    """

    if beat_bytes is not None:
        span = capture_header_span(beat_bytes)
        if len(payload) < span:
            raise CaptureHeaderError(
                f"payload is {len(payload)} bytes, shorter than the {span}-byte header span"
            )
    if len(payload) < CAPTURE_HEADER_BYTES:
        raise CaptureHeaderError(
            f"payload is {len(payload)} bytes, shorter than the {CAPTURE_HEADER_BYTES}-byte header"
        )

    fields = _HEADER_STRUCT.unpack_from(payload, 0)
    header = CaptureHeader(*fields)
    if header.magic != CAPTURE_HEADER_MAGIC:
        raise CaptureHeaderError(
            f"bad capture header magic 0x{header.magic:08x}, expected 0x{CAPTURE_HEADER_MAGIC:08x}"
        )
    if header.version != CAPTURE_HEADER_VERSION:
        raise CaptureHeaderError(
            f"unsupported capture header version {header.version}, "
            f"expected {CAPTURE_HEADER_VERSION}"
        )
    return header


def strip_capture_header(payload: bytes, beat_bytes: int) -> tuple[CaptureHeader | None, bytes]:
    """Split ``payload`` into its header and its sample bytes.

    Returns ``(None, payload)`` unchanged when the buffer carries no
    header, so the same call site works whether tagging is enabled or
    not — which is what makes the default-off byte-identity guarantee
    testable from userspace.
    """

    if not has_capture_header(payload):
        return None, payload
    span = capture_header_span(beat_bytes)
    return parse_capture_header(payload, beat_bytes), payload[span:]


def header_samples_per_channel(beat_bytes: int, channels: int, sample_bytes: int = 2) -> int:
    """Samples per channel consumed by the header.

    The tagger overwrites the head of the buffer, so this many samples
    per channel are header rather than data and must be discarded. Note
    it depends on the *enabled channel count*: with one channel enabled
    the header eats far more samples per channel than with eight. Callers
    must never hardcode the result.
    """

    if channels <= 0:
        raise ValueError(f"channels must be positive, got {channels}")
    if sample_bytes <= 0:
        raise ValueError(f"sample_bytes must be positive, got {sample_bytes}")
    span = capture_header_span(beat_bytes)
    stride = channels * sample_bytes
    return -(-span // stride)  # ceil


# ---------------------------------------------------------------------------
# ADI data_offload register map
# ---------------------------------------------------------------------------
# Verified against drivers/misc/adi-axi-data-offload.c in the ADI Linux
# tree and library/data_offload/data_offload_regmap.v in ADI HDL. The
# driver's register names are used verbatim so drift is easy to spot.

#: Magic word register. Reading this proves the base address is really a
#: data_offload block before anything is written.
OFFLOAD_REG_MAGIC = 0x000C
OFFLOAD_MAGIC_VALUE = 0x44414F46  # "DAOF"
#: Transfer length, in 64-byte units, stored as length-1. Resets to the
#: full offload memory (1 MiB on ZCU102/AD9081).
OFFLOAD_REG_TRANSFER_LENGTH = 0x001C
OFFLOAD_REG_CONTROL = 0x0088
OFFLOAD_CONTROL_BYPASS = 1 << 0
OFFLOAD_CONTROL_ONESHOT = 1 << 1
#: Self-clearing software sync trigger (the ``SOFTWARE`` sync source).
OFFLOAD_REG_SYNC_TRIGGER = 0x0100
#: Sync source select. Resets to ``AUTOMATIC``.
OFFLOAD_REG_SYNC_CONFIG = 0x0104

SYNC_CONFIG_AUTOMATIC = 0
SYNC_CONFIG_HARDWARE = 1
SYNC_CONFIG_SOFTWARE = 2

#: Linux physical base addresses on ZCU102/AD9081. The HDL block design
#: places these at 0x7c44_0000/0x7c45_0000; ZynqMP maps that aperture up
#: by 0x2000_0000.
DATA_OFFLOAD_TX_BASE = 0x9C44_0000
DATA_OFFLOAD_RX_BASE = 0x9C45_0000

#: Path glob for the debugfs route, present only when the kernel is built
#: with ``CONFIG_ADI_AXI_DATA_OFFLOAD``. That symbol is a ``bool`` that no
#: defconfig sets and nothing ``select``s -- but ``Kconfig.adi`` carries
#: ``imply ADI_AXI_DATA_OFFLOAD`` (on both ``2023_R2`` and ``2026_R1``), which
#: is enough to make it default-y: building ``adi_zynqmp_defconfig`` on either
#: release yields ``CONFIG_ADI_AXI_DATA_OFFLOAD=y``. So on a kernel built from
#: ADI's own defconfig this route normally *does* exist. Still probe rather
#: than assume -- a vendor image may configure it off, and the register route
#: stays as the fallback.
OFFLOAD_DEBUGFS_ROOT = "/sys/kernel/debug/adi-axi-data-offload"

ReadReg = Callable[[int], int]
WriteReg = Callable[[int, int], None]


class OffloadAccessError(RuntimeError):
    """Raised when the data-offload block cannot be reached or verified."""


def _ssh_helper() -> str:
    override = os.environ.get("TICK_TARGET_SSH_HELPER")
    if override:
        return override
    from pathlib import Path

    return str(Path(__file__).resolve().parents[2] / "scripts" / "tick_target_ssh.py")


def _ssh_cmd(target: str, remote_cmd: str) -> list[str]:
    if os.environ.get("TICK_TARGET_PASSWORD"):
        return [_ssh_helper(), "ssh", target, remote_cmd]
    return ["ssh", "-o", "BatchMode=yes", target, remote_cmd]


#: The pexpect-based SSH helper echoes the interactive password prompt onto
#: stdout, so command output arrives with a line like "root@10.0.0.213's
#: password: " prepended. Anything parsing that output has to drop it - treating
#: it as data produced, in one memorable case, a debugfs path ending in
#: "/root@10.0.0.213's password:".
_SSH_PROMPT_RE = re.compile(r"^\S+@\S+'s password:\s*$")


def strip_ssh_noise(output: str) -> str:
    """Drop the SSH helper's password prompt from captured output."""

    return "\n".join(line for line in output.splitlines() if not _SSH_PROMPT_RE.match(line.strip()))


def run_remote(target: str, remote_cmd: str, *, timeout_s: float = 15.0) -> str:
    """Run a command on the target over SSH and return its stdout.

    Mirrors ``tick.ffh.run_remote`` so both modules honour
    ``TICK_TARGET_PASSWORD`` identically, and strips the helper's password
    prompt so callers can parse the result.
    """

    proc = subprocess.run(
        _ssh_cmd(target, remote_cmd),
        text=True,
        capture_output=True,
        timeout=timeout_s,
        check=True,
    )
    return strip_ssh_noise(proc.stdout)


# The MMIO route is a small program shipped over SSH rather than a call to
# devmem/devmem2, neither of which is guaranteed present on the image.
_MMIO_PROGRAM = r"""
import mmap, os, struct, sys
base, off, val = int(sys.argv[1], 0), int(sys.argv[2], 0), sys.argv[3]
page = os.sysconf("SC_PAGE_SIZE")
aligned = base & ~(page - 1)
delta = base - aligned
length = ((delta + off + 4 + page - 1) // page) * page
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
try:
    mm = mmap.mmap(fd, length, offset=aligned)
    try:
        pos = delta + off
        if val != "-":
            mm[pos:pos + 4] = struct.pack("<I", int(val, 0) & 0xFFFFFFFF)
        print("0x%08x" % struct.unpack("<I", mm[pos:pos + 4])[0])
    finally:
        mm.close()
finally:
    os.close(fd)
"""


def offload_mmio_io(target: str, base: int, *, timeout_s: float = 15.0) -> tuple[ReadReg, WriteReg]:
    """Register access to a data_offload block through ``/dev/mem``.

    Used when the kernel has no ``adi-axi-data-offload`` driver. Callers
    should gate every write behind :func:`require_offload_magic` so a
    wrong base address fails loudly rather than corrupting an unrelated
    register.
    """

    program = _MMIO_PROGRAM.replace("'", "'\\''")

    def poke(offset: int, value: str) -> int:
        out = run_remote(
            target,
            f"python3 -c '{program}' 0x{base:x} 0x{offset:x} {value}",
            timeout_s=timeout_s,
        )
        return int(out.strip().splitlines()[-1], 0)

    def read_reg(offset: int) -> int:
        return poke(offset, "-")

    def write_reg(offset: int, value: int) -> None:
        poke(offset, f"0x{value:x}")

    return read_reg, write_reg


def offload_debugfs_node(target: str, *, want: str = "rx", timeout_s: float = 15.0) -> str | None:
    """Locate a data_offload debugfs directory on the target, if any.

    Returns ``None`` when the driver is not built into the running kernel.
    That is not the common case on an ADI-built kernel: ``Kconfig.adi``
    ``imply``s ``CONFIG_ADI_AXI_DATA_OFFLOAD``, so ``adi_zynqmp_defconfig``
    builds it in. Treat ``None`` as "this particular image has it off", not as
    the expected outcome.
    """

    try:
        # `test -d` first: without it a missing root still yields output (the
        # helper's prompt), and any non-empty result looks like a node name.
        out = run_remote(
            target,
            f"test -d {OFFLOAD_DEBUGFS_ROOT} && ls -1 {OFFLOAD_DEBUGFS_ROOT} 2>/dev/null || true",
            timeout_s=timeout_s,
        )
    except subprocess.CalledProcessError:
        return None
    names = [line.strip() for line in out.splitlines() if line.strip()]
    if not names:
        return None
    for name in names:
        if want in name:
            return f"{OFFLOAD_DEBUGFS_ROOT}/{name}"
    return f"{OFFLOAD_DEBUGFS_ROOT}/{names[0]}"


def offload_access_mode(target: str, *, want: str = "rx") -> str:
    """Return ``"debugfs"`` or ``"mmio"`` for reaching the offload block."""

    return "debugfs" if offload_debugfs_node(target, want=want) else "mmio"


def require_offload_magic(read_reg: ReadReg, *, base: int | None = None) -> None:
    """Prove the register window really is a data_offload block.

    This is mandatory before any write on the MMIO route. A mismatch means
    the base address is wrong, and continuing would write into an
    unrelated peripheral.
    """

    magic = read_reg(OFFLOAD_REG_MAGIC)
    if magic != OFFLOAD_MAGIC_VALUE:
        where = f" at base 0x{base:08x}" if base is not None else ""
        raise OffloadAccessError(
            f"data_offload magic mismatch{where}: read 0x{magic:08x}, "
            f"expected 0x{OFFLOAD_MAGIC_VALUE:08x} — refusing to write to an "
            "unverified register window"
        )


def read_sync_config(read_reg: ReadReg) -> int:
    return read_reg(OFFLOAD_REG_SYNC_CONFIG) & 0x3


def require_hardware_sync(
    read_reg: ReadReg,
    write_reg: WriteReg,
    *,
    base: int | None = None,
) -> int:
    """Put the offload block into ``HARDWARE`` sync mode, idempotently.

    Returns the previous ``sync_config`` value. Proves the magic word
    first, so this never writes to an unverified window. Raises if the
    write does not take effect rather than reporting success.
    """

    require_offload_magic(read_reg, base=base)
    previous = read_sync_config(read_reg)
    if previous != SYNC_CONFIG_HARDWARE:
        write_reg(OFFLOAD_REG_SYNC_CONFIG, SYNC_CONFIG_HARDWARE)
        readback = read_sync_config(read_reg)
        if readback != SYNC_CONFIG_HARDWARE:
            raise OffloadAccessError(
                f"sync_config did not take: wrote {SYNC_CONFIG_HARDWARE} "
                f"(HARDWARE), read back {readback}"
            )
    return previous


def resolve_offload_io(
    target: str,
    *,
    base: int = DATA_OFFLOAD_RX_BASE,
    want: str = "rx",
) -> tuple[ReadReg, WriteReg, str]:
    """Pick an access route to the offload block and return it.

    Tries debugfs, then MMIO. There is deliberately no silent third
    fallback: if neither route can be established the caller gets an
    :class:`OffloadAccessError` naming both routes and the kernel config
    symbol that would enable the better one.
    """

    node = offload_debugfs_node(target, want=want)
    if node:
        # Prove the route works before returning it. The ADI offload driver can
        # be present yet bound to nothing, and a debugfs directory can exist
        # without the attributes we need - in both cases MMIO is still viable, so
        # fall through rather than failing.
        debugfs_read, debugfs_write = offload_debugfs_io(target, node)
        try:
            require_offload_magic(debugfs_read)
            return debugfs_read, debugfs_write, "debugfs"
        except (OffloadAccessError, subprocess.CalledProcessError, ValueError):
            pass
    read_reg, write_reg = offload_mmio_io(target, base)
    try:
        require_offload_magic(read_reg, base=base)
    except subprocess.CalledProcessError as exc:
        raise OffloadAccessError(
            f"cannot reach the data_offload block on {target}: debugfs "
            f"({OFFLOAD_DEBUGFS_ROOT}) is absent, which means the kernel lacks "
            "CONFIG_ADI_AXI_DATA_OFFLOAD, and the /dev/mem route failed too"
        ) from exc
    return read_reg, write_reg, "mmio"


def offload_debugfs_io(
    target: str, node: str, *, timeout_s: float = 15.0
) -> tuple[ReadReg, WriteReg]:
    """Register access through the ADI driver's debugfs attributes.

    The driver exposes named attributes rather than a raw register
    window, so only the offsets it publishes are reachable. That covers
    everything this module needs.
    """

    names = {
        OFFLOAD_REG_MAGIC: "magic",
        OFFLOAD_REG_SYNC_CONFIG: "sync_config",
        OFFLOAD_REG_TRANSFER_LENGTH: "transfer_length",
    }

    def attr_for(offset: int) -> str:
        try:
            return names[offset]
        except KeyError:
            raise OffloadAccessError(
                f"data_offload offset 0x{offset:03x} has no debugfs attribute; "
                "use the MMIO route for raw register access"
            ) from None

    def read_reg(offset: int) -> int:
        out = run_remote(target, f"cat {node}/{attr_for(offset)}", timeout_s=timeout_s)
        return int(out.strip().splitlines()[-1], 0)

    def write_reg(offset: int, value: int) -> None:
        run_remote(target, f"echo {value} > {node}/{attr_for(offset)}", timeout_s=timeout_s)

    return read_reg, write_reg


# ---------------------------------------------------------------------------
# Descriptor construction
# ---------------------------------------------------------------------------

ReadAttr = Callable[[str], int]
WriteAttr = Callable[[str, str], None]


def descriptor_csv(descriptor: CommandDescriptor) -> str:
    """Encode a descriptor as the twelve little-endian hex words the
    driver's ``command_descriptor`` attribute accepts."""

    payload = descriptor.pack()
    words = [
        f"0x{int.from_bytes(payload[i : i + 4], 'little'):08x}" for i in range(0, len(payload), 4)
    ]
    return ",".join(words)


def build_arm_descriptor(
    *,
    timestamp_ticks: int,
    sequence_id: int,
    capture: bool = True,
    gate_index: int | None = None,
    width_ticks: int = DEFAULT_ARM_PULSE_TICKS,
    flags: Flag = Flag.REPORT_COMPLETION,
) -> CommandDescriptor:
    """Build a ``CAPTURE_ARM`` or ``TRANSMIT_ARM`` descriptor.

    ``width_ticks=0`` asks the device for its own default width. The HDL
    floors any value to a CDC-safe minimum, so a caller cannot request a
    pulse narrow enough to be dropped by the ``sync_ext`` synchronizer.
    """

    if capture:
        command_id = CommandID.CAPTURE_ARM
        target_id = int(TargetFamily.CAPTURE)
        default_gate = CAPTURE_GATE_INDEX
    else:
        command_id = CommandID.TRANSMIT_ARM
        target_id = int(TargetFamily.TRANSMIT)
        default_gate = TRANSMIT_GATE_INDEX
    gate = default_gate if gate_index is None else gate_index

    return CommandDescriptor(
        command_id=command_id,
        target_id=target_id,
        timestamp_ticks=timestamp_ticks,
        flags=flags,
        sequence_id=sequence_id,
        args=(gate, width_ticks, 0, 0),
    )


def arm_capture(
    read_attr: ReadAttr,
    write_attr: WriteAttr,
    *,
    timestamp_ticks: int,
    sequence_id: int,
    gate_index: int = CAPTURE_GATE_INDEX,
    width_ticks: int = DEFAULT_ARM_PULSE_TICKS,
    flags: Flag = Flag.REPORT_COMPLETION,
) -> CommandDescriptor:
    """Schedule the release of an already-armed RX buffer.

    The buffer must already exist (so the offload is parked in its sync
    state) and the offload must be in ``HARDWARE`` sync mode. Returns the
    submitted descriptor so the caller can match its completion.
    """

    descriptor = build_arm_descriptor(
        timestamp_ticks=timestamp_ticks,
        sequence_id=sequence_id,
        capture=True,
        gate_index=gate_index,
        width_ticks=width_ticks,
        flags=flags,
    )
    write_attr("command_descriptor", descriptor_csv(descriptor))
    return descriptor


def arm_transmit(
    read_attr: ReadAttr,
    write_attr: WriteAttr,
    *,
    timestamp_ticks: int,
    sequence_id: int,
    gate_index: int = TRANSMIT_GATE_INDEX,
    width_ticks: int = DEFAULT_ARM_PULSE_TICKS,
    flags: Flag = Flag.REPORT_COMPLETION,
) -> CommandDescriptor:
    """Schedule the release of an already-armed TX buffer."""

    descriptor = build_arm_descriptor(
        timestamp_ticks=timestamp_ticks,
        sequence_id=sequence_id,
        capture=False,
        gate_index=gate_index,
        width_ticks=width_ticks,
        flags=flags,
    )
    write_attr("command_descriptor", descriptor_csv(descriptor))
    return descriptor


#: RX capture device names seen across ADI AD9081 images, most specific first.
#: Older Kuiper releases expose `axi-ad9081-rx-hpc`; newer ones name the TPL core
#: directly. Hardcoding either one breaks on the other.
RX_DEVICE_CANDIDATES = (
    "axi-ad9081-rx-hpc",
    "ad_ip_jesd204_tpl_adc",
)


def find_rx_device(context, candidates=RX_DEVICE_CANDIDATES):
    """Return the RX capture device from a libiio context.

    Raises with the context's actual device list when none matches, because the
    alternative - a bare "device not found" - sends you looking for a deploy
    problem when the real answer is that this image names it differently.
    """

    for name in candidates:
        device = context.find_device(name)
        if device is not None:
            return device
    available = sorted(dev.name for dev in context.devices if dev.name)
    raise RuntimeError(
        "no RX capture device found. Tried "
        f"{', '.join(candidates)}; context has: {', '.join(available)}"
    )


def schedule_ticks(read_attr: ReadAttr, lead_s: float) -> int:
    """Read the device clock and return a tick ``lead_s`` in the future."""

    now = read_attr("current_time_ticks")
    tick_hz = read_attr("time_tick_hz")
    return now + int(lead_s * tick_hz)

"""Descriptor and completion data structures."""

from __future__ import annotations

import struct
from collections.abc import Iterable
from dataclasses import dataclass, field
from typing import cast

from .enums import CommandID, Flag, Status, TargetFamily

DESCRIPTOR_VERSION = 1
ARG_WORDS = 4
DESCRIPTOR_SIZE = 48
_DESCRIPTOR_STRUCT = struct.Struct("<HHHHQII4I8x")
_SUPPORTED_FLAG_BITS = int(
    Flag.REPORT_COMPLETION | Flag.LATE_DROP | Flag.LATE_EXECUTE_NOW | Flag.REPEAT
)


class DescriptorError(ValueError):
    """Raised when a command descriptor does not match the wire contract."""

    def __init__(self, message: str, status: Status = Status.BAD_DESCRIPTOR):
        super().__init__(message)
        self.status = status


def _coerce_args(args: Iterable[int]) -> tuple[int, int, int, int]:
    values = tuple(args)
    if len(values) > ARG_WORDS:
        raise DescriptorError("command descriptors accept at most four argument words")
    padded = values + (0,) * (ARG_WORDS - len(values))
    for value in padded:
        if not 0 <= int(value) <= 0xFFFF_FFFF:
            raise DescriptorError("argument words must fit in uint32")
    return (int(padded[0]), int(padded[1]), int(padded[2]), int(padded[3]))


def _coerce_command_id(command_id: CommandID | int) -> CommandID | int:
    value = int(command_id)
    if not 0 <= value <= 0xFFFF:
        raise DescriptorError("command_id must fit in uint16")
    try:
        return CommandID(value)
    except ValueError:
        return value


def _coerce_target_id(target_id: TargetFamily | int) -> TargetFamily | int:
    value = int(target_id)
    if not 0 <= value <= 0xFFFF:
        raise DescriptorError("target_id must fit in uint16")
    if isinstance(target_id, TargetFamily):
        return target_id
    return value


@dataclass(frozen=True)
class CommandDescriptor:
    """Packed command descriptor.

    The binary layout follows ``docs/command-interface.md``: little-endian,
    versioned, and exactly 48 bytes with four 32-bit argument words.
    """

    command_id: CommandID | int
    target_id: TargetFamily | int
    timestamp_ticks: int
    flags: Flag | int = Flag.NONE
    sequence_id: int = 0
    args: tuple[int, int, int, int] | Iterable[int] = field(default_factory=tuple)
    version: int = DESCRIPTOR_VERSION
    size_bytes: int = DESCRIPTOR_SIZE

    def __post_init__(self) -> None:
        object.__setattr__(self, "args", _coerce_args(self.args))
        object.__setattr__(self, "command_id", _coerce_command_id(self.command_id))
        object.__setattr__(self, "target_id", _coerce_target_id(self.target_id))
        object.__setattr__(self, "flags", Flag(int(self.flags)))
        self.validate()

    def validate(self) -> None:
        """Validate descriptor fields that are independent of scheduler state."""

        if self.version != DESCRIPTOR_VERSION:
            raise DescriptorError(f"unsupported descriptor version {self.version}")
        if self.size_bytes != DESCRIPTOR_SIZE:
            raise DescriptorError(f"descriptor size must be {DESCRIPTOR_SIZE} bytes")
        if not 0 <= self.timestamp_ticks <= 0xFFFF_FFFF_FFFF_FFFF:
            raise DescriptorError("timestamp_ticks must fit in uint64")
        if not 0 <= self.sequence_id <= 0xFFFF_FFFF:
            raise DescriptorError("sequence_id must fit in uint32")
        if self.flags & Flag.LATE_DROP and self.flags & Flag.LATE_EXECUTE_NOW:
            raise DescriptorError("late drop and late execute-now policies are mutually exclusive")
        if int(self.flags) & ~_SUPPORTED_FLAG_BITS:
            raise DescriptorError("descriptor flags contain unsupported bits")
        args = cast(tuple[int, int, int, int], self.args)
        if self.flags & Flag.REPEAT and args[3] == 0:
            raise DescriptorError("repeating descriptors require a nonzero period in args[3]")
        if int(self.command_id) == int(CommandID.SPI_TRANSFER):
            control = args[1]
            chip_select = control & 0xFF
            bit_count = (control >> 8) & 0xFF
            half_period_ticks = (control >> 16) & 0xFFFF
            if chip_select >= 8 or not 1 <= bit_count <= 32 or half_period_ticks == 0:
                raise DescriptorError(
                    "SPI control word has an invalid chip select, bit count, or half-period"
                )

    @property
    def repeat_count(self) -> int | None:
        """Return the total occurrence count, or ``None`` for an infinite repeat."""

        if not self.flags & Flag.REPEAT:
            return None
        args = cast(tuple[int, int, int, int], self.args)
        return args[2] or None

    @property
    def repeat_period_ticks(self) -> int | None:
        """Return the repeat period, or ``None`` for a one-shot descriptor."""

        if not self.flags & Flag.REPEAT:
            return None
        args = cast(tuple[int, int, int, int], self.args)
        return args[3]

    def pack(self) -> bytes:
        """Return the documented 48-byte little-endian representation."""

        self.validate()
        return _DESCRIPTOR_STRUCT.pack(
            self.version,
            self.size_bytes,
            int(self.command_id),
            int(self.target_id),
            self.timestamp_ticks,
            int(self.flags),
            self.sequence_id,
            *self.args,
        )

    @classmethod
    def unpack(cls, payload: bytes) -> CommandDescriptor:
        """Parse a descriptor from its 48-byte little-endian representation."""

        if len(payload) != DESCRIPTOR_SIZE:
            raise DescriptorError(f"descriptor payload must be {DESCRIPTOR_SIZE} bytes")
        version, size_bytes, command_id, target_id, timestamp, flags, sequence_id, *args = (
            _DESCRIPTOR_STRUCT.unpack(payload)
        )
        return cls(
            version=version,
            size_bytes=size_bytes,
            command_id=command_id,
            target_id=target_id,
            timestamp_ticks=timestamp,
            flags=flags,
            sequence_id=sequence_id,
            args=args,
        )


@dataclass(frozen=True)
class Completion:
    """Completion record emitted when a command reaches a terminal status."""

    sequence_id: int
    command_id: CommandID | int
    target_id: TargetFamily | int
    requested_timestamp_ticks: int
    executed_timestamp_ticks: int | None
    status: Status
    detail: int = 0

    @classmethod
    def from_descriptor(
        cls,
        descriptor: CommandDescriptor,
        status: Status,
        executed_timestamp_ticks: int | None = None,
        detail: int = 0,
    ) -> Completion:
        return cls(
            sequence_id=descriptor.sequence_id,
            command_id=descriptor.command_id,
            target_id=descriptor.target_id,
            requested_timestamp_ticks=descriptor.timestamp_ticks,
            executed_timestamp_ticks=executed_timestamp_ticks,
            status=status,
            detail=detail,
        )


@dataclass(frozen=True)
class Event:
    """Executed scheduler event, preserving the submitted descriptor."""

    sequence_id: int
    command_id: CommandID | int
    target_id: TargetFamily | int
    requested_timestamp_ticks: int
    executed_timestamp_ticks: int
    args: tuple[int, int, int, int]

    @classmethod
    def from_descriptor(cls, descriptor: CommandDescriptor, executed_timestamp_ticks: int) -> Event:
        # CommandDescriptor.__post_init__ always normalises args to a 4-tuple via
        # _coerce_args, even though the field type also allows Iterable[int] at
        # the constructor boundary. Cast lets the type checker see the runtime
        # invariant without weakening Event.args.
        args = cast(tuple[int, int, int, int], descriptor.args)
        return cls(
            sequence_id=descriptor.sequence_id,
            command_id=descriptor.command_id,
            target_id=descriptor.target_id,
            requested_timestamp_ticks=descriptor.timestamp_ticks,
            executed_timestamp_ticks=executed_timestamp_ticks,
            args=args,
        )

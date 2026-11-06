"""Deterministic in-memory timed command scheduler."""

from __future__ import annotations

from collections import deque
from collections.abc import Iterable
from dataclasses import dataclass
from typing import cast

from .descriptor import CommandDescriptor, Completion, DescriptorError, Event
from .enums import CommandID, Flag, Status, TargetFamily

_UINT32_MAX = 0xFFFF_FFFF
_UINT64_MAX = 0xFFFF_FFFF_FFFF_FFFF


@dataclass(frozen=True)
class _QueuedCommand:
    descriptor: CommandDescriptor
    occurrence_index: int = 0

    @property
    def timestamp_ticks(self) -> int:
        return self.descriptor.timestamp_ticks

    def __getattr__(self, name: str):
        """Preserve private queue compatibility for the register-level model."""

        return getattr(self.descriptor, name)


class Scheduler:
    """In-memory scheduler with stable ordering and retained-slot repeats.

    A repeating descriptor uses ``args[2]`` as its total occurrence count
    (zero means infinite) and ``args[3]`` as its nonzero period in ticks.
    """

    def __init__(
        self,
        tick_hz: int,
        queue_depth: int = 16,
        supported_commands: Iterable[CommandID] | None = None,
        supported_targets: Iterable[TargetFamily] | None = None,
    ) -> None:
        if tick_hz <= 0:
            raise ValueError("tick_hz must be positive")
        if queue_depth <= 0:
            raise ValueError("queue_depth must be positive")
        self.tick_hz = tick_hz
        self.queue_depth = queue_depth
        self.current_time_ticks = 0
        self.late_count = 0
        self.overflow_count = 0
        self.events: list[Event] = []
        self.completions: list[Completion] = []
        self._queue: deque[_QueuedCommand] = deque()
        self._supported_commands = set(
            CommandID if supported_commands is None else supported_commands
        )
        self._supported_targets = set(
            TargetFamily if supported_targets is None else supported_targets
        )

    @property
    def queue_level(self) -> int:
        """Number of pending descriptors, including active repeat series."""

        return len(self._queue)

    def set_time(self, timestamp_ticks: int) -> None:
        if timestamp_ticks < self.current_time_ticks:
            raise ValueError("scheduler time must be monotonic")
        self.current_time_ticks = timestamp_ticks

    def flush_queue(self) -> None:
        """Discard pending one-shots and all repeat continuations."""

        self._queue.clear()

    def submit(self, descriptor: CommandDescriptor) -> Completion | None:
        try:
            descriptor.validate()
        except DescriptorError:
            completion = Completion.from_descriptor(descriptor, Status.BAD_DESCRIPTOR)
            self.completions.append(completion)
            return completion

        if descriptor.command_id not in self._supported_commands or not self._target_supported(
            descriptor.command_id, descriptor.target_id
        ):
            completion = Completion.from_descriptor(descriptor, Status.UNSUPPORTED)
            self.completions.append(completion)
            return completion

        args = cast(tuple[int, int, int, int], descriptor.args)
        if (
            len(self._queue) >= self.queue_depth
            and descriptor.flags & Flag.REPEAT
            and (args[2] == 0 or args[2] > 1)
        ):
            self._increment_overflow()
            completion = Completion.from_descriptor(descriptor, Status.OVERFLOW)
            self.completions.append(completion)
            return completion

        entry = _QueuedCommand(descriptor)
        if descriptor.timestamp_ticks < self.current_time_ticks:
            return self._handle_occurrence(entry, late=True)
        if len(self._queue) >= self.queue_depth:
            self._increment_overflow()
            completion = Completion.from_descriptor(descriptor, Status.OVERFLOW)
            self.completions.append(completion)
            return completion
        self._enqueue(entry)
        return None

    def advance_to(self, timestamp_ticks: int) -> list[Completion]:
        """Advance time and handle due commands.

        A late repeat handles one occurrence and rebases to now plus its period,
        preventing unbounded catch-up after a time jump or disabled interval.
        """

        if timestamp_ticks < self.current_time_ticks:
            raise ValueError("scheduler time must be monotonic")
        self.current_time_ticks = timestamp_ticks
        completions: list[Completion] = []
        while self._queue and self._queue[0].timestamp_ticks <= timestamp_ticks:
            entry = self._queue.popleft()
            completion_start = len(self.completions)
            self._handle_occurrence(
                entry,
                late=entry.timestamp_ticks < timestamp_ticks
                and bool(entry.descriptor.flags & Flag.REPEAT),
            )
            completions.extend(self.completions[completion_start:])
        return completions

    def _handle_occurrence(self, entry: _QueuedCommand, *, late: bool) -> Completion | None:
        descriptor = entry.descriptor
        status = Status.EXECUTED
        executed_at: int | None = descriptor.timestamp_ticks
        if late:
            self._increment_late()
            if descriptor.flags & Flag.LATE_EXECUTE_NOW:
                status = Status.LATE_EXECUTED
                executed_at = self.current_time_ticks
            else:
                status = Status.LATE_DROPPED
                executed_at = None

        continues = self._repeat_continues(entry)
        next_timestamp: int | None = None
        if continues:
            base = self.current_time_ticks if late else descriptor.timestamp_ticks
            args = cast(tuple[int, int, int, int], descriptor.args)
            period = args[3]
            if base > _UINT64_MAX - period:
                self._increment_overflow()
                status = Status.REPEAT_OVERFLOW
                continues = False
            else:
                next_timestamp = base + period

        reported = bool(
            descriptor.flags & Flag.REPORT_COMPLETION
            or status not in {Status.EXECUTED, Status.LATE_EXECUTED}
        )
        if executed_at is not None:
            self.events.append(Event.from_descriptor(descriptor, executed_at))
            completion = Completion.from_descriptor(
                descriptor, status, executed_at, detail=entry.occurrence_index
            )
        else:
            completion = Completion.from_descriptor(
                descriptor, status, detail=entry.occurrence_index
            )
        if reported:
            self.completions.append(completion)

        if continues and next_timestamp is not None:
            next_descriptor = CommandDescriptor(
                command_id=descriptor.command_id,
                target_id=descriptor.target_id,
                timestamp_ticks=next_timestamp,
                flags=descriptor.flags,
                sequence_id=descriptor.sequence_id,
                args=descriptor.args,
                version=descriptor.version,
                size_bytes=descriptor.size_bytes,
            )
            self._enqueue(_QueuedCommand(next_descriptor, entry.occurrence_index + 1))

        return completion if reported else None

    def _repeat_continues(self, entry: _QueuedCommand) -> bool:
        descriptor = entry.descriptor
        if not descriptor.flags & Flag.REPEAT:
            return False
        args = cast(tuple[int, int, int, int], descriptor.args)
        count = args[2]
        return count == 0 or entry.occurrence_index + 1 < count

    def _execute(
        self,
        descriptor: CommandDescriptor | _QueuedCommand,
        status: Status,
        executed_timestamp_ticks: int,
    ) -> Completion:
        """Execute one entry; retained for the register-level model adapter."""

        source = descriptor.descriptor if isinstance(descriptor, _QueuedCommand) else descriptor
        detail = descriptor.occurrence_index if isinstance(descriptor, _QueuedCommand) else 0
        self.events.append(Event.from_descriptor(source, executed_timestamp_ticks))
        completion = Completion.from_descriptor(
            source, status, executed_timestamp_ticks, detail=detail
        )
        if source.flags & Flag.REPORT_COMPLETION or status not in {
            Status.EXECUTED,
            Status.LATE_EXECUTED,
        }:
            self.completions.append(completion)
        return completion

    def _enqueue(self, entry: _QueuedCommand) -> None:
        self._queue.append(entry)
        self._queue = deque(sorted(self._queue, key=lambda queued: queued.timestamp_ticks))

    def _increment_late(self) -> None:
        self.late_count = min(self.late_count + 1, _UINT32_MAX)

    def _increment_overflow(self) -> None:
        self.overflow_count = min(self.overflow_count + 1, _UINT32_MAX)

    def _target_supported(self, command_id: CommandID | int, target_id: TargetFamily | int) -> bool:
        """Return whether the descriptor uses a supported command/family pair."""

        try:
            family = TargetFamily.family_for(int(target_id))
        except ValueError:
            return False
        if family not in self._supported_targets:
            return False
        command_families = {
            CommandID.GPIO_WRITE: TargetFamily.GPIO,
            CommandID.MARKER_PULSE: TargetFamily.MARKER,
            CommandID.STREAM_GATE: TargetFamily.STREAM_GATE,
            CommandID.TDD_SYNC: TargetFamily.AXI_TDD,
            CommandID.DDS_TX_GATE: TargetFamily.DDS_TX,
            CommandID.CAPTURE_ARM: TargetFamily.CAPTURE,
            CommandID.TRANSMIT_ARM: TargetFamily.TRANSMIT,
            CommandID.SPI_TRANSFER: TargetFamily.SPI,
            CommandID.AD9081_PROFILE_TRIGGER: TargetFamily.AD9081_CONTROL,
        }
        try:
            return command_families.get(CommandID(int(command_id))) is family
        except ValueError:
            return False

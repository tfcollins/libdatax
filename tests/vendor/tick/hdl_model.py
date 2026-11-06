"""Register-level model of the initial HDL scheduler IP."""

from __future__ import annotations

from dataclasses import dataclass
from typing import cast

from .descriptor import CommandDescriptor, Completion, Event
from .enums import Capability, CommandID, Flag, Status, TargetFamily
from .scheduler import Scheduler


@dataclass(frozen=True)
class HdlCompletion:
    """Small completion view used by HDL-facing tests."""

    sequence_id: int
    command_id: CommandID | int
    target_id: TargetFamily | int
    status_name: str
    requested_timestamp_ticks: int
    executed_timestamp_ticks: int | None
    detail: int

    @classmethod
    def from_completion(cls, completion: Completion) -> HdlCompletion:
        return cls(
            sequence_id=completion.sequence_id,
            command_id=completion.command_id,
            target_id=completion.target_id,
            status_name=completion.status.name,
            requested_timestamp_ticks=completion.requested_timestamp_ticks,
            executed_timestamp_ticks=completion.executed_timestamp_ticks,
            detail=completion.detail,
        )


@dataclass(frozen=True)
class MarkerPulse:
    """Marker pulse event applied to ``marker_out[marker_id]``."""

    marker_id: int
    width_ticks: int
    executed_at_tick: int


@dataclass(frozen=True)
class TddSyncEvent:
    """TDD sync (and optional start) pulse applied to an AXI TDD instance."""

    instance: int
    start: bool
    executed_at_tick: int


@dataclass(frozen=True)
class SpiTransfer:
    """A mode-0 SPI write launched by the timed scheduler."""

    chip_select: int
    tx_word: int
    bit_count: int
    half_period_ticks: int
    executed_at_tick: int


#: Pulse width the HDL substitutes when ``args[1]`` is 0, and the floor it
#: applies to any requested width. Mirrors ARM_PULSE_DEFAULT_TICKS /
#: ARM_PULSE_MIN_TICKS in the RTL.
ARM_PULSE_DEFAULT_TICKS = 16
ARM_PULSE_MIN_TICKS = 4


@dataclass(frozen=True)
class ArmPulse:
    """One-shot arm-release pulse applied to ``stream_gate_en[gate_id]``.

    ``width_ticks`` is the width the HDL actually used, after substituting the
    device default for 0 and applying the CDC floor - not the raw ``args[1]``.
    """

    gate_id: int
    width_ticks: int
    executed_at_tick: int
    direction: str  # "capture" | "transmit"


@dataclass(frozen=True)
class CaptureTag:
    """Latched capture tag, as exposed by the edge-capture registers."""

    ticks: int
    sequence_id: int
    target_id: int
    command_id: int
    gate_id: int
    late: bool


class AxiTimedCommandSchedulerModel:
    """Python model of the AXI-Lite HDL block.

    Mirrors the RTL dispatch for the implemented FPGA-local command families:
    ``GPIO_WRITE``, ``MARKER_PULSE``, ``STREAM_GATE``, ``TDD_SYNC``,
    ``CAPTURE_ARM``, and ``TRANSMIT_ARM``. Reuses the deterministic
    submission-order :class:`Scheduler` for queueing and completion-window
    semantics, dispatching to per-command side effects on execution.

    ``CAPTURE_ARM``/``TRANSMIT_ARM`` drive the same ``stream_gate_en`` bus as
    ``STREAM_GATE``, but as one-shot pulses rather than latched levels. This
    model has no per-tick notion of a pulse retiring, so - exactly as it already
    does for ``MARKER_PULSE`` - it records each pulse in :attr:`arm_pulses` and
    sets the corresponding ``stream_gate_en`` bit. A pulse never clears a bit,
    which is what keeps it from clobbering a level ``STREAM_GATE`` established.
    """

    def __init__(self, queue_depth: int = 16, tick_hz: int = 250_000_000) -> None:
        self._scheduler = Scheduler(
            tick_hz=tick_hz,
            queue_depth=queue_depth,
            supported_commands={
                CommandID.GPIO_WRITE,
                CommandID.MARKER_PULSE,
                CommandID.STREAM_GATE,
                CommandID.TDD_SYNC,
                CommandID.CAPTURE_ARM,
                CommandID.TRANSMIT_ARM,
                CommandID.SPI_TRANSFER,
            },
            supported_targets={
                TargetFamily.GPIO,
                TargetFamily.MARKER,
                TargetFamily.STREAM_GATE,
                TargetFamily.AXI_TDD,
                TargetFamily.CAPTURE,
                TargetFamily.TRANSMIT,
                TargetFamily.SPI,
            },
        )
        self.gpio_out = 0
        self.marker_out = 0
        self.marker_pulses: list[MarkerPulse] = []
        self.stream_gate_en = 0
        self.arm_pulses: list[ArmPulse] = []
        self.tdd_sync_events: list[TddSyncEvent] = []
        self.spi_transfers: list[SpiTransfer] = []
        self.capture_tag: CaptureTag | None = None
        self.capture_tag_count = 0
        self.capture_tag_unread = False
        self.capture_tag_overrun = False
        self.capture_tag_enable = 0
        self.current_time_ticks = 0
        self.time_epoch_ticks = 0
        self.pps_present = False
        self.time_aligned = False
        self.pps_arm_next = False
        self.enabled = True
        self.completion: HdlCompletion | None = None
        self.completion_pending = False
        self.command_irq = False
        self._deferred_late_execute_descriptor_ids: set[int] = set()

    @property
    def late_count(self) -> int:
        return self._scheduler.late_count

    @property
    def overflow_count(self) -> int:
        return self._scheduler.overflow_count

    @property
    def queue_level(self) -> int:
        return self._scheduler.queue_level

    @property
    def capability_bitmap(self) -> Capability:
        return (
            Capability.GPIO_TARGET
            | Capability.PPS_EPOCH
            | Capability.MARKER_TARGET
            | Capability.STREAM_GATE_TARGET
            | Capability.AXI_TDD_TARGET
            | Capability.ARM_RELEASE_TARGET
            | Capability.REPEAT
            | Capability.SPI_TARGET
        )

    @property
    def control_status(self) -> int:
        return int(self.enabled) | (int(self.pps_arm_next) << 3)

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled

    def flush_queue(self) -> None:
        self._scheduler.flush_queue()
        self._deferred_late_execute_descriptor_ids.clear()

    def reset_counters(self) -> None:
        self._scheduler.late_count = 0
        self._scheduler.overflow_count = 0

    def write_time(self, timestamp_ticks: int) -> None:
        self._load_time(timestamp_ticks)

    def write_time_epoch(self, timestamp_ticks: int) -> None:
        self.time_epoch_ticks = timestamp_ticks

    def arm_time_next_pps(self) -> None:
        self.pps_arm_next = True
        self.time_aligned = False

    def pps_edge(self) -> None:
        self.pps_present = True
        if self.pps_arm_next:
            self._load_time(self.time_epoch_ticks)
            self.pps_arm_next = False
            self.time_aligned = True

    def ack_completion(self) -> None:
        self.completion_pending = False
        self.command_irq = False

    def push_gpio(
        self,
        sequence_id: int,
        timestamp: int,
        mask: int,
        value: int,
        target_id: TargetFamily | int = TargetFamily.GPIO,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        return self._push(
            command_id=CommandID.GPIO_WRITE,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(mask, value, repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def push_marker(
        self,
        sequence_id: int,
        timestamp: int,
        marker_id: int,
        width_ticks: int = 1,
        target_id: TargetFamily | int = TargetFamily.MARKER,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        return self._push(
            command_id=CommandID.MARKER_PULSE,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(marker_id, max(width_ticks, 1), repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def push_stream_gate(
        self,
        sequence_id: int,
        timestamp: int,
        gate_id: int,
        enable: bool,
        target_id: TargetFamily | int = TargetFamily.STREAM_GATE,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        return self._push(
            command_id=CommandID.STREAM_GATE,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(gate_id, 1 if enable else 0, repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def push_tdd_sync(
        self,
        sequence_id: int,
        timestamp: int,
        instance: int,
        start: bool = False,
        target_id: TargetFamily | int = TargetFamily.AXI_TDD,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        return self._push(
            command_id=CommandID.TDD_SYNC,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(instance, 1 if start else 0, repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def push_capture_arm(
        self,
        sequence_id: int,
        timestamp: int,
        gate_id: int = 0,
        width_ticks: int = 0,
        target_id: TargetFamily | int = TargetFamily.CAPTURE,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        """Release an armed RX buffer at ``timestamp``.

        ``width_ticks=0`` asks the device for its default pulse width.
        """

        return self._push(
            command_id=CommandID.CAPTURE_ARM,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(gate_id, width_ticks, repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def push_transmit_arm(
        self,
        sequence_id: int,
        timestamp: int,
        gate_id: int = 1,
        width_ticks: int = 0,
        target_id: TargetFamily | int = TargetFamily.TRANSMIT,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        """Release an armed TX buffer at ``timestamp``."""

        return self._push(
            command_id=CommandID.TRANSMIT_ARM,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(gate_id, width_ticks, repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def push_spi(
        self,
        sequence_id: int,
        timestamp: int,
        chip_select: int,
        tx_word: int,
        bit_count: int = 32,
        half_period_ticks: int = 1,
        target_id: TargetFamily | int = TargetFamily.SPI,
        late_policy: str = "execute_now",
        report_completion: bool = True,
        repeat_count: int | None = None,
        period_ticks: int | None = None,
    ) -> HdlCompletion | None:
        """Launch a mode-0 SPI write at ``timestamp``.

        ``args[0]`` is the 32-bit transmit word and ``args[1]`` packs chip
        select, bit count, and half-period; descriptor-wide repeat fields stay
        in ``args[2:4]``.
        """

        if not 0 <= chip_select < 8:
            raise ValueError("chip_select must select one of eight active-low outputs")
        if not 0 <= tx_word <= 0xFFFF_FFFF:
            raise ValueError("tx_word must fit in uint32")
        if not 1 <= bit_count <= 32:
            raise ValueError("bit_count must be in the range 1..32")
        if not 1 <= half_period_ticks <= 0xFFFF:
            raise ValueError("half_period_ticks must be in the range 1..65535")
        control = chip_select | (bit_count << 8) | (half_period_ticks << 16)
        return self._push(
            command_id=CommandID.SPI_TRANSFER,
            target_id=target_id,
            sequence_id=sequence_id,
            timestamp=timestamp,
            args=(tx_word, control, repeat_count or 0, period_ticks or 0),
            late_policy=late_policy,
            report_completion=report_completion,
            repeat=repeat_count is not None,
        )

    def advance_to(self, timestamp_ticks: int) -> list[HdlCompletion]:
        self.current_time_ticks = timestamp_ticks
        if not self.enabled:
            self._scheduler.set_time(timestamp_ticks)
            return []
        self._scheduler.current_time_ticks = timestamp_ticks
        reported_completions: list[Completion] = []
        remaining = self._scheduler._queue.__class__()
        while self._scheduler._queue:
            entry = self._scheduler._queue.popleft()
            descriptor = entry.descriptor
            if entry.timestamp_ticks > timestamp_ticks:
                remaining.append(entry)
                continue
            deferred_late = id(entry) in self._deferred_late_execute_descriptor_ids
            repeat_late = bool(
                descriptor.flags & Flag.REPEAT and descriptor.timestamp_ticks < timestamp_ticks
            )
            late = deferred_late or repeat_late
            dropped = late and not descriptor.flags & Flag.LATE_EXECUTE_NOW
            continues = self._scheduler._repeat_continues(entry)
            base = timestamp_ticks if late else descriptor.timestamp_ticks
            args = cast(tuple[int, int, int, int], descriptor.args)
            repeat_overflow = continues and base > 0xFFFF_FFFF_FFFF_FFFF - args[3]
            produces_completion = bool(
                descriptor.flags & Flag.REPORT_COMPLETION or dropped or repeat_overflow
            )
            if self.completion_pending and produces_completion:
                remaining.appendleft(entry)
                remaining.extend(self._scheduler._queue)
                break

            self._deferred_late_execute_descriptor_ids.discard(id(entry))
            if repeat_late and not deferred_late:
                self._scheduler._increment_late()
            status = (
                Status.LATE_DROPPED
                if dropped
                else (Status.LATE_EXECUTED if late else Status.EXECUTED)
            )
            executed_at = (
                None if dropped else (timestamp_ticks if late else descriptor.timestamp_ticks)
            )

            if repeat_overflow:
                self._scheduler._increment_overflow()
                status = Status.REPEAT_OVERFLOW
                continues = False

            if executed_at is None:
                completion = Completion.from_descriptor(
                    descriptor, status, detail=entry.occurrence_index
                )
                self._scheduler.completions.append(completion)
            else:
                completion = self._scheduler._execute(entry, status, executed_at)
                self._apply_event(descriptor, executed_at)

            if continues:
                next_descriptor = CommandDescriptor(
                    command_id=descriptor.command_id,
                    target_id=descriptor.target_id,
                    timestamp_ticks=base + args[3],
                    flags=descriptor.flags,
                    sequence_id=descriptor.sequence_id,
                    args=descriptor.args,
                )
                remaining.append(type(entry)(next_descriptor, entry.occurrence_index + 1))

            if descriptor.flags & Flag.REPORT_COMPLETION or status not in {
                Status.EXECUTED,
                Status.LATE_EXECUTED,
            }:
                hdl_completion = HdlCompletion.from_completion(completion)
                self._record_completion(hdl_completion)
                reported_completions.append(completion)

        self._scheduler._queue = remaining.__class__(
            sorted(remaining, key=lambda queued: queued.timestamp_ticks)
        )
        return [HdlCompletion.from_completion(completion) for completion in reported_completions]

    def _push(
        self,
        *,
        command_id: CommandID,
        target_id: TargetFamily | int,
        sequence_id: int,
        timestamp: int,
        args: tuple[int, int, int, int],
        late_policy: str,
        report_completion: bool,
        repeat: bool = False,
    ) -> HdlCompletion | None:
        flags = Flag.REPORT_COMPLETION if report_completion else Flag.NONE
        if repeat:
            flags |= Flag.REPEAT
        if late_policy == "drop":
            flags |= Flag.LATE_DROP
        elif late_policy == "execute_now":
            flags |= Flag.LATE_EXECUTE_NOW
        else:
            raise ValueError(f"unsupported late policy {late_policy!r}")

        descriptor = CommandDescriptor(
            command_id=command_id,
            target_id=target_id,
            timestamp_ticks=timestamp,
            flags=flags,
            sequence_id=sequence_id,
            args=args,
        )
        immediate_late = descriptor.timestamp_ticks < self.current_time_ticks
        immediate_late_produces_completion = bool(
            descriptor.flags & Flag.REPORT_COMPLETION
            or not descriptor.flags & Flag.LATE_EXECUTE_NOW
        )
        if self.completion_pending and immediate_late and immediate_late_produces_completion:
            self._scheduler._increment_late()
            if self._scheduler.queue_level >= self._scheduler.queue_depth:
                self._scheduler._increment_overflow()
                return None
            from .scheduler import _QueuedCommand

            entry = _QueuedCommand(descriptor)
            self._scheduler._queue.append(entry)
            self._deferred_late_execute_descriptor_ids.add(id(entry))
            self._sort_queue_by_timestamp()
            return None
        event_start = len(self._scheduler.events)
        completion_start = len(self._scheduler.completions)
        completion = self._scheduler.submit(descriptor)
        if completion is None:
            self._sort_queue_by_timestamp()
        for event in self._scheduler.events[event_start:]:
            self._apply_event_from_event(event)
        if completion is None or len(self._scheduler.completions) == completion_start:
            return None
        hdl_completion = HdlCompletion.from_completion(completion)
        if self.completion_pending:
            if completion.status is not Status.OVERFLOW:
                self._scheduler._increment_overflow()
            return None
        self._record_completion(hdl_completion)
        return hdl_completion

    def _apply_event(self, descriptor: CommandDescriptor, executed_at: int) -> None:
        command_id = int(descriptor.command_id)
        a0, a1, _a2, _a3 = descriptor.args
        if command_id == int(CommandID.GPIO_WRITE):
            self._apply_gpio(a0, a1)
        elif command_id == int(CommandID.MARKER_PULSE):
            self._apply_marker(a0, max(a1, 1), executed_at)
        elif command_id == int(CommandID.STREAM_GATE):
            self._apply_stream_gate(a0, a1 & 1)
            if a1 & 1:
                self._latch_capture_tag(descriptor, a0, executed_at, command_id)
        elif command_id == int(CommandID.TDD_SYNC):
            self._apply_tdd_sync(a0, bool(a1 & 1), executed_at)
        elif command_id in (int(CommandID.CAPTURE_ARM), int(CommandID.TRANSMIT_ARM)):
            self._apply_arm_release(descriptor, a0, a1, executed_at)
        elif command_id == int(CommandID.SPI_TRANSFER):
            self._apply_spi(a0, a1, executed_at)

    def _apply_event_from_event(self, event: Event) -> None:
        command_id = int(event.command_id)
        a0, a1, _a2, _a3 = event.args
        executed_at = event.executed_timestamp_ticks
        if command_id == int(CommandID.GPIO_WRITE):
            self._apply_gpio(a0, a1)
        elif command_id == int(CommandID.MARKER_PULSE):
            self._apply_marker(a0, max(a1, 1), executed_at)
        elif command_id == int(CommandID.STREAM_GATE):
            self._apply_stream_gate(a0, a1 & 1)
            if a1 & 1:
                self._latch_capture_tag(event, a0, executed_at, command_id)
        elif command_id == int(CommandID.TDD_SYNC):
            self._apply_tdd_sync(a0, bool(a1 & 1), executed_at)
        elif command_id in (int(CommandID.CAPTURE_ARM), int(CommandID.TRANSMIT_ARM)):
            self._apply_arm_release(event, a0, a1, executed_at)
        elif command_id == int(CommandID.SPI_TRANSFER):
            self._apply_spi(a0, a1, executed_at)

    def _apply_gpio(self, mask: int, value: int) -> None:
        self.gpio_out = (self.gpio_out & ~mask) | (value & mask)

    def _apply_marker(self, marker_id: int, width_ticks: int, executed_at: int) -> None:
        bit = marker_id & 0x1F
        self.marker_out |= 1 << bit
        self.marker_pulses.append(
            MarkerPulse(marker_id=marker_id, width_ticks=width_ticks, executed_at_tick=executed_at)
        )

    def _apply_stream_gate(self, gate_id: int, enable: int) -> None:
        bit = gate_id & 0x1F
        if enable:
            self.stream_gate_en |= 1 << bit
        else:
            self.stream_gate_en &= ~(1 << bit)

    def _apply_tdd_sync(self, instance: int, start: bool, executed_at: int) -> None:
        self.tdd_sync_events.append(
            TddSyncEvent(instance=instance, start=start, executed_at_tick=executed_at)
        )

    def _apply_spi(self, tx_word: int, control: int, executed_at: int) -> None:
        self.spi_transfers.append(
            SpiTransfer(
                chip_select=control & 0xFF,
                tx_word=tx_word,
                bit_count=(control >> 8) & 0xFF,
                half_period_ticks=(control >> 16) & 0xFFFF,
                executed_at_tick=executed_at,
            )
        )

    @staticmethod
    def arm_pulse_ticks(requested: int) -> int:
        """Resolve ``args[1]`` into the width the HDL will actually use.

        0 selects the device default; anything below the floor is raised to it,
        because ``sync_ext`` crosses a two-stage synchronizer and a narrower
        pulse could be dropped entirely.
        """

        if requested == 0:
            return ARM_PULSE_DEFAULT_TICKS
        return max(requested, ARM_PULSE_MIN_TICKS)

    def _apply_arm_release(
        self,
        source: CommandDescriptor | Event,
        gate_id: int,
        width_ticks: int,
        executed_at: int,
    ) -> None:
        command_id = int(source.command_id)
        direction = "capture" if command_id == int(CommandID.CAPTURE_ARM) else "transmit"
        # A pulse only ever sets a bit, mirroring the RTL's level | pulse, so it
        # cannot clear a level a previous STREAM_GATE established.
        self.stream_gate_en |= 1 << (gate_id & 0x1F)
        self.arm_pulses.append(
            ArmPulse(
                gate_id=gate_id,
                width_ticks=self.arm_pulse_ticks(width_ticks),
                executed_at_tick=executed_at,
                direction=direction,
            )
        )
        self._latch_capture_tag(source, gate_id, executed_at, command_id)

    def _latch_capture_tag(
        self,
        source: CommandDescriptor | Event,
        gate_id: int,
        executed_at: int,
        command_id: int,
    ) -> None:
        """Latch the edge-capture registers, mirroring the RTL."""

        # CommandDescriptor names this timestamp_ticks; Event names it
        # requested_timestamp_ticks. Both reach here, one per dispatch path.
        requested = getattr(source, "timestamp_ticks", None)
        if requested is None:
            requested = source.requested_timestamp_ticks

        self.capture_tag = CaptureTag(
            ticks=executed_at,
            sequence_id=source.sequence_id,
            target_id=int(source.target_id),
            command_id=command_id,
            gate_id=gate_id & 0xF,
            late=executed_at != requested,
        )
        self.capture_tag_count += 1
        # A second latch before the host acks is an overrun: a capture happened
        # that software never got to see.
        if self.capture_tag_unread:
            self.capture_tag_overrun = True
        self.capture_tag_unread = True

    def ack_edge_capture(self) -> None:
        """Clear the software-visible capture-tag status bits.

        The latched tag itself is preserved, matching the RTL: only the unread
        and overrun flags clear.
        """

        self.capture_tag_unread = False
        self.capture_tag_overrun = False

    def set_capture_tag_enable(self, mask: int) -> None:
        """Set the per-gate in-band timestamp header enable mask."""

        if not 0 <= mask <= 0xFFFF:
            raise ValueError(f"capture tag enable mask must be a uint16, got {mask}")
        self.capture_tag_enable = mask

    def _record_completion(self, completion: HdlCompletion) -> None:
        self.completion = completion
        self.completion_pending = True
        self.command_irq = True

    def _load_time(self, timestamp_ticks: int) -> None:
        self.current_time_ticks = timestamp_ticks
        self._scheduler.current_time_ticks = timestamp_ticks

    def _sort_queue_by_timestamp(self) -> None:
        self._scheduler._queue = self._scheduler._queue.__class__(
            sorted(self._scheduler._queue, key=lambda descriptor: descriptor.timestamp_ticks)
        )

"""Public constants for the timed command interface."""

from __future__ import annotations

from enum import IntEnum, IntFlag


class Flag(IntFlag):
    """Descriptor flag values."""

    NONE = 0
    REPORT_COMPLETION = 0x0000_0001
    LATE_DROP = 0x0000_0002
    LATE_EXECUTE_NOW = 0x0000_0004
    REPEAT = 0x0000_0008


class Capability(IntFlag):
    """Capability bits reported by the HDL feature register."""

    GPIO_TARGET = 0x0000_0001
    PPS_EPOCH = 0x0000_0002
    MARKER_TARGET = 0x0000_0004
    STREAM_GATE_TARGET = 0x0000_0008
    AXI_TDD_TARGET = 0x0000_0010
    ARM_RELEASE_TARGET = 0x0000_0020
    STREAM_TIMESTAMP = 0x0000_0040
    REPEAT = 0x0000_0080
    SPI_TARGET = 0x0000_0100


class CommandID(IntEnum):
    """Command IDs reserved by the command-interface document."""

    GPIO_WRITE = 0x0001
    MARKER_PULSE = 0x0002
    STREAM_GATE = 0x0003
    TDD_SYNC = 0x0004
    DDS_TX_GATE = 0x0005
    CAPTURE_ARM = 0x0006
    TRANSMIT_ARM = 0x0007
    SPI_TRANSFER = 0x0008
    AD9081_PROFILE_TRIGGER = 0x1000
    SOFTWARE_CONTROL_NOTIFY = 0x2000


class TargetFamily(IntEnum):
    """Target ID families. Values represent the first ID in each range."""

    GPIO = 0x0000
    MARKER = 0x0100
    STREAM_GATE = 0x0200
    AXI_TDD = 0x0300
    DDS_TX = 0x0400
    CAPTURE = 0x0500
    TRANSMIT = 0x0600
    SPI = 0x0700
    AD9081_CONTROL = 0x1000

    @classmethod
    def family_for(cls, target_id: int) -> TargetFamily:
        if 0x0000 <= target_id <= 0x00FF:
            return cls.GPIO
        if 0x0100 <= target_id <= 0x01FF:
            return cls.MARKER
        if 0x0200 <= target_id <= 0x02FF:
            return cls.STREAM_GATE
        if 0x0300 <= target_id <= 0x03FF:
            return cls.AXI_TDD
        if 0x0400 <= target_id <= 0x04FF:
            return cls.DDS_TX
        if 0x0500 <= target_id <= 0x05FF:
            return cls.CAPTURE
        if 0x0600 <= target_id <= 0x06FF:
            return cls.TRANSMIT
        if 0x0700 <= target_id <= 0x07FF:
            return cls.SPI
        if 0x1000 <= target_id <= 0x1FFF:
            return cls.AD9081_CONTROL
        raise ValueError(f"unsupported target id 0x{target_id:04x}")


class Status(IntEnum):
    """Completion status values."""

    EXECUTED = 0
    LATE_EXECUTED = 1
    LATE_DROPPED = 2
    OVERFLOW = 3
    UNSUPPORTED = 4
    BAD_DESCRIPTOR = 5
    REPEAT_OVERFLOW = 6

/* SPDX-License-Identifier: MIT */
/* Tick timed-command descriptor contract (REQ-TEST-001).
 * Field names follow Tick's UAPI struct tick_cmd_desc; the packed form is the
 * 48-byte little-endian layout of the Tick Python model. */
#ifndef DATAX_TICK_DESCRIPTOR_H
#define DATAX_TICK_DESCRIPTOR_H

#include <stdint.h>

#define DATAX_TICK_DESC_VERSION 1u
#define DATAX_TICK_DESC_SIZE 48u

/* X(group, member, value): enum name is DATAX_TICK_<group>_<member>. */
#define DATAX_TICK_ENUM_LIST(X) \
	X(FLAG, NONE, 0x00000000u) \
	X(FLAG, REPORT_COMPLETION, 0x00000001u) \
	X(FLAG, LATE_DROP, 0x00000002u) \
	X(FLAG, LATE_EXECUTE_NOW, 0x00000004u) \
	X(FLAG, REPEAT, 0x00000008u) \
	X(CAPABILITY, GPIO_TARGET, 0x00000001u) \
	X(CAPABILITY, PPS_EPOCH, 0x00000002u) \
	X(CAPABILITY, MARKER_TARGET, 0x00000004u) \
	X(CAPABILITY, STREAM_GATE_TARGET, 0x00000008u) \
	X(CAPABILITY, AXI_TDD_TARGET, 0x00000010u) \
	X(CAPABILITY, ARM_RELEASE_TARGET, 0x00000020u) \
	X(CAPABILITY, STREAM_TIMESTAMP, 0x00000040u) \
	X(CAPABILITY, REPEAT, 0x00000080u) \
	X(CAPABILITY, SPI_TARGET, 0x00000100u) \
	X(COMMAND_ID, GPIO_WRITE, 0x0001u) \
	X(COMMAND_ID, MARKER_PULSE, 0x0002u) \
	X(COMMAND_ID, STREAM_GATE, 0x0003u) \
	X(COMMAND_ID, TDD_SYNC, 0x0004u) \
	X(COMMAND_ID, DDS_TX_GATE, 0x0005u) \
	X(COMMAND_ID, CAPTURE_ARM, 0x0006u) \
	X(COMMAND_ID, TRANSMIT_ARM, 0x0007u) \
	X(COMMAND_ID, SPI_TRANSFER, 0x0008u) \
	X(COMMAND_ID, AD9081_PROFILE_TRIGGER, 0x1000u) \
	X(COMMAND_ID, SOFTWARE_CONTROL_NOTIFY, 0x2000u) \
	X(TARGET_FAMILY, GPIO, 0x0000u) \
	X(TARGET_FAMILY, MARKER, 0x0100u) \
	X(TARGET_FAMILY, STREAM_GATE, 0x0200u) \
	X(TARGET_FAMILY, AXI_TDD, 0x0300u) \
	X(TARGET_FAMILY, DDS_TX, 0x0400u) \
	X(TARGET_FAMILY, CAPTURE, 0x0500u) \
	X(TARGET_FAMILY, TRANSMIT, 0x0600u) \
	X(TARGET_FAMILY, SPI, 0x0700u) \
	X(TARGET_FAMILY, AD9081_CONTROL, 0x1000u) \
	X(STATUS, EXECUTED, 0) \
	X(STATUS, LATE_EXECUTED, 1) \
	X(STATUS, LATE_DROPPED, 2) \
	X(STATUS, OVERFLOW, 3) \
	X(STATUS, UNSUPPORTED, 4) \
	X(STATUS, BAD_DESCRIPTOR, 5) \
	X(STATUS, REPEAT_OVERFLOW, 6)

#define DATAX_TICK_DEFINE(group, member, value) \
	DATAX_TICK_##group##_##member = (value),
enum datax_tick_const { DATAX_TICK_ENUM_LIST(DATAX_TICK_DEFINE) };

struct datax_tick_descriptor {
	uint16_t version;
	uint16_t size_bytes;
	uint16_t command_id;
	uint16_t target_id;
	uint64_t timestamp_ticks;
	uint32_t flags;
	uint32_t sequence_id;
	uint32_t args[4];
};

/* Test-only. Packs d little-endian into out (reserved bytes zeroed).
 * Returns 0, or -1 if d or out is NULL. No validation is performed here. */
int datax_internal_tick_descriptor_pack(const struct datax_tick_descriptor *d,
					unsigned char out[48]);

/* Test-only. Value of the DATAX_TICK_* constant called name, -1 if unknown. */
long datax_internal_tick_enum(const char *name);

#endif

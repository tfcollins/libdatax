/* SPDX-License-Identifier: MIT */
#ifndef _UAPI_LINUX_TICK_TIMED_COMMAND_H
#define _UAPI_LINUX_TICK_TIMED_COMMAND_H

#include <linux/types.h>

#define TICK_CMD_DESC_VERSION		 1u
#define TICK_CMD_ARG_WORDS		 4u
#define TICK_CMD_DESC_SIZE		 48u

#define TICK_CMD_GPIO_WRITE		 0x0001u
#define TICK_CMD_MARKER_PULSE		 0x0002u
#define TICK_CMD_STREAM_GATE		 0x0003u
#define TICK_CMD_TDD_SYNC		 0x0004u
#define TICK_CMD_DDS_TX_GATE		 0x0005u
#define TICK_CMD_CAPTURE_ARM		 0x0006u
#define TICK_CMD_TRANSMIT_ARM		 0x0007u
#define TICK_CMD_SPI_TRANSFER		 0x0008u
#define TICK_CMD_AD9081_PROFILE_TRIGGER	 0x1000u
#define TICK_CMD_SOFTWARE_CONTROL_NOTIFY 0x2000u

#define TICK_TARGET_GPIO		 0x0000u
#define TICK_TARGET_MARKER		 0x0100u
#define TICK_TARGET_STREAM_GATE		 0x0200u
#define TICK_TARGET_AXI_TDD		 0x0300u
#define TICK_TARGET_DDS_TX		 0x0400u
#define TICK_TARGET_CAPTURE		 0x0500u
#define TICK_TARGET_TRANSMIT		 0x0600u
#define TICK_TARGET_SPI			 0x0700u
#define TICK_TARGET_AD9081_CONTROL	 0x1000u

#define TICK_CMD_F_NONE			 0x00000000u
#define TICK_CMD_F_REPORT_COMPLETION	 0x00000001u
#define TICK_CMD_F_LATE_DROP		 0x00000002u
#define TICK_CMD_F_LATE_EXECUTE_NOW	 0x00000004u
#define TICK_CMD_F_REPEAT		 0x00000008u

/* With TICK_CMD_F_REPEAT: args[2] is total count (0 = infinite), args[3]
 * is the nonzero period in scheduler ticks.  One queue slot is retained for
 * the series, sequence_id is unchanged, and completion.detail is the
 * zero-based occurrence index modulo 2^32.
 */
#define TICK_REPEAT_COUNT_ARG	    2u
#define TICK_REPEAT_PERIOD_ARG	    3u

#define TICK_CAP_GPIO_TARGET	    0x00000001u
#define TICK_CAP_PPS_EPOCH	    0x00000002u
#define TICK_CAP_MARKER_TARGET	    0x00000004u
#define TICK_CAP_STREAM_GATE_TARGET 0x00000008u
#define TICK_CAP_AXI_TDD_TARGET	    0x00000010u
#define TICK_CAP_ARM_RELEASE_TARGET 0x00000020u
#define TICK_CAP_STREAM_TIMESTAMP   0x00000040u
#define TICK_CAP_REPEAT		    0x00000080u
#define TICK_CAP_SPI_TARGET	    0x00000100u

enum tick_cmd_status {
	TICK_STATUS_EXECUTED = 0,
	TICK_STATUS_LATE_EXECUTED = 1,
	TICK_STATUS_LATE_DROPPED = 2,
	TICK_STATUS_OVERFLOW = 3,
	TICK_STATUS_UNSUPPORTED = 4,
	TICK_STATUS_BAD_DESCRIPTOR = 5,
	TICK_STATUS_REPEAT_OVERFLOW = 6,
};

struct tick_cmd_desc {
	__u16 version;
	__u16 size_bytes;
	__u16 command_id;
	__u16 target_id;
	__u64 timestamp_ticks;
	__u32 flags;
	__u32 sequence_id;
	__u32 args[TICK_CMD_ARG_WORDS];
	__u32 reserved[2];
} __attribute__((packed));

struct tick_cmd_completion {
	__u32 sequence_id;
	__u16 command_id;
	__u16 target_id;
	__u64 requested_timestamp_ticks;
	__u64 executed_timestamp_ticks;
	__u32 status;
	__u32 detail;
} __attribute__((packed));

#endif

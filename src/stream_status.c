// SPDX-License-Identifier: MIT
/*
 * DMA overflow / underflow detection through the converter core's status
 * register, read over the streaming device's direct_reg_access debug
 * attribute.
 *
 * Registers (ADI HDL e57851f, library/common; the stock design's release):
 *  - RX, up_adc_common.v (COMMON_ID 6'h00): status at word address 7'h22,
 *    byte offset 0x88 (read decode line 496). Bit 2 is up_status_ovf, the
 *    DMA overflow flag, write-1-to-clear (line 379:
 *    up_status_ovf & ~up_wdata[2]). The lablp spike read 0x4 there.
 *  - TX, up_dac_common.v (COMMON_ID 6'h10, so up_waddr[13:7] == 7'h20):
 *    status at word address 7'h22 of that window, full word address 0x1022,
 *    byte offset 0x4088 (read decode line 483). Bit 0 is up_status_unf, the
 *    DMA underflow flag, write-1-to-clear (line 385).
 *
 * direct_reg_access. Writing "0x80000000 | byte_offset" (a single number)
 * selects a core register for the next read; the bit-31 magic routes the
 * access to the core rather than to the converter's SPI map. Writing
 * "<offset> <value>" (two numbers) writes the register; libiio's own
 * iio_device_reg_write fallback uses that form. The ADI driver's handlers
 * (cf_axi_adc_core.c / cf_axi_dds.c) were not reachable when this was
 * written: the write form, including the magic bit on the offset, is the
 * rule above, to be confirmed on a board by the hardware test.
 *
 * Sticky flags make rate limiting lossless: a poll sees everything raised
 * since the last one. The select-then-read sequence is not atomic against
 * another client using the same attribute.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <iio/iio.h>

#include "stream.h"

#define REG_MAGIC 0x80000000u
#define RX_STATUS_OFF 0x88u   /* up_adc_common.v: word 7'h22 */
#define RX_OVF_BIT 2          /* up_status_ovf */
#define TX_STATUS_OFF 0x4088u /* up_dac_common.v: word 0x1022 */
#define TX_UNF_BIT 0          /* up_status_unf */
#define POLL_INTERVAL_US 100000LL
#define ATTR "direct_reg_access"

enum { DETECT_UNKNOWN = 0, DETECT_YES, DETECT_NO };

static long long mono_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static uint32_t status_off(enum datax_direction dir)
{
	return dir == DATAX_RX ? RX_STATUS_OFF : TX_STATUS_OFF;
}

static uint32_t sticky_bit(enum datax_direction dir)
{
	return 1u << (dir == DATAX_RX ? RX_OVF_BIT : TX_UNF_BIT);
}

void datax_stream_status_parse(enum datax_direction dir, uint32_t word,
			       bool *overflow, bool *underflow)
{
	bool hit = word & sticky_bit(dir);

	if (overflow)
		*overflow = dir == DATAX_RX && hit;
	if (underflow)
		*underflow = dir == DATAX_TX && hit;
}

int datax_stream_status_poll(struct datax_stream *s, bool *overflow, bool *underflow)
{
	const struct iio_attr *attr;
	enum datax_direction dir = s->cfg.direction;
	char buf[64];
	unsigned long long word;
	uint32_t off = status_off(dir);
	ssize_t n;
	char *end;

	if (overflow)
		*overflow = false;
	if (underflow)
		*underflow = false;
	attr = iio_device_find_debug_attr(s->iio_dev, ATTR);
	if (!attr)
		return -ENOENT;
	snprintf(buf, sizeof(buf), "0x%" PRIx32, REG_MAGIC | off);
	n = iio_attr_write_string(attr, buf);
	if (n < 0)
		return (int)n;
	n = iio_attr_read_raw(attr, buf, sizeof(buf));
	if (n < 0)
		return (int)n;
	buf[sizeof(buf) - 1] = '\0';
	word = strtoull(buf, &end, 0);
	if (end == buf)
		return -EIO;
	datax_stream_status_parse(dir, (uint32_t)word, overflow, underflow);
	if (word & sticky_bit(dir)) {
		/* Write-1-to-clear: write the set bit back. */
		snprintf(buf, sizeof(buf), "0x%" PRIx32 " 0x%" PRIx32,
			 REG_MAGIC | off, sticky_bit(dir));
		n = iio_attr_write_string(attr, buf);
		if (n < 0)
			return (int)n;
	}
	return 0;
}

void datax_stream_status_check(struct datax_stream *s, bool discard)
{
	enum datax_direction dir = s->cfg.direction;
	bool ovf, unf;
	long long now;
	int ret;

	if (s->detect_off)
		return;
	now = mono_us();
	if (s->polled_once && now - s->last_poll_us < POLL_INTERVAL_US)
		return;
	s->polled_once = true;
	s->last_poll_us = now;
	s->stats.status_polls++;
	ret = datax_stream_status_poll(s, &ovf, &unf);
	if (ret) {
		/* No usable status register: detection stays off for the
		 * stream's life. Not an error of the stream itself. */
		s->detect_off = true;
		s->stats.overflow_detect = false;
		s->dev->status_detect[dir] = DETECT_NO;
		return;
	}
	s->stats.overflow_detect = true;
	s->dev->status_detect[dir] = DETECT_YES;
	if (discard)
		return;
	if (ovf) {
		s->stats.overflows++;
		s->disc_pending = true;
	}
	if (unf)
		s->stats.underflows++;
}

int datax_internal_device_stream_info(const struct datax_device *dev,
				      enum datax_direction dir, char *name,
				      size_t len, int *detect)
{
	const struct iio_device *sd;
	const char *n;

	if (!dev || (dir != DATAX_RX && dir != DATAX_TX))
		return -EINVAL;
	sd = dev->stream_dev[dir];
	if (!sd)
		return -ENOENT;
	if (name && len) {
		n = iio_device_get_name(sd);
		snprintf(name, len, "%s", n ? n : iio_device_get_id(sd));
	}
	if (detect) {
		if (!iio_device_find_debug_attr(sd, ATTR) ||
		    dev->status_detect[dir] == DETECT_NO)
			*detect = 0;
		else
			*detect = dev->status_detect[dir] == DETECT_YES ? 1 : -1;
	}
	return 0;
}

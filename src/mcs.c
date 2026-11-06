// SPDX-License-Identifier: MIT
/*
 * MCS verification and the PHASE_COHERENT rule.
 *
 * run_mcs only reads. On the ADRV9009-ZU11EG the JESD204 FSM of each phy is
 * exposed as device attributes (spike results, spec 2026-11-05: tron reads
 * jesd204_fsm_state=opt_post_running_stage, jesd204_fsm_error=0,
 * jesd204_fsm_paused=0, jesd204_fsm_ctrl=1 on both phys). A completed MCS
 * leaves both in opt_post_running_stage; writing jesd204_fsm_ctrl would
 * resynchronise running links, which the SDK never does.
 *
 * RF PLL phase sync: PHASE_COHERENT needs it, and no attribute for it can be
 * cited from the ADI adrv9009 driver (no ADI Linux tree was available to
 * check, and the spike found no adi,*phase* device-tree property on tron).
 * So dev->phase_sync is permanently false on adrv9009 in P3a and
 * PHASE_COHERENT is never set. The rule itself (MCS verified AND phase sync
 * AND no single-chip tune since) is implemented in caps.c and covered by a
 * test through datax_internal_mcs_force_phase_sync.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iio/iio.h>

#include <datax/datax_internal.h>

#include "error.h"
#include "internal.h"
#include "tick_ops.h"

#define STATE_OK "opt_post_running_stage"

static const char *phy_name(const struct datax_device *dev, unsigned int i)
{
	const char *n = dev->phy[i] ? iio_device_get_name(dev->phy[i]) : NULL;

	return n ? n : (i ? "adrv9009-phy-b" : "adrv9009-phy");
}

/* Read a phy attribute as trimmed text. 0, or -errno. */
static int phy_attr(const struct iio_device *phy, const char *attr, char *buf, size_t len)
{
	const struct iio_attr *a = iio_device_find_attr(phy, attr);
	ssize_t n;

	if (!a)
		return -ENOENT;
	n = iio_attr_read_raw(a, buf, len - 1);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

static bool is_zero(const char *s)
{
	char *end;
	long v = strtol(s, &end, 0);

	return end != s && *end == '\0' && v == 0;
}

/* Verify one phy. 0, or -EIO with the phy and its state in *why. */
static int check_phy(const struct datax_device *dev, unsigned int i, char *why, size_t wlen,
		     char *state, size_t slen)
{
	char err[32] = "", paused[32] = "";
	int ret;

	ret = phy_attr(dev->phy[i], "jesd204_fsm_state", state, slen);
	if (ret) {
		snprintf(why, wlen, "%s: cannot read jesd204_fsm_state (%d)", phy_name(dev, i), ret);
		snprintf(state, slen, "unreadable");
		return -EIO;
	}
	if (strcmp(state, STATE_OK)) {
		snprintf(why, wlen, "%s: JESD204 FSM state is '%s', expected '%s'",
			 phy_name(dev, i), state, STATE_OK);
		return -EIO;
	}
	ret = phy_attr(dev->phy[i], "jesd204_fsm_error", err, sizeof(err));
	if (ret || !is_zero(err)) {
		snprintf(why, wlen, "%s: jesd204_fsm_error is '%s' in state '%s'",
			 phy_name(dev, i), ret ? "unreadable" : err, state);
		return -EIO;
	}
	ret = phy_attr(dev->phy[i], "jesd204_fsm_paused", paused, sizeof(paused));
	if (ret || !is_zero(paused)) {
		snprintf(why, wlen, "%s: JESD204 FSM is paused ('%s') in state '%s'",
			 phy_name(dev, i), ret ? "unreadable" : paused, state);
		return -EIO;
	}
	return 0;
}

static bool have_mcs(const struct datax_device *dev)
{
	return dev->family == DATAX_FAMILY_ADRV9009 && dev->nphy == 2 && dev->caps.mcs_attrs;
}

int datax_device_run_mcs(struct datax_device *dev)
{
	char why[200] = "", state[64];
	int ret = 0;

	if (!dev)
		return -EINVAL;
	if (!have_mcs(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "mcs",
				       "run_mcs: this device has no JESD204 multi-chip sync "
				       "(needs both adrv9009 phys with jesd204_fsm_ctrl)");
	for (unsigned int i = 0; i < dev->nphy && !ret; i++)
		ret = check_phy(dev, i, why, sizeof(why), state, sizeof(state));

	/* single_chip_tuned is deliberately not cleared here: verifying only
	 * reads the FSM and does not restore the LO phase relationship a
	 * single-chip tune broke. */
	pthread_mutex_lock(&dev->ctx->lock);
	dev->mcs_verified = !ret;
	pthread_mutex_unlock(&dev->ctx->lock);
	datax_caps_refresh_flags(dev);
	if (ret)
		return datax_error_ret(dev->ctx, EIO, "mcs", "run_mcs: %s", why);
	return 0;
}

/* Test-only: set the RF PLL phase sync input to the PHASE_COHERENT rule. */
void datax_internal_mcs_force_phase_sync(struct datax_device *dev, bool on)
{
	if (!dev)
		return;
	pthread_mutex_lock(&dev->ctx->lock);
	dev->phase_sync = on;
	pthread_mutex_unlock(&dev->ctx->lock);
	datax_caps_refresh_flags(dev);
}

/*
 * Diagnostics for doctor: the live FSM state of the phys ("a; b" when they
 * differ) and the PHASE_COHERENT answer with its reason. Reads only. Returns
 * 1 when PHASE_COHERENT holds, 0 when not, -ENOTSUP without MCS.
 */
int datax_internal_mcs_info(const struct datax_device *dev, char *state, size_t slen,
			    char *reason, size_t rlen)
{
	char st[2][64] = { "", "" };
	bool verified, tuned, sync;
	int ret = 0;

	if (!dev || !state || !slen || !reason || !rlen)
		return -EINVAL;
	if (!have_mcs(dev))
		return -ENOTSUP;
	for (unsigned int i = 0; i < 2; i++)
		if (phy_attr(dev->phy[i], "jesd204_fsm_state", st[i], sizeof(st[i])))
			snprintf(st[i], sizeof(st[i]), "unreadable");
	if (!strcmp(st[0], st[1]))
		snprintf(state, slen, "%s", st[0]);
	else
		snprintf(state, slen, "%s / %s", st[0], st[1]);

	pthread_mutex_lock(&dev->ctx->lock);
	verified = dev->mcs_verified;
	tuned = dev->single_chip_tuned;
	sync = dev->phase_sync;
	pthread_mutex_unlock(&dev->ctx->lock);
	if (!verified)
		snprintf(reason, rlen, "MCS not verified; call run_mcs");
	else if (!sync)
		snprintf(reason, rlen, "RF PLL phase sync not enabled");
	else if (tuned)
		snprintf(reason, rlen, "single-chip tune since MCS");
	else
		ret = 1, snprintf(reason, rlen, "MCS verified, RF PLL phase sync on");
	return ret;
}

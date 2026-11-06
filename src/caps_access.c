// SPDX-License-Identifier: MIT
#include <errno.h>

#include "internal.h"

bool datax_caps_has(const struct datax_caps *caps, enum datax_cap cap)
{
	if (!caps || (unsigned int)cap >= DATAX_CAP_COUNT)
		return false;
	return (caps->flags >> cap) & 1u;
}

int datax_caps_get_limit(const struct datax_caps *caps, enum datax_limit which,
			 unsigned long long *value)
{
	if (!caps || !value || (unsigned int)which >= DATAX_LIMIT_COUNT)
		return -EINVAL;
	/* Measured at run time: REPORT_SPACING_US when a scheduler is created
	 * (until it is destroyed), HOP_STAGE_US at hop staging (Task 4). */
	if ((which == DATAX_LIMIT_REPORT_SPACING_US || which == DATAX_LIMIT_HOP_STAGE_US) &&
	    !(caps->measured & (1u << which)))
		return -ENOTSUP;
	*value = caps->limits[which];
	return 0;
}

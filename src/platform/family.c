// SPDX-License-Identifier: MIT
#include "family.h"

/* The one place families are registered; add new families here. */
const struct datax_family_ops *datax_family_ops(enum datax_family family)
{
	switch (family) {
	case DATAX_FAMILY_AD936X:
		return &datax_ad936x_ops;
	default:
		return NULL;
	}
}

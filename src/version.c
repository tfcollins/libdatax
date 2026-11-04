// SPDX-License-Identifier: MIT
#include <datax/datax.h>

/* ctx is unused: the library version does not depend on a context, so NULL is allowed. */
int datax_context_get_version(const struct datax_context *ctx,
			      unsigned int *major, unsigned int *minor)
{
	(void)ctx;
	if (major)
		*major = DATAX_VERSION_MAJOR;
	if (minor)
		*minor = DATAX_VERSION_MINOR;
	return 0;
}

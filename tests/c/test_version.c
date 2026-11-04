// SPDX-License-Identifier: MIT
#include <datax/datax.h>

#include "check.h"

int main(void)
{
	unsigned int maj = 99, min = 99;

	CHECK(datax_context_get_version(NULL, &maj, &min) == 0);
	CHECK(maj == DATAX_VERSION_MAJOR);
	CHECK(min == DATAX_VERSION_MINOR);
	/* either out-pointer may be NULL */
	CHECK(datax_context_get_version(NULL, NULL, NULL) == 0);
	return 0;
}

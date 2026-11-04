// SPDX-License-Identifier: MIT
#ifndef DATAX_TEST_CHECK_H
#define DATAX_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
				__LINE__, #cond);                            \
			exit(1);                                             \
		}                                                            \
	} while (0)

#endif

// SPDX-License-Identifier: MIT
#ifndef DATAX_ERROR_H
#define DATAX_ERROR_H

#include "internal.h"

/* Record a formatted message and subsystem on ctx. NULL ctx is a no-op. */
void datax_error_set(struct datax_context *ctx, const char *subsystem,
		     const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* datax_error_set(), then return -err (err is a positive errno value). */
int datax_error_ret(struct datax_context *ctx, int err, const char *subsystem,
		    const char *fmt, ...) __attribute__((format(printf, 4, 5)));

#endif

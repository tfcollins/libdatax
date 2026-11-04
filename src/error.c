// SPDX-License-Identifier: MIT
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "error.h"

static void set_v(struct datax_context *ctx, const char *subsystem,
		  const char *fmt, va_list ap)
{
	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->lock);
	vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap);
	snprintf(ctx->last_subsystem, sizeof(ctx->last_subsystem), "%s",
		 subsystem ? subsystem : "");
	pthread_mutex_unlock(&ctx->lock);
}

void datax_error_set(struct datax_context *ctx, const char *subsystem,
		     const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	set_v(ctx, subsystem, fmt, ap);
	va_end(ap);
}

int datax_error_ret(struct datax_context *ctx, int err, const char *subsystem,
		    const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	set_v(ctx, subsystem, fmt, ap);
	va_end(ap);
	return -err;
}

const char *datax_context_get_last_error(const struct datax_context *ctx)
{
	return ctx ? ctx->last_error : "";
}

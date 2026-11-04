// SPDX-License-Identifier: MIT
#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "internal.h"
#include "check.h"

int main(void)
{
	struct datax_context ctx;
	struct datax_device dev;
	struct datax_stream *s = NULL;
	struct datax_sched *sched = NULL;
	unsigned long long f = 1;

	memset(&ctx, 0, sizeof(ctx));
	memset(&dev, 0, sizeof(dev));
	pthread_mutex_init(&ctx.lock, NULL);
	dev.ctx = &ctx;

	CHECK(datax_stream_create(&dev, NULL, &s) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(&ctx), "arrives in P2"));
	CHECK(datax_sched_create(&dev, &sched) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(&ctx), "arrives in P3"));
	CHECK(strstr(datax_context_get_last_error(&ctx), "sched_create"));
	CHECK(datax_device_stage_hop_table(&dev, &f, 1) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(&ctx), "stage_hop_table: arrives in P3"));
	CHECK(datax_device_set_time_next_pps(&dev, 0) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(&ctx), "set_time_next_pps: arrives in P3"));
	CHECK(datax_device_run_mcs(&dev) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(&ctx), "run_mcs: arrives in P3"));
	CHECK(strcmp(ctx.last_subsystem, "stub") == 0);

	pthread_mutex_destroy(&ctx.lock);
	return 0;
}

// SPDX-License-Identifier: MIT
#include <errno.h>
#include <datax/datax.h>

#include "check.h"

int main(void)
{
	struct datax_stream *s = NULL;
	struct datax_sched *sched = NULL;
	unsigned long long f = 1;

	CHECK(datax_stream_create(NULL, NULL, &s) == -ENOTSUP);
	CHECK(datax_sched_create(NULL, &sched) == -ENOTSUP);
	CHECK(datax_device_run_mcs(NULL) == -ENOTSUP);
	CHECK(datax_device_stage_hop_table(NULL, &f, 1) == -ENOTSUP);
	CHECK(datax_device_set_time_next_pps(NULL, 0) == -ENOTSUP);
	CHECK(datax_stream_start(NULL) == -ENOTSUP);
	CHECK(datax_stream_start_at(NULL, 5) == -ENOTSUP);
	CHECK(datax_stream_stop(NULL) == -ENOTSUP);
	CHECK(datax_stream_recv(NULL, NULL, 0, NULL, 0) == -ENOTSUP);
	CHECK(datax_stream_send(NULL, NULL, 0, 0) == -ENOTSUP);
	CHECK(datax_stream_get_block(NULL, NULL, NULL, 0) == -ENOTSUP);
	CHECK(datax_stream_put_block(NULL, NULL) == -ENOTSUP);
	CHECK(datax_sched_queue_gpio(NULL, 0, 0, 0) == -ENOTSUP);
	CHECK(datax_sched_queue_marker(NULL, 0, 0, 0) == -ENOTSUP);
	CHECK(datax_sched_queue_tune(NULL, 0, 0, 0) == -ENOTSUP);
	CHECK(datax_sched_queue_stream_gate(NULL, 0, NULL, false) == -ENOTSUP);
	CHECK(datax_sched_commit(NULL) == -ENOTSUP);
	CHECK(datax_sched_flush(NULL) == -ENOTSUP);
	CHECK(datax_sched_wait(NULL, 0, 0, NULL) == -ENOTSUP);
	CHECK(datax_sched_get_completions(NULL, NULL, 0, NULL) == -ENOTSUP);
	datax_stream_destroy(NULL);
	datax_sched_destroy(NULL);

	/* caps accessors tolerate NULL */
	{
		unsigned long long v;

		CHECK(!datax_caps_has(NULL, DATAX_CAP_MCS));
		CHECK(datax_caps_get_limit(NULL, DATAX_LIMIT_TICK_HZ, &v) == -EINVAL);
	}
	return 0;
}

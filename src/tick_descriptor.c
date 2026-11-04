// SPDX-License-Identifier: MIT
#include "tick_descriptor.h"

#include <string.h>

static void put_le(unsigned char *p, uint64_t v, unsigned int bytes)
{
	for (unsigned int i = 0; i < bytes; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

int datax_internal_tick_descriptor_pack(const struct datax_tick_descriptor *d,
					unsigned char out[48])
{
	if (!d || !out)
		return -1;
	memset(out, 0, DATAX_TICK_DESC_SIZE);
	put_le(out + 0, d->version, 2);
	put_le(out + 2, d->size_bytes, 2);
	put_le(out + 4, d->command_id, 2);
	put_le(out + 6, d->target_id, 2);
	put_le(out + 8, d->timestamp_ticks, 8);
	put_le(out + 16, d->flags, 4);
	put_le(out + 20, d->sequence_id, 4);
	for (unsigned int i = 0; i < 4; i++)
		put_le(out + 24 + 4 * i, d->args[i], 4);
	return 0;
}

struct tick_enum_entry {
	const char *name;
	long value;
};

#define DATAX_TICK_ENTRY(group, member, value) \
	{ "DATAX_TICK_" #group "_" #member, (long)(value) },

long datax_internal_tick_enum(const char *name)
{
	static const struct tick_enum_entry table[] = {
		DATAX_TICK_ENUM_LIST(DATAX_TICK_ENTRY)
	};

	if (!name)
		return -1;
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (strcmp(table[i].name, name) == 0)
			return table[i].value;
	return -1;
}

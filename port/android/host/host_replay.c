/*
HOST_REPLAY.C

A replay's records, kept in the host's memory for the guest to read a window
of at a time (port/linux/game/network_replay_playback.c). A long match's
replay inflates to more than the guest's memory, which lies below 4 GB, has
room for in one piece: a 46 MB file of 22 minutes inflated to 152 MB, which
the Switch's guest could not be given (libnx scatters its thread stacks and
its own mappings across that range). The host's heap has gigabytes.

The bytes are kept in chunks, so that no one allocation is large and growing
never copies what is already kept. One replay at a time, on the game's
thread alone; the guest's addresses are the host's (the guest's memory is
the host's to read and write).

Shared by the Android app's host, the Switch's (tools/switch_build.py) and
the PS5's (tools/ps5_build.py): it uses the C library alone.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE (4u * 1024 * 1024)

static struct
{
	unsigned char **chunks;
	uint32_t chunk_count;
	uint32_t chunk_room;
	uint32_t size;
} store;

/* the replay kept before let go (none kept: nothing done) */
void host_replay_store_free(void)
{
	uint32_t index;

	for (index = 0; index < store.chunk_count; index++)
		free(store.chunks[index]);
	free(store.chunks);
	memset(&store, 0, sizeof(store));
}

/* size bytes of the guest's at data added to the replay's end; 0 if the
host has not the memory for them (what was kept before is kept) */
int host_replay_store_append(uint32_t data, uint32_t size)
{
	const unsigned char *from = (const unsigned char *)(uintptr_t)data;

	if (size > UINT32_MAX - store.size)
		return 0;
	while (size)
	{
		uint32_t used = store.size % CHUNK_SIZE;
		uint32_t part;

		if (!used && store.size / CHUNK_SIZE == store.chunk_count)
		{
			unsigned char *chunk;

			if (store.chunk_count == store.chunk_room)
			{
				uint32_t room = store.chunk_room ? store.chunk_room * 2 : 64;
				unsigned char **chunks = realloc(store.chunks, room * sizeof(*chunks));

				if (!chunks)
					return 0;
				store.chunks = chunks;
				store.chunk_room = room;
			}
			if (!(chunk = malloc(CHUNK_SIZE)))
				return 0;
			store.chunks[store.chunk_count++] = chunk;
		}
		part = CHUNK_SIZE - used;
		if (part > size)
			part = size;
		memcpy(store.chunks[store.size / CHUNK_SIZE] + used, from, part);
		from += part;
		size -= part;
		store.size += part;
	}
	return 1;
}

/* up to size bytes of the replay from offset copied to the guest's
destination; how many there were */
uint32_t host_replay_store_read(uint32_t offset, uint32_t destination, uint32_t size)
{
	unsigned char *to = (unsigned char *)(uintptr_t)destination;
	uint32_t copied = 0;

	if (offset >= store.size)
		return 0;
	if (size > store.size - offset)
		size = store.size - offset;
	while (copied < size)
	{
		uint32_t at = offset + copied;
		uint32_t part = CHUNK_SIZE - at % CHUNK_SIZE;

		if (part > size - copied)
			part = size - copied;
		memcpy(to + copied, store.chunks[at / CHUNK_SIZE] + at % CHUNK_SIZE, part);
		copied += part;
	}
	return copied;
}

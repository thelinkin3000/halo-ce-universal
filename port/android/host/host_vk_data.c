/*
HOST_VK_DATA.C

The Vulkan renderer's host half, phase 3 (port/android/VULKAN.md): the upload rings
and the frame's data table. What a draw reads was copied by the guest into the command
stream at the draw (vk_data_put in d3d8_vk.c); here the host copies it out of the
stream, during the hand-over, into host-visible memory the GPU reads, and keeps where
each piece (an id) went, so that later commands can name it. The game's own memory is
never read here: only the bytes in the stream.

One ring for each frame in flight: a list of buffers, filled front to back and never
moved once something is placed in them (commands already recorded name them), reset
when the frame's fence has passed. Nothing here waits on the GPU for a resource: the
only wait is for the frame's fence, which the frame's command buffer needs as well.
*/

#include "host.h"
#include "host_vk.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define B host_vkb

/* a ring's buffers: 16 MB to start; a single piece of data larger than that gets a buffer of its own size */
#define RING_BUFFER_SIZE (16u * 1024 * 1024)

struct ring_buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	unsigned char *mapped;
	VkDeviceSize size, used;
};

struct host_vk_ring
{
	struct ring_buffer *buffers;
	unsigned count, capacity;
};

/* a piece of data of the frame, by id - 1 */
struct entry
{
	VkBuffer buffer; /* VK_NULL_HANDLE: it could not be placed (said) */
	VkDeviceSize offset;
	unsigned char *mapped;
	uint32_t total, received;
};

static struct
{
	struct entry *entries;
	unsigned count, capacity;
} table;
/* the frames whose tables were let go (VK_COMMAND_DATA_PATCH's copy is of one frame's) */
static unsigned long data_frames;

int host_vk_buffer_make(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, void **mapped)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements requirements;
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type;

	*buffer = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!HOST_VK_CHECK(vkCreateBuffer(B.device, &info, NULL, buffer)))
		return 0;
	vkGetBufferMemoryRequirements(B.device, *buffer, &requirements);
	type = host_vk_memory_type(requirements.memoryTypeBits, wanted);
	if (type == UINT32_MAX || (B.memory.memoryTypes[type].propertyFlags & wanted) != wanted)
	{
		host_logf(HOST_LOG_ERROR, "vk: no host-visible, coherent memory for a buffer of %llu bytes", (unsigned long long)size);
		goto fail;
	}
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = type;
	if (!HOST_VK_CHECK(vkAllocateMemory(B.device, &allocation, NULL, memory)) ||
		!HOST_VK_CHECK(vkBindBufferMemory(B.device, *buffer, *memory, 0)) ||
		!HOST_VK_CHECK(vkMapMemory(B.device, *memory, 0, VK_WHOLE_SIZE, 0, mapped)))
		goto fail;
	return 1;
fail:
	if (*memory)
		vkFreeMemory(B.device, *memory, NULL);
	vkDestroyBuffer(B.device, *buffer, NULL);
	*buffer = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;
	return 0;
}

static VkDeviceSize alignment(void)
{
	VkDeviceSize align = host_vk.properties.limits.minUniformBufferOffsetAlignment;

	return align > 256 ? align : 256;
}

static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize align)
{
	return (value + align - 1) / align * align;
}

/* a place for size bytes in the ring, at an offset aligned for a uniform block; NULL if none can be made */
static struct ring_buffer *ring_place(struct host_vk_ring *ring, VkDeviceSize size, VkDeviceSize *offset)
{
	struct ring_buffer *place;
	unsigned index;
	VkDeviceSize buffer_size = RING_BUFFER_SIZE;
	void *mapped;

	for (index = 0; index < ring->count; index++)
	{
		VkDeviceSize at = align_up(ring->buffers[index].used, alignment());

		if (at + size <= ring->buffers[index].size)
		{
			*offset = at;
			ring->buffers[index].used = at + size;
			return &ring->buffers[index];
		}
	}
	/* another buffer, kept for the next time; none of what is placed moves */
	if (ring->count == ring->capacity)
	{
		unsigned capacity = ring->capacity ? ring->capacity * 2 : 4;
		struct ring_buffer *buffers = realloc(ring->buffers, sizeof(*buffers) * capacity);

		if (!buffers)
			return NULL;
		ring->buffers = buffers;
		ring->capacity = capacity;
	}
	if (size > buffer_size)
		buffer_size = size;
	place = &ring->buffers[ring->count];
	memset(place, 0, sizeof(*place));
	if (!host_vk_buffer_make(buffer_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &place->buffer, &place->memory, &mapped))
		return NULL;
	place->mapped = mapped;
	place->size = buffer_size;
	place->used = size;
	*offset = 0;
	ring->count++;
	return place;
}

/* the frame's ring and table are ready: once its fence has passed (the frame's command buffer waits for it),
everything placed in the ring is reset */
static struct host_vk_ring *ring_ready(void)
{
	struct host_vk_frame *frame = &B.frames[B.frame];
	unsigned index;

	if (!frame->ring)
		frame->ring = calloc(1, sizeof(*frame->ring));
	if (!frame->ring)
		return NULL;
	if (!frame->ring_valid)
	{
		host_vk_frame_command();
		for (index = 0; index < frame->ring->count; index++)
			frame->ring->buffers[index].used = 0;
		table.count = 0;
		frame->ring_valid = 1;
	}
	return frame->ring;
}

unsigned char *host_vk_data_place(VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset)
{
	struct host_vk_ring *ring = ring_ready();
	struct ring_buffer *place = ring ? ring_place(ring, size, offset) : NULL;

	if (!place)
		return NULL;
	*buffer = place->buffer;
	return place->mapped + *offset;
}

void host_vk_data_frame_end(void)
{
	B.frames[B.frame].ring_valid = 0;
	table.count = 0;
	data_frames++;
}

unsigned host_vk_data_buffers(void)
{
	unsigned index, count = 0;

	for (index = 0; index < HOST_VK_FRAMES; index++)
	{
		if (B.frames[index].ring)
			count += B.frames[index].ring->count;
	}
	return count;
}

static int bad_record(const char *what, const struct vk_command_data *command)
{
	static int said;

	if (!said)
		host_logf(HOST_LOG_ERROR, "vk: a data record is bad (%s: id %u, part at %u of %u bytes, %u in all; %u ids so far); "
			"the rest of the hand-over is dropped", what, (unsigned)command->id, (unsigned)command->part_offset,
			(unsigned)command->part_size, (unsigned)command->total_size, table.count);
	said = 1;
	return 0;
}

int host_vk_data_command(const struct vk_command_data *command, uint32_t size)
{
	struct host_vk_ring *ring;
	struct entry *entry;

	if (size < sizeof(*command) || ((uint64_t)command->part_size + 3 & ~3ull) + sizeof(*command) > size)
		return bad_record("the record is shorter than its part", command);
	if ((uint64_t)command->part_offset + command->part_size > command->total_size)
		return bad_record("the part is past the total", command);
	ring = ring_ready();
	if (!ring)
		return 1;
	if (command->part_offset == 0)
	{
		/* a new id: the next one, and the one before it complete */
		if (command->id != table.count + 1)
			return bad_record("an id reused or out of order", command);
		if (table.count && table.entries[table.count - 1].received != table.entries[table.count - 1].total)
			return bad_record("an id began before the one before it was complete", command);
		if (table.count == table.capacity)
		{
			unsigned capacity = table.capacity ? table.capacity * 2 : 256;
			struct entry *entries = realloc(table.entries, sizeof(*entries) * capacity);

			if (!entries)
				return 1;
			table.entries = entries;
			table.capacity = capacity;
		}
		entry = &table.entries[table.count++];
		memset(entry, 0, sizeof(*entry));
		entry->total = command->total_size;
		{
			struct ring_buffer *place = ring_place(ring, command->total_size ? command->total_size : 1, &entry->offset);

			if (place)
			{
				entry->buffer = place->buffer;
				entry->mapped = place->mapped + entry->offset;
			}
		}
	}
	else
	{
		if (!table.count || command->id != table.count)
			return bad_record("a part of an id that is not the latest", command);
		entry = &table.entries[table.count - 1];
		if (command->part_offset != entry->received)
			return bad_record("a part out of order", command);
	}
	if (entry->mapped && command->part_size)
		memcpy(entry->mapped + command->part_offset, command->payload, command->part_size);
	entry->received += command->part_size;
	B.counts.data_records++;
	B.counts.data_bytes += command->part_size;
	return 1;
}

/* ---------- static buffers

The game's geometry written once as a map loads (an Xbox map's models in its tags, its BSP's vertices and indices; the
ranges xgpu_static_buffers_add names, as the OpenGL renderer's static buffers), each copied once into a host-visible
buffer of its own when a draw first reads it (host_vk_static_make, on the game's thread, during the call: the guest's
memory is read then and never later), which draws name instead of putting the bytes again. Made on the game's thread
and let go on the render thread (VK_COMMAND_STATIC_FREE, after the draws that name it): a slot goes back to the game's
thread for another only once its buffer is destroyed, after the frames that may use it. */

#define STATIC_LIMIT 4096

static struct
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t size;
	/* the game's thread may give it out (0), or it is in use or being let go */
	int used;
} statics[STATIC_LIMIT];
static pthread_mutex_t statics_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long static_bytes;

/* the slots let go in each frame slot, destroyed once its fence has passed */
static uint32_t *static_graveyard[HOST_VK_FRAMES];
static unsigned static_graveyard_count[HOST_VK_FRAMES], static_graveyard_capacity[HOST_VK_FRAMES];

uint32_t host_vk_static_make(uint32_t address, uint32_t size)
{
	static int said_full;
	uint32_t id;
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;

	if (!size || !host_vk_backend_ensure())
		return 0;
	pthread_mutex_lock(&statics_lock);
	for (id = 1; id < STATIC_LIMIT && statics[id].used; id++)
		;
	if (id < STATIC_LIMIT)
		statics[id].used = 1;
	pthread_mutex_unlock(&statics_lock);
	if (id >= STATIC_LIMIT)
	{
		if (!said_full)
			host_logf(HOST_LOG_WARN, "vk: %u static buffers: the rest are put at each draw", (unsigned)STATIC_LIMIT);
		said_full = 1;
		return 0;
	}
	if (!host_vk_buffer_make(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &buffer, &memory,
		&mapped))
	{
		pthread_mutex_lock(&statics_lock);
		statics[id].used = 0;
		pthread_mutex_unlock(&statics_lock);
		return 0;
	}
	memcpy(mapped, (const void *)(uintptr_t)address, size);
	statics[id].buffer = buffer;
	statics[id].memory = memory;
	statics[id].size = size;
	pthread_mutex_lock(&statics_lock);
	static_bytes += size;
	pthread_mutex_unlock(&statics_lock);
	host_logf(HOST_LOG_INFO, "vk: static buffer %u: %u KB at 0x%08x (%lu KB in all)", (unsigned)id, (unsigned)(size / 1024),
		(unsigned)address, static_bytes / 1024);
	return id | VK_DATA_STATIC;
}

void host_vk_static_free_command(const struct vk_command_static_free *command)
{
	uint32_t id = command->id & ~VK_DATA_STATIC;
	unsigned slot;

	if (!(command->id & VK_DATA_STATIC) || id < 1 || id >= STATIC_LIMIT || !statics[id].buffer)
		return;
	/* (the graveyard of a frame being recorded: between frames - a map's load, as its ranges are let go - B.frame is
	the next frame's slot, whose graveyard goes as its recording begins, under the frame before, which may still be
	drawing with it: the validation layer's VUID-vkDestroyBuffer-buffer-00922) */
	host_vk_frame_command();
	slot = (unsigned)B.frame;
	if (static_graveyard_count[slot] == static_graveyard_capacity[slot])
	{
		unsigned capacity = static_graveyard_capacity[slot] ? static_graveyard_capacity[slot] * 2 : 64;
		uint32_t *grown = realloc(static_graveyard[slot], capacity * sizeof(*grown));

		if (!grown)
			return; /* (kept: a leak rather than a buffer destroyed under the GPU) */
		static_graveyard[slot] = grown;
		static_graveyard_capacity[slot] = capacity;
	}
	static_graveyard[slot][static_graveyard_count[slot]++] = id;
}

void host_vk_static_frame_reset(unsigned slot)
{
	unsigned index;

	for (index = 0; index < static_graveyard_count[slot]; index++)
	{
		uint32_t id = static_graveyard[slot][index];

		vkDestroyBuffer(B.device, statics[id].buffer, NULL);
		vkFreeMemory(B.device, statics[id].memory, NULL);
		pthread_mutex_lock(&statics_lock);
		static_bytes -= statics[id].size;
		statics[id].buffer = VK_NULL_HANDLE;
		statics[id].memory = VK_NULL_HANDLE;
		statics[id].size = 0;
		statics[id].used = 0;
		pthread_mutex_unlock(&statics_lock);
	}
	static_graveyard_count[slot] = 0;
}

/* the bytes of the latest patched data, in the host's own memory: a patch's base is nearly always the one before, and
the ring's memory, which the GPU reads, may be slow for the CPU to read back */
static struct
{
	uint32_t id;
	uint32_t size;
	unsigned char *bytes;
	/* the frame's table it belongs to (ids start again at each frame) */
	unsigned long frame;
} patched;

int host_vk_data_patch_command(const struct vk_command_data_patch *command, uint32_t size)
{
	struct host_vk_ring *ring;
	struct entry *entry;
	const struct entry *base;
	struct ring_buffer *place;
	uint32_t index;

	if (size < sizeof(*command) || (uint64_t)command->count * sizeof(command->patches[0]) + sizeof(*command) > size)
		return bad_record("the patch is shorter than its pieces", (const struct vk_command_data *)command);
	ring = ring_ready();
	if (!ring)
		return 1;
	if (command->id != table.count + 1 || command->base < 1 || command->base > table.count)
		return bad_record("a patch's id out of order, or its base unknown", (const struct vk_command_data *)command);
	if (table.entries[table.count - 1].received != table.entries[table.count - 1].total)
		return bad_record("a patch began before the id before it was complete", (const struct vk_command_data *)command);
	base = &table.entries[command->base - 1];
	if (table.count == table.capacity)
	{
		unsigned capacity = table.capacity ? table.capacity * 2 : 256;
		struct entry *entries = realloc(table.entries, sizeof(*entries) * capacity);

		if (!entries)
			return 1;
		table.entries = entries;
		table.capacity = capacity;
		base = &table.entries[command->base - 1];
	}
	entry = &table.entries[table.count++];
	memset(entry, 0, sizeof(*entry));
	entry->total = base->total;
	entry->received = base->total;
	if (!base->mapped || base->received != base->total)
		return 1;
	/* the base's bytes, from the host's copy where it has them */
	if (!(patched.frame == data_frames && patched.id == command->base && patched.size == base->total))
	{
		if (patched.size < base->total)
		{
			unsigned char *grown = realloc(patched.bytes, base->total);

			if (!grown)
				return 1;
			patched.bytes = grown;
		}
		memcpy(patched.bytes, base->mapped, base->total);
	}
	for (index = 0; index < command->count; index++)
	{
		const struct vk_data_patch *patch = &command->patches[index];

		if ((uint64_t)patch->offset + sizeof(patch->value) <= base->total)
			memcpy(patched.bytes + patch->offset, patch->value, sizeof(patch->value));
	}
	patched.id = command->id;
	patched.size = base->total;
	patched.frame = data_frames;
	place = ring_place(ring, entry->total ? entry->total : 1, &entry->offset);
	if (!place)
		return 1;
	entry->buffer = place->buffer;
	entry->mapped = place->mapped + entry->offset;
	memcpy(entry->mapped, patched.bytes, entry->total);
	B.counts.data_records++;
	B.counts.data_bytes += command->count * sizeof(command->patches[0]);
	return 1;
}

int host_vk_data_find(uint32_t id, uint32_t offset, uint32_t size, VkBuffer *buffer, VkDeviceSize *buffer_offset)
{
	static int said;
	const struct entry *entry = id >= 1 && id <= table.count ? &table.entries[id - 1] : NULL;

	if (id & VK_DATA_STATIC)
	{
		uint32_t index = id & ~VK_DATA_STATIC;

		if (index >= 1 && index < STATIC_LIMIT && statics[index].buffer && (uint64_t)offset + size <= statics[index].size)
		{
			*buffer = statics[index].buffer;
			*buffer_offset = offset;
			return 1;
		}
		if (!said)
			host_logf(HOST_LOG_ERROR, "vk: a command names %u bytes at %u of static buffer %u, which is %s; the command is "
				"skipped", (unsigned)size, (unsigned)offset, (unsigned)index, index >= 1 && index < STATIC_LIMIT &&
				statics[index].buffer ? "too short" : "not known");
		said = 1;
		return 0;
	}

	if (!entry || !entry->buffer || entry->received != entry->total || (uint64_t)offset + size > entry->total)
	{
		if (!said)
			host_logf(HOST_LOG_ERROR, "vk: a command names %u bytes at %u of data %u, which is %s; the command is skipped",
				(unsigned)size, (unsigned)offset, (unsigned)id, !entry ? "not known in this frame" :
				!entry->buffer ? "not placed" : entry->received != entry->total ? "not all there" : "too short");
		said = 1;
		return 0;
	}
	*buffer = entry->buffer;
	*buffer_offset = entry->offset + offset;
	return 1;
}

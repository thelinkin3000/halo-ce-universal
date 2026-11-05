/*
MENU_CHIEF.C

The Multiplayer Color and Select Profile screens' Spartan, drawn from the model the game plays
rather than a flat picture. ui.map has none of him: his model, its shaders and
their bitmaps are read from a multiplayer map (wizard.map, the smallest) on a
thread of their own while ui.map is loaded, and added to its tag table the
first time he is drawn after that, as the menus' are (menu_tags.c): the menus
do not wait for him. A map's tags name each other by addresses and by indices
of its own: each kept tag's are moved as its schema (tag_schema.h) says, and
any tag of another group is none. The vertices and indices lie in the tags and
are given buffers of their own, on the game's thread; the pixels lie outside
them, in the file, and are kept for the texture cache
(menu_chief_bitmap_read).

All of it goes with ui.map's tags. Until it has been read, and without
wizard.map or with one this cannot read, the screen keeps its picture.

On the Multiplayer Color screen he answers to the controller
(chief_controls): the right stick turns him, the north button changes what he
holds, and left alone for CONTROLS_REST_MILLISECONDS he walks for that long and
stands for that long in turn, until they are used again.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include <xtl.h>
#include "bungie_net/common/thread.h"
#include "bitmaps/bitmap_group.h"
#include "math/real_math.h"
#include "models/model_animation_definitions.h"
#include "models/model_definitions.h"
#include "objects/objects.h"
#include "models/models.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/xbox/rasterizer_xbox_internal.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "render/render_cameras_internal.h"
#include "cache/cache_files.h"
#include "input/input.h"
#include "memory/zlib/zlib.h"
#include "tag_schema.h"
#include "menu_chief.h"
#include "halo_overlay_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, not the game's tracked one (cseries.h), which
is not to be called from another thread: the map is read on one, and what it
allocates is freed on the game's) */
#undef malloc
#undef free

void platform_log(char const *format, ...);
void rasterizer_port_set_viewport(rectangle2d const *bounds);
void halo_screen_ui_offset(unsigned char centered);
void platform_translate_path(char const *xbox_path, char *host_path, unsigned long host_path_size);
real_rgb_color player_profile_get_rgb_color(long color_index);
extern D3DDevice *global_d3d_device;

/* ---------- constants */

#define SOURCE_MAP "wizard.map"
#define CHIEF_MODEL "characters\\cyborg\\cyborg"
/* where a map's tags were written to be, whichever map */
#define TAGS_LINKED_ADDRESS 0x803A6000UL
#define MAP_HEADER_BYTES 2048
#define CHIEF_ANIMATIONS "characters\\cyborg\\cyborg"
#define MAXIMUM_KEPT_TAGS 64
#define MAXIMUM_KEPT_PICTURES 128
#define HELD_MILLISECONDS 10000UL
#define BLEND_MILLISECONDS 400UL
#define ANIMATION_FRAMES_PER_SECOND 30UL
#define D3D_BUFFER_DATA_OFFSET 4
/* (models.c's _render_model_menu_bit) */
#define RENDER_MODEL_MENU_FLAG (1UL << 4)
#define VIEW_VERTICAL_ANGLE 0.42f
#define VIEW_ANGLE_AROUND 0.08f
/* his controls: how far the right stick must go, how fast it turns him at its
furthest, how long each of walking and standing lasts left alone, and how long
he is not drawn for before he is taken to be on a new screen */
#define CONTROLS_DEAD_ZONE 0.2f
#define CONTROLS_TURN_RADIANS_PER_SECOND 3.0f
#define CONTROLS_REST_MILLISECONDS 5000UL
#define CONTROLS_AWAY_MILLISECONDS 500UL
/* (the picture he stands for is narrower than the panel it is in, and than
what he holds: he is drawn this much past it on each side, inside the panel) */
#define VIEW_SIDE_MARGIN 36

/* what he holds, in turn: its model, and the weapon class of his animations
that holds it (the weapon tag's label) */
static struct
{
	char const *model;
	char const *weapon_class;
} const held_weapons[] =
{
	{ "weapons\\assault rifle\\assault rifle", "rifle" },
	{ "weapons\\plasma rifle\\plasma rifle", "rifle" },
	{ "weapons\\ball\\ball", "pistol" },
};
#define HELD_WEAPON_COUNT ((long)(sizeof(held_weapons) / sizeof(held_weapons[0])))

/* ---------- structures */

struct cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct tags_header
{
	struct cache_file_tag_instance *tag_instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
};

struct map_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	byte reservedC[4];
	long tag_data_offset;
	long tag_data_size;
};

/* (models.c's) */
struct model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

typedef char verify_menu_chief_part_size[sizeof(struct model_geometry_part) == 0x68 ? 1 : -1];

struct kept_model
{
	long tag_index;
	struct model *model;
	/* each node, where the model's default pose has it */
	real_matrix4x3 nodes[MAXIMUM_NODES_PER_MODEL];
	long node_count;
};

/* the animations of a weapon class */
struct held_stance
{
	struct animation_graph_weapon_class const *weapon_class;
	struct animation const *idle;
	/* (none in the pistol class's: it is blended to) */
	struct animation const *ready;
	/* (none, and he stands) */
	struct animation const *walk;
};

struct kept_picture
{
	struct bitmap_data const *bitmap;
	void *pixels;
	long size;
};

/* ---------- globals */

static struct
{
	/* the thread reading the map, until it has been waited for; whether it
	read him, once it has exited */
	struct thread_reference *thread;
	volatile long read;
	/* the map being read */
	byte *file;
	long file_size;
	struct cache_file_tag_instance const *source_instances;
	long source_count;
	long first_index;
	long first_salt;
	boolean failed;
	/* kept to the end */
	byte *tags;
	unsigned long tags_size;
	long kept_sources[MAXIMUM_KEPT_TAGS];
	long kept_count;
	struct cache_file_tag_instance *instances;
	void *menu_instances;
	struct kept_picture pictures[MAXIMUM_KEPT_PICTURES];
	long picture_count;
	struct kept_model body;
	struct kept_model weapons[HELD_WEAPON_COUNT];
	long animations_index;
	struct held_stance stances[HELD_WEAPON_COUNT];
	/* what he holds, since when, and the pose he had then */
	long held;
	unsigned long held_since;
	struct real_orientation pose[MAXIMUM_NODES_PER_ANIMATION];
	struct real_orientation blended_from[MAXIMUM_NODES_PER_ANIMATION];
	unsigned long blended_since;
	boolean blending;
	boolean readying;
	/* his controls: whether they are heard, how far they have turned him,
	when they were last used, when he was last drawn, whether the north button
	is down, and whether he is walking and since when */
	boolean controlled;
	real turned;
	unsigned long touched_at;
	unsigned long drawn_at;
	boolean north_down;
	boolean walking;
	unsigned long walking_since;
	real_point3d centre;
	real height;
	boolean ready;
} chief;

/* ---------- moving a map's tags */

static void rgb_set(real_rgb_color *color, real red, real green, real blue)
{
	color->red = red;
	color->green = green;
	color->blue = blue;
}

static void *moved(void const *address, unsigned long size)
{
	unsigned long linked = (unsigned long)address;

	if (linked >= (unsigned long)chief.tags && linked - (unsigned long)chief.tags < chief.tags_size)
		return (void *)address;
	if (linked < TAGS_LINKED_ADDRESS || linked - TAGS_LINKED_ADDRESS > chief.tags_size ||
		size > chief.tags_size - (linked - TAGS_LINKED_ADDRESS))
	{
		return NULL;
	}
	return chief.tags + (linked - TAGS_LINKED_ADDRESS);
}

static struct tag_schema_group const *schema_of(long group_tag)
{
	struct tag_schema_group const *const *list;

	for (list = tag_schema_group_lists; *list; list++)
	{
		struct tag_schema_group const *group;

		for (group = *list; group->group_tag; group++)
		{
			if ((long)group->group_tag == group_tag)
				return group;
		}
	}
	return NULL;
}

static boolean group_kept(long group_tag)
{
	return group_tag == 'mode' || group_tag == 'antr' || group_tag == 'bitm' || group_tag == 'shdr';
}

static void fields_move(byte *base, struct tag_schema_definition const *definition);

static long tag_keep(long source_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(source_index);
	struct cache_file_tag_instance const *source;
	struct tag_schema_group const *group;
	byte *data;
	long kept;

	if (source_index == NONE || absolute >= chief.source_count)
		return NONE;
	source = &chief.source_instances[absolute];
	if (source->tag_index != source_index ||
		(!group_kept(source->group_tag) && !group_kept(source->parent_group_tags[0]) &&
			!group_kept(source->parent_group_tags[1])))
	{
		return NONE;
	}
	for (kept = 0; kept < chief.kept_count; kept++)
	{
		if (chief.kept_sources[kept] == absolute)
			return ((chief.first_salt + kept) << 16) | (chief.first_index + kept);
	}
	group = schema_of(source->group_tag);
	data = moved(source->base_address, group ? (unsigned long)group->definition->size : 0);
	if (!group || !data || chief.kept_count == MAXIMUM_KEPT_TAGS)
	{
		platform_log("menu chief: tag %ld (group %08lx) cannot be kept", absolute, (unsigned long)source->group_tag);
		chief.failed = TRUE;
		return NONE;
	}
	kept = chief.kept_count++;
	chief.kept_sources[kept] = absolute;
	fields_move(data, group->definition);
	return ((chief.first_salt + kept) << 16) | (chief.first_index + kept);
}

static void field_move(byte *place, struct tag_schema_field const *field)
{
	switch (field->type)
	{
	case _tag_schema_block:
	{
		struct tag_schema_definition const *definition = field->definition;
		struct tag_block *block = (struct tag_block *)place;
		long index;

		block->definition = NULL;
		if (block->count <= 0)
		{
			block->count = 0;
			block->address = NULL;
			break;
		}
		block->address = moved(block->address, (unsigned long)block->count * (unsigned long)definition->size);
		if (!block->address)
		{
			platform_log("menu chief: a block (%s) is not in the tags", field->name);
			block->count = 0;
			chief.failed = TRUE;
			break;
		}
		for (index = 0; index < block->count; index++)
			fields_move((byte *)block->address + index * definition->size, definition);
		break;
	}
	case _tag_schema_data:
	{
		struct tag_data *data = (struct tag_data *)place;

		data->definition = NULL;
		data->address = data->size > 0 ? moved(data->address, (unsigned long)data->size) : NULL;
		if (!data->address)
			data->size = 0;
		break;
	}
	case _tag_schema_file_data:
	{
		struct tag_data *data = (struct tag_data *)place;

		data->definition = NULL;
		data->address = NULL;
		break;
	}
	case _tag_schema_reference:
	{
		struct tag_reference *reference = (struct tag_reference *)place;

		reference->name = "";
		reference->name_length = 0;
		reference->index = tag_keep(reference->index);
		if (reference->index == NONE)
			reference->group_tag = NONE;
		break;
	}
	case _tag_schema_tag_index:
		*(long *)place = tag_keep(*(long *)place);
		break;
	case _tag_schema_reset:
		if (field->size > 4)
			memset(place, 0, field->size);
		else
			memcpy(place, &field->target_offset, field->size);
		break;
	case _tag_schema_struct:
		fields_move(place, field->definition);
		break;
	default:
		break;
	}
}

static void fields_move(byte *base, struct tag_schema_definition const *definition)
{
	struct tag_schema_field const *field;

	for (field = definition->fields; field->type != _tag_schema_terminator && !chief.failed; field++)
	{
		long index;

		for (index = 0; index < field->count; index++)
			field_move(base + field->offset + index * field->size, field);
	}
}

/* ---------- what lies outside a tag's own structures */

static boolean part_buffers_make(struct model_geometry_part *part)
{
	struct vertex_buffer *vertices = &part->vertex_buffer;
	struct triangle_buffer *triangles = &part->triangle_buffer;
	long vertex_size = rasterizer_geometry_get_vertex_size(vertices->type);
	long index_count = triangles->type == _triangle_buffer_type_precompiled_strip ?
		triangles->count + 2 : 3 * triangles->count;
	byte *vertex_d3d = moved(vertices->hardware_format, D3D_BUFFER_DATA_OFFSET + 4);
	void *vertex_data = vertex_d3d ?
		moved(*(void **)(vertex_d3d + D3D_BUFFER_DATA_OFFSET), (unsigned long)vertices->count * (unsigned long)vertex_size) :
		NULL;
	void *index_data = moved(triangles->base_address, (unsigned long)index_count * sizeof(word));
	long vertex_type = vertices->type;
	short triangle_type = triangles->type;
	long vertex_count = vertices->count;
	long triangle_count = triangles->count;

	memset(vertices, 0, sizeof(*vertices));
	memset(triangles, 0, sizeof(*triangles));
	if (!vertex_data || !index_data)
		return FALSE;
	return rasterizer_vertex_buffer_new(vertices, vertex_type, vertex_count, vertex_data, vertex_count * vertex_size) &&
		rasterizer_triangle_buffer_new(triangles, triangle_type, triangle_count, index_data);
}

static boolean geometry_make(struct model *model)
{
	long geometry_index;

	for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
	{
		struct model_geometry *geometry = (struct model_geometry *)model->geometries.address + geometry_index;
		long part_index;

		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			if (!part_buffers_make((struct model_geometry_part *)geometry->parts.address + part_index))
			{
				platform_log("menu chief: geometry %ld's part %ld has no buffers", geometry_index, part_index);
				return FALSE;
			}
		}
	}
	return TRUE;
}

static void geometry_delete(struct model *model)
{
	long geometry_index;

	for (geometry_index = 0; geometry_index < model->geometries.count; geometry_index++)
	{
		struct model_geometry *geometry = (struct model_geometry *)model->geometries.address + geometry_index;
		long part_index;

		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct model_geometry_part *part = (struct model_geometry_part *)geometry->parts.address + part_index;

			if (part->vertex_buffer.hardware_format)
				rasterizer_vertex_buffer_delete(&part->vertex_buffer);
			if (part->triangle_buffer.hardware_format)
				rasterizer_triangle_buffer_delete(&part->triangle_buffer);
		}
	}
}

static boolean pictures_keep(struct bitmap_group *group, long tag_index)
{
	long index;

	for (index = 0; index < group->bitmaps.count; index++)
	{
		struct bitmap_data *bitmap = (struct bitmap_data *)group->bitmaps.address + index;
		struct kept_picture *picture;

		bitmap->tag_index = tag_index;
		bitmap->cache_block_index = NONE;
		bitmap->hardware_format = NULL;
		bitmap->base_address = NULL;
		if (bitmap->pixels_size == 0)
			continue;
		if (chief.picture_count == MAXIMUM_KEPT_PICTURES || bitmap->pixels_offset < 0 || bitmap->pixels_size < 0 ||
			bitmap->pixels_size > chief.file_size - bitmap->pixels_offset)
		{
			return FALSE;
		}
		picture = &chief.pictures[chief.picture_count];
		picture->pixels = malloc(bitmap->pixels_size);
		if (!picture->pixels)
			return FALSE;
		memcpy(picture->pixels, chief.file + bitmap->pixels_offset, bitmap->pixels_size);
		picture->bitmap = bitmap;
		picture->size = bitmap->pixels_size;
		chief.picture_count++;
	}
	return TRUE;
}

boolean menu_chief_bitmap_read(struct bitmap_data const *bitmap, void *buffer)
{
	long index;

	if (!chief.ready)
		return FALSE;
	for (index = 0; index < chief.picture_count; index++)
	{
		if (chief.pictures[index].bitmap == bitmap)
		{
			memcpy(buffer, chief.pictures[index].pixels, chief.pictures[index].size);
			return TRUE;
		}
	}
	return FALSE;
}

/* ---------- the map */

/* zlib's own allocator is the game's tracked one too (zutil.c's zcalloc) */
static voidpf inflate_allocate(voidpf opaque, uInt items, uInt size)
{
	void *memory = malloc((size_t)items * size);

	(void)opaque;
	if (memory)
		memset(memory, 0, (size_t)items * size);
	return memory;
}

static void inflate_free(voidpf opaque, voidpf memory)
{
	(void)opaque;
	free(memory);
}

static boolean map_inflate(byte *to, unsigned long to_size, byte *from, unsigned long from_size)
{
	z_stream stream;
	boolean inflated = FALSE;

	memset(&stream, 0, sizeof(stream));
	stream.next_in = from;
	stream.avail_in = from_size;
	stream.next_out = to;
	stream.avail_out = to_size;
	stream.zalloc = inflate_allocate;
	stream.zfree = inflate_free;
	if (inflateInit(&stream) == Z_OK)
	{
		inflated = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == to_size;
		inflateEnd(&stream);
	}
	return inflated;
}

static byte *map_read(long *size)
{
	char path[256];
	struct map_header header;
	FILE *stream;
	byte *packed = NULL;
	byte *file = NULL;
	long packed_size;

	snprintf(path, sizeof(path), "%s%s", cache_files_map_directory(), SOURCE_MAP);
	stream = fopen(path, "rb");
	/* (a build whose fopen takes only the host's paths) */
	if (!stream)
	{
		char host_path[512];

		platform_translate_path(path, host_path, sizeof(host_path));
		stream = fopen(host_path, "rb");
	}
	if (!stream)
	{
		platform_log("menu chief: no %s", path);
		return NULL;
	}
	fseek(stream, 0, SEEK_END);
	packed_size = ftell(stream);
	fseek(stream, 0, SEEK_SET);
	if (packed_size > MAP_HEADER_BYTES && (packed = malloc(packed_size)) != NULL &&
		fread(packed, 1, packed_size, stream) == (size_t)packed_size)
	{
		memcpy(&header, packed, sizeof(header));
		if (header.file_length == packed_size)
		{
			fclose(stream);
			*size = packed_size;
			return packed;
		}
		if (header.file_length > MAP_HEADER_BYTES && (file = malloc(header.file_length)) != NULL)
		{
			memcpy(file, packed, MAP_HEADER_BYTES);
			if (!map_inflate(file + MAP_HEADER_BYTES, header.file_length - MAP_HEADER_BYTES,
					packed + MAP_HEADER_BYTES, packed_size - MAP_HEADER_BYTES))
			{
				free(file);
				file = NULL;
			}
			else
			{
				*size = header.file_length;
			}
		}
	}
	fclose(stream);
	if (packed)
		free(packed);
	return file;
}

static long source_find(long group_tag, char const *wanted)
{
	long index;

	for (index = 0; index < chief.source_count; index++)
	{
		char const *name = moved(chief.source_instances[index].name, 1);

		if (chief.source_instances[index].group_tag == group_tag && name && !strcmp(name, wanted))
			return chief.source_instances[index].tag_index;
	}
	platform_log("menu chief: %s has no %s", SOURCE_MAP, wanted);
	return NONE;
}

/* a kept tag's data, by the index it was kept under */
static void *kept_data(long tag_index, unsigned long size)
{
	long kept = DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) - chief.first_index;

	return moved(chief.source_instances[chief.kept_sources[kept]].base_address, size);
}

static boolean model_keep(struct kept_model *kept, char const *name)
{
	long index;

	kept->tag_index = tag_keep(source_find('mode', name));
	if (chief.failed || kept->tag_index == NONE)
		return FALSE;
	kept->model = kept_data(kept->tag_index, sizeof(struct model));
	if (!kept->model)
		return FALSE;
	kept->node_count = MIN(kept->model->nodes.count, MAXIMUM_NODES_PER_MODEL);
	for (index = 0; index < kept->node_count; index++)
	{
		struct model_node *node = (struct model_node *)kept->model->nodes.address + index;

		matrix4x3_inverse(&node->runtime_default_inverse_matrix, &kept->nodes[index]);
	}
	return TRUE;
}

static struct animation const *animation_named(struct animation_graph const *graph, char const *name)
{
	long index;

	for (index = 0; index < graph->animations.count; index++)
	{
		struct animation const *animation = (struct animation const *)graph->animations.address + index;

		if (!strcmp(animation->name, name))
			return animation;
	}
	return NULL;
}

static boolean stances_find(void)
{
	struct animation_graph const *graph = kept_data(chief.animations_index, sizeof(*graph));
	struct animation_graph_unit_seat const *seats;
	long seat, weapon;

	if (!graph || graph->unit_seats.count <= 0)
		return FALSE;
	seats = graph->unit_seats.address;
	for (seat = 0; seat < graph->unit_seats.count && strcmp(seats[seat].label, "stand"); seat++)
	{
	}
	if (seat == graph->unit_seats.count)
		return FALSE;
	for (weapon = 0; weapon < HELD_WEAPON_COUNT; weapon++)
	{
		struct held_stance *stance = &chief.stances[weapon];
		struct animation_graph_weapon_class const *classes = seats[seat].weapon_classes.address;
		char name[64];
		long index;

		for (index = 0; index < seats[seat].weapon_classes.count; index++)
		{
			if (!strcmp(classes[index].label, held_weapons[weapon].weapon_class))
				stance->weapon_class = &classes[index];
		}
		snprintf(name, sizeof(name), "stand %s idle", held_weapons[weapon].weapon_class);
		stance->idle = animation_named(graph, name);
		snprintf(name, sizeof(name), "stand %s ready", held_weapons[weapon].weapon_class);
		stance->ready = animation_named(graph, name);
		snprintf(name, sizeof(name), "stand %s move-front", held_weapons[weapon].weapon_class);
		stance->walk = animation_named(graph, name);
		if (!stance->weapon_class || !stance->idle || stance->idle->frame_count <= 0)
			return FALSE;
	}
	return TRUE;
}

static void chief_forget(void)
{
	long index;

	if (chief.thread)
	{
		while (!thread_has_exited(chief.thread))
			SwitchToThread();
		dispose_thread(chief.thread);
	}
	chief.ready = FALSE;
	if (chief.instances)
	{
		long weapon;

		if (chief.body.model)
			geometry_delete(chief.body.model);
		for (weapon = 0; weapon < HELD_WEAPON_COUNT; weapon++)
		{
			if (chief.weapons[weapon].model)
				geometry_delete(chief.weapons[weapon].model);
		}
	}
	if (chief.instances)
	{
		cache_files_set_tag_instances(chief.menu_instances, chief.first_index);
		free(chief.instances);
	}
	for (index = 0; index < chief.picture_count; index++)
		free(chief.pictures[index].pixels);
	if (chief.tags)
		free(chief.tags);
	if (chief.file)
		free(chief.file);
	memset(&chief, 0, sizeof(chief));
}

/* the thread: everything but the renderer's buffers and the game's tag table */
static boolean chief_read(void)
{
	struct map_header header;
	struct tags_header const *tags_header;
	long index;

	chief.file = map_read(&chief.file_size);
	if (!chief.file)
		return FALSE;
	memcpy(&header, chief.file, sizeof(header));
	if (header.tag_data_offset < MAP_HEADER_BYTES || header.tag_data_size <= (long)sizeof(*tags_header) ||
		header.tag_data_size > chief.file_size - header.tag_data_offset)
	{
		return FALSE;
	}
	chief.tags_size = header.tag_data_size;
	chief.tags = malloc(chief.tags_size);
	if (!chief.tags)
		return FALSE;
	memcpy(chief.tags, chief.file + header.tag_data_offset, chief.tags_size);
	tags_header = (struct tags_header const *)chief.tags;
	chief.source_count = tags_header->tag_count;
	chief.source_instances = chief.source_count > 0 ?
		moved(tags_header->tag_instances, (unsigned long)chief.source_count * sizeof(*chief.source_instances)) : NULL;
	if (!chief.source_instances)
		return FALSE;
	if (!model_keep(&chief.body, CHIEF_MODEL))
		return FALSE;
	chief.animations_index = tag_keep(source_find('antr', CHIEF_ANIMATIONS));
	if (chief.failed || chief.animations_index == NONE || !stances_find())
	{
		platform_log("menu chief: his animations are not all there");
		return FALSE;
	}
	for (index = 0; index < HELD_WEAPON_COUNT; index++)
	{
		if (!model_keep(&chief.weapons[index], held_weapons[index].model))
			return FALSE;
	}
	for (index = 0; index < chief.kept_count; index++)
	{
		struct cache_file_tag_instance const *source = &chief.source_instances[chief.kept_sources[index]];

		if (source->group_tag == 'bitm' && !pictures_keep(moved(source->base_address, 0),
				((chief.first_salt + index) << 16) | (chief.first_index + index)))
		{
			platform_log("menu chief: the pixels of a bitmap are not all there");
			return FALSE;
		}
	}
	/* (standing on the ground, he is as high as his head; framed around the
	default pose's middle, which every stance stays about) */
	chief.height = 0.72f;
	set_real_point3d(&chief.centre, 0.0f, -0.025f, 0.315f);
	free(chief.file);
	chief.file = NULL;
	return TRUE;
}

static unsigned long __stdcall chief_read_thread(void *input)
{
	unsigned long started = GetTickCount();

	(void)input;
	chief.read = chief_read();
	if (chief.read)
	{
		unsigned long picture_bytes = 0;
		long index;

		for (index = 0; index < chief.picture_count; index++)
			picture_bytes += (unsigned long)chief.pictures[index].size;
		platform_log("menu chief: %ld tags and %ld pictures from %s (%lu KB of tags, %lu KB of pixels), in %lu ms, on its own thread",
			chief.kept_count, chief.picture_count, SOURCE_MAP, chief.tags_size / 1024, picture_bytes / 1024,
			GetTickCount() - started);
	}
	else
	{
		platform_log("menu chief: not read from %s", SOURCE_MAP);
	}
	return 0;
}

void menu_chief_tags_loaded(char const *map_name)
{
	struct cache_file_tag_instance *menu_instances;
	long index, salt = 0;

	if (strcmp(map_name, "ui"))
		return;
	memset(&chief, 0, sizeof(chief));
	menu_instances = cache_files_tag_instances(&chief.first_index);
	for (index = 0; index < chief.first_index; index++)
	{
		long instance_salt = (unsigned long)menu_instances[index].tag_index >> 16;

		if (instance_salt > salt)
			salt = instance_salt;
	}
	chief.first_salt = salt + 1;
	chief.menu_instances = menu_instances;
	if (!create_thread(1, chief_read_thread, NULL, &chief.thread))
		chief.thread = NULL;
}

/* on the game's thread, when he was read: TRUE if he can be drawn */
static boolean chief_finish(void)
{
	long count, index;
	struct cache_file_tag_instance *menu_instances;
	unsigned long started;

	if (chief.ready)
		return TRUE;
	if (!chief.thread || !thread_has_exited(chief.thread))
		return FALSE;
	dispose_thread(chief.thread);
	chief.thread = NULL;
	started = GetTickCount();
	/* (the table he was numbered after, which only a map's load changes) */
	menu_instances = cache_files_tag_instances(&count);
	if (!chief.read || (void *)menu_instances != chief.menu_instances || count != chief.first_index)
		goto failed;
	chief.instances = malloc((count + chief.kept_count) * sizeof(*chief.instances));
	if (!chief.instances)
		goto failed;
	memcpy(chief.instances, menu_instances, count * sizeof(*chief.instances));
	for (index = 0; index < chief.kept_count; index++)
	{
		struct cache_file_tag_instance const *source = &chief.source_instances[chief.kept_sources[index]];
		struct cache_file_tag_instance *instance = &chief.instances[count + index];

		*instance = *source;
		instance->tag_index = ((chief.first_salt + index) << 16) | (chief.first_index + index);
		instance->name = moved(source->name, 1);
		instance->base_address = moved(source->base_address, 0);
	}
	if (!geometry_make(chief.body.model))
		goto failed;
	for (index = 0; index < HELD_WEAPON_COUNT; index++)
	{
		if (!geometry_make(chief.weapons[index].model))
			goto failed;
	}
	cache_files_set_tag_instances(chief.instances, count + chief.kept_count);
	chief.held_since = GetTickCount();
	chief.ready = TRUE;
	platform_log("menu chief: drawn from now on, his buffers made in %lu ms", GetTickCount() - started);
	return TRUE;

failed:
	platform_log("menu chief: not added to the menus' tags");
	chief_forget();
	return FALSE;
}

void menu_chief_tags_unloaded(void)
{
	chief_forget();
}

/* ---------- his pose */

static void animation_pose(
	struct animation const *animation,
	unsigned long milliseconds,
	struct real_orientation *pose)
{
	struct real_orientation next[MAXIMUM_NODES_PER_ANIMATION];
	unsigned long ticks = milliseconds * ANIMATION_FRAMES_PER_SECOND;
	long frame = (long)((ticks / 1000UL) % (unsigned long)animation->frame_count);
	long fraction = (long)(ticks % 1000UL);

	animation_get_node_orientations(chief.body.model, animation, (short)frame, pose);
	animation_get_node_orientations(chief.body.model, animation,
		(short)((frame + 1) % animation->frame_count), next);
	/* (toward the next frame, in the thousandth it has come) */
	interpolate_node_orientations((short)chief.body.node_count, pose, next, (short)(fraction - 1), 1000);
	memcpy(pose, next, chief.body.node_count * sizeof(*pose));
}

/* from the pose he has, to whatever he does next */
static void pose_blend_begin(unsigned long now)
{
	memcpy(chief.blended_from, chief.pose, sizeof(chief.pose));
	chief.blended_since = now;
	chief.blending = TRUE;
}

/* the next of what he holds, readied if his stance has that and he stands */
static void weapon_next(unsigned long now)
{
	chief.held = (chief.held + 1) % HELD_WEAPON_COUNT;
	chief.held_since = now;
	chief.readying = chief.stances[chief.held].ready != NULL && !chief.walking;
	pose_blend_begin(now);
}

/* the right stick turns him and the north button changes what he holds, on any
controller; used, he stands, and left alone he walks for
CONTROLS_REST_MILLISECONDS and stands for that long in turn. On a screen that does
not hear them, or come back to one after being away, he stands facing front.
The north button is where it is on each controller (xinput_sdl.c's Y) */
static void chief_controls(boolean controlled)
{
	unsigned long now = GetTickCount();
	unsigned long since_drawn = now - chief.drawn_at;
	real turn = 0.0f;
	boolean north = FALSE;
	boolean wanted;
	short controller;

	if (!overlay_menu_open())
	{
		for (controller = 0; controller < MAXIMUM_GAMEPADS; controller++)
		{
			struct gamepad_state const *gamepad = input_get_gamepad_state(controller);

			if (!gamepad)
				continue;
			turn += gamepad->sticks[_gamepad_stick_right].x / 32767.0f;
			north |= gamepad->buttons[FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y] > 0;
		}
	}
	if (turn > -CONTROLS_DEAD_ZONE && turn < CONTROLS_DEAD_ZONE)
		turn = 0.0f;
	chief.drawn_at = now;
	if (controlled != chief.controlled || since_drawn >= CONTROLS_AWAY_MILLISECONDS)
	{
		chief.controlled = controlled;
		chief.turned = 0.0f;
		chief.touched_at = now;
		/* (a press from the screen before is not one for him) */
		chief.north_down = north;
		if (chief.walking)
		{
			chief.walking = FALSE;
			pose_blend_begin(now);
		}
		return;
	}
	if (!controlled)
		return;
	if (turn != 0.0f || (north && !chief.north_down))
	{
		chief.touched_at = now;
		if (chief.walking)
		{
			chief.walking = FALSE;
			pose_blend_begin(now);
		}
		chief.turned += turn * CONTROLS_TURN_RADIANS_PER_SECOND * since_drawn / 1000.0f;
		if (north && !chief.north_down)
			weapon_next(now);
	}
	chief.north_down = north;
	wanted = ((now - chief.touched_at) / CONTROLS_REST_MILLISECONDS) % 2 == 1 &&
		chief.stances[chief.held].walk != NULL && !chief.readying;
	if (wanted != chief.walking)
	{
		chief.walking = wanted;
		chief.walking_since = now;
		pose_blend_begin(now);
	}
}

/* what he is doing now: readying what he now holds if his stance has that,
then idling or walking, each blended from the pose he had, facing as he has
been turned; every so often, the next of what he holds */
static void pose_update(real_matrix4x3 *nodes)
{
	static real_point3d const origin = { 0.0f, 0.0f, 0.0f };
	real_vector3d forward;
	static real_vector3d const up = { 0.0f, 0.0f, 1.0f };
	struct real_orientation pose[MAXIMUM_NODES_PER_ANIMATION];
	struct held_stance const *stance;
	unsigned long now = GetTickCount();
	unsigned long elapsed = now - chief.held_since;
	unsigned long ready_milliseconds;

	if (elapsed >= HELD_MILLISECONDS)
	{
		weapon_next(now);
		elapsed = 0;
	}
	stance = &chief.stances[chief.held];
	ready_milliseconds = stance->ready ?
		(unsigned long)stance->ready->frame_count * 1000UL / ANIMATION_FRAMES_PER_SECOND : 0;
	if (chief.readying && elapsed >= ready_milliseconds)
	{
		chief.readying = FALSE;
		pose_blend_begin(now);
	}
	if (chief.readying)
		animation_pose(stance->ready, elapsed, pose);
	else if (chief.walking && stance->walk)
		animation_pose(stance->walk, now - chief.walking_since, pose);
	else
		animation_pose(stance->idle, elapsed, pose);
	if (chief.blending)
	{
		unsigned long blended = now - chief.blended_since;

		if (blended < BLEND_MILLISECONDS)
		{
			interpolate_node_orientations((short)chief.body.node_count, chief.blended_from, pose,
				(short)(blended * 1000UL / BLEND_MILLISECONDS), 1001);
		}
		else
		{
			chief.blending = FALSE;
		}
	}
	memcpy(chief.pose, pose, sizeof(chief.pose));
	forward.i = (real)cos(chief.turned);
	forward.j = (real)sin(chief.turned);
	forward.k = 0.0f;
	animation_graph_node_matrices_from_orientations(chief.animations_index, nodes, pose, &origin, &forward, &up);
}

/* a marker's matrix, or FALSE */
static boolean marker_matrix(
	struct kept_model const *kept,
	char const *name,
	real_matrix4x3 const *nodes,
	struct object_marker *marker)
{
	return name[0] && model_get_marker_by_name(kept->tag_index, name, NULL, NULL, 0, nodes, FALSE, marker, 1) > 0;
}

/* the weapon's nodes, its grip at his hand; then his other hand to the
weapon, as the game does (object_inverse_kinematics) */
static void weapon_attach(
	struct held_stance const *stance,
	struct kept_model const *weapon,
	real_matrix4x3 *body_nodes,
	real_matrix4x3 *weapon_nodes,
	boolean other_hand)
{
	struct object_marker hand, grip;
	real_matrix4x3 root, inverse;
	long index;

	if (!marker_matrix(&chief.body, stance->weapon_class->hand_marker_name, body_nodes, &hand))
		matrix4x3_identity(&hand.matrix);
	root = hand.matrix;
	if (marker_matrix(weapon, stance->weapon_class->grip_marker_name, weapon->nodes, &grip))
	{
		matrix4x3_inverse(&grip.matrix, &inverse);
		matrix4x3_multiply(&hand.matrix, &inverse, &root);
	}
	for (index = 0; index < weapon->node_count; index++)
		matrix4x3_multiply(&root, &weapon->nodes[index], &weapon_nodes[index]);
	if (!other_hand)
		return;
	for (index = 0; index < stance->weapon_class->ik_points.count; index++)
	{
		struct animation_graph_ik_point const *point =
			(struct animation_graph_ik_point const *)stance->weapon_class->ik_points.address + index;
		struct object_marker hand_marker, grip_marker;

		if (marker_matrix(&chief.body, point->marker_name, body_nodes, &hand_marker) &&
			marker_matrix(weapon, point->attached_to_marker_name, weapon_nodes, &grip_marker))
		{
			struct model_node const *nodes = chief.body.model->nodes.address;
			short hand_node = hand_marker.node_index;
			short parent_node = nodes[hand_node].parent_node_index;
			short grandparent_node = parent_node != NONE ? nodes[parent_node].parent_node_index : NONE;

			if (grandparent_node != NONE)
			{
				real_matrix4x3 desired;

				matrix4x3_inverse(&hand_marker.node_matrix, &desired);
				matrix4x3_multiply(&grip_marker.matrix, &desired, &desired);
				inverse_kinematics_adjust_matrices(&desired, &body_nodes[grandparent_node],
					&body_nodes[parent_node], &body_nodes[hand_node]);
			}
		}
	}
}

/* ---------- drawing */

boolean menu_chief_draw(short x0, short y0, short x1, short y1, short color_index, boolean controlled)
{
	struct render_camera saved_render_camera, saved_rasterizer_camera;
	struct render_frustum saved_render_frustum, saved_rasterizer_frustum;
	real_rgb_color colors[MAXIMUM_CHANGE_COLORS_PER_MODEL];
	real_rgb_color white[MAXIMUM_CHANGE_COLORS_PER_MODEL];
	real_matrix4x3 body_nodes[MAXIMUM_NODES_PER_ANIMATION];
	real_matrix4x3 weapon_nodes[MAXIMUM_NODES_PER_MODEL];
	struct render_lighting lighting;
	rectangle2d panel;
	real distance;
	/* (turned, he turns about the model's origin, which must be in the middle
	of the view; not turned, he is framed by the middle of his default pose) */
	real look_y = controlled ? 0.0f : chief.centre.y;
	long index;

	if (x1 <= x0 || y1 <= y0 || !chief_finish())
		return FALSE;

	saved_render_camera = render.camera;
	saved_render_frustum = render.frustum;
	saved_rasterizer_camera = global_window_parameters.camera;
	saved_rasterizer_frustum = global_window_parameters.frustum;

	panel.x0 = (short)(x0 - VIEW_SIDE_MARGIN);
	panel.y0 = y0;
	panel.x1 = (short)(x1 + VIEW_SIDE_MARGIN);
	panel.y1 = y1;
	distance = chief.height * 0.56f / (real)tan(VIEW_VERTICAL_ANGLE * 0.5f);
	render.camera.position.x = chief.centre.x + (real)cos(VIEW_ANGLE_AROUND) * distance;
	render.camera.position.y = look_y + (real)sin(VIEW_ANGLE_AROUND) * distance;
	render.camera.position.z = chief.centre.z;
	render.camera.forward.i = -(real)cos(VIEW_ANGLE_AROUND);
	render.camera.forward.j = -(real)sin(VIEW_ANGLE_AROUND);
	render.camera.forward.k = 0.0f;
	render.camera.up.i = 0.0f;
	render.camera.up.j = 0.0f;
	render.camera.up.k = 1.0f;
	render.camera.mirrored = FALSE;
	render.camera.vertical_field_of_view = VIEW_VERTICAL_ANGLE;
	render.camera.viewport_bounds = panel;
	render.camera.window_bounds = panel;
	render.camera.z_near = 0.05f;
	render.camera.z_far = 50.0f;
	global_window_parameters.camera = render.camera;
	render_camera_build_frustum(&render.camera, NULL, &render.frustum, TRUE);
	render_camera_build_frustum(&global_window_parameters.camera, NULL, &global_window_parameters.frustum, TRUE);
	/* (viewports are the screen's; while the menus draw, every vertex is
	moved by their centring, which would put him out of his) */
	halo_screen_ui_offset(FALSE);
	rasterizer_port_set_viewport(&panel);
	rasterizer_set_frustum_z(-1.0f, -1.0f);
	IDirect3DDevice8_Clear(global_d3d_device, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);

	memset(&lighting, 0, sizeof(lighting));
	rgb_set(&lighting.ambient_color, 0.45f, 0.45f, 0.5f);
	lighting.distant_light_count = 1;
	rgb_set(&lighting.distant_lights[0].color, 0.9f, 0.9f, 0.85f);
	set_real_vector3d(&lighting.distant_lights[0].direction, -0.7f, -0.5f, -0.5f);
	lighting.reflection_tint_color.alpha = 1.0f;
	lighting.reflection_tint_color.red = 0.5f;
	lighting.reflection_tint_color.green = 0.5f;
	lighting.reflection_tint_color.blue = 0.5f;
	/* (all four, as a player's of that color are: placement_data_set_change_color) */
	colors[0] = player_profile_get_rgb_color(color_index);
	for (index = 1; index < MAXIMUM_CHANGE_COLORS_PER_MODEL; index++)
		colors[index] = colors[0];
	for (index = 0; index < MAXIMUM_CHANGE_COLORS_PER_MODEL; index++)
		white[index] = *global_real_rgb_white;

	chief_controls(controlled);
	pose_update(body_nodes);
	{
		struct kept_model const *weapon = &chief.weapons[chief.held];

		weapon_attach(&chief.stances[chief.held], weapon, body_nodes, weapon_nodes,
			!chief.readying && !chief.blending);
		rasterizer_models_begin(TRUE);
		render_model(chief.body.tag_index, 1000.0f, body_nodes, NULL, colors, NULL, &lighting, &chief.centre,
			chief.height, NULL, 0, 0, RENDER_MODEL_MENU_FLAG);
		render_model(weapon->tag_index, 1000.0f, weapon_nodes, NULL, white, NULL, &lighting, &chief.centre,
			chief.height, NULL, 0, 0, RENDER_MODEL_MENU_FLAG);
		rasterizer_models_end();
	}

	halo_screen_ui_offset(TRUE);
	rasterizer_port_set_viewport(&saved_rasterizer_camera.viewport_bounds);
	render.camera = saved_render_camera;
	render.frustum = saved_render_frustum;
	global_window_parameters.camera = saved_rasterizer_camera;
	global_window_parameters.frustum = saved_rasterizer_frustum;
	rasterizer_set_frustum_z(-1.0f, -1.0f);
	return TRUE;
}


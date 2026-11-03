/*
CUSTOM_EDITION_MAPS.C

Lists Custom Edition maps for the menus (custom_edition_maps.h).

Whenever a map list opens, the custom_maps folder and then the Halo Custom
Edition install's maps folder are scanned for CE caches (.map; OpenSauce
caches are refused by the loader, so not listed). One level of subfolders
is scanned too: a map there is still named custom_maps\<file>, and the
menus list the subfolder as its own row. Copies of a name in different
subfolders are each listed, "Foo (packA)", and the row picked chooses its
copy, by its file and hash (map_share.h), which the loader then takes over
the others of the name. A folder inside a folder is not
listed. Multiplayer maps join the level list after the Xbox levels, and
the map lists' CUSTOM MULTIPLAYER; campaign maps (solo scenarios) are the
map lists' CUSTOM SINGLEPLAYER, played alone or as network co-op. The stock
campaign levels get display indices here too, so the menus can show them
the same way.

A map's level name is custom_maps\<name> (CUSTOM_EDITION_LEVEL_NAME_PREFIX),
which the cache loader reads from the custom maps folders alone: a map named
as one of the game's own levels (a30.map, bloodgulch.map) is listed and
played as itself. The game engine keeps 63 characters of a level name, so a
map whose name is longer than 51 is skipped. A map keeps its name: a network
game names it so, and says which version it is on by its hash (map_share.h).
A downloaded map of a name another version has is named <name>_<hash>.map;
the menus show it without the hash, with (2), (3) and so on after the first
of the name. A map a build of before named for its hash has its name given
back (map_share_map_found).

Each map can have, beside it in its folder:
- <name>.bmp, its picture (bmp_files.c; for a map named for its hash, a
  picture named without the hash also does). The middle is cropped to the
  menus' picture shape. It is read the first time it is drawn, and freed
  when the maps are rescanned. Without one, the unknown level's picture
  is shown.
- <name>.txt, its description, line by line (the Xbox descriptions use
  lines of about 20 characters).

A scan costs little more than listing the folders, however many maps there
are: each folder is listed once, whole, before its maps are looked at, so a
description is opened only if the listing has it (a description looked for
and not there cost a lookup that fails, on the Switch 10 ms or so, for
nearly every map); and a map's kind (campaign, multiplayer, neither) is kept
across scans by its path, size and time (identity_cache), so a map list
opened again opens only the maps that are new or have changed since.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "tag_files/files.h"
#include "tag_files/tag_files.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "main/main.h"
#include "saved games/player_profile.h"
#include "scenario/scenario_definitions.h"
#include "text/unicode.h"
#include "bmp_files.h"
#include "custom_edition_cache.h"
#include "custom_edition_maps.h"
#include "map_share.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* ---------- constants */

/* Custom Edition campaign maps, for co-op */
#define MAXIMUM_CUSTOM_EDITION_CAMPAIGNS 1024
#define FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX 0x6000
/* room for the level list's Xbox levels (ui_widget_event_handler_functions.c has 13) */
#define MAXIMUM_XBOX_LEVELS 16

/* the longest map file name whose level name
(CUSTOM_EDITION_LEVEL_NAME_PREFIX and it) the game engine keeps whole */
#define MAXIMUM_MAP_NAME_LENGTH \
	(CUSTOM_EDITION_MAXIMUM_LEVEL_NAME_LENGTH - (NUMBEROF(CUSTOM_EDITION_LEVEL_NAME_PREFIX) - 1))

/* Display indices for CE multiplayer maps, clear of every string and frame
index in the menus' tags (15 level names, 14 level pictures), and below the
custom campaigns' and ui_widget.c's spinner descriptions' (0x7000). */
#define FIRST_DISPLAY_INDEX 0x4000

/* The stock campaign levels, for co-op: their display indices, names, and
pictures (the campaign menu's, one frame per level in order). */
#define FIRST_CAMPAIGN_DISPLAY_INDEX 0x3000
#define CAMPAIGN_LEVEL_PICTURES_TAG_NAME "ui\\shell\\bitmaps\\sp_levels"
static wchar_t campaign_level_names[NUMBER_OF_SINGLE_PLAYER_LEVELS][32] =
{
	L"The Pillar of Autumn", L"Halo", L"The Truth and Reconciliation", L"The Silent Cartographer",
	L"Assault on the Control Room", L"343 Guilty Spark", L"The Library", L"Two Betrayals", L"Keyes", L"The Maw",
};
static wchar_t campaign_level_description[] = L"A campaign level, played co-op";

/* The level pictures tag, the picture shape (140x114 in the menus' 640x480;
the lobby's is 139x113), and the unknown level's frame
(ui_widget_game_data_input_functions.c). */
#define LEVEL_PICTURES_TAG_NAME "ui\\shell\\bitmaps\\mp_map_grafix"
#define LEVEL_PICTURE_SHAPE_WIDTH 140
#define LEVEL_PICTURE_SHAPE_HEIGHT 114
#define UNKNOWN_LEVEL_FRAME 13

#define PICTURE_EXTENSION ".bmp"
/* a 4K screenshot is 25 MB as a bmp file */
#define MAXIMUM_PICTURE_FILE_BYTES 0x04000000L
/* picture textures are square, about twice the menus' picture size */
#define PICTURE_TEXTURE_SIZE 256

#define DESCRIPTION_EXTENSION ".txt"
/* descriptions are a few short lines */
#define MAXIMUM_DESCRIPTION_FILE_BYTES 1024
#define MAXIMUM_DESCRIPTION_LENGTH 127

/* bitmaps.c's format of 32-bit color with alpha */
enum
{
	_bitmap_format_a8r8g8b8 = 11,
};

/* (the multiplayer maps', custom campaigns' and spinner descriptions'
display indices do not meet) */
typedef char verify_multiplayer_display_indices[
	FIRST_DISPLAY_INDEX + CUSTOM_EDITION_MAPS_MAXIMUM <= FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX ? 1 : -1];
typedef char verify_campaign_display_indices[
	FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX + MAXIMUM_CUSTOM_EDITION_CAMPAIGNS <= 0x7000 ? 1 : -1];

/* ---------- structures */

struct custom_edition_map
{
	/* file name without extension, and the maps folder it was found in
	(d:\custom_maps\). category is the subfolder, or empty in that folder. */
	char name[MAXIMUM_MAP_NAME_LENGTH + 1];
	char folder[32];
	char category[CUSTOM_EDITION_MAP_CATEGORY_LENGTH];
	/* saved_game_file_remember_last_used_multiplayer_map writes
	MAXIMUM_FILENAME_LENGTH+1 characters of a level name */
	char level_name[MAXIMUM_FILENAME_LENGTH + 1];
	/* (and room for a version's number, " (2)", and a copy's folder, " (packA)") */
	wchar_t display_name[MAXIMUM_MAP_NAME_LENGTH + 8 + CUSTOM_EDITION_MAP_CATEGORY_LENGTH + 3];
	wchar_t description[MAXIMUM_DESCRIPTION_LENGTH + 1];
	boolean picture_read;
	struct bitmap_data *picture;
};

struct custom_edition_maps_globals
{
	boolean looked_for;
	short map_count;
	struct custom_edition_map maps[CUSTOM_EDITION_MAPS_MAXIMUM];
	/* campaign maps, kept out of the level list */
	short campaign_count;
	struct custom_edition_map campaigns[MAXIMUM_CUSTOM_EDITION_CAMPAIGNS];
	/* the latest level list: the Xbox levels, then the CE maps */
	short xbox_level_count;
	char *levels[MAXIMUM_XBOX_LEVELS + CUSTOM_EDITION_MAPS_MAXIMUM];
};

/* ---------- globals */

static struct custom_edition_maps_globals custom_edition_maps_globals;

/* Halo PC's own multiplayer maps (not on the Xbox), with their menu names
from ui.map's mp_map_list. They are listed as VANILLA. */
static struct
{
	char const *file;
	wchar_t const *name;
} const stock_maps[] =
{
	{ "icefields", L"Ice Fields" }, { "deathisland", L"Death Island" }, { "dangercanyon", L"Danger Canyon" },
	{ "infinity", L"Infinity" }, { "timberland", L"Timberland" }, { "gephyrophobia", L"Gephyrophobia" },
};

/* for a map without a description file */
static wchar_t const default_description[] = L"Halo Custom\r\nEdition map";

/* ---------- private code */

/* ---------- the maps' kinds, kept across scans */

/* room for every map the lists hold, twice over (a hash table: half full at
most) */
#define IDENTITY_CACHE_SIZE 32768

/* a map file's kind, as its cache header says: its scenario type, or NONE
for a file that is no Custom Edition cache; by its path's hash, and its size
and time, which a file that changed does not keep */
struct identity_cache_entry
{
	uint64_t key;
	uint64_t time;
	unsigned long size;
	short scenario_type;
	boolean used;
};

static struct identity_cache_entry identity_cache[IDENTITY_CACHE_SIZE];
static long identity_cache_count;

/* a name's hash, whatever its case (FNV-1a) */
static uint64_t name_hash(
	char const *first,
	char const *second)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	char const *part;
	short index;

	for (index = 0; index < 2; index++)
	{
		for (part = index ? second : first; part && *part; part++)
		{
			char character = *part;

			if (character >= 'A' && character <= 'Z')
				character = (char)(character - 'A' + 'a');
			hash ^= (unsigned char)character;
			hash *= 0x100000001b3ULL;
		}
	}

	return hash;
}

/* the kind of the map file at `path`, `size` bytes and last written at
`time`: kept, or read and kept */
static short identity_cached(
	char const *path,
	unsigned long size,
	uint64_t time)
{
	uint64_t key = name_hash(path, NULL);
	long slot = (long)(key % IDENTITY_CACHE_SIZE);
	struct identity_cache_entry *entry;

	for (;;)
	{
		entry = &identity_cache[slot];
		if (!entry->used || entry->key == key)
			break;
		slot = (slot + 1) % IDENTITY_CACHE_SIZE;
	}
	if (entry->used && entry->size == size && entry->time == time)
	{
		return entry->scenario_type;
	}
	if (!entry->used)
	{
		/* (full: forgotten, all of it, and filled again) */
		if (identity_cache_count >= IDENTITY_CACHE_SIZE / 2)
		{
			csmemset(identity_cache, 0, sizeof(identity_cache));
			identity_cache_count = 0;
			entry = &identity_cache[key % IDENTITY_CACHE_SIZE];
		}
		identity_cache_count++;
	}
	entry->key = key;
	entry->size = size;
	entry->time = time;
	entry->used = TRUE;
	entry->scenario_type = custom_edition_cache_file_scenario_type(path);

	return entry->scenario_type;
}

static void custom_edition_maps_forget(
	void)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	short map_index;

	for (map_index = 0; map_index < globals->map_count; map_index++)
	{
		if (globals->maps[map_index].picture)
			bitmap_delete(globals->maps[map_index].picture);
	}
	for (map_index = 0; map_index < globals->campaign_count; map_index++)
	{
		if (globals->campaigns[map_index].picture)
			bitmap_delete(globals->campaigns[map_index].picture);
	}
	globals->map_count = 0;
	globals->campaign_count = 0;

	return;
}

/* the map's index in stock_maps, or NONE (by its name without its hash) */
static short stock_map_index(
	char const *name)
{
	short length = map_share_name_base_length(name);
	short index;

	for (index = 0; index < NUMBEROF(stock_maps); index++)
	{
		if ((short)csstrlen(stock_maps[index].file) == length && !_strnicmp(name, stock_maps[index].file, length))
			return index;
	}
	return NONE;
}

/* A map's menu name: Halo PC's name for its own maps, otherwise the file
name (without a hash it ends in) with underscores as spaces and words
capitalized ("the_bay_of_pigs" becomes "The Bay Of Pigs"). */
static void display_name_make(
	char const *name,
	wchar_t *display_name)
{
	boolean word_start = TRUE;
	short index = stock_map_index(name);
	short length = map_share_name_base_length(name);

	if (index != NONE)
	{
		ustrcpy(display_name, stock_maps[index].name);
		return;
	}

	for (index = 0; index < length; index++)
	{
		unsigned char character = (unsigned char)name[index];

		if (character == '_')
		{
			character = ' ';
		}
		else if (word_start && character >= 'a' && character <= 'z')
		{
			character = (unsigned char)(character - 'a' + 'A');
		}
		word_start = character == ' ';
		display_name[index] = (wchar_t)character;
	}
	display_name[index] = 0;

	return;
}

/* <folder><category\><name><extension> in path. FALSE when it does not fit. */
static boolean custom_edition_map_beside(
	struct custom_edition_map const *map,
	char const *extension,
	char *path,
	long path_size)
{
	long length = csstrlen(map->folder) + csstrlen(map->name) + csstrlen(extension);

	if (map->category[0])
		length += csstrlen(map->category) + 1;
	if (length >= path_size)
		return FALSE;
	if (map->category[0])
		csprintf(path, "%s%s\\%s%s", map->folder, map->category, map->name, extension);
	else
		csprintf(path, "%s%s%s", map->folder, map->name, extension);

	return TRUE;
}

/* Reads <name>.txt into map->description: line ends become \r\n (as in the
menus' strings), tabs become spaces, and non-ASCII characters become '?'.
A missing or empty file gives the default description; `found` is which
of the two names the folder's listing has (DESCRIPTION_OWN_NAME, the map's
name; DESCRIPTION_BASE_NAME, a map named for its hash, its name without it),
and none is not looked for. */
#define DESCRIPTION_OWN_NAME 1
#define DESCRIPTION_BASE_NAME 2

static void custom_edition_map_description_read(
	struct custom_edition_map *map,
	short found)
{
	char path[MAXIMUM_FILENAME_LENGTH + 1];
	byte text[MAXIMUM_DESCRIPTION_FILE_BYTES];
	FILE *stream;
	long text_size = 0;
	long text_index = 0;
	short length = 0;

	stream = NULL;
	if ((found & DESCRIPTION_OWN_NAME) && custom_edition_map_beside(map, DESCRIPTION_EXTENSION, path, sizeof(path)))
		stream = fopen(path, "rb");
	/* (a map named for its hash: its description named without it too) */
	if (!stream && (found & DESCRIPTION_BASE_NAME) && map_share_name_hashed(map->name))
	{
		csprintf(path, "%s%.*s%s", map->folder, (int)map_share_name_base_length(map->name), map->name,
			DESCRIPTION_EXTENSION);
		stream = fopen(path, "rb");
	}
	if (stream)
	{
		text_size = (long)fread(text, 1, sizeof(text), stream);
		fclose(stream);
	}
	/* the byte order mark of UTF-8 text */
	if (text_size >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF)
	{
		text_index = 3;
	}
	for (; text_index < text_size && length < MAXIMUM_DESCRIPTION_LENGTH; text_index++)
	{
		byte character = text[text_index];

		if (character == '\n')
		{
			if (length + 2 > MAXIMUM_DESCRIPTION_LENGTH)
			{
				break;
			}
			map->description[length++] = '\r';
			map->description[length++] = '\n';
		}
		else if (character == '\t')
		{
			map->description[length++] = ' ';
		}
		else if (character >= ' ' && character < 0x7F)
		{
			map->description[length++] = character;
		}
		else if (character >= 0xC0)
		{
			/* the lead byte of a multi-byte UTF-8 character */
			map->description[length++] = '?';
		}
	}
	while (length > 0 &&
		(map->description[length - 1] == ' ' ||
			map->description[length - 1] == '\r' ||
			map->description[length - 1] == '\n'))
	{
		length--;
	}
	map->description[length] = 0;

	if (!length)
	{
		csmemcpy(map->description, default_description, sizeof(default_description));
	}

	return;
}

/* Adds the file `name`.`extension` in `directory` (`root`, or its subfolder
`category`) if it is a CE map that isn't listed yet in that folder: a copy
of its name in another subfolder is listed too (shown with its folder,
custom_edition_maps_tell_folders_apart), and the one a row picks is the one
played (custom_edition_maps_name_given). custom_maps is scanned before the
install, so its copy in a folder wins over the install's in the same
folder. The file is identified where the listing found it, once (by
name, as the loader finds a map, it was looked for twice, a campaign's
then a multiplayer map's, each search a lookup in the folder and the
file's opening). */
static void custom_edition_map_add(
	char const *root,
	char const *directory,
	char const *category,
	char const *name,
	char const *extension,
	unsigned long size,
	uint64_t time,
	short descriptions)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	struct custom_edition_map *map;
	char path[MAXIMUM_FILENAME_LENGTH + 1];
	boolean campaign;
	short map_index;
	short scenario_type;

	if (csstrcasecmp(extension, "map"))
	{
		return;
	}
	/* campaign maps go to co-op; anything that isn't multiplayer either is skipped */
	if (csstrlen(directory) + csstrlen(name) + 1 + csstrlen(extension) > MAXIMUM_FILENAME_LENGTH)
	{
		return;
	}
	csprintf(path, "%s%s.%s", directory, name, extension);
	scenario_type = identity_cached(path, size, time);
	campaign = scenario_type == _scenario_type_solo;
	if (!campaign && scenario_type != _scenario_type_multiplayer)
	{
		return;
	}
	/* (its hash found ahead, every copy's: map_share_files.c) */
	map_share_map_listed(path, size, time);
	/* (a copy of a name in another subfolder is listed too, told apart by
	its folder; one in the same folder of the install, not) */
	for (map_index = 0; map_index < globals->map_count; map_index++)
	{
		if (!csstrcasecmp(globals->maps[map_index].name, name) &&
			!csstrcasecmp(globals->maps[map_index].category, category))
		{
			error(_error_silent, "custom edition: the map '%s' is already listed; the copy in '%s%s%s' is skipped",
				name, root, category[0] ? category : "", category[0] ? "\\" : "");
			return;
		}
	}
	for (map_index = 0; map_index < globals->campaign_count; map_index++)
	{
		if (!csstrcasecmp(globals->campaigns[map_index].name, name) &&
			!csstrcasecmp(globals->campaigns[map_index].category, category))
		{
			error(_error_silent, "custom edition: the map '%s' is already listed; the copy in '%s%s%s' is skipped",
				name, root, category[0] ? category : "", category[0] ? "\\" : "");
			return;
		}
	}
	if (campaign)
	{
		if (globals->campaign_count == MAXIMUM_CUSTOM_EDITION_CAMPAIGNS || csstrlen(name) > MAXIMUM_MAP_NAME_LENGTH)
		{
			error(_error_silent, "custom edition: the campaign map '%s' is not listed (too many, or too long a name)",
				name);
			return;
		}
		map = &globals->campaigns[globals->campaign_count++];
	}
	else if (csstrlen(name) > MAXIMUM_MAP_NAME_LENGTH)
	{
		error(
			_error_silent,
			"custom edition: the map '%s' is not in the level list: its name is longer than %d characters",
			name,
			MAXIMUM_MAP_NAME_LENGTH);
		return;
	}
	else if (globals->map_count == CUSTOM_EDITION_MAPS_MAXIMUM)
	{
		error(
			_error_silent,
			"custom edition: the map '%s' is not in the level list, which holds %d of them",
			name,
			CUSTOM_EDITION_MAPS_MAXIMUM);
		return;
	}
	else
	{
		map = &globals->maps[globals->map_count++];
	}
	csmemset(map, 0, sizeof(*map));
	csstrcpy(map->name, name);
	csstrncpy(map->folder, root, sizeof(map->folder) - 1);
	csstrncpy(map->category, category, sizeof(map->category) - 1);
	csprintf(map->level_name, "%s%s", CUSTOM_EDITION_LEVEL_NAME_PREFIX, name);
	display_name_make(name, map->display_name);
	custom_edition_map_description_read(map, descriptions);
	/* (named for its hash by a build of before: its name given back,
	map_share_files.c; custom_maps' own files alone, not a subfolder's) */
	if (!category[0])
		map_share_map_found(root, name);

	return;
}

/* by name, then a name's copies by folder (the maps folder's own first),
then the custom_maps folder's before the install's */
static int custom_edition_map_compare(
	void const *first,
	void const *second)
{
	struct custom_edition_map const *one = first;
	struct custom_edition_map const *other = second;
	int order = (int)csstrcasecmp(one->name, other->name);

	if (!order)
		order = (int)csstrcasecmp(one->category, other->category);
	if (!order)
		order = (int)csstrcasecmp(one->folder, other->folder);
	return order;
}

/* The files in `directory` (the maps folder, or one subfolder of it).
`category` is that subfolder's name, or empty for the maps folder itself.
A folder is listed through a handle of its own, never with
find_files_start and find_files_next: those keep their place in one
global, and as the game starts a thread of the menus' deletes the files
of folders with them (perform_filesystem_initialization in ui_widget.c,
directory_create_or_delete_contents). A listing here at the same time
takes that one's place, and the thread then deletes the files of this
folder: the maps. */
static void custom_edition_maps_scan_files(
	char const *root,
	char const *directory,
	char const *category)
{
	/* the folder's files, whole (the maps among them looked at after) */
	struct listed_file
	{
		char name[sizeof(((WIN32_FIND_DATAA *)0)->cFileName) + 1];
		char *extension;
		unsigned long size;
		uint64_t time;
	} *files = NULL;
	long file_count = 0, file_room = 0;
	/* the hashes of the names its descriptions have (<name>.txt) */
	uint64_t *descriptions = NULL;
	long description_count = 0;
	char pattern[MAXIMUM_FILENAME_LENGTH + 1];
	WIN32_FIND_DATAA data;
	HANDLE find;
	long index;

	if (csstrlen(directory) + 3 > MAXIMUM_FILENAME_LENGTH)
	{
		error(_error_silent, "custom edition: the folder '%s' is too long to list", directory);
		return;
	}
	csprintf(pattern, "%s*.*", directory);
	find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		struct listed_file *file;
		char *extension;

		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (file_count == file_room)
		{
			long room = file_room ? file_room * 2 : 256;
			struct listed_file *more = realloc(files, (size_t)room * sizeof(*files));

			if (!more)
				break;
			files = more;
			file_room = room;
		}
		file = &files[file_count];
		csmemcpy(file->name, data.cFileName, sizeof(data.cFileName));
		file->name[sizeof(data.cFileName)] = 0;
		extension = strrchr(file->name, '.');
		if (!extension)
			continue;
		*extension++ = 0;
		file->extension = extension;
		file->size = data.nFileSizeLow;
		file->time = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
		file_count++;
	} while (FindNextFileA(find, &data));
	CloseHandle(find);

	descriptions = file_count ? malloc((size_t)file_count * sizeof(*descriptions)) : NULL;
	for (index = 0; descriptions && index < file_count; index++)
	{
		if (!csstrcasecmp(files[index].extension, DESCRIPTION_EXTENSION + 1))
			descriptions[description_count++] = name_hash(files[index].name, NULL);
	}
	for (index = 0; index < file_count; index++)
	{
		struct listed_file const *file = &files[index];
		short found = 0;
		long other;

		if (csstrcasecmp(file->extension, "map"))
			continue;
		/* (its description, and for a map named for its hash, the one named
		without it) */
		{
			uint64_t own = name_hash(file->name, NULL);
			uint64_t base = 0;

			if (map_share_name_hashed(file->name))
			{
				char base_name[sizeof(file->name)];
				short length = map_share_name_base_length(file->name);

				csmemcpy(base_name, file->name, length);
				base_name[length] = 0;
				base = name_hash(base_name, NULL);
			}
			for (other = 0; other < description_count; other++)
			{
				if (descriptions[other] == own)
					found |= DESCRIPTION_OWN_NAME;
				if (base && descriptions[other] == base)
					found |= DESCRIPTION_BASE_NAME;
			}
		}
		custom_edition_map_add(root, directory, category, file->name, file->extension, file->size, file->time, found);
	}
	free(descriptions);
	free(files);

	return;
}

/* `root`, then one level of its subfolders. A folder inside a folder is
not listed. */
static void custom_edition_maps_scan_root(
	char const *root)
{
	char pattern[MAXIMUM_FILENAME_LENGTH + 1];
	WIN32_FIND_DATAA data;
	HANDLE find;

	custom_edition_maps_scan_files(root, root, "");
	if (csstrlen(root) + 3 > MAXIMUM_FILENAME_LENGTH)
		return;
	csprintf(pattern, "%s*.*", root);
	find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char name[sizeof(data.cFileName) + 1];
		char directory[MAXIMUM_FILENAME_LENGTH + 1];

		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		csmemcpy(name, data.cFileName, sizeof(data.cFileName));
		name[sizeof(data.cFileName)] = 0;
		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		if (csstrlen(name) >= CUSTOM_EDITION_MAP_CATEGORY_LENGTH ||
			csstrlen(root) + csstrlen(name) + 1 > MAXIMUM_FILENAME_LENGTH)
		{
			error(_error_silent, "custom edition: the folder '%s' is too long to list", name);
			continue;
		}
		csprintf(directory, "%s%s\\", root, name);
		custom_edition_maps_scan_files(root, directory, name);
	} while (FindNextFileA(find, &data));
	CloseHandle(find);

	return;
}

/* Versions of a map listed (its name the same but for the hash a version's
file name ends in, a download's beside another of its name): the first in
the order the maps are sorted by name (the one of the name alone, which
sorts before them) shown by its name, the others with (2), (3) and so on. */
static void custom_edition_maps_tell_versions_apart(
	struct custom_edition_map *maps,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct custom_edition_map *map = &maps[index];
		short length = map_share_name_base_length(map->name);
		short number = 1;
		short other;

		if (!map_share_name_hashed(map->name))
			continue;
		for (other = 0; other < count; other++)
		{
			struct custom_edition_map const *version = &maps[other];

			if (other == index || map_share_name_base_length(version->name) != length ||
				_strnicmp(version->name, map->name, length))
			{
				continue;
			}
			if (other < index)
				number++;
		}
		if (number > 1)
		{
			char suffix[8];
			short end = 0;
			short character;

			while (map->display_name[end])
				end++;
			csprintf(suffix, " (%d)", (int)MIN(number, 999));
			for (character = 0; suffix[character] && end + 1 < NUMBEROF(map->display_name); character++)
				map->display_name[end++] = (wchar_t)suffix[character];
			map->display_name[end] = 0;
		}
	}
}

/* Copies of a name listed, each in its own subfolder: shown with their
folder after the name, "Foo (packA)", the maps folder's own by its name
alone. */
static void custom_edition_maps_tell_folders_apart(
	struct custom_edition_map *maps,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct custom_edition_map *map = &maps[index];
		boolean copied = FALSE;
		short other;

		if (!map->category[0])
			continue;
		/* (sorted by name: a copy is beside it) */
		for (other = index - 1; other >= 0 && !csstrcasecmp(maps[other].name, map->name) && !copied; other--)
			copied = TRUE;
		for (other = index + 1; other < count && !csstrcasecmp(maps[other].name, map->name) && !copied; other++)
			copied = TRUE;
		if (copied)
		{
			short end = 0;
			short character;

			while (map->display_name[end])
				end++;
			if (end + 3 < NUMBEROF(map->display_name))
			{
				map->display_name[end++] = ' ';
				map->display_name[end++] = '(';
				for (character = 0; map->category[character] && end + 2 < NUMBEROF(map->display_name); character++)
					map->display_name[end++] = (wchar_t)(unsigned char)map->category[character];
				map->display_name[end++] = ')';
			}
			map->display_name[end] = 0;
		}
	}
}

static void custom_edition_maps_look_for(
	void)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	char const *folders[] = { CUSTOM_EDITION_MAP_DIRECTORY, CUSTOM_EDITION_INSTALL_MAP_DIRECTORY };
	short folder_index;
	unsigned long started = GetTickCount();

	custom_edition_maps_forget();
	globals->looked_for = TRUE;
	if (!halo_custom_edition_tag_cache())
	{
		return;
	}

	/* the game's custom_maps first, so its copy wins */
	map_share_maps_listing_begin();
	for (folder_index = 0; folder_index < NUMBEROF(folders); folder_index++)
	{
		/* (the install's only when there is one) */
		if (folder_index && !custom_edition_install_present())
			continue;
		custom_edition_maps_scan_root(folders[folder_index]);
	}
	map_share_maps_listing_end();
	qsort(globals->maps, globals->map_count, sizeof(globals->maps[0]), custom_edition_map_compare);
	qsort(globals->campaigns, globals->campaign_count, sizeof(globals->campaigns[0]), custom_edition_map_compare);
	custom_edition_maps_tell_versions_apart(globals->maps, globals->map_count);
	custom_edition_maps_tell_versions_apart(globals->campaigns, globals->campaign_count);
	custom_edition_maps_tell_folders_apart(globals->maps, globals->map_count);
	custom_edition_maps_tell_folders_apart(globals->campaigns, globals->campaign_count);
	error(_error_silent, "custom edition: %d multiplayer maps for the level list, %d campaign maps (%lu ms)",
		globals->map_count, globals->campaign_count, GetTickCount() - started);

	return;
}

/* Loads the map's picture as a texture. Returns NULL if it has none, or if
it can't be shown (logged). */
static struct bitmap_data *custom_edition_map_picture_read(
	struct custom_edition_map const *map)
{
	char path[MAXIMUM_FILENAME_LENGTH + 1];
	struct bmp_file_picture picture;
	enum bmp_file_status status;
	struct bitmap_data *bitmap = NULL;
	uint8_t *file = NULL;
	FILE *stream;
	long size = 0;

	if (!custom_edition_map_beside(map, PICTURE_EXTENSION, path, sizeof(path)))
		return NULL;
	stream = fopen(path, "rb");
	/* (a map named for its hash: its picture named without it too) */
	if (!stream && map_share_name_hashed(map->name))
	{
		csprintf(path, "%s%.*s%s", map->folder, (int)map_share_name_base_length(map->name), map->name,
			PICTURE_EXTENSION);
		stream = fopen(path, "rb");
	}
	if (!stream)
	{
		return NULL;
	}
	if (fseek(stream, 0, SEEK_END) == 0 &&
		(size = ftell(stream)) >= 0 &&
		size <= MAXIMUM_PICTURE_FILE_BYTES &&
		fseek(stream, 0, SEEK_SET) == 0)
	{
		/* +1 so an empty file doesn't look like a failed allocation */
		file = malloc((size_t)size + 1);
		if (file && fread(file, 1, (size_t)size, stream) != (size_t)size)
		{
			free(file);
			file = NULL;
		}
	}
	fclose(stream);
	if (!file)
	{
		error(_error_silent, "custom edition: the picture '%s' could not be read", path);
		return NULL;
	}

	status = bmp_file_open(file, (uint32_t)size, &picture);
	if (status == _bmp_file_status_ok)
	{
		bitmap = bitmap_2d_new(PICTURE_TEXTURE_SIZE, PICTURE_TEXTURE_SIZE, 0, _bitmap_format_a8r8g8b8);
		if (bitmap && bitmap->base_address)
		{
			bmp_file_fit(
				file,
				&picture,
				LEVEL_PICTURE_SHAPE_WIDTH,
				LEVEL_PICTURE_SHAPE_HEIGHT,
				PICTURE_TEXTURE_SIZE,
				PICTURE_TEXTURE_SIZE,
				XBOX_POINTER(uint32_t, bitmap->base_address));
			bitmap_rebuild(bitmap);
		}
		if (bitmap && !bitmap->hardware_format)
		{
			bitmap_delete(bitmap);
			bitmap = NULL;
		}
	}
	else
	{
		error(_error_silent, "custom edition: the picture '%s' cannot be shown: %s", path, bmp_file_status_describe(status));
	}
	free(file);

	return bitmap;
}

/* the CE map with this display index, or NULL */
static struct custom_edition_map *custom_edition_map_get(
	short display_index)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	short map_index = display_index - FIRST_DISPLAY_INDEX;
	short campaign_index = display_index - FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX;

	if (campaign_index >= 0 && campaign_index < globals->campaign_count)
		return &globals->campaigns[campaign_index];
	return map_index >= 0 && map_index < globals->map_count ? &globals->maps[map_index] : NULL;
}

/* the stock campaign level with this display index, or NONE */
static short campaign_level_get(
	short display_index)
{
	short level = display_index - FIRST_CAMPAIGN_DISPLAY_INDEX;

	return level >= 0 && level < NUMBER_OF_SINGLE_PLAYER_LEVELS ? level : NONE;
}

/* the stock campaign level a level name refers to (levels\a10\a10 is the
first), or NONE */
static short campaign_level_from_name(
	char const *level_name)
{
	char const *name = tag_name_strip_path(level_name);
	short level;

	for (level = 0; level < NUMBER_OF_SINGLE_PLAYER_LEVELS; level++)
	{
		if (!csstrcasecmp(tag_name_strip_path(main_get_solo_level_name(level)), name))
			return level;
	}

	return NONE;
}

/* ---------- public code */

char **custom_edition_maps_level_list(
	char **xbox_levels,
	short xbox_level_count,
	short *level_count)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	short level_index;
	short map_index;

	globals->xbox_level_count = MIN(xbox_level_count, MAXIMUM_XBOX_LEVELS);
	for (level_index = 0; level_index < globals->xbox_level_count; level_index++)
	{
		globals->levels[level_index] = xbox_levels[level_index];
	}
	/* (the menus ask for the list every tick, so the folders are scanned
	once, and again when a map list opens: custom_edition_maps_look_again) */
	if (!globals->looked_for)
	{
		custom_edition_maps_look_for();
	}
	for (map_index = 0; map_index < globals->map_count; map_index++)
	{
		globals->levels[globals->xbox_level_count + map_index] = globals->maps[map_index].level_name;
	}
	*level_count = globals->xbox_level_count + globals->map_count;

	return globals->levels;
}

/* the map of one of the lists' own level names (its row's), or NULL */
static struct custom_edition_map const *custom_edition_map_of_row(
	char const *level_name)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	struct custom_edition_map const *map = NULL;

	if (!level_name)
		return NULL;
	if (level_name >= globals->maps[0].level_name &&
		level_name < (char const *)(globals->maps + globals->map_count))
	{
		map = &globals->maps[((char const *)level_name - (char const *)globals->maps) / (long)sizeof(globals->maps[0])];
	}
	else if (level_name >= globals->campaigns[0].level_name &&
		level_name < (char const *)(globals->campaigns + globals->campaign_count))
	{
		map = &globals->campaigns[((char const *)level_name - (char const *)globals->campaigns) /
			(long)sizeof(globals->campaigns[0])];
	}
	return map && level_name == map->level_name ? map : NULL;
}

void custom_edition_maps_name_given(
	char const *level_name)
{
	struct custom_edition_map const *map = custom_edition_map_of_row(level_name);
	char path[MAP_SHARE_PATH_SIZE];

	if (!level_name)
		return;
	/* (one of the lists' own names: the copy of that row) */
	if (map && custom_edition_map_beside(map, ".map", path, sizeof(path)))
	{
		map_share_map_choose_file(map->level_name, path);
		return;
	}
	/* (another Custom Edition map's: the copy chosen kept for its own name
	alone, a network game's or a rotation's of another map choosing none;
	the game's own levels, the main menu's as it loads again among them,
	leave it) */
	if (custom_edition_level_name(level_name))
		map_share_map_choose_name(level_name);
}

boolean custom_edition_maps_row_chosen(
	char const *level_name)
{
	struct custom_edition_map const *map = custom_edition_map_of_row(level_name);
	char path[MAP_SHARE_PATH_SIZE];
	char chosen[MAP_SHARE_PATH_SIZE];

	return map && custom_edition_map_beside(map, ".map", path, sizeof(path)) &&
		map_share_map_chosen(map->level_name, chosen) && !csstrcasecmp(path, chosen);
}

void custom_edition_maps_look_again(
	void)
{
	custom_edition_maps_globals.looked_for = FALSE;
	/* (a network game's map's file, found again) */
	custom_edition_map_paths_forget();

	return;
}

short custom_edition_maps_level_display_index(
	short level_index)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	short map_index = level_index - globals->xbox_level_count;

	return map_index >= 0 && map_index < globals->map_count ? FIRST_DISPLAY_INDEX + map_index : level_index;
}

short custom_edition_maps_display_index(
	char const *level_name)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	char const *name = tag_name_strip_path(level_name);
	char path[MAP_SHARE_PATH_SIZE];
	short map_index;

	/* (only a custom_maps\ name is a CE map's; any other is the game's own,
	a stock campaign level or none of these) */
	if (!custom_edition_level_name(level_name))
	{
		short level = campaign_level_from_name(level_name);

		return level != NONE ? FIRST_CAMPAIGN_DISPLAY_INDEX + level : NONE;
	}
	if (!globals->looked_for)
	{
		custom_edition_maps_look_for();
	}
	/* (the map by its file: a network game's of its version, which may be
	one of another name, <name>_<hash>.map, shown with its number; the copy
	a player chose, in its folder) */
	if (custom_edition_map_file_path(level_name, path))
	{
		char const *file_name = tag_name_strip_path(path);
		char listed[MAP_SHARE_PATH_SIZE];

		for (map_index = 0; map_index < globals->map_count; map_index++)
		{
			if (custom_edition_map_beside(&globals->maps[map_index], ".map", listed, sizeof(listed)) &&
				!csstrcasecmp(listed, path))
			{
				return FIRST_DISPLAY_INDEX + map_index;
			}
		}
		for (map_index = 0; map_index < globals->campaign_count; map_index++)
		{
			if (custom_edition_map_beside(&globals->campaigns[map_index], ".map", listed, sizeof(listed)) &&
				!csstrcasecmp(listed, path))
			{
				return FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX + map_index;
			}
		}
		short length = (short)csstrlen(file_name) - (short)(sizeof(".map") - 1);

		for (map_index = 0; length > 0 && map_index < globals->map_count; map_index++)
		{
			if ((short)csstrlen(globals->maps[map_index].name) == length &&
				!_strnicmp(globals->maps[map_index].name, file_name, length))
			{
				return FIRST_DISPLAY_INDEX + map_index;
			}
		}
		for (map_index = 0; length > 0 && map_index < globals->campaign_count; map_index++)
		{
			if ((short)csstrlen(globals->campaigns[map_index].name) == length &&
				!_strnicmp(globals->campaigns[map_index].name, file_name, length))
			{
				return FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX + map_index;
			}
		}
	}
	for (map_index = 0; map_index < globals->map_count; map_index++)
	{
		if (!csstrcasecmp(globals->maps[map_index].name, name))
		{
			return FIRST_DISPLAY_INDEX + map_index;
		}
	}
	for (map_index = 0; map_index < globals->campaign_count; map_index++)
	{
		if (!csstrcasecmp(globals->campaigns[map_index].name, name))
		{
			return FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX + map_index;
		}
	}

	return NONE;
}

boolean custom_edition_maps_campaign(
	short display_index)
{
	return campaign_level_get(display_index) != NONE ||
		(display_index >= FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX && custom_edition_map_get(display_index));
}

short custom_edition_maps_campaign_level(
	short display_index)
{
	return campaign_level_get(display_index);
}

boolean custom_edition_maps_level_campaign(
	char const *level_name)
{
	return level_name && custom_edition_maps_campaign(custom_edition_maps_display_index(level_name));
}

short custom_edition_maps_count(
	boolean campaign)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;

	if (!globals->looked_for)
	{
		custom_edition_maps_look_for();
	}

	return campaign ? globals->campaign_count : globals->map_count;
}

short custom_edition_maps_display_index_of(
	boolean campaign,
	short index)
{
	return index < 0 || index >= custom_edition_maps_count(campaign) ? NONE :
		(campaign ? FIRST_CUSTOM_CAMPAIGN_DISPLAY_INDEX : FIRST_DISPLAY_INDEX) + index;
}

char const *custom_edition_maps_category(
	boolean campaign,
	short index)
{
	struct custom_edition_maps_globals *globals = &custom_edition_maps_globals;
	struct custom_edition_map *maps;
	short count;

	if (!globals->looked_for)
		custom_edition_maps_look_for();
	maps = campaign ? globals->campaigns : globals->maps;
	count = campaign ? globals->campaign_count : globals->map_count;
	if (index < 0 || index >= count)
		return NULL;

	return maps[index].category;
}

char const *custom_edition_maps_level_name(
	short display_index)
{
	struct custom_edition_map *map = custom_edition_map_get(display_index);
	short level = campaign_level_get(display_index);

	if (level != NONE)
	{
		return main_get_solo_level_name(level);
	}

	return map ? map->level_name : NULL;
}

boolean custom_edition_maps_stock(
	short display_index)
{
	struct custom_edition_map *map = custom_edition_map_get(display_index);

	return map && stock_map_index(map->name) != NONE;
}

wchar_t *custom_edition_maps_name(
	short display_index)
{
	struct custom_edition_map *map = custom_edition_map_get(display_index);
	short level = campaign_level_get(display_index);

	if (level != NONE)
	{
		return campaign_level_names[level];
	}

	return map ? map->display_name : NULL;
}

wchar_t *custom_edition_maps_description(
	short display_index)
{
	struct custom_edition_map *map = custom_edition_map_get(display_index);

	if (campaign_level_get(display_index) != NONE)
	{
		return campaign_level_description;
	}

	return map ? map->description : NULL;
}

struct bitmap_data *custom_edition_maps_picture(
	long bitmap_tag_index,
	short *frame_index)
{
	struct custom_edition_map *map;
	short level = campaign_level_get(*frame_index);

	if (level != NONE &&
		bitmap_tag_index != NONE &&
		!csstrcasecmp(tag_get_name(bitmap_tag_index), LEVEL_PICTURES_TAG_NAME))
	{
		long pictures = tag_loaded('bitm', CAMPAIGN_LEVEL_PICTURES_TAG_NAME);

		*frame_index = UNKNOWN_LEVEL_FRAME;
		return pictures != NONE ? bitmap_group_get_bitmap_from_sequence(pictures, 0, level) : NULL;
	}
	if (*frame_index < FIRST_DISPLAY_INDEX ||
		bitmap_tag_index == NONE ||
		csstrcasecmp(tag_get_name(bitmap_tag_index), LEVEL_PICTURES_TAG_NAME))
	{
		return NULL;
	}

	map = custom_edition_map_get(*frame_index);
	*frame_index = UNKNOWN_LEVEL_FRAME;
	if (!map)
	{
		return NULL;
	}
	if (!map->picture_read)
	{
		map->picture = custom_edition_map_picture_read(map);
		map->picture_read = TRUE;
	}

	return map->picture;
}

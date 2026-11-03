/*
MAP_SHARE_HASHES.C (test)

The maps' hashes kept across runs (port/linux/game/map_share_files.c): their
table by path, whatever its case, as it grows, and the lines of
d:\map_hashes.txt, written and read back the same, and a line that is not
one passed over. test_map_share_hashes.py takes the constants (config.inc)
and the code (under_test.inc).
*/

#include "harness.h"
#include "config.inc"

#include <stdint.h>
#include <strings.h>

#define csstrlen(string) strlen(string)
#define csstrcpy(destination, source) strcpy(destination, source)
#define csstrcasecmp(first, second) strcasecmp(first, second)
#define csmemset(destination, value, size) memset(destination, value, size)
#define csmemcmp(first, second, size) memcmp(first, second, size)

#include "under_test.inc"

static void fake_hash(byte *hash, int seed)
{
	int index;

	for (index = 0; index < MAP_SHARE_HASH_BYTES; index++)
		hash[index] = (byte)(seed * 31 + index * 7);
}

/* an entry written as a line, its line end cut */
static char const *written(struct map_hash const *entry)
{
	static char line[1024];
	FILE *stream = tmpfile();
	size_t length;

	map_hash_line_write(stream, entry);
	rewind(stream);
	length = fread(line, 1, sizeof(line) - 1, stream);
	fclose(stream);
	line[length] = 0;
	CHECK(length >= 2 && line[length - 2] == '\r' && line[length - 1] == '\n', "a line does not end in CR LF");
	line[length - 2] = 0;
	return line;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* a path found again, whatever its case, among more than the first room */
	CASE("table")
	{
		char path[MAP_SHARE_PATH_SIZE];
		int index;

		CHECK(!map_hash_find("d:\\custom_maps\\a.map"), "an empty table found a path");
		for (index = 0; index < MAP_HASHES_FIRST_ROOM * 5; index++)
		{
			struct map_hash *entry;

			sprintf(path, "d:\\custom_maps\\pack%d\\map%d.map", index % 7, index);
			entry = map_hash_add(path);
			CHECK(entry, "no room for the map %d", index);
			entry->size = (unsigned long)index;
		}
		CHECK(map_share_files_globals.hash_count == MAP_HASHES_FIRST_ROOM * 5, "%ld kept, not %d",
			map_share_files_globals.hash_count, MAP_HASHES_FIRST_ROOM * 5);
		CHECK(map_share_files_globals.hash_slot_count >= map_share_files_globals.hash_count * 2,
			"the slots are more than half full");
		for (index = 0; index < MAP_HASHES_FIRST_ROOM * 5; index++)
		{
			struct map_hash *entry;

			sprintf(path, "D:\\CUSTOM_MAPS\\PACK%d\\MAP%d.MAP", index % 7, index);
			entry = map_hash_find(path);
			CHECK(entry && entry->size == (unsigned long)index, "the map %d was not found by its path in capitals",
				index);
		}
		CHECK(map_hash_add("d:\\custom_maps\\pack0\\MAP0.map")->size == 0, "a path added again made a second entry");
		CHECK(map_share_files_globals.hash_count == MAP_HASHES_FIRST_ROOM * 5, "a path added again was counted");
		CHECK(!map_hash_find("d:\\custom_maps\\pack1\\map0.map"), "a map was found in another folder");
		{
			char longer[MAP_SHARE_PATH_SIZE + 1];

			memset(longer, 'a', MAP_SHARE_PATH_SIZE);
			longer[MAP_SHARE_PATH_SIZE] = 0;
			CHECK(!map_hash_add(longer), "a path too long to keep was kept");
			longer[MAP_SHARE_PATH_SIZE - 1] = 0;
			CHECK(map_hash_add(longer), "the longest path kept was not");
		}
		return 0;
	}
	/* a line written is read back the same */
	CASE("lines")
	{
		struct map_hash entry;
		byte hash[MAP_SHARE_HASH_BYTES];
		char path[MAP_SHARE_PATH_SIZE];
		unsigned long size;
		uint64_t time;

		memset(&entry, 0, sizeof(entry));
		strcpy(entry.path, "d:\\custom_maps\\Pack A\\bloodgulch 2.map");
		entry.size = 0xFFFFFFFFUL;
		entry.time = 0x01DB2C3D4E5F6071ULL;
		fake_hash(entry.hash, 3);
		CHECK(map_hash_line_read(written(&entry), hash, &size, &time, path), "a line written was not read: %s",
			written(&entry));
		CHECK(!memcmp(hash, entry.hash, MAP_SHARE_HASH_BYTES), "the hash read is not the one written");
		CHECK(size == entry.size, "the size read is %lu", size);
		CHECK(time == entry.time, "the time read is not the one written");
		CHECK(!strcmp(path, entry.path), "the path read is '%s' (a space in it is the path's)", path);
		entry.size = 0;
		entry.time = 0;
		CHECK(map_hash_line_read(written(&entry), hash, &size, &time, path) && !size && !time,
			"a line of size and time 0 was not read back");
		return 0;
	}
	/* a line that is not one is passed over */
	CASE("bad-lines")
	{
		static char const hex[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
		char const *const bad[] =
		{
			"",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1 5 0000000000000001 d:\\a.map",
			"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F 5 0000000000000001 d:\\a.map",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f  5 0000000000000001 d:\\a.map",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 4294967296 0000000000000001 d:\\a.map",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 99999999999 0000000000000001 d:\\a.map",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 5 000000000000001 d:\\a.map",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 5 0000000000000001",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 5 0000000000000001 ",
			"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f 5x 0000000000000001 d:\\a.map",
		};
		char line[1024];
		byte hash[MAP_SHARE_HASH_BYTES];
		char path[MAP_SHARE_PATH_SIZE];
		unsigned long size;
		uint64_t time;
		size_t index;

		sprintf(line, "%s 5 0000000000000001 d:\\a.map", hex);
		CHECK(map_hash_line_read(line, hash, &size, &time, path) && size == 5 && time == 1 && hash[31] == 0x1f,
			"a good line was not read");
		for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++)
		{
			CHECK(!map_hash_line_read(bad[index], hash, &size, &time, path), "a bad line was read: '%s'", bad[index]);
		}
		/* (a path too long to keep) */
		sprintf(line, "%s 5 0000000000000001 ", hex);
		memset(line + strlen(line), 'a', MAP_SHARE_PATH_SIZE);
		line[strlen(hex) + 20 + MAP_SHARE_PATH_SIZE] = 0;
		CHECK(!map_hash_line_read(line, hash, &size, &time, path), "a path too long to keep was read");
		return 0;
	}

	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}

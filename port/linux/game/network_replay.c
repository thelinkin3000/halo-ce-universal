/*
NETWORK_REPLAY.C

The host's replay recording, phase 0 of port/linux/REPLAY.md: with
network.record_replays on, the host of a network game (a player's or a
dedicated server) writes what a client that sees everything would be sent
to a file in the data folder's replays/, from the game's first tick until
its map is unloaded.

That client is the recorder: a machine slot no client and no bot has
(network_distributed.c's distributed_recorder_machine), with no players, so
the distributed netcode sends it every player every tick and the objects
and actors as often as they are near anyone's players, as it does a client
whose players have never been alive. Its datagrams are written here instead
of sent; so are the distributed messages sent every client reliably, and
the game's own messages to every machine (a player joining or leaving, the
game's end). Nothing else changes: the clients are sent what they were.

A client of the network game records too (network_replay_client_tick):
what it reads of its host (network_replay_record_received: the host's
datagrams to it, its reliable messages and the game's own), and its own
players' input, which the host never sends it (network_replay_own_actions,
kind 6, as the host relays a player's: network_distributed.c). Its replay
is the game as that client had it: the players far from its own and out
of sight less often, and only the events the host sent it.

The records are the bytes as they would go out, after an uncompressed
header through one zlib stream (format 3, zlib's fastest level: about a
fifth of their size), synced every few seconds so that a file a crash cut
short plays to its last sync, and written by a thread of its own (the
game thread only deflates). tools/replay_stats.py says how large they
are and which messages take the room; network_replay_playback.c plays them.
The folder keeps the newest network.replays_kept (the oldest are deleted
as a recording starts).
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"
#include "networking/network_server_manager_internal.h"
#include "game/players.h"
#include "network_distributed.h"
#include "network_replay.h"
#include "../src/posix.h" /* port: port/linux/src/posix.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xtl.h>

#include "zlib.h" /* source/memory/zlib */

int config_boolean(char const *name);
long config_integer(char const *name);
const char *platform_data_root(void);
FILE *platform_fopen_host(char const *path, char const *mode);
int platform_remove_host(char const *path);
int platform_rename_host(char const *old_path, char const *new_path);
void platform_log(char const *format, ...);
void main_build_identity(long *number, char *flavor, size_t flavor_size, char *repository, size_t repository_size,
	char *version, size_t version_size);

/* the file's header (REPLAY.md, "The file"): its first bytes, and its format */
#define REPLAY_MAGIC "OCREPLAY"
#define REPLAY_FORMAT 4
#define REPLAY_HEADER_SIZE 384
/* the build's version text, at the header's end (format 0's 32 bytes cut
"OpenCE Android | local build (<commit>)" short) */
#define REPLAY_VERSION_SIZE 96
/* (format 4) after it: the game's length in ticks (written again as it is
recorded: REPLAY_DURATION_OFFSET), flags (REPLAY_FLAG_CLIENT: a client's
recording), the game's name (the server's, as its players see it) and the
recording player's, in UTF-8 */
#define REPLAY_DURATION_OFFSET 280
#define REPLAY_FLAGS_OFFSET 284
#define REPLAY_SERVER_OFFSET 288
#define REPLAY_SERVER_SIZE 64
#define REPLAY_PLAYER_OFFSET 352
#define REPLAY_PLAYER_SIZE 32
#define REPLAY_FLAG_CLIENT 1
/* a message's type (message_header.h): the distributed netcode's, and the
game's own */
#define REPLAY_MESSAGE_TYPE_DATA 2
#define REPLAY_MESSAGE_TYPE_PACKET 3
/* a record's header: its kind, a byte left 0, its size and the tick */
#define REPLAY_RECORD_HEADER_SIZE 8
/* a recording shorter than this (ticks) is deleted as it ends */
#define REPLAY_MINIMUM_TICKS (15 * TICKS_PER_SECOND)
/* the file written out this often (ticks): what a crash loses */
#define REPLAY_FLUSH_TICKS (5 * TICKS_PER_SECOND)
/* the file's buffer: on the Switch each write waits for the card, so
fewer, larger ones */
#define REPLAY_BUFFER_SIZE 0x10000
/* what the stream has made, kept until the next flush hands it to the
writer: a few seconds' are some tens of KB; past the most, the recording
stops rather than take the memory */
#define REPLAY_PENDING_MINIMUM 0x40000
#define REPLAY_PENDING_MAXIMUM 0x4000000
#define REPLAY_PATH_SIZE 512
/* a replay's file name ends so */
#define REPLAY_EXTENSION ".ocreplay"

static struct
{
	FILE *file;
	/* the game's first tick has been and network.record_replays was read
	then: a game is recorded whole or not at all, so the setting turned on
	in a game applies from the next */
	boolean considered;
	/* the recording is a client's (network_replay_client_tick) */
	boolean client;
	long start_time;
	long last_flush_time;
	char path[REPLAY_PATH_SIZE];
	/* what was written, for the log at its end */
	unsigned long records[NUMBER_OF_NETWORK_REPLAY_RECORDS];
	unsigned long bytes[NUMBER_OF_NETWORK_REPLAY_RECORDS];
	unsigned long total_bytes;
	char buffer[REPLAY_BUFFER_SIZE];
	/* the records' stream (format 3), and what it has made not yet given to
	the writer */
	z_stream stream;
	byte *pending;
	unsigned long pending_size;
	unsigned long pending_capacity;
} replay;

/* The file is written by a thread of its own: each write waits for the
card, and on the Switch a flush (the stream's bytes, the duration in the
header, the seeks between) cost the game thread 40 to 110 ms every five
seconds, a dropped frame each time in every network game. The game thread
deflates into replay.pending and hands that over at a flush; the writer
writes it while the game goes on. Without the thread the hand-over writes
it there and then, as before. */
static struct
{
	/* work: something to write; idle: the writer has nothing (manual reset) */
	HANDLE thread, work, idle;
	FILE *file;
	byte *data;
	unsigned long size;
	unsigned long capacity;
	/* the game's length so far, for the header (REPLAY_DURATION_OFFSET) */
	unsigned long duration;
	/* a write failed: the recording stops at the next hand-over */
	boolean failed;
} replay_writer = { 0 };

static void replay_put_long(
	byte *destination,
	unsigned long value);

static void replay_writer_write(
	void)
{
	byte duration[4];
	boolean written = !replay_writer.size ||
		fwrite(replay_writer.data, replay_writer.size, 1, replay_writer.file) == 1;

	/* (its length in the header, for a file a crash cuts short) */
	replay_put_long(duration, replay_writer.duration);
	if (written && fseek(replay_writer.file, REPLAY_DURATION_OFFSET, SEEK_SET) == 0)
		fwrite(duration, sizeof(duration), 1, replay_writer.file);
	if (written)
		written = fseek(replay_writer.file, 0, SEEK_END) == 0 && fflush(replay_writer.file) == 0;
	replay_writer.size = 0;
	if (!written)
		replay_writer.failed = TRUE;
}

static DWORD WINAPI replay_writer_main(
	LPVOID unused)
{
	(void)unused;
	for (;;)
	{
		WaitForSingleObject(replay_writer.work, INFINITE);
		replay_writer_write();
		SetEvent(replay_writer.idle);
	}
	return 0;
}

/* the writer started; FALSE if it cannot be, and the hand-over writes */
static boolean replay_writer_ready(
	void)
{
	if (replay_writer.thread)
		return TRUE;
	replay_writer.work = CreateEventA(NULL, FALSE, FALSE, NULL);
	replay_writer.idle = CreateEventA(NULL, TRUE, TRUE, NULL);
	if (replay_writer.work && replay_writer.idle)
		replay_writer.thread = CreateThread(NULL, 0, replay_writer_main, NULL, 0, NULL);
	if (!replay_writer.thread)
	{
		if (replay_writer.work)
			CloseHandle(replay_writer.work);
		if (replay_writer.idle)
			CloseHandle(replay_writer.idle);
		replay_writer.work = replay_writer.idle = NULL;
		return FALSE;
	}
	return TRUE;
}

/* until the writer has written what it was given: then the file is the
game thread's again */
static void replay_writer_wait(
	void)
{
	if (replay_writer.thread)
		WaitForSingleObject(replay_writer.idle, INFINITE);
}

/* the writer still writing what it was given last */
static boolean replay_writer_busy(
	void)
{
	return replay_writer.thread && WaitForSingleObject(replay_writer.idle, 0) == WAIT_TIMEOUT;
}

/* what the stream has made, to the writer (the buffers swapped, nothing
copied), with the game's length; FALSE if an earlier write failed */
static boolean replay_hand_off(
	void)
{
	byte *data;
	unsigned long capacity;

	replay_writer_wait();
	if (replay_writer.failed)
		return FALSE;
	data = replay_writer.data;
	capacity = replay_writer.capacity;
	replay_writer.data = replay.pending;
	replay_writer.capacity = replay.pending_capacity;
	replay_writer.size = replay.pending_size;
	replay_writer.file = replay.file;
	replay_writer.duration = (unsigned long)(game_time_get() - replay.start_time);
	replay.pending = data;
	replay.pending_capacity = capacity;
	replay.pending_size = 0;
	if (replay_writer_ready())
	{
		ResetEvent(replay_writer.idle);
		SetEvent(replay_writer.work);
	}
	else
		replay_writer_write();
	return TRUE;
}

/* the recording given up: the file closed once the writer is done with it */
static void replay_abandon(
	void)
{
	error(_error_log, "replay: could not write to %s; the recording stops", replay.path);
	replay_writer_wait();
	replay_writer.failed = FALSE;
	deflateEnd(&replay.stream);
	fclose(replay.file);
	replay.file = NULL;
	replay.pending_size = 0;
}

/* room for at least another REPLAY_BUFFER_SIZE of the stream's bytes */
static boolean replay_pending_grow(
	void)
{
	unsigned long capacity = replay.pending_capacity ? replay.pending_capacity * 2 : REPLAY_PENDING_MINIMUM;
	byte *pending;

	if (replay.pending_capacity - replay.pending_size >= REPLAY_BUFFER_SIZE)
		return TRUE;
	if (capacity > REPLAY_PENDING_MAXIMUM || !(pending = realloc(replay.pending, capacity)))
		return FALSE;
	replay.pending = pending;
	replay.pending_capacity = capacity;
	return TRUE;
}

/* through the stream into replay.pending: flush Z_NO_FLUSH, Z_SYNC_FLUSH
(all of it made, readable once written) or Z_FINISH; FALSE if the stream
failed or the bytes have no room */
static boolean replay_deflate(
	void const *data,
	unsigned long size,
	int flush)
{
	replay.stream.next_in = (Bytef *)data;
	replay.stream.avail_in = (uInt)size;
	for (;;)
	{
		int result;
		unsigned long room;

		if (!replay_pending_grow())
			return FALSE;
		room = replay.pending_capacity - replay.pending_size;
		replay.stream.next_out = replay.pending + replay.pending_size;
		replay.stream.avail_out = (uInt)room;
		result = deflate(&replay.stream, flush);
		replay.pending_size += room - replay.stream.avail_out;
		if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
			return FALSE;
		/* (done: everything taken, and for a flush all of it made) */
		if (result == Z_STREAM_END || (!replay.stream.avail_in && replay.stream.avail_out))
			return TRUE;
	}
}

/* the replays past the newest `kept` deleted (0 keeps them all) */
static void replay_prune(
	long kept)
{
	static char names[256][NETWORK_REPLAY_NAME_SIZE];
	long count;
	long index;

	if (kept <= 0)
		return;
	count = network_replay_list(names, NUMBEROF(names));
	for (index = kept; index < count; index++)
	{
		if (network_replay_delete(names[index]))
			error(_error_log, "replay: %s deleted (network.replays_kept is %ld)", names[index], kept);
	}
}

boolean network_replay_delete(
	char const *name)
{
	char path[REPLAY_PATH_SIZE + NETWORK_REPLAY_NAME_SIZE];

	/* (a name of the folder's, no further) */
	if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\'))
		return FALSE;
	network_replay_folder(path, REPLAY_PATH_SIZE);
	snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", name);
	/* (the path the card's, as it is: DeleteFileA, and the game's remove,
	take the game's, and put this one under the data folder again, where
	there is no such file) */
	return platform_remove_host(path) == 0;
}

static unsigned long replay_get_long(
	byte const *source)
{
	return (unsigned long)source[0] | (unsigned long)source[1] << 8 | (unsigned long)source[2] << 16 |
		(unsigned long)source[3] << 24;
}

static void replay_put_long(
	byte *destination,
	unsigned long value)
{
	destination[0] = (byte)value;
	destination[1] = (byte)(value >> 8);
	destination[2] = (byte)(value >> 16);
	destination[3] = (byte)(value >> 24);
}

/* the text, in printable ASCII, a file name's letters only (others '_') when
for_name */
static void replay_printable(
	char *destination,
	long size,
	char const *source,
	boolean for_name)
{
	long index;

	for (index = 0; index < size - 1 && source[index]; index++)
	{
		char character = source[index];
		boolean kept = for_name ?
			(character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				(character >= '0' && character <= '9') || character == '-' :
			character >= ' ' && character < 127;

		destination[index] = kept ? character : '_';
	}
	destination[index] = 0;
}

/* the game's text (UTF-16, as the game has names) in UTF-8, at most size
bytes with its end */
static void replay_utf8(
	char *destination,
	long size,
	wchar_t const *source,
	long length)
{
	long used = 0;
	long index;

	for (index = 0; index < length && source[index]; index++)
	{
		unsigned long character = (unsigned short)source[index];
		char bytes[3];
		long count;

		if (character < 0x80)
		{
			bytes[0] = (char)character;
			count = 1;
		}
		else if (character < 0x800)
		{
			bytes[0] = (char)(0xC0 | (character >> 6));
			bytes[1] = (char)(0x80 | (character & 0x3F));
			count = 2;
		}
		else
		{
			bytes[0] = (char)(0xE0 | (character >> 12));
			bytes[1] = (char)(0x80 | ((character >> 6) & 0x3F));
			bytes[2] = (char)(0x80 | (character & 0x3F));
			count = 3;
		}
		if (used + count >= size)
			break;
		csmemcpy(destination + used, bytes, (size_t)count);
		used += count;
	}
	destination[used] = 0;
}

static boolean replay_start(
	boolean client)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game_client *game_client = global_network_game_client_get();
	struct network_game *game = client ? (game_client ? network_game_client_get_game(game_client) : NULL) :
		server ? network_game_server_get_game(server) : NULL;
	byte header[REPLAY_HEADER_SIZE];
	char map[0x80] = "";
	char map_for_name[48];
	char variant[16] = "";
	char flavor[32], repository[64], version[REPLAY_VERSION_SIZE];
	char when[32] = "unknown";
	char const *base_name;
	long number = 0;
	time_t now = time(NULL);
	struct tm *local = localtime(&now);
	short index;

	if (!game)
		return FALSE;
	/* (room for this one: as many as network.replays_kept, with it) */
	replay_prune(config_integer("network.replays_kept") - 1);
	csmemset(header, 0, sizeof(header));
	replay_printable(map, sizeof(map), game->map.name, FALSE);
	for (index = 0; index < 11 && game->variant.human_readable_game_description[index]; index++)
		variant[index] = player_name_character_ascii(game->variant.human_readable_game_description[index]);
	variant[index] = 0;
	main_build_identity(&number, flavor, sizeof(flavor), repository, sizeof(repository), version, sizeof(version));
	/* (the map's name past its folders, as levels\test\bloodgulch\bloodgulch) */
	base_name = strrchr(map, '\\');
	base_name = base_name ? base_name + 1 : map;
	replay_printable(map_for_name, sizeof(map_for_name), base_name, TRUE);
	if (local)
		strftime(when, sizeof(when), "%Y%m%d-%H%M%S", local);
	network_replay_folder(replay.path, sizeof(replay.path));
	posix_make_directory(replay.path);
	snprintf(replay.path + strlen(replay.path), sizeof(replay.path) - strlen(replay.path), "/%s_%s%s", when,
		map_for_name[0] ? map_for_name : "map", REPLAY_EXTENSION);
	replay.file = platform_fopen_host(replay.path, "wb");
	if (!replay.file)
	{
		error(_error_log, "replay: could not open %s to record the game", replay.path);
		return FALSE;
	}
	setvbuf(replay.file, replay.buffer, _IOFBF, sizeof(replay.buffer));
	csmemcpy(header, REPLAY_MAGIC, 8);
	replay_put_long(header + 8, REPLAY_FORMAT);
	replay_put_long(header + 12, HALO_PORT_NETWORK_VERSION);
	replay_put_long(header + 16, (unsigned long)number);
	replay_put_long(header + 20, TICKS_PER_SECOND);
	replay_put_long(header + 24, (unsigned long)game_time_get());
	replay_put_long(header + 28, REPLAY_HEADER_SIZE);
	replay_put_long(header + 32, (unsigned long)now);
	replay_put_long(header + 36, (unsigned long)((unsigned long long)now >> 32));
	csmemcpy(header + 40, map, MIN(sizeof(map), 128));
	csmemcpy(header + 168, variant, sizeof(variant));
	replay_printable((char *)header + 184, REPLAY_VERSION_SIZE, version, FALSE);
	/* (format 4: who recorded it, and where; its length as it goes) */
	replay_put_long(header + REPLAY_FLAGS_OFFSET, client ? REPLAY_FLAG_CLIENT : 0);
	replay_utf8((char *)header + REPLAY_SERVER_OFFSET, REPLAY_SERVER_SIZE, game->name, NUMBEROF(game->name));
	{
		/* (this machine's first player: a dedicated server has none) */
		long player_index = local_player_get_player_index(local_player_get_next(NONE));
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

		if (player)
			replay_utf8((char *)header + REPLAY_PLAYER_OFFSET, REPLAY_PLAYER_SIZE, player->name, NUMBEROF(player->name));
	}
	csmemset(&replay.stream, 0, sizeof(replay.stream));
	if (fwrite(header, sizeof(header), 1, replay.file) != 1 || deflateInit(&replay.stream, 1) != Z_OK)
	{
		error(_error_log, "replay: could not write to %s", replay.path);
		fclose(replay.file);
		replay.file = NULL;
		return FALSE;
	}
	replay.start_time = game_time_get();
	replay.last_flush_time = replay.start_time;
	csmemset(replay.records, 0, sizeof(replay.records));
	csmemset(replay.bytes, 0, sizeof(replay.bytes));
	replay.total_bytes = REPLAY_HEADER_SIZE;
	/* (format 2: the game itself first, which a playback loads) */
	network_replay_record(_network_replay_game, game, sizeof(*game));
	if (!replay.file)
		return FALSE;
	error(_error_log, "replay: recording %s (%s) to %s", map, variant, replay.path);
	return TRUE;
}

void network_replay_folder(
	char *path,
	long size)
{
	snprintf(path, (size_t)size, "%s/replays", platform_data_root());
}

void network_replay_summary(
	long *count,
	unsigned long long *bytes)
{
	char folder[REPLAY_PATH_SIZE];
	char name[256];
	void *directory;

	*count = 0;
	*bytes = 0;
	network_replay_folder(folder, sizeof(folder));
	directory = posix_directory_open(folder);
	while (directory && posix_directory_next(directory, name, sizeof(name)))
	{
		size_t length = strlen(name);
		char path[REPLAY_PATH_SIZE + 256];
		struct posix_file_information information;

		if (length <= strlen(REPLAY_EXTENSION) ||
			strcmp(name + length - strlen(REPLAY_EXTENSION), REPLAY_EXTENSION))
		{
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", folder, name);
		if (posix_stat(path, &information) != 0 || (information.flags & _posix_file_is_directory))
			continue;
		(*count)++;
		*bytes += ((unsigned long long)information.size_high << 32) | information.size_low;
	}
	if (directory)
		posix_directory_close(directory);
}

long network_replay_list(
	char (*names)[NETWORK_REPLAY_NAME_SIZE],
	long maximum)
{
	char folder[REPLAY_PATH_SIZE];
	char name[256];
	void *directory;
	long count = 0;

	network_replay_folder(folder, sizeof(folder));
	directory = posix_directory_open(folder);
	while (directory && posix_directory_next(directory, name, sizeof(name)))
	{
		size_t length = strlen(name);
		long place;

		if (length >= NETWORK_REPLAY_NAME_SIZE || length <= strlen(REPLAY_EXTENSION) ||
			strcmp(name + length - strlen(REPLAY_EXTENSION), REPLAY_EXTENSION))
		{
			continue;
		}
		/* (in order, newest first: an insertion, the folder's few) */
		for (place = count; place > 0 && strcmp(names[place - 1], name) < 0; place--)
		{
			if (place < maximum)
				strcpy(names[place], names[place - 1]);
		}
		if (place < maximum)
		{
			strcpy(names[place], name);
			if (count < maximum)
				count++;
		}
	}
	if (directory)
		posix_directory_close(directory);
	return count;
}

boolean network_replay_info(
	char const *name,
	struct network_replay_info *info)
{
	char path[REPLAY_PATH_SIZE + NETWORK_REPLAY_NAME_SIZE];
	byte header[REPLAY_HEADER_SIZE];
	struct posix_file_information information;
	unsigned long format, network_version, header_size;
	char map[0x80];
	char const *base_name;
	FILE *file;
	size_t read;

	csmemset(info, 0, sizeof(*info));
	network_replay_folder(path, sizeof(path));
	snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", name);
	if (posix_stat(path, &information) == 0)
		info->bytes = ((unsigned long long)information.size_high << 32) | information.size_low;
	file = platform_fopen_host(path, "rb");
	read = file ? fread(header, 1, sizeof(header), file) : 0;
	if (file)
		fclose(file);
	if (read < 216 || memcmp(header, REPLAY_MAGIC, 8))
	{
		snprintf(info->why, sizeof(info->why), "Not a replay.");
		return FALSE;
	}
	info->valid = TRUE;
	format = replay_get_long(header + 8);
	network_version = replay_get_long(header + 12);
	header_size = replay_get_long(header + 28);
	csmemcpy(map, header + 40, sizeof(map));
	map[sizeof(map) - 1] = 0;
	base_name = strrchr(map, '\\');
	base_name = base_name ? base_name + 1 : map;
	snprintf(info->map, sizeof(info->map), "%s", base_name);
	csmemcpy(info->variant, header + 168, sizeof(info->variant) - 1);
	info->variant[sizeof(info->variant) - 1] = 0;
	/* (when: the header's, else the name's first fifteen characters,
	YYYYMMDD-HHMMSS) */
	{
		time_t when = (time_t)replay_get_long(header + 32);
		struct tm *local = when ? localtime(&when) : NULL;

		if (local)
		{
			strftime(info->date, sizeof(info->date), "%Y-%m-%d %H:%M", local);
			strftime(info->short_date, sizeof(info->short_date), "%m-%d %H:%M", local);
		}
		else
		{
			snprintf(info->date, sizeof(info->date), "%.4s-%.2s-%.2s %.2s:%.2s", name, name + 4, name + 6, name + 9,
				name + 11);
			snprintf(info->short_date, sizeof(info->short_date), "%.2s-%.2s %.2s:%.2s", name + 4, name + 6, name + 9,
				name + 11);
		}
	}
	/* (format 4: its length, where and by whom) */
	if (format >= 4 && header_size >= REPLAY_HEADER_SIZE && read >= REPLAY_HEADER_SIZE)
	{
		info->seconds = (long)(replay_get_long(header + REPLAY_DURATION_OFFSET) / TICKS_PER_SECOND);
		csmemcpy(info->server, header + REPLAY_SERVER_OFFSET, REPLAY_SERVER_SIZE);
		info->server[REPLAY_SERVER_SIZE] = 0;
		csmemcpy(info->player, header + REPLAY_PLAYER_OFFSET, REPLAY_PLAYER_SIZE);
		info->player[REPLAY_PLAYER_SIZE] = 0;
		info->client = (replay_get_long(header + REPLAY_FLAGS_OFFSET) & REPLAY_FLAG_CLIENT) != 0;
		info->has_metadata = TRUE;
	}
	if (format < 2 || header_size < 216)
	{
		snprintf(info->why, sizeof(info->why), "Recorded before replays could be played back: it cannot be watched.");
		return FALSE;
	}
	if (!network_replay_network_version_playable(network_version))
	{
		snprintf(info->why, sizeof(info->why), "Recorded by a build of network version %lu: this one plays %d.",
			network_version, HALO_PORT_NETWORK_VERSION);
		return FALSE;
	}
	info->playable = TRUE;
	return TRUE;
}

void network_replay_info_text(
	struct network_replay_info const *info,
	char *text,
	long size)
{
	if (!info->valid)
	{
		snprintf(text, (size_t)size, "%s", info->why);
		return;
	}
	snprintf(text, (size_t)size, "%s, %s\n%s", info->map, info->variant[0] ? info->variant : "?", info->date);
	if (info->has_metadata)
	{
		if (info->seconds)
		{
			snprintf(text + strlen(text), (size_t)size - strlen(text), "\n%ld:%02ld long", info->seconds / 60,
				info->seconds % 60);
		}
		if (info->server[0])
			snprintf(text + strlen(text), (size_t)size - strlen(text), "\nServer: %s", info->server);
		if (info->player[0])
			snprintf(text + strlen(text), (size_t)size - strlen(text), "\n%s by %s", info->client ? "Played" : "Hosted", info->player);
		else
			snprintf(text + strlen(text), (size_t)size - strlen(text), "\nRecorded by the server");
	}
	snprintf(text + strlen(text), (size_t)size - strlen(text), "\n%llu KB", info->bytes / 1024);
	if (info->why[0])
		snprintf(text + strlen(text), (size_t)size - strlen(text), "\n%s", info->why);
}

boolean network_replay_describe(
	char const *name,
	char *text,
	long size)
{
	struct network_replay_info info;
	boolean playable = network_replay_info(name, &info);

	network_replay_info_text(&info, text, size);
	return playable;
}

boolean network_replay_recording(
	void)
{
	return replay.file != NULL && !replay.client;
}

boolean network_replay_recording_as_client(
	void)
{
	return replay.file != NULL && replay.client;
}

void network_replay_client_tick(
	void)
{
	if (replay.file)
	{
		/* (written out every few seconds, as the host's) */
		replay.client = TRUE;
		network_replay_host_tick();
		return;
	}
	/* (not a replay's playback, which is a client too) */
	if (replay.considered || network_replay_playing())
		return;
	replay.considered = TRUE;
	if (config_boolean("network.record_replays"))
	{
		replay.client = TRUE;
		if (!replay_start(TRUE))
			replay.client = FALSE;
	}
}

void network_replay_record_received(
	void const *message,
	long size,
	boolean unreliable)
{
	word header;

	if (!replay.file || !replay.client || size < (long)sizeof(word))
		return;
	csmemcpy(&header, message, sizeof(header));
	switch (GET_MESSAGE_TYPE(header))
	{
	case REPLAY_MESSAGE_TYPE_DATA:
		network_replay_record(unreliable ? _network_replay_datagram : _network_replay_reliable, message, size);
		break;
	case REPLAY_MESSAGE_TYPE_PACKET:
		/* (the game's own, over the connection: not a datagram's
		advertisement or pong) */
		if (!unreliable)
			network_replay_record(_network_replay_game_message, message, size);
		break;
	}
}

void network_replay_host_tick(
	void)
{
	if (replay.file)
	{
		/* (not while the writer is still at the last flush: the card can
		stall for seconds (d20 saw 3.4 s), and the hand-over would wait out
		the stall on the game thread. The stream goes on into
		replay.pending, and is handed over a tick after the writer is done) */
		if (game_time_get() - replay.last_flush_time >= REPLAY_FLUSH_TICKS && !replay_writer_busy())
		{
			replay.last_flush_time = game_time_get();
			/* (written by the writer, the game going on meanwhile) */
			if (!replay_deflate(NULL, 0, Z_SYNC_FLUSH) || !replay_hand_off())
				replay_abandon();
		}
		return;
	}
	if (replay.considered)
		return;
	replay.considered = TRUE;
	if (config_boolean("network.record_replays"))
	{
		replay.client = FALSE;
		replay_start(FALSE);
	}
}

void network_replay_record(
	byte kind,
	void const *data,
	long size)
{
	byte header[REPLAY_RECORD_HEADER_SIZE];

	if (!replay.file || size < 0 || size > 0xFFFF || kind <= 0 || kind >= NUMBER_OF_NETWORK_REPLAY_RECORDS)
		return;
	header[0] = kind;
	header[1] = 0;
	header[2] = (byte)size;
	header[3] = (byte)(size >> 8);
	replay_put_long(header + 4, (unsigned long)game_time_get());
	if (!replay_deflate(header, sizeof(header), Z_NO_FLUSH) || (size && !replay_deflate(data, (unsigned long)size, Z_NO_FLUSH)))
	{
		replay_abandon();
		return;
	}
	replay.records[kind]++;
	replay.bytes[kind] += (unsigned long)size;
	replay.total_bytes += REPLAY_RECORD_HEADER_SIZE + (unsigned long)size;
}

void network_replay_stop(
	void)
{
	if (replay.file)
	{
		long ticks = game_time_get() - replay.start_time;
		boolean written = replay_deflate(NULL, 0, Z_FINISH) && replay_hand_off();
		long file_size;

		deflateEnd(&replay.stream);
		/* (the last of it written before the file is closed) */
		replay_writer_wait();
		written = written && !replay_writer.failed;
		replay_writer.failed = FALSE;
		replay.pending_size = 0;
		file_size = ftell(replay.file);
		written = fclose(replay.file) == 0 && written;
		replay.file = NULL;
		/* (a game of a few seconds, left at once, is no replay) */
		if (ticks < REPLAY_MINIMUM_TICKS)
		{
			error(_error_log, "replay: %s was %ld ticks long, under %d seconds: %s", replay.path, ticks,
				REPLAY_MINIMUM_TICKS / TICKS_PER_SECOND, platform_remove_host(replay.path) == 0 ? "deleted" : "NOT deleted");
			replay.considered = FALSE;
			replay.client = FALSE;
			return;
		}
		error(_error_log,
			"replay: %s %s: %ld seconds, %ld KB (%lu KB before compression: %lu datagrams %lu KB, %lu reliable %lu KB, "
			"%lu to the recorder %lu KB, %lu game messages %lu KB)",
			written ? "recorded" : "could not finish", replay.path, ticks / TICKS_PER_SECOND,
			file_size / 1024, replay.total_bytes / 1024,
			replay.records[_network_replay_datagram], replay.bytes[_network_replay_datagram] / 1024,
			replay.records[_network_replay_reliable], replay.bytes[_network_replay_reliable] / 1024,
			replay.records[_network_replay_reliable_to_machine], replay.bytes[_network_replay_reliable_to_machine] / 1024,
			replay.records[_network_replay_game_message], replay.bytes[_network_replay_game_message] / 1024);
	}
	replay.considered = FALSE;
	replay.client = FALSE;
}

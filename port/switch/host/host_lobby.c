/*
HOST_LOBBY.C

What the Switch port asks before the game starts: to play as the game always
has (the campaign, and system link games on the local network), or to join
one of the internet games the community list publishes
(https://halo.milenko.org/api, GET /v1/games.txt).

The console cannot host a listed game yet, so the list is only for joining.
The joining is the game's own: the chosen game's invite becomes the guest's
one command line argument (posix_command_line_argument, host_net.c), which
p2p.c reads as soon as internet play starts (command_line_invite), the way
the desktop builds read a halo://join/ link they were opened with. Nothing in
the guest knows the list exists.

The list is fetched over HTTPS - the site answers plain HTTP with a redirect
- through the console's own TLS (host_https.c). Before the menu, the updater
(host_update.c) offers a newer release of the port if there is one.

The menu is libnx's text console, drawn on the default window before SDL takes
that window for the game; consoleExit gives it back. The console takes over
stdout only (the log is on stderr and in the file), and nothing may be printed
to stdout once it has gone.
*/

#include "host.h"

#include "../../linux/include/halo_port_limits.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#define LOBBY_HOST "halo.milenko.org"
#define LOBBY_PATH "/v1/games.txt"

enum
{
	MAXIMUM_GAMES = 64,
	/* a few hundred games' lines; the list holds a handful */
	MAXIMUM_RESPONSE_SIZE = 128 * 1024,
	INVITE_DIGITS = 64,
	/* the console is 80 columns by 45 rows at 1280x720 */
	VISIBLE_GAMES = 32,
};

struct lobby_game
{
	char invite[INVITE_DIGITS + 1];
	char name[25];
	char map[17];
	int engine;
	int players;
	int maximum_players;
};

static struct lobby_game games[MAXIMUM_GAMES];
static int game_count;
/* the games the list held that this build cannot join (a closed game, or
another network version) */
static int games_left_out;

static char join_link[16 + INVITE_DIGITS + 1];

const char *host_join_link(void)
{
	return join_link[0] ? join_link : NULL;
}


/* ---------- fetching the list (host_https.c) */

static int fail(char *error, size_t error_size, const char *format, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_WARN, "lobby: %s", error);
	return 0;
}

struct list_buffer
{
	char *text;
	size_t length;
};

static int list_body(void *context, const void *data, size_t size, long long total)
{
	struct list_buffer *buffer = context;

	(void)total;
	if (buffer->length + size >= MAXIMUM_RESPONSE_SIZE)
		return 0;
	memcpy(buffer->text + buffer->length, data, size);
	buffer->length += size;
	return 1;
}

/* the list, null-terminated, in response (MAXIMUM_RESPONSE_SIZE); 0 with
error said */
static int fetch_the_list(char *response, char *error, size_t error_size)
{
	struct list_buffer buffer = { response, 0 };
	int status = host_https_get("https://" LOBBY_HOST LOBBY_PATH, 1, list_body, &buffer, NULL, 0, error,
		error_size);

	if (status < 0)
		return 0;
	if (status == 429)
		return fail(error, error_size, "The list is busy (too many requests). Try again in a minute.");
	if (status != 200)
		return fail(error, error_size, "The list answered with error %d.", status);
	response[buffer.length] = 0;
	return 1;
}

/* ---------- reading it

One game a line, its fields split by tabs: invite, name, map, engine,
players, maximum_players, open, version, age, score_limit, teams, roster. */

/* text for the console, which draws ASCII: a character outside it is one
'?', however many bytes UTF-8 took */
static void copy_printable(char *destination, size_t size, const char *source)
{
	size_t length = 0;

	for (; *source && length + 1 < size; source++)
	{
		unsigned char character = (unsigned char)*source;

		if (character >= 0x80 && character < 0xC0)
			continue;
		destination[length++] = character >= 0x20 && character < 0x7F ? (char)character : '?';
	}
	destination[length] = 0;
}

static int is_invite(const char *text)
{
	int index;

	for (index = 0; index < INVITE_DIGITS; index++)
	{
		char character = text[index];

		if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
			(character >= 'A' && character <= 'F')))
			return 0;
	}
	return text[INVITE_DIGITS] == 0;
}

static void read_the_list(char *body)
{
	char *line = body;

	game_count = 0;
	games_left_out = 0;
	while (line && *line)
	{
		char *next = strchr(line, '\n');
		char *fields[12];
		int field_count = 0;
		char *cursor = line;

		if (next)
			*next++ = 0;
		if (*line && line[strlen(line) - 1] == '\r')
			line[strlen(line) - 1] = 0;
		while (field_count < 12)
		{
			fields[field_count++] = cursor;
			cursor = strchr(cursor, '\t');
			if (!cursor)
				break;
			*cursor++ = 0;
		}
		line = next;
		if (field_count < 8 || !is_invite(fields[0]))
			continue;
		if (atoi(fields[6]) != 1 || atoi(fields[7]) != HALO_PORT_NETWORK_VERSION || game_count == MAXIMUM_GAMES)
		{
			games_left_out++;
			continue;
		}
		{
			struct lobby_game *game = &games[game_count++];
			const char *map = strrchr(fields[2], '\\');

			memcpy(game->invite, fields[0], INVITE_DIGITS + 1);
			copy_printable(game->name, sizeof(game->name), fields[1]);
			copy_printable(game->map, sizeof(game->map), map ? map + 1 : fields[2]);
			game->engine = atoi(fields[3]);
			game->players = atoi(fields[4]);
			game->maximum_players = atoi(fields[5]);
		}
	}
}

static int refresh(char *error, size_t error_size)
{
	char *response = malloc(MAXIMUM_RESPONSE_SIZE);
	int fetched;

	if (!response)
		return fail(error, error_size, "Out of memory for the list.");
	fetched = fetch_the_list(response, error, error_size);
	if (fetched)
	{
		read_the_list(response);
		host_logf(HOST_LOG_INFO, "lobby: %d games to join (%d left out)", game_count, games_left_out);
	}
	free(response);
	return fetched;
}

/* ---------- the menu */

static const char *engine_name(int engine)
{
	switch (engine)
	{
	case 1: return "CTF";
	case 2: return "Slayer";
	case 3: return "Oddball";
	case 4: return "King";
	case 5: return "Race";
	default: return "?";
	}
}

/* the console's colours: reverse video for the selected line */
#define SELECTED "\x1b[7m"
#define PLAIN "\x1b[0m"
#define DIM "\x1b[2m"

static void draw_choice(int selected)
{
	consoleClear();
	printf("\x1b[3;3HHalo: Combat Evolved");
	printf("\x1b[6;3H%s Campaign and system link  " PLAIN, selected == 0 ? SELECTED : "");
	printf("\x1b[7;3H%s Internet games            " PLAIN, selected == 1 ? SELECTED : "");
	printf("\x1b[10;3H" DIM "Internet games are the ones listed at %s." PLAIN, LOBBY_HOST);
	printf("\x1b[43;3HA select   + quit");
}

static void draw_message(const char *title, const char *message, const char *keys)
{
	consoleClear();
	printf("\x1b[3;3H%s", title);
	printf("\x1b[6;3H%.76s", message);
	if (strlen(message) > 76)
		printf("\x1b[7;3H%.76s", message + 76);
	printf("\x1b[43;3H%s", keys);
}

static void draw_list(int selected, int first)
{
	int row;

	consoleClear();
	printf("\x1b[3;3HInternet games");
	printf("\x1b[3;60H%3d to join", game_count);
	printf("\x1b[5;3H" DIM "  %-24s %-16s %-8s %s" PLAIN, "Name", "Map", "Type", "Players");
	for (row = 0; row < VISIBLE_GAMES && first + row < game_count; row++)
	{
		const struct lobby_game *game = &games[first + row];

		printf("\x1b[%d;3H%s  %-24s %-16s %-8s %3d/%-3d " PLAIN, 6 + row, first + row == selected ? SELECTED : "",
			game->name, game->map, engine_name(game->engine), game->players, game->maximum_players);
	}
	if (!game_count)
		printf("\x1b[7;5HNo games this build can join are being hosted right now.");
	if (games_left_out)
		printf("\x1b[40;3H" DIM "%d more listed games are full or need another version." PLAIN, games_left_out);
	printf("\x1b[43;3HA join   Y refresh   B back");
	/* the list is Milenko's, run for the community alongside ChupaThingyCe */
	printf("\x1b[45;3H" DIM "Thanks Milenko! Go check out ChupaThingyCe!" PLAIN);
}

static void quit_from_the_menu(void)
{
	host_logf(HOST_LOG_INFO, "lobby: quit from the menu");
	consoleExit(NULL);
	host_exit(0);
}

/* waits for a press, or for the console to ask the program to close */
static u64 wait_for_buttons(PadState *pad)
{
	for (;;)
	{
		u64 down;

		if (!appletMainLoop())
			quit_from_the_menu();
		padUpdate(pad);
		down = padGetButtonsDown(pad);
		if (down & HidNpadButton_Plus)
			quit_from_the_menu();
		if (down)
			return down;
		consoleUpdate(NULL);
	}
}

/* the internet games list; returns 1 once a game is chosen, 0 to go back */
static int choose_a_game(PadState *pad)
{
	char error[256];
	int selected = 0, first = 0;

	for (;;)
	{
		u64 down;

		draw_message("Internet games", "Fetching the list from " LOBBY_HOST "...", "");
		consoleUpdate(NULL);
		if (!refresh(error, sizeof(error)))
		{
			draw_message("Internet games", error, "Y try again   B back");
			do
				down = wait_for_buttons(pad);
			while (!(down & (HidNpadButton_Y | HidNpadButton_B)));
			if (down & HidNpadButton_B)
				return 0;
			continue;
		}
		selected = 0;
		first = 0;
		for (;;)
		{
			draw_list(selected, first);
			down = wait_for_buttons(pad);
			if (down & HidNpadButton_B)
				return 0;
			if (down & HidNpadButton_Y)
				break;
			if ((down & HidNpadButton_A) && game_count)
			{
				const struct lobby_game *game = &games[selected];

				snprintf(join_link, sizeof(join_link), "halo://join/%s", game->invite);
				host_logf(HOST_LOG_INFO, "lobby: joining %s (%s, %s, %d/%d)", game->name, game->map,
					engine_name(game->engine), game->players, game->maximum_players);
				return 1;
			}
			if ((down & HidNpadButton_AnyDown) && selected + 1 < game_count)
				selected++;
			if ((down & HidNpadButton_AnyUp) && selected > 0)
				selected--;
			if (selected < first)
				first = selected;
			if (selected >= first + VISIBLE_GAMES)
				first = selected - VISIBLE_GAMES + 1;
		}
	}
}

void host_lobby_choose(void)
{
	PadState pad;
	int selected = 0;

	padInitializeDefault(&pad);
	consoleInit(NULL);
	/* a newer release first: taking it restarts the program from here */
	host_update_offer(&pad);
	for (;;)
	{
		u64 down;

		draw_choice(selected);
		down = wait_for_buttons(&pad);
		if (down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))
			selected = !selected;
		if (!(down & HidNpadButton_A))
			continue;
		if (selected == 0 || choose_a_game(&pad))
			break;
	}
	/* the window goes back to SDL, for the game */
	consoleClear();
	consoleUpdate(NULL);
	consoleExit(NULL);
	if (!join_link[0])
		host_logf(HOST_LOG_INFO, "lobby: playing without joining an internet game");
}

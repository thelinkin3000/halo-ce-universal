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

The screens are host_ui.c's, drawn on the default window before SDL takes that
window for the game; host_ui_close gives it back.
*/

#include "host.h"

#include "../../linux/include/halo_port_limits.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "host_ui.h"

#define LOBBY_HOST "halo.milenko.org"
#define LOBBY_PATH "/v1/games.txt"

enum
{
	MAXIMUM_GAMES = 64,
	/* a few hundred games' lines; the list holds a handful */
	MAXIMUM_RESPONSE_SIZE = 128 * 1024,
	INVITE_DIGITS = 64,
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

static const char *const choice_keys[] = { "A", "Select", "+", "Quit", NULL };

static void draw_choice(int selected)
{
	static const char *const items[] = { "Campaign and system link", "Internet games" };

	host_ui_choice("HALO: COMBAT EVOLVED", items, 2, selected, "Internet games are the ones listed at " LOBBY_HOST ".",
		choice_keys);
}

static void draw_list(int selected, int first)
{
	static const char *const headings[] = { "NAME", "MAP", "TYPE", "PLAYERS" };
	static const int columns[] = { 0, 470, 730, 930 };
	static struct host_ui_row rows[MAXIMUM_GAMES];
	static char players[MAXIMUM_GAMES][16];
	char aside[32], status[96];
	int index;

	for (index = 0; index < game_count; index++)
	{
		snprintf(players[index], sizeof(players[index]), "%d/%d", games[index].players, games[index].maximum_players);
		rows[index].columns[0] = games[index].name;
		rows[index].columns[1] = games[index].map;
		rows[index].columns[2] = engine_name(games[index].engine);
		rows[index].columns[3] = players[index];
	}
	snprintf(aside, sizeof(aside), "%d to join", game_count);
	snprintf(status, sizeof(status), "%d more listed games are full or need another version.", games_left_out);
	/* the list is Milenko's, run for the community alongside ChupaThingyCe */
	host_ui_list("INTERNET GAMES", aside, headings, columns, 4, rows, game_count, selected, first,
		"No games this build can join are being hosted right now.", games_left_out ? status : NULL,
		"Thanks Milenko! Go check out ChupaThingyCe!",
		(const char *const[]){ "A", "Join", "Y", "Refresh", "B", "Back", NULL });
}

static void quit_from_the_menu(void)
{
	host_logf(HOST_LOG_INFO, "lobby: quit from the menu");
	host_ui_close();
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
		/* a frame: the screen is drawn only when it changes */
		svcSleepThread(16666667);
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

		host_ui_message("INTERNET GAMES", "Fetching the list from " LOBBY_HOST "...", NULL);
		if (!refresh(error, sizeof(error)))
		{
			host_ui_message("INTERNET GAMES", error, (const char *const[]){ "Y", "Try again", "B", "Back", NULL });
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
			if (selected >= first + host_ui_list_rows())
				first = selected - host_ui_list_rows() + 1;
		}
	}
}

void host_lobby_choose(void)
{
	PadState pad;
	int selected = 0;

	padInitializeDefault(&pad);
	if (!host_ui_open())
	{
		/* without its screens the menu cannot be shown: play as before it */
		host_logf(HOST_LOG_ERROR, "lobby: the menu's screens could not be set up; starting the game");
		host_ui_close();
		return;
	}
	/* a newer release first */
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
	host_ui_close();
	if (!join_link[0])
		host_logf(HOST_LOG_INFO, "lobby: playing without joining an internet game");
}

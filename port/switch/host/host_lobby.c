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
- through the console's own TLS (the ssl service), so the build carries no TLS
library. The site's certificate leads to ISRG Root X1 through cross-signed
intermediates; that root and X2 are imported into the connection's context,
so the fetch does not depend on which roots this console's firmware carries.

The menu is libnx's text console, drawn on the default window before SDL takes
that window for the game; consoleExit gives it back. The console takes over
stdout only (the log is on stderr and in the file), and nothing may be printed
to stdout once it has gone.
*/

#include "host.h"

#include "../../linux/include/halo_port_limits.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <switch.h>

#define LOBBY_HOST "halo.milenko.org"
#define LOBBY_PATH "/v1/games.txt"
#define LOBBY_TIMEOUT_SECONDS 10

enum
{
	MAXIMUM_GAMES = 64,
	/* a few hundred games' lines; the list holds a handful */
	MAXIMUM_RESPONSE_SIZE = 128 * 1024,
	INVITE_DIGITS = 64,
	/* the console is 80 columns by 45 rows at 1280x720 */
	VISIBLE_GAMES = 32,
};

/* ISRG Root X1 and X2, which the site's chain ends at */
static const char isrg_roots[] =
	"-----BEGIN CERTIFICATE-----\n"
	"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\n"
	"-----END CERTIFICATE-----\n"
	"-----BEGIN CERTIFICATE-----\n"
	"MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
	"CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
	"R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
	"MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
	"ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
	"EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
	"+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
	"ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
	"AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
	"zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
	"tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
	"/q4AaOeMSQ+2b1tbFfLn\n"
	"-----END CERTIFICATE-----\n";

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

/* ---------- fetching the list */

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

static int connect_to_the_list(char *error, size_t error_size)
{
	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	struct addrinfo *address;
	struct timeval timeout = { LOBBY_TIMEOUT_SECONDS, 0 };
	int descriptor = -1;
	int found;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	found = getaddrinfo(LOBBY_HOST, "443", &hints, &addresses);
	if (found != 0 || !addresses)
	{
		fail(error, error_size, "Cannot look up %s. Is the console connected to the internet?", LOBBY_HOST);
		return -1;
	}
	for (address = addresses; address && descriptor < 0; address = address->ai_next)
	{
		descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (descriptor < 0)
			continue;
		setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		if (connect(descriptor, address->ai_addr, address->ai_addrlen) != 0)
		{
			close(descriptor);
			descriptor = -1;
		}
	}
	freeaddrinfo(addresses);
	if (descriptor < 0)
		fail(error, error_size, "Cannot reach %s (%s).", LOBBY_HOST, strerror(errno));
	return descriptor;
}

/* the body's length, once the headers are all there and say it; else -1 */
static long content_length(const char *response, size_t length, size_t *body_offset)
{
	const char *end = NULL;
	const char *field;
	size_t index;

	for (index = 0; index + 4 <= length; index++)
	{
		if (!memcmp(response + index, "\r\n\r\n", 4))
		{
			end = response + index;
			break;
		}
	}
	if (!end)
		return -1;
	*body_offset = (size_t)(end - response) + 4;
	for (field = response; field && field < end; field = strstr(field, "\r\n"), field = field ? field + 2 : NULL)
	{
		if (!strncasecmp(field, "Content-Length:", 15))
			return strtol(field + 15, NULL, 10);
	}
	return -1;
}

/* the list's body, null-terminated, in response; returns its offset there,
or -1 with error said */
static long fetch_the_list(char *response, size_t size, char *error, size_t error_size)
{
	static const char request[] =
		"GET " LOBBY_PATH " HTTP/1.1\r\n"
		"Host: " LOBBY_HOST "\r\n"
		"User-Agent: halo-ce-universal-switch\r\n"
		"Accept: text/plain\r\n"
		"Connection: close\r\n"
		"\r\n";
	SslContext context;
	SslConnection connection;
	int have_ssl = 0, have_context = 0, have_connection = 0;
	int descriptor;
	long result = -1;
	size_t length = 0;
	size_t body_offset = 0;
	long body_length = -1;
	Result status;

	descriptor = connect_to_the_list(error, error_size);
	if (descriptor < 0)
		return -1;
	status = sslInitialize(1);
	if (R_FAILED(status))
	{
		fail(error, error_size, "The console's TLS service is not available (0x%08x).", (unsigned)status);
		goto done;
	}
	have_ssl = 1;
	status = sslCreateContext(&context, SslVersion_Auto);
	if (R_FAILED(status))
	{
		fail(error, error_size, "Cannot set up TLS (0x%08x).", (unsigned)status);
		goto done;
	}
	have_context = 1;
	/* not fatal: the firmware's own roots may be enough */
	status = sslContextImportServerPki(&context, isrg_roots, sizeof(isrg_roots), SslCertificateFormat_Pem, NULL);
	if (R_FAILED(status))
		host_logf(HOST_LOG_WARN, "lobby: could not import the ISRG roots (0x%08x)", (unsigned)status);
	status = sslContextCreateConnection(&context, &connection);
	if (R_FAILED(status))
	{
		fail(error, error_size, "Cannot set up TLS (0x%08x).", (unsigned)status);
		goto done;
	}
	have_connection = 1;
	/* the socket stays this file's to close, whatever the service does */
	sslConnectionSetOption(&connection, SslOptionType_DoNotCloseSocket, true);
	if (socketSslConnectionSetSocketDescriptor(&connection, descriptor) < 0 && errno != ENOENT)
	{
		fail(error, error_size, "Cannot hand the connection to TLS (%s).", strerror(errno));
		goto done;
	}
	sslConnectionSetHostName(&connection, LOBBY_HOST, sizeof(LOBBY_HOST) - 1);
	status = sslConnectionDoHandshake(&connection, NULL, NULL, NULL, 0);
	if (R_FAILED(status))
	{
		fail(error, error_size, "The secure connection to %s failed (0x%08x).", LOBBY_HOST, (unsigned)status);
		goto done;
	}
	{
		u32 written = 0;
		size_t sent = 0;

		while (sent < sizeof(request) - 1)
		{
			status = sslConnectionWrite(&connection, request + sent, (u32)(sizeof(request) - 1 - sent), &written);
			if (R_FAILED(status) || !written)
			{
				fail(error, error_size, "Cannot send the request to %s (0x%08x).", LOBBY_HOST, (unsigned)status);
				goto done;
			}
			sent += written;
		}
	}
	/* to the end of the body, or until the server closes */
	while (length < size - 1)
	{
		u32 received = 0;

		status = sslConnectionRead(&connection, response + length, (u32)(size - 1 - length), &received);
		if (R_FAILED(status) || !received)
			break;
		length += received;
		if (body_length < 0)
			body_length = content_length(response, length, &body_offset);
		if (body_length >= 0 && length >= body_offset + (size_t)body_length)
			break;
	}
	response[length] = 0;
	if (body_length < 0)
		body_length = content_length(response, length, &body_offset);
	if (strncmp(response, "HTTP/1.", 7) || length < 12)
	{
		fail(error, error_size, "%s sent no answer.", LOBBY_HOST);
		goto done;
	}
	{
		int code = atoi(response + 9);

		if (code == 429)
		{
			fail(error, error_size, "The list is busy (too many requests). Try again in a minute.");
			goto done;
		}
		if (code != 200)
		{
			fail(error, error_size, "The list answered with error %d.", code);
			goto done;
		}
	}
	if (!body_offset)
	{
		fail(error, error_size, "The list's answer was cut off.");
		goto done;
	}
	if (body_length >= 0 && body_offset + (size_t)body_length < length)
		response[body_offset + (size_t)body_length] = 0;
	result = (long)body_offset;

done:
	if (have_connection)
		sslConnectionClose(&connection);
	if (have_context)
		sslContextClose(&context);
	if (have_ssl)
		sslExit();
	close(descriptor);
	return result;
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
	long body;

	if (!response)
		return fail(error, error_size, "Out of memory for the list.");
	body = fetch_the_list(response, MAXIMUM_RESPONSE_SIZE, error, error_size);
	if (body >= 0)
	{
		read_the_list(response + body);
		host_logf(HOST_LOG_INFO, "lobby: %d games to join (%d left out)", game_count, games_left_out);
	}
	free(response);
	return body >= 0;
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

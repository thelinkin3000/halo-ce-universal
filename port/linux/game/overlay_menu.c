/*
OVERLAY_MENU.C

The game's own overlay menu, on every build alike (the Switch, the PS5,
Android): a panel down the left 30% of the screen, black at 40%, titled
"OpenCE for <the platform>", with a list of entries. An entry is a toggle
(a cheat, a setting: on or off, as the game has it now), a command run once
(spawn the weapons, a checkpoint), a choice among several (the game's
speed, the bots' difficulty), or a folder of more entries. They began as
hardran3's OpenCE-Ultrahand and HaloDrones packages' (the bots': its
difficulty is the bots' bots.skill here), with the bots' vehicles and routes
besides. A command is run at once: a line for the console
(hs_compile_and_evaluate), or "config <setting> <value>" (config_set_text,
saved as the menus save it).

Holding L and R (the shoulders) and pressing the d-pad's down opens it and
closes it, on any controller; on Android without one, three fingers (or
four, the touch controls' General option) on the screen together
(TouchControls.java over the touch controls). While
it is open the game gets none of the controllers, the keyboard, the mouse
or the touchscreen (port/linux/src/xinput_sdl.c, touch_input.c): the d-pad
or the left stick moves through the list, A chooses, left and right turn a
toggle off and on (else right goes into a folder and left back out), B goes
back out of a folder and closes it at the top, Back closes it; the
keyboard's arrows, Enter and Escape are those; the mouse and the
touchscreen choose a row with a click or a tap (one outside the panel closes
it, one on the folder's name goes back out of it) and scroll with the wheel
or a drag. The game itself goes on underneath: it is not paused.

game.overlay_menu (on by default) lets those buttons open it.

The entries are built in (folders of entries, each naming its commands and
where its state is read), and files of one's own add to them: overlay/ in
the data root (below, "the files' entries").
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "hs/hs.h"
#include "interface/hud.h"
#include "interface/interface.h"
#include "interface/ui_widget.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "tag_files/tag_groups.h"
#include "text/font_group.h"
#include "halo_overlay_menu.h"
#include "halo_ui_pointer.h"

#include "../src/posix.h"
#include "../../third_party/tomlc17/tomlc17.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, not the game's tracked one (cseries.h): the
files' text comes from config_file_read, which allocates with it) */
#undef malloc
#undef free
#undef realloc

/* ---------- constants */

enum
{
	/* the panel's width, in hundredths of the screen's */
	OVERLAY_PANEL_PERCENT = 30,
	/* its black's alpha (40%) */
	OVERLAY_PANEL_ALPHA = 0x66,
	/* the margin inside it, and the room around a row's text (the screen's
	480 lines) */
	OVERLAY_MARGIN = 10,
	OVERLAY_ROW_PADDING = 7,
	/* the rows' bands: the gap between two, their corners' radius, the
	chosen one's outline, and the room for a value (its arrows either side
	of it) at the right */
	OVERLAY_ROW_GAP = 3,
	OVERLAY_ROW_RADIUS = 8,
	OVERLAY_OUTLINE = 2,
	OVERLAY_VALUE_WIDTH = 64,
	OVERLAY_ARROW_SIZE = 4,
	/* a held direction steps again after this long, then this often */
	OVERLAY_REPEAT_DELAY_MILLISECONDS = 400,
	OVERLAY_REPEAT_MILLISECONDS = 90,
	/* how long what a command did shows at the panel's foot */
	OVERLAY_STATUS_MILLISECONDS = 3000,
	/* the chosen row's label too long for its room scrolls across: still
	this long at each end, moving this many units (the screen's 480 lines) a
	second between */
	OVERLAY_SCROLL_PAUSE_MILLISECONDS = 1000,
	OVERLAY_SCROLL_UNITS_PER_SECOND = 40,
	/* folders within folders */
	MAXIMUM_OVERLAY_DEPTH = 8,
	/* the controllers */
	OVERLAY_PORTS = 4,
	/* draw_string.c's justifications (its enum is its own) */
	_overlay_text_left = 0,
	_overlay_text_right = 1,
};

#if defined(HALO_SWITCH)
#define OVERLAY_TITLE "OpenCE for Switch"
#elif defined(HALO_PS5)
#define OVERLAY_TITLE "OpenCE for PS5"
#elif defined(HALO_ANDROID)
#define OVERLAY_TITLE "OpenCE for Android"
#else
#define OVERLAY_TITLE "OpenCE"
#endif
/* the foot's buttons, as the platform's pictures (the menus' icons in text:
ui_widget.c's draw_string_and_hack_in_icons; the Switch's and the PS5's are
their own) */
#define OVERLAY_HINT L"%a-button Select   %b-button Back"

/* what an entry is */
enum overlay_entry_type
{
	/* a command run when chosen */
	_overlay_entry_command,
	/* on or off: its command for on or for off run when chosen */
	_overlay_entry_toggle,
	/* one of a folder's several: its command run when chosen, and shown as
	the one chosen while its state says it is */
	_overlay_entry_choice,
	/* a folder of more entries */
	_overlay_entry_folder,
	/* a file's entry that could not be read (overlay/, below): shown in red,
	and chosen, says why */
	_overlay_entry_malformed,
};

/* where a toggle's or a choice's state is read */
enum overlay_entry_state
{
	_overlay_state_none,
	/* a console global, on while true (cheat_deathless_player) */
	_overlay_state_global,
	/* ... on while false (rider_ejection: Easy Rider keeps a rider in) */
	_overlay_state_global_inverted,
	/* ... on while it is the entry's value (rasterizer_stats 5) */
	_overlay_state_global_equals,
	/* a true or false setting (debug.show_resolution) */
	_overlay_state_config,
	/* a number setting, on while it is not 0 (bots.vehicles) */
	_overlay_state_config_integer,
	/* a word setting, on while it is the entry's word (bots.skill "easy") */
	_overlay_state_config_equals,
	/* the map's show_hud */
	_overlay_state_show_hud,
	/* the game's speed (game_speed), as the entry's value */
	_overlay_state_game_speed,
	/* what the overlay last set it to (the HUD's parts, which the console
	sets but does not say) */
	_overlay_state_remembered,
	/* a file's "config:<setting>" (on while it is true, or a number not 0)
	or "config:<setting>=<word>" (on while it is that word), of any kind of
	setting (config_text) */
	_overlay_state_config_text,
};

/* ---------- definitions */

struct overlay_folder;
struct overlay_source;

struct overlay_entry
{
	short type;
	short state;
	char const *label;
	/* a command, a choice's, a toggle's that turns it on */
	char const *command;
	/* a toggle's that turns it off */
	char const *command_off;
	/* the global or the setting its state is */
	char const *state_name;
	/* the value it is on at (_overlay_state_global_equals,
	_overlay_state_game_speed) */
	real state_value;
	/* a built-in folder's entries */
	struct overlay_source *source;
	/* _overlay_state_remembered's */
	boolean remembered;
	/* the global's index, once looked for (NONE: not one) */
	boolean global_looked_up;
	short global_index;
	/* the word it is on at (_overlay_state_config_equals,
	_overlay_state_config_text) */
	char const *state_text;
	/* a folder's, as the overlay builds it from the built-in entries and the
	files' (overlay_build) */
	struct overlay_folder *folder;
	/* a malformed file entry's: what is wrong, and the file */
	char const *problem;
	char const *file;
	/* a line about it, shown above the foot while it is chosen ("Applies
	at the next restart"), or NULL */
	char const *note;
};

/* a built-in folder: its entries */
struct overlay_source
{
	char const *label;
	short count;
	struct overlay_entry *entries;
};

/* a folder as shown: the built-in entries of its name and those of the
files in overlay/ of its name, in order */
struct overlay_folder
{
	char const *label;
	short count;
	struct overlay_entry **entries;
};

/* ---------- prototypes */

/* port_config.c's, cseries_windows.c's, cinematics.c's */
int config_boolean(const char *name);
long config_integer(const char *name);
const char *config_string(const char *name);
unsigned long config_changes(void);
int config_set_text(const char *name, const char *value, char *error, size_t error_size);
int config_text(const char *name, char *text, size_t size);
char *config_file_read(const char *path, size_t *size);
const char *platform_data_root(void);
unsigned long system_milliseconds(void);
void draw_quad(rectangle2d *rectangle, pixel32 color);
/* network_chat.c's: a chat line is typed (with its on-screen keyboard or not) */
unsigned char network_chat_typing(void);

/* ---------- the built-in entries */

#define OVERLAY_COMMAND(label, command) \
	{ _overlay_entry_command, _overlay_state_none, label, command, NULL, NULL, 0.0f, NULL, FALSE, FALSE, NONE }
#define OVERLAY_GLOBAL(label, global) \
	{ _overlay_entry_toggle, _overlay_state_global, label, global " true", global " false", global, 0.0f, NULL, \
		FALSE, FALSE, NONE }
#define OVERLAY_TOGGLE(label, state, on, off, name, value, remembered) \
	{ _overlay_entry_toggle, state, label, on, off, name, value, NULL, remembered, FALSE, NONE }
#define OVERLAY_SPEED(label, command, value) \
	{ _overlay_entry_choice, _overlay_state_game_speed, label, command, NULL, NULL, value, NULL, FALSE, FALSE, NONE }
#define OVERLAY_SETTING_CHOICE(label, setting, word) \
	{ _overlay_entry_choice, _overlay_state_config_equals, label, "config " setting " " word, NULL, setting, 0.0f, \
		NULL, FALSE, FALSE, NONE, word }
#define OVERLAY_TOGGLE_NOTE(label, state, on, off, name, note) \
	{ _overlay_entry_toggle, state, label, on, off, name, 0.0f, NULL, FALSE, FALSE, NONE, NULL, NULL, NULL, NULL, \
		note }
#define OVERLAY_FOLDER(label, source) \
	{ _overlay_entry_folder, _overlay_state_none, label, NULL, NULL, NULL, 0.0f, &source, FALSE, FALSE, NONE }
#define OVERLAY_FOLDER_OF(label, entries) { label, (short)NUMBEROF(entries), entries }

static struct overlay_entry overlay_cheats_entries[] =
{
	OVERLAY_GLOBAL("God Mode", "cheat_deathless_player"),
	OVERLAY_GLOBAL("Omnipotent", "cheat_omnipotent"),
	OVERLAY_GLOBAL("Infinite Ammo", "cheat_infinite_ammo"),
	OVERLAY_GLOBAL("Bottomless Clip", "cheat_bottomless_clip"),
	OVERLAY_GLOBAL("Super Jump", "cheat_super_jump"),
	OVERLAY_GLOBAL("No Fall Damage", "cheat_jetpack"),
	OVERLAY_GLOBAL("Medusa Mode", "cheat_medusa"),
	OVERLAY_GLOBAL("Bump Possession", "cheat_bump_possession"),
	OVERLAY_TOGGLE("Easy Rider", _overlay_state_global_inverted, "rider_ejection false", "rider_ejection true",
		"rider_ejection", 0.0f, FALSE),
	OVERLAY_COMMAND("Spawn All Weapons", "cheat_all_weapons"),
	OVERLAY_COMMAND("Spawn All Powerups", "cheat_all_powerups"),
	OVERLAY_COMMAND("Spawn All Vehicles", "cheat_all_vehicles"),
};
static struct overlay_source overlay_cheats = OVERLAY_FOLDER_OF("Cheats", overlay_cheats_entries);

/* the bots' (HaloDrones' package's, with the settings of bots.c: how well
they play, the vehicles, their routes); add_bot and drop_bot are the host's
alone, and say so on a client */
static struct overlay_entry overlay_bots_add_entries[] =
{
	OVERLAY_COMMAND("Add Bot (Any Team)", "add_bot"),
	OVERLAY_COMMAND("Add Bot (Blue Team)", "add_bot_blue"),
	OVERLAY_COMMAND("Add Bot (Red Team)", "add_bot_red"),
};
static struct overlay_source overlay_bots_add = OVERLAY_FOLDER_OF("Add Bots", overlay_bots_add_entries);

static struct overlay_entry overlay_bots_drop_entries[] =
{
	OVERLAY_COMMAND("Drop Bot (Any Team)", "drop_bot"),
	OVERLAY_COMMAND("Drop Bot (Blue Team)", "drop_bot_blue"),
	OVERLAY_COMMAND("Drop Bot (Red Team)", "drop_bot_red"),
};
static struct overlay_source overlay_bots_drop = OVERLAY_FOLDER_OF("Drop Bots", overlay_bots_drop_entries);

static struct overlay_entry overlay_bots_skill_entries[] =
{
	OVERLAY_SETTING_CHOICE("Recruit", "bots.skill", "recruit"),
	OVERLAY_SETTING_CHOICE("Easy", "bots.skill", "easy"),
	OVERLAY_SETTING_CHOICE("Normal", "bots.skill", "normal"),
	OVERLAY_SETTING_CHOICE("Heroic", "bots.skill", "heroic"),
	OVERLAY_SETTING_CHOICE("Veteran", "bots.skill", "veteran"),
	OVERLAY_SETTING_CHOICE("Legendary", "bots.skill", "legendary"),
};
static struct overlay_source overlay_bots_skill = OVERLAY_FOLDER_OF("Difficulty", overlay_bots_skill_entries);

static struct overlay_entry overlay_bots_routes_entries[] =
{
	OVERLAY_SETTING_CHOICE("Direct", "bots.routes", "direct"),
	OVERLAY_SETTING_CHOICE("Lanes", "bots.routes", "lanes"),
	OVERLAY_SETTING_CHOICE("Mixed", "bots.routes", "mixed"),
};
static struct overlay_source overlay_bots_routes = OVERLAY_FOLDER_OF("Routes", overlay_bots_routes_entries);

static struct overlay_entry overlay_bots_entries[] =
{
	OVERLAY_FOLDER("Add Bots", overlay_bots_add),
	OVERLAY_FOLDER("Drop Bots", overlay_bots_drop),
	OVERLAY_FOLDER("Difficulty", overlay_bots_skill),
	OVERLAY_TOGGLE("Vehicles", _overlay_state_config_integer, "config bots.vehicles 1", "config bots.vehicles 0",
		"bots.vehicles", 0.0f, FALSE),
	OVERLAY_FOLDER("Routes", overlay_bots_routes),
};
static struct overlay_source overlay_bots = OVERLAY_FOLDER_OF("Bots", overlay_bots_entries);

static struct overlay_entry overlay_speed_entries[] =
{
	OVERLAY_SPEED("1x (Normal)", "game_speed 1", 1.0f),
	OVERLAY_SPEED("2x", "game_speed 2", 2.0f),
	OVERLAY_SPEED("3x", "game_speed 3", 3.0f),
	OVERLAY_SPEED("4x", "game_speed 4", 4.0f),
	OVERLAY_SPEED("5x", "game_speed 5", 5.0f),
	OVERLAY_SPEED("10x", "game_speed 10", 10.0f),
};
static struct overlay_source overlay_speed = OVERLAY_FOLDER_OF("Game Speed", overlay_speed_entries);

static struct overlay_entry overlay_hud_entries[] =
{
	OVERLAY_TOGGLE("HUD Visibility", _overlay_state_show_hud, "show_hud true", "show_hud false", NULL, 0.0f, FALSE),
	OVERLAY_TOGGLE("Crosshair", _overlay_state_remembered, "hud_show_crosshair true", "hud_show_crosshair false",
		NULL, 0.0f, TRUE),
	OVERLAY_TOGGLE("Health Bar", _overlay_state_remembered, "hud_show_health true", "hud_show_health false",
		NULL, 0.0f, TRUE),
	OVERLAY_TOGGLE("Shield Bar", _overlay_state_remembered, "hud_show_shield true", "hud_show_shield false",
		NULL, 0.0f, TRUE),
	OVERLAY_TOGGLE("Motion Sensor", _overlay_state_remembered, "hud_show_motion_sensor true",
		"hud_show_motion_sensor false", NULL, 0.0f, TRUE),
};
static struct overlay_source overlay_hud = OVERLAY_FOLDER_OF("HUD", overlay_hud_entries);

static struct overlay_entry overlay_camera_entries[] =
{
	OVERLAY_COMMAND("Set Default Player Camera", "camera_control 0"),
	OVERLAY_COMMAND("Set Cinematic Camera", "camera_control 1"),
	OVERLAY_COMMAND("Load Debug Camera", "debug_camera_load"),
	OVERLAY_COMMAND("Save Debug Camera", "debug_camera_save"),
	OVERLAY_COMMAND("Teleport to Camera", "cheat_teleport_to_camera"),
};
static struct overlay_source overlay_camera = OVERLAY_FOLDER_OF("Camera", overlay_camera_entries);

static struct overlay_entry overlay_system_entries[] =
{
	OVERLAY_GLOBAL("Display Framerate", "display_framerate"),
	OVERLAY_TOGGLE("Show Resolution & FPS", _overlay_state_config, "config debug.show_resolution true",
		"config debug.show_resolution false", "debug.show_resolution", 0.0f, FALSE),
	OVERLAY_TOGGLE("Detailed FPS", _overlay_state_global_equals, "rasterizer_stats 5", "rasterizer_stats 0",
		"rasterizer_stats", 5.0f, FALSE),
	OVERLAY_COMMAND("Force Checkpoint", "game_save_totally_unsafe"),
	OVERLAY_COMMAND("Revert to Last Checkpoint", "game_revert"),
	OVERLAY_GLOBAL("Wireframe Mode", "rasterizer_wireframe"),
	OVERLAY_GLOBAL("Environment Textures", "rasterizer_environment_diffuse_textures"),
	OVERLAY_GLOBAL("Atmospheric Fog", "rasterizer_fog_atmosphere"),
	OVERLAY_GLOBAL("Fog Plane", "rasterizer_fog_plane"),
	OVERLAY_GLOBAL("Environment Shadows", "rasterizer_environment_shadows"),
	OVERLAY_GLOBAL("Lens Flares", "rasterizer_lens_flares"),
	OVERLAY_GLOBAL("Screen Effects", "rasterizer_screen_effects"),
	OVERLAY_GLOBAL("Weather Particles", "weather"),
	/* (a host's recording of its games: network_replay.c, port/linux/REPLAY.md) */
	OVERLAY_TOGGLE_NOTE("Record Replays", _overlay_state_config, "config network.record_replays true",
		"config network.record_replays false", "network.record_replays", "From the next network game"),
	/* (the game's log, source/cseries/errors.c; the host's, from the next
	start: port/android/host/host_main.c, port/switch/host/host_main.c,
	port/ps5/host/host_main.c) */
	OVERLAY_TOGGLE("Write debug.txt", _overlay_state_config, "config debug.log_files true",
		"config debug.log_files false", "debug.log_files", 0.0f, FALSE),
#if defined(HALO_ANDROID) || defined(HALO_PS5)
	OVERLAY_TOGGLE_NOTE("Write halo.log", _overlay_state_config, "config debug.halo_log true",
		"config debug.halo_log false", "debug.halo_log", "Applies at the next restart"),
#endif
#if defined(HALO_SWITCH) || defined(HALO_PS5)
	OVERLAY_TOGGLE_NOTE("Xbox Button Pictures", _overlay_state_config, "config display.xbox_buttons true",
		"config display.xbox_buttons false", "display.xbox_buttons", "Applies at the next restart"),
#endif
#ifdef HALO_SWITCH
	OVERLAY_TOGGLE("Nintendo Menu Buttons", _overlay_state_config, "config controls.nintendo_menu_buttons true",
		"config controls.nintendo_menu_buttons false", "controls.nintendo_menu_buttons", 0.0f, FALSE),
#endif
};
static struct overlay_source overlay_system = OVERLAY_FOLDER_OF("System", overlay_system_entries);

static struct overlay_entry overlay_root_entries[] =
{
	OVERLAY_FOLDER("Cheats", overlay_cheats),
	OVERLAY_FOLDER("Bots", overlay_bots),
	OVERLAY_FOLDER("Game Speed", overlay_speed),
	OVERLAY_FOLDER("HUD", overlay_hud),
	OVERLAY_FOLDER("Camera", overlay_camera),
	OVERLAY_FOLDER("System", overlay_system),
};
static struct overlay_source overlay_root = OVERLAY_FOLDER_OF(OVERLAY_TITLE, overlay_root_entries);

/* ---------- the files' entries

Entries of one's own, beside the built-in ones: files in the overlay folder
of the data root (the folder with maps/ and config.toml: the Switch's
sdmc:/switch/halo/overlay, the PS5's /data/halo/overlay, Android's
Android/data/com.halo.decomp/files/overlay). Each folder in it is a folder of
the overlay, and each folder in one of those a folder in that, by its name;
one of a built-in folder's name (Cheats, in any case) adds to it. Each .toml
file is an entry:

	label   = "Make It Rain"
	type    = "command"                (or "toggle", or "choice")
	command = "weather_start 10"       (a command's, a choice's)
	on      = "cheat_big_heads true"   (a toggle's)
	off     = "cheat_big_heads false"  (a toggle's)
	state   = "global:cheat_big_heads" (where a toggle's ON or a choice's
	                                    choosing is read: global:<name> or
	                                    global:<name>=<number>, a console
	                                    global's; config:<setting> or
	                                    config:<setting>=<word>, a setting's;
	                                    speed=<number>, the game's speed;
	                                    remembered, what the overlay set; a
	                                    toggle's is remembered if not given)
	note    = "Applies at the next restart" (a line shown while it is
	                                    chosen; any entry's, not needed)

A command is a console line or "config <setting> <value>", as the built-in
ones'. A folder's built-in entries come first, then its folders and then its
files' entries, each by name. A file that is not a whole entry is shown in
red, "<label>: malformed entry" ("malformed entry: no label" without its
label), and choosing it says what is wrong (and debug.txt, as it is read).
The files are read when the overlay opens, again only after one changed. */

enum
{
	MAXIMUM_OVERLAY_FOLDERS = 128,
	MAXIMUM_OVERLAY_SLOTS = 1536,
	MAXIMUM_OVERLAY_FILE_ENTRIES = 512,
	OVERLAY_TEXT_POOL_SIZE = 64 * 1024,
	/* the entries of one folder, and the files and folders read in one */
	MAXIMUM_OVERLAY_FOLDER_ENTRIES = 192,
	OVERLAY_FILE_NAME_SIZE = 128,
	OVERLAY_PATH_SIZE = 512,
};

static struct
{
	/* the folders as shown, their entries' places, the files' entries and
	their text */
	struct overlay_folder folders[MAXIMUM_OVERLAY_FOLDERS];
	short folder_count;
	struct overlay_entry *slots[MAXIMUM_OVERLAY_SLOTS];
	short slot_count;
	struct overlay_entry entries[MAXIMUM_OVERLAY_FILE_ENTRIES];
	short entry_count;
	char text[OVERLAY_TEXT_POOL_SIZE];
	long text_used;
	/* the files' names, sizes and times when last read */
	unsigned long signature;
	boolean built;
	/* more than fit was left out (said once a build) */
	boolean full;
} overlay_tree;

/* ---------- globals */

static struct
{
	boolean open;
	/* a request to open or close it, for the next frame */
	boolean toggle_requested;
	/* the folders gone into, the root first, and the row chosen in each */
	short depth;
	struct overlay_folder *folders[MAXIMUM_OVERLAY_DEPTH];
	short selected[MAXIMUM_OVERLAY_DEPTH];
	/* the first row shown */
	short first_row;
	/* each controller's buttons at its last read, and whether L, R and the
	d-pad's down were all held at it */
	unsigned long port_buttons[OVERLAY_PORTS];
	boolean port_combination[OVERLAY_PORTS];
	/* the buttons at the last frame's update, and a held direction's time */
	unsigned long previous_buttons;
	unsigned long repeat_at;
	/* what the last command did, and until when it shows */
	char status[96];
	unsigned long status_until;
	/* where the last frame drew the rows, for the pointer (the screen's
	coordinates) */
	boolean drawn;
	/* why it could not be drawn has been logged, since it opened */
	boolean logged_draw;
	short panel_right;
	short back_top, back_bottom;
	short rows_top;
	short row_height;
	short rows_shown;
	/* the chosen row's label's scroll: the folder and the row, and since
	when */
	struct overlay_folder *scroll_folder;
	short scroll_row;
	unsigned long scroll_since;
} overlay;

/* ---------- private code */

static struct overlay_folder *overlay_folder(
	void)
{
	return overlay.folders[overlay.depth];
}

static struct hs_external_global_definition *overlay_global(
	struct overlay_entry *entry)
{
	if (!entry->global_looked_up)
	{
		short index = entry->state_name ? hs_find_global_by_name(entry->state_name) : NONE;

		entry->global_looked_up = TRUE;
		/* (the console's own globals, not a map's) */
		entry->global_index = index != NONE && (index & 0x8000) ? (short)(index & 0x7FFF) : NONE;
	}
	return entry->global_index == NONE ? NULL : hs_global_external_get(entry->global_index);
}

static boolean overlay_global_value(
	struct overlay_entry *entry,
	real *value)
{
	struct hs_external_global_definition *global = overlay_global(entry);

	if (!global || !global->address)
		return FALSE;
	switch (global->type)
	{
	case _hs_type_boolean:
		*value = *(boolean const *)global->address ? 1.0f : 0.0f;
		return TRUE;
	case _hs_type_short_integer:
		*value = (real)*(short const *)global->address;
		return TRUE;
	case _hs_type_long_integer:
		*value = (real)*(long const *)global->address;
		return TRUE;
	case _hs_type_real:
		*value = *(real const *)global->address;
		return TRUE;
	}
	return FALSE;
}

/* whether a toggle is on, or a choice the one chosen */
static boolean overlay_entry_on(
	struct overlay_entry *entry)
{
	real value;

	switch (entry->state)
	{
	case _overlay_state_global:
		return overlay_global_value(entry, &value) && value != 0.0f;
	case _overlay_state_global_inverted:
		return overlay_global_value(entry, &value) && value == 0.0f;
	case _overlay_state_global_equals:
		return overlay_global_value(entry, &value) && value == entry->state_value;
	case _overlay_state_config:
		return config_boolean(entry->state_name) != 0;
	case _overlay_state_config_integer:
		return config_integer(entry->state_name) != 0;
	case _overlay_state_config_equals:
	{
		char const *word = config_string(entry->state_name);

		return word && entry->state_text && !strcmp(word, entry->state_text);
	}
	case _overlay_state_show_hud:
		return game_map_loaded() && hud_scripted_globals && hud_scripted_globals->show_hud;
	case _overlay_state_game_speed:
		value = game_time_get_speed();
		return value > entry->state_value - 0.01f && value < entry->state_value + 0.01f;
	case _overlay_state_remembered:
		return entry->remembered;
	case _overlay_state_config_text:
	{
		char text[128];

		if (!config_text(entry->state_name, text, sizeof(text)))
			return FALSE;
		if (entry->state_text)
			return !strcmp(text, entry->state_text);
		/* (true, or a number not 0) */
		return !strcmp(text, "true") || atof(text) != 0.0;
	}
	}
	return FALSE;
}

static void overlay_set_status(
	char const *text)
{
	snprintf(overlay.status, sizeof(overlay.status), "%s", text);
	overlay.status_until = system_milliseconds() + OVERLAY_STATUS_MILLISECONDS;
}

/* a command: "config <setting> <value>" sets and saves a setting, anything
else is the console's; TRUE if it was done */
static boolean overlay_run(
	char const *command,
	char *error,
	size_t error_size)
{
	error[0] = 0;
	if (!strncmp(command, "config ", 7))
	{
		char name[128];
		char const *value;
		size_t length;

		command += 7;
		length = strcspn(command, " ");
		if (length >= sizeof(name))
			length = sizeof(name) - 1;
		memcpy(name, command, length);
		name[length] = 0;
		value = command + length;
		while (*value == ' ')
			value++;
		return config_set_text(name, value, error, error_size) != 0;
	}
	if (hs_compile_and_evaluate(command))
		return TRUE;
	snprintf(error, error_size, "the console did not run it");
	return FALSE;
}

static void overlay_keep_selection_shown(
	void)
{
	short selected = overlay.selected[overlay.depth];

	if (overlay.rows_shown <= 0)
		return;
	if (selected < overlay.first_row)
		overlay.first_row = selected;
	if (selected >= overlay.first_row + overlay.rows_shown)
		overlay.first_row = (short)(selected - overlay.rows_shown + 1);
}

/* ---------- the files' entries: reading them */

static boolean overlay_same_name(
	char const *a,
	char const *b)
{
	for (; *a && *b; a++, b++)
	{
		char x = (char)(*a >= 'A' && *a <= 'Z' ? *a - 'A' + 'a' : *a);
		char y = (char)(*b >= 'A' && *b <= 'Z' ? *b - 'A' + 'a' : *b);

		if (x != y)
			return FALSE;
	}
	return *a == *b;
}

static int overlay_compare_names(
	void const *a,
	void const *b)
{
	char const *x = (char const *)a;
	char const *y = (char const *)b;

	for (; *x && *y; x++, y++)
	{
		char p = (char)(*x >= 'A' && *x <= 'Z' ? *x - 'A' + 'a' : *x);
		char q = (char)(*y >= 'A' && *y <= 'Z' ? *y - 'A' + 'a' : *y);

		if (p != q)
			return p < q ? -1 : 1;
	}
	return *x ? 1 : *y ? -1 : 0;
}

static int overlay_compare_entries(
	void const *a,
	void const *b)
{
	struct overlay_entry const *x = *(struct overlay_entry *const *)a;
	struct overlay_entry const *y = *(struct overlay_entry *const *)b;

	return overlay_compare_names(x->label ? x->label : "", y->label ? y->label : "");
}

/* a copy of text kept with the files' entries; NULL if there is no room */
static char const *overlay_keep_text(
	char const *text,
	long length)
{
	char *kept;

	if (length < 0)
		length = (long)strlen(text);
	if (overlay_tree.text_used + length + 1 > OVERLAY_TEXT_POOL_SIZE)
	{
		overlay_tree.full = TRUE;
		return NULL;
	}
	kept = overlay_tree.text + overlay_tree.text_used;
	memcpy(kept, text, (size_t)length);
	kept[length] = 0;
	overlay_tree.text_used += length + 1;
	return kept;
}

static struct overlay_entry *overlay_new_entry(
	void)
{
	struct overlay_entry *entry;

	if (overlay_tree.entry_count >= MAXIMUM_OVERLAY_FILE_ENTRIES)
	{
		overlay_tree.full = TRUE;
		return NULL;
	}
	entry = &overlay_tree.entries[overlay_tree.entry_count++];
	memset(entry, 0, sizeof(*entry));
	entry->global_index = NONE;
	return entry;
}

/* a file's string, or NULL (and why, if it is there but not a string) */
static char const *overlay_file_string(
	toml_datum_t table,
	char const *key,
	char const **problem)
{
	toml_datum_t value = toml_get(table, key);

	if (value.type == TOML_UNKNOWN)
		return NULL;
	if (value.type != TOML_STRING)
	{
		if (!*problem)
			*problem = "a value that is not a string in quotes";
		return NULL;
	}
	return overlay_keep_text(value.u.s, value.u.str.len);
}

/* a label in a file that is not TOML: a line label = "..." */
static char const *overlay_file_label_line(
	char const *text)
{
	char const *line = text;

	while (line && *line)
	{
		char const *at = line;

		while (*at == ' ' || *at == '\t')
			at++;
		if (!strncmp(at, "label", 5))
		{
			at += 5;
			while (*at == ' ' || *at == '\t')
				at++;
			if (*at == '=')
			{
				char const *end;

				at++;
				while (*at == ' ' || *at == '\t')
					at++;
				if (*at == '"')
				{
					at++;
					end = strchr(at, '"');
					if (end && end > at && !memchr(at, '\n', (size_t)(end - at)))
						return overlay_keep_text(at, (long)(end - at));
				}
			}
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return NULL;
}

/* where a file's state is read ("global:<name>[=<number>]",
"config:<setting>[=<word>]", "speed=<number>", "remembered"); FALSE, with
why, for one that is not */
static boolean overlay_file_state(
	struct overlay_entry *entry,
	char const *state,
	char const **problem)
{
	char const *equals;

	if (!strcmp(state, "remembered"))
	{
		entry->state = _overlay_state_remembered;
		return TRUE;
	}
	if (!strncmp(state, "speed=", 6))
	{
		entry->state = _overlay_state_game_speed;
		entry->state_value = (real)atof(state + 6);
		if (entry->state_value <= 0.0f)
		{
			*problem = "speed= is not a speed";
			return FALSE;
		}
		return TRUE;
	}
	if (!strncmp(state, "global:", 7) || !strncmp(state, "config:", 7))
	{
		boolean global = state[0] == 'g';
		char name[128];
		size_t length;

		state += 7;
		equals = strchr(state, '=');
		length = equals ? (size_t)(equals - state) : strlen(state);
		if (!length || length >= sizeof(name))
		{
			*problem = "a state with no name";
			return FALSE;
		}
		memcpy(name, state, length);
		name[length] = 0;
		entry->state_name = overlay_keep_text(name, (long)length);
		if (!entry->state_name)
			return FALSE;
		if (global)
		{
			entry->state = equals ? _overlay_state_global_equals : _overlay_state_global;
			if (equals)
				entry->state_value = (real)atof(equals + 1);
			if (!overlay_global(entry))
			{
				*problem = "no console global of that name";
				return FALSE;
			}
		}
		else
		{
			char text[128];

			entry->state = _overlay_state_config_text;
			if (equals)
				entry->state_text = overlay_keep_text(equals + 1, -1);
			if (!config_text(name, text, sizeof(text)))
			{
				*problem = "no setting of that name";
				return FALSE;
			}
		}
		return TRUE;
	}
	*problem = "a state not global:, config:, speed= or remembered";
	return FALSE;
}

/* the entry of a file; NULL if there is no room */
static struct overlay_entry *overlay_file_entry(
	char const *path,
	char const *name)
{
	struct overlay_entry *entry = overlay_new_entry();
	char const *problem = NULL;
	char const *type = NULL;
	char const *state = NULL;
	size_t size = 0;
	char *text;

	if (!entry)
		return NULL;
	entry->file = overlay_keep_text(name, -1);
	text = config_file_read(path, &size);
	if (!text)
	{
		problem = "it cannot be read";
	}
	else
	{
		toml_result_t result = toml_parse(text, (int)size);

		if (!result.ok)
		{
			entry->label = overlay_file_label_line(text);
			problem = overlay_keep_text(result.errmsg, -1);
			if (!problem)
				problem = "it is not TOML";
		}
		else
		{
			entry->label = overlay_file_string(result.toptab, "label", &problem);
			type = overlay_file_string(result.toptab, "type", &problem);
			entry->command = overlay_file_string(result.toptab, "command", &problem);
			entry->command_off = overlay_file_string(result.toptab, "off", &problem);
			state = overlay_file_string(result.toptab, "state", &problem);
			entry->note = overlay_file_string(result.toptab, "note", &problem);
			if (!problem && !strcmp(type ? type : "", "toggle"))
				entry->command = overlay_file_string(result.toptab, "on", &problem);
		}
		toml_free(result);
		free(text);
	}
	if (!problem && (!entry->label || !entry->label[0]))
		problem = "no label";
	if (!problem)
	{
		if (!type)
		{
			problem = "no type (command, toggle or choice)";
		}
		else if (!strcmp(type, "command"))
		{
			entry->type = _overlay_entry_command;
			if (!entry->command)
				problem = "no command";
		}
		else if (!strcmp(type, "toggle"))
		{
			entry->type = _overlay_entry_toggle;
			if (!entry->command)
				problem = "no on";
			else if (!entry->command_off)
				problem = "no off";
			else if (!state)
			{
				entry->state = _overlay_state_remembered;
			}
			else
			{
				overlay_file_state(entry, state, &problem);
			}
		}
		else if (!strcmp(type, "choice"))
		{
			entry->type = _overlay_entry_choice;
			if (!entry->command)
				problem = "no command";
			else if (!state)
				problem = "no state (a choice shows which is chosen)";
			else
				overlay_file_state(entry, state, &problem);
		}
		else
		{
			problem = "a type not command, toggle or choice";
		}
	}
	if (problem)
	{
		entry->type = _overlay_entry_malformed;
		entry->problem = problem;
		error(_error_silent, "overlay menu: %s: a malformed entry: %s", path, problem);
	}
	return entry;
}

/* a folder as shown: the built-in entries of source (or none), then the
folders and the files' entries of the folder at path (or none), each by
name; NULL if there is no room */
static struct overlay_folder *overlay_build_folder(
	char const *label,
	struct overlay_source *source,
	char const *path,
	short depth)
{
	struct overlay_entry **collected;
	struct overlay_folder *folder;
	char (*names)[OVERLAY_FILE_NAME_SIZE] = NULL;
	short name_count = 0;
	short count = 0;
	short builtin_count = 0;
	short folder_count;
	short index;

	if (overlay_tree.folder_count >= MAXIMUM_OVERLAY_FOLDERS)
	{
		overlay_tree.full = TRUE;
		return NULL;
	}
	folder = &overlay_tree.folders[overlay_tree.folder_count++];
	folder->label = label;
	folder->count = 0;
	folder->entries = NULL;
	collected = (struct overlay_entry **)malloc(sizeof(*collected) * MAXIMUM_OVERLAY_FOLDER_ENTRIES);
	if (!collected)
		return folder;
	/* (the folder's own folders and files, by name) */
	if (path)
	{
		void *directory = posix_directory_open(path);
		char name[OVERLAY_FILE_NAME_SIZE];

		names = malloc(sizeof(*names) * MAXIMUM_OVERLAY_FOLDER_ENTRIES);
		while (directory && names && posix_directory_next(directory, name, sizeof(name)))
		{
			if (name[0] == '.')
				continue;
			if (name_count >= MAXIMUM_OVERLAY_FOLDER_ENTRIES)
			{
				overlay_tree.full = TRUE;
				break;
			}
			memcpy(names[name_count++], name, sizeof(name));
		}
		if (directory)
			posix_directory_close(directory);
		if (names && name_count)
			qsort(names, (size_t)name_count, sizeof(*names), overlay_compare_names);
	}
	/* the built-in entries, a built-in folder with the files of its name */
	for (index = 0; source && index < source->count && count < MAXIMUM_OVERLAY_FOLDER_ENTRIES; index++)
	{
		struct overlay_entry *entry = &source->entries[index];

		if (entry->type == _overlay_entry_folder && entry->source && depth + 1 < MAXIMUM_OVERLAY_DEPTH)
		{
			char child[OVERLAY_PATH_SIZE];
			char const *child_path = NULL;
			short name_index;

			for (name_index = 0; names && name_index < name_count; name_index++)
			{
				if (overlay_same_name(names[name_index], entry->label))
				{
					snprintf(child, sizeof(child), "%s/%s", path, names[name_index]);
					child_path = child;
					/* (taken: not a folder of its own below) */
					names[name_index][0] = 0;
					break;
				}
			}
			entry->folder = overlay_build_folder(entry->label, entry->source, child_path, (short)(depth + 1));
		}
		collected[count++] = entry;
	}
	builtin_count = count;
	/* then the folder's folders, then its files' entries */
	for (index = 0; names && index < name_count; index++)
	{
		char child[OVERLAY_PATH_SIZE];
		struct posix_file_information information;

		if (!names[index][0])
			continue;
		snprintf(child, sizeof(child), "%s/%s", path, names[index]);
		if (posix_stat(child, &information) == 0 && (information.flags & _posix_file_is_directory) &&
			depth + 1 < MAXIMUM_OVERLAY_DEPTH && count < MAXIMUM_OVERLAY_FOLDER_ENTRIES)
		{
			struct overlay_entry *entry = overlay_new_entry();

			if (!entry)
				break;
			entry->type = _overlay_entry_folder;
			entry->label = overlay_keep_text(names[index], -1);
			if (!entry->label)
				break;
			entry->folder = overlay_build_folder(entry->label, NULL, child, (short)(depth + 1));
			collected[count++] = entry;
			names[index][0] = 0;
		}
	}
	folder_count = count;
	for (index = 0; names && index < name_count; index++)
	{
		char child[OVERLAY_PATH_SIZE];
		size_t length = strlen(names[index]);

		if (length < 5 || !overlay_same_name(names[index] + length - 5, ".toml"))
			continue;
		if (count >= MAXIMUM_OVERLAY_FOLDER_ENTRIES)
		{
			overlay_tree.full = TRUE;
			break;
		}
		snprintf(child, sizeof(child), "%s/%s", path, names[index]);
		collected[count] = overlay_file_entry(child, names[index]);
		if (!collected[count])
			break;
		count++;
	}
	/* (the files' entries by their labels) */
	if (count - folder_count > 1)
		qsort(collected + folder_count, (size_t)(count - folder_count), sizeof(*collected), overlay_compare_entries);
	(void)builtin_count;
	if (overlay_tree.slot_count + count > MAXIMUM_OVERLAY_SLOTS)
	{
		overlay_tree.full = TRUE;
		count = (short)(MAXIMUM_OVERLAY_SLOTS - overlay_tree.slot_count);
	}
	folder->entries = &overlay_tree.slots[overlay_tree.slot_count];
	memcpy(folder->entries, collected, sizeof(*collected) * (size_t)count);
	overlay_tree.slot_count = (short)(overlay_tree.slot_count + count);
	folder->count = count;
	free(names);
	free(collected);
	return folder;
}

/* the names, sizes and times of the files in the folder at path and its
folders, as one number */
static unsigned long overlay_signature(
	char const *path,
	short depth,
	unsigned long signature)
{
	void *directory = depth < MAXIMUM_OVERLAY_DEPTH ? posix_directory_open(path) : NULL;
	char name[OVERLAY_FILE_NAME_SIZE];

	if (!directory)
		return signature;
	while (posix_directory_next(directory, name, sizeof(name)))
	{
		char child[OVERLAY_PATH_SIZE];
		struct posix_file_information information;
		char const *character;

		if (name[0] == '.')
			continue;
		snprintf(child, sizeof(child), "%s/%s", path, name);
		if (posix_stat(child, &information) != 0)
			continue;
		/* (FNV-1a: in any order the folder lists them, the same) */
		{
			unsigned long hash = 2166136261UL;

			for (character = child; *character; character++)
				hash = (hash ^ (unsigned char)*character) * 16777619UL;
			hash = (hash ^ information.size_low) * 16777619UL;
			hash = (hash ^ information.modification_seconds) * 16777619UL;
			hash = (hash ^ information.modification_nanoseconds) * 16777619UL;
			signature += hash;
		}
		if (information.flags & _posix_file_is_directory)
			signature = overlay_signature(child, (short)(depth + 1), signature);
	}
	posix_directory_close(directory);
	return signature;
}

/* the folders as shown, built again if the files changed (or at the first
open); TRUE if they were */
static boolean overlay_build(
	void)
{
	char path[OVERLAY_PATH_SIZE];
	struct posix_file_information information;
	unsigned long signature;

	snprintf(path, sizeof(path), "%s/overlay", platform_data_root());
	/* (made for the player to put files in) */
	if (posix_stat(path, &information) != 0)
		posix_make_directory(path);
	signature = overlay_signature(path, 0, 0);
	if (overlay_tree.built && signature == overlay_tree.signature)
		return FALSE;
	overlay_tree.signature = signature;
	overlay_tree.built = TRUE;
	overlay_tree.folder_count = 0;
	overlay_tree.slot_count = 0;
	overlay_tree.entry_count = 0;
	overlay_tree.text_used = 0;
	overlay_tree.full = FALSE;
	overlay_build_folder(OVERLAY_TITLE, &overlay_root, path, 0);
	error(_error_silent, "overlay menu: %s read: %d folders, %d of its own entries%s", path,
		overlay_tree.folder_count, overlay_tree.entry_count,
		overlay_tree.full ? " (more than fit were left out)" : "");
	return TRUE;
}

static void overlay_set_open(
	boolean open)
{
	short port;

	if (overlay.open == open)
		return;
	overlay.open = open;
	overlay.logged_draw = FALSE;
	/* (the buttons that opened or closed it act on nothing: each acts at
	its next press) */
	overlay.previous_buttons = 0;
	for (port = 0; port < OVERLAY_PORTS; port++)
		overlay.previous_buttons |= overlay.port_buttons[port];
	overlay.repeat_at = 0;
	overlay.drawn = FALSE;
	if (open)
	{
		/* (the files read again if one changed: the overlay back at its top,
		its folders being new) */
		if (overlay_build())
		{
			overlay.depth = 0;
			overlay.selected[0] = 0;
			overlay.first_row = 0;
		}
		overlay.folders[0] = &overlay_tree.folders[0];
		overlay_keep_selection_shown();
	}
}

static void overlay_back(
	void)
{
	if (overlay.depth > 0)
	{
		overlay.depth--;
		overlay.first_row = 0;
		overlay_keep_selection_shown();
	}
	else
	{
		overlay_set_open(FALSE);
	}
}

static void overlay_move(
	short rows)
{
	struct overlay_folder *folder = overlay_folder();
	short selected = overlay.selected[overlay.depth];

	if (folder->count <= 0)
		return;
	/* (a step wraps round) */
	selected = (short)((selected + rows + folder->count) % folder->count);
	overlay.selected[overlay.depth] = selected;
	overlay_keep_selection_shown();
}

static void overlay_choose(
	short row)
{
	struct overlay_folder *folder = overlay_folder();
	struct overlay_entry *entry;
	char const *command;
	char why[128];
	char status[96];
	boolean on;

	if (row < 0 || row >= folder->count)
		return;
	overlay.selected[overlay.depth] = row;
	entry = folder->entries[row];
	switch (entry->type)
	{
	case _overlay_entry_folder:
		if (overlay.depth + 1 < MAXIMUM_OVERLAY_DEPTH && entry->folder)
		{
			overlay.depth++;
			overlay.folders[overlay.depth] = entry->folder;
			overlay.selected[overlay.depth] = 0;
			overlay.first_row = 0;
		}
		return;
	case _overlay_entry_malformed:
		snprintf(status, sizeof(status), "%s: %s", entry->file ? entry->file : "?", entry->problem);
		overlay_set_status(status);
		return;
	case _overlay_entry_toggle:
		on = !overlay_entry_on(entry);
		command = on ? entry->command : entry->command_off;
		break;
	default:
		on = TRUE;
		command = entry->command;
		break;
	}
	if (!command)
		return;
	if (overlay_run(command, why, sizeof(why)))
	{
		if (entry->type == _overlay_entry_toggle)
		{
			entry->remembered = on;
			snprintf(status, sizeof(status), "%s: %s", entry->label, on ? "on" : "off");
		}
		else
		{
			snprintf(status, sizeof(status), "%s: done", entry->label);
		}
	}
	else
	{
		snprintf(status, sizeof(status), "%s: %s", entry->label, why[0] ? why : "not done");
	}
	overlay_set_status(status);
	error(_error_silent, "overlay menu: %s (%s)", command, status);
}

/* a font of the maps' (the menus' ui\large_ui, ui\small_ui), else the
console's; and its lines' height; FALSE if there is none */
static boolean overlay_font(
	char const *tag_name,
	long *font_index,
	short *line_height)
{
	struct font_header *font;

	*font_index = tag_loaded(FONT_GROUP_TAG, tag_name);
	if (*font_index == NONE)
		*font_index = interface_get_tag_index(_interface_font_terminal);
	if (*font_index == NONE)
		return FALSE;
	font = font_definition_get(*font_index);
	*line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	return *line_height > 0;
}

static void overlay_text(
	long font_index,
	rectangle2d const *bounds,
	short justification,
	real_argb_color const *color,
	char const *text)
{
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_string(bounds, bounds, NULL, 0, text);
}

/* a rectangle with rounded corners, of strips a unit tall at its top and
bottom (draw_quad draws none other), none of them over another */
static void overlay_rounded(
	rectangle2d const *rectangle,
	short radius,
	pixel32 color)
{
	short height = (short)(rectangle->y1 - rectangle->y0);
	short row;
	rectangle2d strip;

	radius = (short)MIN(radius, height / 2);
	for (row = 0; row < radius; row++)
	{
		real dy = (real)(radius - row) - 0.5f;
		short inset = (short)((real)radius - (real)sqrt((real)(radius * radius) - dy * dy) + 0.5f);

		strip.x0 = (short)(rectangle->x0 + inset);
		strip.x1 = (short)(rectangle->x1 - inset);
		strip.y0 = (short)(rectangle->y0 + row);
		strip.y1 = (short)(strip.y0 + 1);
		draw_quad(&strip, color);
		strip.y0 = (short)(rectangle->y1 - row - 1);
		strip.y1 = (short)(strip.y0 + 1);
		draw_quad(&strip, color);
	}
	strip = *rectangle;
	strip.y0 = (short)(rectangle->y0 + radius);
	strip.y1 = (short)(rectangle->y1 - radius);
	if (strip.y1 > strip.y0)
		draw_quad(&strip, color);
}

/* a small arrow, as the settings screens' spinners' (bitmaps/arrow_sm_left
and arrow_sm_right): a triangle pointing left or right, its middle at y */
static void overlay_arrow(
	short x,
	short y,
	boolean right,
	pixel32 color)
{
	short column;
	rectangle2d strip;

	for (column = 0; column < OVERLAY_ARROW_SIZE; column++)
	{
		strip.x0 = right ? (short)(x + column) : (short)(x + OVERLAY_ARROW_SIZE - 1 - column);
		strip.x1 = (short)(strip.x0 + 1);
		strip.y0 = (short)(y - (OVERLAY_ARROW_SIZE - column));
		strip.y1 = (short)(y + (OVERLAY_ARROW_SIZE - column));
		draw_quad(&strip, color);
	}
}

/* a label in capitals, as the menus' */
static void overlay_capitals(
	char *capitals,
	size_t size,
	char const *text)
{
	size_t index;

	for (index = 0; index + 1 < size && text[index]; index++)
		capitals[index] = (char)(text[index] >= 'a' && text[index] <= 'z' ? text[index] - 'a' + 'A' : text[index]);
	capitals[index] = 0;
}

/* a label in its room, or, for the chosen row, too long for it, moved across
it and back (still at each end a while), clipped to it */
static void overlay_label(
	long font_index,
	rectangle2d const *room,
	real_argb_color const *color,
	char const *text,
	boolean chosen)
{
	rectangle2d wide = *room;
	rectangle2d text_bounds;
	rectangle2d cursor_bounds;
	short overflow;

	draw_string_set_draw_mode(font_index, NONE, _overlay_text_left, 0, color);
	if (chosen)
	{
		/* (its width, laid out in as much room as it wants) */
		wide.x1 = (short)(wide.x0 + 4000);
		draw_string_compute_bounds(&wide, text, &text_bounds, &cursor_bounds);
		overflow = (short)(cursor_bounds.x0 - room->x1);
		if (overflow > 0)
		{
			unsigned long travel = (unsigned long)overflow * 1000 / OVERLAY_SCROLL_UNITS_PER_SECOND;
			unsigned long cycle = 2 * OVERLAY_SCROLL_PAUSE_MILLISECONDS + travel;
			unsigned long at = (system_milliseconds() - overlay.scroll_since) % cycle;
			short offset;

			if (at < OVERLAY_SCROLL_PAUSE_MILLISECONDS)
				offset = 0;
			else if (at < OVERLAY_SCROLL_PAUSE_MILLISECONDS + travel)
				offset = (short)((at - OVERLAY_SCROLL_PAUSE_MILLISECONDS) * OVERLAY_SCROLL_UNITS_PER_SECOND / 1000);
			else
				offset = overflow;
			wide.x0 = (short)(room->x0 - offset);
			wide.x1 = (short)(wide.x0 + 4000);
			rasterizer_draw_string(&wide, room, NULL, 0, text);
			return;
		}
	}
	rasterizer_draw_string(room, room, NULL, 0, text);
}

/* ---------- public code */

unsigned char overlay_menu_open(
	void)
{
	return overlay.open;
}

void overlay_menu_toggle_request(
	void)
{
	overlay.toggle_requested = TRUE;
}

unsigned char overlay_menu_take_pad(
	short port,
	unsigned long buttons)
{
	enum
	{
		COMBINATION = (1UL << HALO_OVERLAY_PAD_LEFT_SHOULDER) | (1UL << HALO_OVERLAY_PAD_RIGHT_SHOULDER) |
			(1UL << HALO_OVERLAY_PAD_DPAD_DOWN),
	};
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = TRUE;
	boolean combination = (buttons & COMBINATION) == COMBINATION;
	boolean was_open = overlay.open;

	if (port < 0 || port >= OVERLAY_PORTS)
		return overlay.open;
	if (changes != config_changes())
	{
		changes = config_changes();
		enabled = config_boolean("game.overlay_menu") != 0;
	}
	overlay.port_buttons[port] = buttons;
	/* (L, R and the d-pad's down, the last of them pressed with the others
	held, in any order: opens it, or closes it; not while a chat line is
	typed, whose on-screen keyboard reads the controller too) */
	if (combination && !overlay.port_combination[port] && (enabled || overlay.open) &&
		(overlay.open || !network_chat_typing()))
	{
		overlay_set_open(!overlay.open);
	}
	overlay.port_combination[port] = combination;
	/* (the read that closes it is taken too: the button that closed it is
	held over, not a grenade switched or a step back) */
	return overlay.open || was_open;
}

void overlay_menu_pointer(
	struct halo_ui_pointer const *pointer)
{
	short x;
	short y;

	if (!overlay.open || !overlay.drawn)
		return;
	/* (the menus' centred 640, as the screen's) */
	x = (short)(pointer->x + (halo_screen_width() - 640) / 2);
	y = pointer->y;
	if (pointer->wheel_steps)
	{
		struct overlay_folder *folder = overlay_folder();
		short last_first = (short)MAX(folder->count - overlay.rows_shown, 0);

		overlay.first_row = (short)PIN(overlay.first_row - pointer->wheel_steps, 0, last_first);
		overlay.selected[overlay.depth] = (short)PIN(overlay.selected[overlay.depth], overlay.first_row,
			overlay.first_row + overlay.rows_shown - 1);
	}
	/* (a mouse over a row chooses it, as a menu's) */
	if (pointer->moved && !pointer->touch && x >= 0 && x < overlay.panel_right && y >= overlay.rows_top &&
		overlay.row_height > 0)
	{
		short row = (short)(overlay.first_row + (y - overlay.rows_top) / overlay.row_height);

		if (row < overlay.first_row + overlay.rows_shown && row < overlay_folder()->count)
			overlay.selected[overlay.depth] = row;
	}
	if (pointer->right_clicks)
		overlay_back();
	if (pointer->left_clicks)
	{
		short click_x = (short)(pointer->click_x + (halo_screen_width() - 640) / 2);
		short click_y = pointer->click_y;

		if (click_x < 0 || click_x >= overlay.panel_right)
		{
			overlay_set_open(FALSE);
		}
		else if (overlay.depth > 0 && click_y >= overlay.back_top && click_y < overlay.back_bottom)
		{
			overlay_back();
		}
		else if (click_y >= overlay.rows_top && overlay.row_height > 0)
		{
			short row = (short)(overlay.first_row + (click_y - overlay.rows_top) / overlay.row_height);

			if (row < overlay.first_row + overlay.rows_shown)
				overlay_choose(row);
		}
	}
}

void overlay_menu_update(
	void)
{
	enum
	{
		DIRECTIONS = (1UL << HALO_OVERLAY_PAD_UP) | (1UL << HALO_OVERLAY_PAD_DOWN),
	};
	unsigned long buttons = 0;
	unsigned long pressed;
	unsigned long now = system_milliseconds();
	short port;

	if (overlay.toggle_requested)
	{
		overlay.toggle_requested = FALSE;
		overlay_set_open(!overlay.open);
	}
	if (!overlay.open)
		return;
	for (port = 0; port < OVERLAY_PORTS; port++)
		buttons |= overlay.port_buttons[port];
	pressed = buttons & ~overlay.previous_buttons;
	overlay.previous_buttons = buttons;
	/* (up and down held step again and again) */
	if (pressed & DIRECTIONS)
	{
		overlay.repeat_at = now + OVERLAY_REPEAT_DELAY_MILLISECONDS;
	}
	else if ((buttons & DIRECTIONS) && overlay.repeat_at && (long)(now - overlay.repeat_at) >= 0)
	{
		pressed |= buttons & DIRECTIONS;
		overlay.repeat_at = now + OVERLAY_REPEAT_MILLISECONDS;
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_UP))
		overlay_move(-1);
	if (pressed & (1UL << HALO_OVERLAY_PAD_DOWN))
		overlay_move(1);
	if (pressed & (1UL << HALO_OVERLAY_PAD_A))
	{
		overlay_choose(overlay.selected[overlay.depth]);
	}
	else if (pressed & ((1UL << HALO_OVERLAY_PAD_LEFT) | (1UL << HALO_OVERLAY_PAD_RIGHT)))
	{
		/* (left and right turn a toggle on or off, as a settings screen's
		arrows; otherwise right goes into a folder and left back out) */
		struct overlay_folder *folder = overlay_folder();
		short selected = overlay.selected[overlay.depth];
		short type = selected < folder->count ? folder->entries[selected]->type : NONE;

		if (type == _overlay_entry_toggle)
			overlay_choose(selected);
		else if (pressed & (1UL << HALO_OVERLAY_PAD_LEFT))
			overlay_back();
		else if (type == _overlay_entry_folder)
			overlay_choose(selected);
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_B))
		overlay_back();
	if (pressed & ((1UL << HALO_OVERLAY_PAD_BACK) | (1UL << HALO_OVERLAY_PAD_START)))
		overlay_set_open(FALSE);
}

void overlay_menu_draw(
	void)
{
	/* the settings screens' colours (port/assets/menus, tools/port_settings.py):
	their items' blue (#2896FF), the chosen item's and the values' white (the
	menus' own, get_ui_argb_white), the rows' bands (bitmaps/option_bkds: a
	dark one, and the chosen one's with its blue outline) and the rules'
	blue */
	static real_argb_color const blue = { 1.0f, 0x28 / 255.0f, 0x96 / 255.0f, 1.0f };
	static real_argb_color const red = { 1.0f, 1.0f, 0.3f, 0.3f };
	static pixel32 const BLUE = 0xFF2896FFUL;
	static pixel32 const BAND = 0x99001122UL;
	static pixel32 const CHOSEN_BAND = 0xFF002244UL;
	static pixel32 const OUTLINE = 0xFF2299FFUL;
	static pixel32 const RULE = 0xC02896FFUL;
	real_argb_color white = get_ui_argb_white();
	struct overlay_folder *folder;
	/* the title's and the rows' font, the menus' own, and the foot's,
	smaller */
	long font_index;
	short line_height;
	long small_font_index;
	short small_line_height;
	short width;
	short height;
	short y;
	short row;
	rectangle2d panel;
	rectangle2d bounds;

	if (!overlay.open)
		return;
	overlay.drawn = FALSE;
	if (!overlay_font("ui\\large_ui", &font_index, &line_height) ||
		!overlay_font("ui\\small_ui", &small_font_index, &small_line_height))
	{
		if (!overlay.logged_draw)
		{
			overlay.logged_draw = TRUE;
			error(_error_silent, "overlay menu: not drawn: no font");
		}
		return;
	}
	width = (short)(render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0);
	height = (short)(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0);
	if (line_height <= 0 || width <= 0 || height <= 0)
		return;
	folder = overlay_folder();

	/* the panel, down the left of the screen */
	panel.x0 = 0;
	panel.y0 = 0;
	panel.x1 = (short)(width * OVERLAY_PANEL_PERCENT / 100);
	panel.y1 = height;
	draw_quad(&panel, (pixel32)OVERLAY_PANEL_ALPHA << 24);

	/* the title, and under it the folder gone into (chosen, it goes back) */
	y = OVERLAY_MARGIN;
	bounds.x0 = OVERLAY_MARGIN;
	bounds.x1 = (short)(panel.x1 - OVERLAY_MARGIN);
	bounds.y0 = y;
	bounds.y1 = (short)(y + line_height);
	{
		char title[64];

		overlay_capitals(title, sizeof(title), OVERLAY_TITLE);
		overlay_text(font_index, &bounds, _overlay_text_left, &blue, title);
	}
	y = (short)(y + line_height + OVERLAY_ROW_PADDING);
	overlay.back_top = overlay.back_bottom = y;
	if (overlay.depth > 0)
	{
		char text[96];

		overlay_capitals(text, sizeof(text), folder->label);
		bounds.x0 = (short)(OVERLAY_MARGIN + 2 * OVERLAY_ARROW_SIZE);
		overlay_arrow(OVERLAY_MARGIN, (short)(y + line_height / 2), FALSE, BLUE);
		bounds.y0 = y;
		bounds.y1 = (short)(y + line_height);
		overlay_text(font_index, &bounds, _overlay_text_left, &white, text);
		bounds.x0 = OVERLAY_MARGIN;
		y = (short)(y + line_height + OVERLAY_ROW_PADDING);
		overlay.back_bottom = y;
	}
	{
		rectangle2d rule;

		rule.x0 = OVERLAY_MARGIN;
		rule.x1 = (short)(panel.x1 - OVERLAY_MARGIN);
		rule.y0 = y;
		rule.y1 = (short)(y + 1);
		draw_quad(&rule, RULE);
		y = (short)(y + OVERLAY_ROW_PADDING);
	}

	/* the rows, as many as fit above the foot's line: each on a band, the
	chosen one's outlined, its label at the left and its value at the right
	between arrows, as a settings screen's */
	overlay.rows_top = y;
	overlay.row_height = (short)(line_height + 2 * OVERLAY_ROW_PADDING + OVERLAY_ROW_GAP);
	overlay.rows_shown = (short)MAX((height - OVERLAY_MARGIN - 3 * small_line_height - 3 * OVERLAY_ROW_PADDING - y) /
		overlay.row_height, 1);
	overlay.first_row = (short)PIN(overlay.first_row, 0, MAX(folder->count - overlay.rows_shown, 0));
	overlay_keep_selection_shown();
	for (row = overlay.first_row; row < folder->count && row < overlay.first_row + overlay.rows_shown; row++)
	{
		struct overlay_entry *entry = folder->entries[row];
		boolean chosen = row == overlay.selected[overlay.depth];
		rectangle2d band;
		rectangle2d room;
		char const *value = NULL;
		boolean arrows = FALSE;
		short middle;
		char label[96];

		band.x0 = (short)(OVERLAY_MARGIN / 2);
		band.x1 = (short)(panel.x1 - OVERLAY_MARGIN / 2);
		band.y0 = y;
		band.y1 = (short)(y + overlay.row_height - OVERLAY_ROW_GAP);
		if (chosen)
		{
			rectangle2d inside = band;

			overlay_rounded(&band, OVERLAY_ROW_RADIUS, OUTLINE);
			inside.x0 = (short)(inside.x0 + OVERLAY_OUTLINE);
			inside.x1 = (short)(inside.x1 - OVERLAY_OUTLINE);
			inside.y0 = (short)(inside.y0 + OVERLAY_OUTLINE);
			inside.y1 = (short)(inside.y1 - OVERLAY_OUTLINE);
			overlay_rounded(&inside, (short)(OVERLAY_ROW_RADIUS - OVERLAY_OUTLINE), CHOSEN_BAND);
		}
		else
		{
			overlay_rounded(&band, OVERLAY_ROW_RADIUS, BAND);
		}
		middle = (short)((band.y0 + band.y1) / 2);
		room.x0 = OVERLAY_MARGIN;
		room.x1 = (short)(band.x1 - OVERLAY_MARGIN / 2);
		room.y0 = (short)(y + OVERLAY_ROW_PADDING);
		room.y1 = (short)(room.y0 + line_height);
		switch (entry->type)
		{
		case _overlay_entry_folder:
			/* (a folder: an arrow into it) */
			overlay_arrow((short)(room.x1 - OVERLAY_ARROW_SIZE - 2), middle, TRUE, BLUE);
			room.x1 = (short)(room.x1 - 3 * OVERLAY_ARROW_SIZE);
			break;
		case _overlay_entry_toggle:
			value = overlay_entry_on(entry) ? "ON" : "OFF";
			arrows = TRUE;
			break;
		case _overlay_entry_choice:
			value = overlay_entry_on(entry) ? "ON" : NULL;
			break;
		}
		if (value || arrows)
		{
			rectangle2d value_room = room;

			value_room.x0 = (short)(room.x1 - OVERLAY_VALUE_WIDTH);
			if (arrows)
			{
				overlay_arrow(value_room.x0, middle, FALSE, BLUE);
				overlay_arrow((short)(value_room.x1 - OVERLAY_ARROW_SIZE), middle, TRUE, BLUE);
			}
			if (value)
				overlay_text(font_index, &value_room, 2 /* centred */, &white, value);
			/* (the label clear of it) */
			room.x1 = (short)(value_room.x0 - 4);
		}
		/* (a newly chosen row's label starts at its start) */
		if (chosen && (overlay.scroll_row != row || overlay.scroll_folder != folder))
		{
			overlay.scroll_row = row;
			overlay.scroll_folder = folder;
			overlay.scroll_since = system_milliseconds();
		}
		if (entry->type == _overlay_entry_malformed)
		{
			/* (a file's entry that could not be read: its label in red) */
			char text[96];

			if (entry->label && entry->label[0])
				snprintf(text, sizeof(text), "%s: malformed entry", entry->label);
			else
				snprintf(text, sizeof(text), "malformed entry: no label");
			overlay_capitals(label, sizeof(label), text);
			overlay_label(font_index, &room, &red, label, chosen);
		}
		else
		{
			overlay_capitals(label, sizeof(label), entry->label);
			overlay_label(font_index, &room, chosen ? &white : &blue, label, chosen);
		}
		y = (short)(y + overlay.row_height);
	}
	/* (more above or below: arrows at the list's right) */
	bounds.x0 = OVERLAY_MARGIN;
	bounds.x1 = (short)(panel.x1 - OVERLAY_MARGIN / 2);
	if (overlay.first_row > 0)
	{
		bounds.y0 = (short)(overlay.rows_top - line_height);
		bounds.y1 = overlay.rows_top;
		overlay_text(font_index, &bounds, _overlay_text_right, &blue, "^");
	}
	if (overlay.first_row + overlay.rows_shown < folder->count)
	{
		bounds.y0 = y;
		bounds.y1 = (short)(y + line_height);
		overlay_text(font_index, &bounds, _overlay_text_right, &blue, "v");
	}

	/* the chosen entry's note, above the foot (moving across if it is too
	long, as a chosen label) */
	{
		short selected = overlay.selected[overlay.depth];
		struct overlay_entry *entry = selected >= 0 && selected < folder->count ? folder->entries[selected] : NULL;

		if (entry && entry->note && entry->note[0])
		{
			bounds.x0 = OVERLAY_MARGIN;
			bounds.x1 = (short)(panel.x1 - OVERLAY_MARGIN);
			bounds.y1 = (short)(height - OVERLAY_MARGIN - small_line_height - OVERLAY_ROW_PADDING);
			bounds.y0 = (short)(bounds.y1 - small_line_height);
			overlay_label(small_font_index, &bounds, &blue, entry->note, TRUE);
		}
	}

	/* the foot: what the last command did, else the buttons */
	bounds.x0 = OVERLAY_MARGIN;
	bounds.x1 = (short)(panel.x1 - OVERLAY_MARGIN);
	bounds.y1 = (short)(height - OVERLAY_MARGIN);
	bounds.y0 = (short)(bounds.y1 - small_line_height);
	if (overlay.status[0] && (long)(system_milliseconds() - overlay.status_until) < 0)
		overlay_text(small_font_index, &bounds, _overlay_text_left, &white, overlay.status);
	else
	{
		draw_string_set_draw_mode(small_font_index, NONE, _overlay_text_left, 0, &white);
		draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, OVERLAY_HINT, FALSE);
	}

	overlay.panel_right = panel.x1;
	overlay.drawn = TRUE;
}

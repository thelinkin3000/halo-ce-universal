/*
NETWORK_REPLAY_PLAYBACK.C

A replay's playback, phase 1 of port/linux/REPLAY.md: the game a host
recorded (network_replay.c), watched from behind any of its players.

It plays as a client of the distributed netcode does, with no player of
its own and no host: the global client is made one joined to the game the
replay's first record holds (network_game_client_port_replay_begin), which
loads it as a client loads the game it joins; its clock starts at the
replay's first tick, and each frame the records of the ticks it has
reached are handed to what a client hands the host's messages it reads to:
the distributed netcode's (network_distributed_handle_message, the
datagrams marked unreliable and the rest reliable, as they came) and the
game's own (network_game_client_handle_message). So the replay is the game
as a client simulates it from the host's messages: the players driven by
their relayed input, corrected toward the host's states, the actors as the
host ran them, the damage, scores and the game's end as the host decided
them. The client sends nothing (network_game_client_write).

The view (network_replay_camera, made once a frame by
network_replay_playback_frame: main.c's window and the sound's listener)
is one of four, Y going round them: behind the player watched (or the
vehicle they ride), drawn where the frame draws them (render
interpolation), kept on this side of walls, turning with them until the
right stick flies it round them (playback_behind_camera; its click turns
it with them again), the left stick zooming; what they saw, where they aim:
through their eyes with their arms and weapon (first_person_weapons.c),
or, in a seat with a third-person camera (a Warthog's, a Ghost's), the
game's view from behind it (following_camera_unit_view); or a free
camera, as the game's debug flying camera: the left stick moving it level
(toward where it looks across the ground, whatever its pitch; a slight tilt
slowly, its speed eased up and down: playback_free_camera_speed), the right
one turning it and the triggers taking it down (left) and up (right), eased
up from still (the Switch's are buttons: playback_free_camera_rise),
sliding along walls rather than through them (playback_free_camera_move);
a tripod, placed where behind's would be when it is chosen, tracking the
player (playback_free_camera_track) as it moves until the right stick takes
the aim (R3 or D-pad Up tracks again, or stops; D-pad Down or L3 puts it
behind the player again, tracking them, as when it was chosen), and zooming on them
as they go further until it first moves (playback_free_camera_zoom); or a
cinematic one, the free camera with no controls: placed and tracking as it,
and placed behind the player again whenever the level hides them from it
for CINEMATIC_HIDDEN_TIME (playback_cinematic_sight), at once in the
CINEMATIC_SPAWN_TIME after they spawn. D-pad
Up turns behind with the player again too, as R3 does.
Through the eyes, the aim turns smoothly between the facings the replay
has of the player (playback_index_facings, playback_eyes_aim) and the host's
corrections of where they are are glided over (playback_eyes_glide): a
client's recording has the players far from its own, or out of their
sight, every second to sixth tick, and its other players' eyes show
SMOOTHED. A watches the next player, X the one before; the right shoulder
button (RB, the Switch's R, the PS5's R1) plays faster and the left one
slower, to a pause (game_time_set_speed); Back held shows the scores (the
watched's row as a player's own); B hides the lines over the screen. Start
puts up the pause menu (the game stopped, the camera kept): B in it leaves,
Start resumes, the north button (Y) hides the hints' bars or shows them
again (network.replay_hints, kept in config.toml). A line at the top says who is
watched, the view and the speed, a dark bar near the bottom edge the
buttons, each as Kenney's picture of the controller's
(hud_hires_prompt_draw), and under it the seek bar: the time, a Halo blue
line with the playhead a circle on it, and the replay's length
(playback_draw_seek)
(network_replay_playback_draw). The game's end, or the replay's, returns
to the Theater half a second later (the main menu opens it:
network_replay_theater_wanted).

Keyframes (playback_keyframe_take): every 10 seconds of the game, as the
playback first reaches them, the game state's parts in use and the
simulation's and the netcode's variables outside it (gathered by the guest
image's linker script, guest.ld's .replay_data and .replay_bss), with
where the file's records were: copied in one go, then compressed in memory
on a thread of its own (playback_compressor_main) while the game goes on. A tap of
the d-pad's left puts the last one back (the one before it, just after
one), as a core is loaded (game_state_port_restore), and of its right the
next, or with none ahead sets a target ten seconds on and plays to it: at
50 times, its records before each tick (network_replay_playback_tick), the
frame it was asked in held, the lines over it as they change, and the
sound paused (network_replay_frozen, rasterizer_port_hold_draw), then at
normal speed. Taps add up, each from the last's target. Held, a direction
scrubs (playback_scrub): the target, a marker on the seek bar, slides that
way, faster as it is held, and the playback chases it, the keyframes on
the way put back (playback_seek_chase); back, once it is let go.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/players.h"
#include "input/input.h"
#include "interface/hud_messaging.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_client_manager.h"
#include "networking/network_client_message_handler.h"
#include "bungie_net/network/transport.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "text/draw_string.h"
#include "tag_files/tag_groups.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "camera/director.h"
#include "camera/following_camera.h"
#include "camera/observer.h"
#include "sound/sound_manager.h"
#include "network_distributed.h"
#include "network_replay.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h" /* source/memory/zlib */

/* the replay's records, kept in the host's memory (port/android/host/host_replay.c;
port/android/guest/runtime/guest_host.h) */
int host_replay_store_append(unsigned int data, unsigned int size);
unsigned int host_replay_store_read(unsigned int offset, unsigned int destination, unsigned int size);
void host_replay_store_free(void);

/* the touch controls (port/linux/src/touch_input.c): whether they show, and
the replay's buttons for them to press, each TOUCH_TARGET_FIELDS ints (x0,
y0, x1, y1 in ten-thousandths of the screen, and its controller button: its
touch_input.h's TOUCH_REPLAY_*); and the view swipe, as port 0's player's
(xinput_sdl.c). The Switch has the Android guest, and no touchscreen */
#if defined(HALO_ANDROID) && !defined(HALO_SWITCH)
#define REPLAY_TOUCH 1
int touch_input_screen_buttons(void);
void touch_input_replay_targets(const int *targets, int count);
int touch_input_move(float *forward, float *strafe);
#endif
int halo_linux_touch_look(short gamepad_index, real *yaw, real *pitch, real *gyro_yaw, real *gyro_pitch);
#define REPLAY_TOUCH_MAXIMUM_TARGETS 16
#define REPLAY_TOUCH_TARGET_FIELDS 5

FILE *platform_fopen_host(char const *path, char const *mode);
int config_boolean(char const *name);
int config_write_boolean(const char *name, int value);
void network_distributed_handle_message(long machine_index, word const *message, word size);
void network_connection_port_set_last_read_unreliable(boolean unreliable);
boolean network_game_client_port_replay_begin(struct network_game const *game);
boolean network_game_client_port_in_postgame(struct network_game_client *client);
void network_game_abort(void);
void render_interpolation_object_point(long object_index, real_point3d *point);
real render_interpolation_fraction(void);
real main_get_seconds_elapsed(void);
void game_engine_port_rasterize_scores(long player_index);
void draw_quad(rectangle2d *rectangle, pixel32 color);
unsigned long system_milliseconds(void);
void game_state_port_used(void **cpu, long *cpu_size, void **gpu, long *gpu_size);
void game_state_port_restore(void (*read)(void *context), void *context);
void render_interpolation_reset(void);
void first_person_weapons_initialize_for_new_map(void);
boolean hud_hires_prompt_draw(char const *name, real x, real y, real size, pixel32 color, boolean draw);

/* the simulation's and the netcode's variables outside the game state, all
together (port/android/guest/guest.ld's .replay_data and .replay_bss) */
extern byte __replay_data_start[], __replay_data_end[];
extern byte __replay_bss_start[], __replay_bss_end[];

/* a keyframe: the game's tick it was taken at, where the file's records
were then, and the game state and those variables, compressed */
struct replay_keyframe
{
	long tick;
	long next;
	byte *data;
	unsigned long size;
};

/* a player's facing as the client had it: the tick the record that brought
it was read at, and its yaw and pitch as the wire has them
(distributed_angle_unpack's) */
struct replay_facing
{
	long tick;
	short yaw;
	short pitch;
};

/* the file (network_replay.c, REPLAY.md's "The file") */
#define REPLAY_MAGIC "OCREPLAY"
#define REPLAY_RECORD_HEADER_SIZE 8
/* (format 4) the header's flags, and a client's recording's */
#define REPLAY_FLAGS_OFFSET 284
#define REPLAY_FLAG_CLIENT 1
/* the oldest format a playback reads: the first with the game's record;
and the first whose records are one zlib stream after the header */
#define REPLAY_PLAYABLE_FORMAT 2
#define REPLAY_COMPRESSED_FORMAT 3
/* the oldest network version played (the first whose replays could be:
format 2), and the distributed messages whose layout changed since, not
read from a replay older than the version that changed them
(port/linux/network_format.txt's fingerprints; 40 changed only a client's
join request, which no replay has) */
#define REPLAY_OLDEST_NETWORK_VERSION 39
static struct
{
	unsigned long changed_in;
	byte type;
} const replay_changed_messages[] = {
	/* (41: a vote's status, with the vote to end the game's) */
	{ 41, _distributed_message_votekick_status },
};
/* the largest replay file read, and the most of its records kept (inflated,
in the host's memory: host_replay.c) */
#define REPLAY_MAXIMUM_FILE_SIZE (256 * 1024 * 1024)
#define REPLAY_MAXIMUM_RECORDS_SIZE (1024 * 1024 * 1024)
/* the window of the records the guest reads them through (larger than the
largest record, a header and 64 KB); as the file is read, its first half takes
the file's bytes and its second their inflated records */
#define REPLAY_WINDOW_SIZE (1024 * 1024)
#define REPLAY_LOAD_PART (REPLAY_WINDOW_SIZE / 2)
/* how long the main menu waits after the replay's end, or the game's */
#define REPLAY_END_MILLISECONDS 500
/* the least time between two of D-pad Down's resets of the free camera */
#define REPLAY_CAMERA_RESET_MILLISECONDS 150

/* the camera: how far behind and above what it watches (a unit on foot, or
the vehicle it rides), its pitch, and how much of its turn toward the
watched's facing it makes a frame */
#define CAMERA_DISTANCE 3.5f
#define CAMERA_VEHICLE_DISTANCE 7.0f
#define CAMERA_HEIGHT 0.6f
#define CAMERA_VEHICLE_HEIGHT 1.5f
#define CAMERA_PITCH -0.3f
#define CAMERA_TURN 0.12f
#define CAMERA_FIELD_OF_VIEW DEGREES_TO_RADIANS(70.0f)
/* (how far from a wall the camera stays) */
#define CAMERA_WALL_MARGIN 0.2f
/* behind, flown round the player with the right stick: how fast it turns
and rises (radians a second, the stick all the way), and how high and low
it goes (a little short of straight down on them, a little below level);
and the left stick's zoom (how fast, a second, and how near and far, of
the distance behind) */
#define CAMERA_ORBIT_TURN 2.5f
#define CAMERA_ORBIT_HIGHEST -1.5f
#define CAMERA_ORBIT_LOWEST 0.35f
#define CAMERA_ZOOM_RATE 1.2f
#define CAMERA_ZOOM_NEAREST 0.4f
#define CAMERA_ZOOM_FARTHEST 4.0f
/* the eyes view: the most ticks between two of a player's facings it turns
smoothly across (a far player's come every second to sixth tick, one lost
between them doubling that); the most the facing may be from the unit's
aim and be the view's (radians: more, and the facing is not the unit's: a
dead player's, a respawn's); and the host's corrections of where the player is that it
glides over: the least a frame's move may differ from their speed's to be
one, the most (more is a teleport, taken at once), and how fast what is
left of one goes (seconds of the game to go to a third) */
#define EYES_FACING_MAXIMUM_TICKS 13
#define EYES_FACING_MAXIMUM_TURN (_pi / 2.0f)
#define EYES_CORRECTION_MINIMUM 0.02f
#define EYES_CORRECTION_MAXIMUM 2.0f
#define EYES_CORRECTION_TIME 0.1f
/* the free camera: how fast it moves (world units a second), turns
(radians a second, the stick all the way) and how far it pitches */
#define FREE_CAMERA_SPEED 6.0f
#define FREE_CAMERA_TURN 2.5f
#define FREE_CAMERA_MAXIMUM_PITCH 1.5f
/* ... how its speed follows the left stick: as the stick's tilt to this
power (a slight tilt a slow move: a quarter of the way, a sixteenth of the
speed), reaching it in this long from still to full and stopping in this
long from full (seconds); and how long a trigger held takes it to its full
speed up or down, from none (seconds: the Switch's ZL and ZR are buttons,
all or nothing, so a tap is a small step; another's, pulled slowly, is as
slow as it is pulled) */
#define FREE_CAMERA_MOVE_POWER 2.0f
#define FREE_CAMERA_ACCELERATION_TIME 0.2f
#define FREE_CAMERA_BRAKE_TIME 0.08f
#define FREE_CAMERA_RISE_TIME 0.25f
/* what the free camera does not fly through: the level, its scenery and its
machines (doors, lifts), invisible walls too (the map's edges); how far off
them it keeps; and how many walls a frame's move slides along */
#define FREE_CAMERA_COLLISION_FLAGS (FLAG(_collision_test_front_facing_surfaces_bit) | \
	FLAG(_collision_test_structure_bit) | FLAG(_collision_test_objects_scenery_bit) | \
	FLAG(_collision_test_objects_machines_bit))
#define FREE_CAMERA_WALL_MARGIN 0.15f
#define FREE_CAMERA_SLIDES 3
/* the free camera tracking the player, as on a tripod: how long its aim
lags theirs (seconds, the time it takes to make most of a turn toward
them); and, while it has not moved since it was placed, its zoom: the view
narrowed as they go further than this (world units), down to this, so they
stay the size they were, eased over this long (seconds) */
#define FREE_CAMERA_TRACK_TIME 0.15f
#define FREE_CAMERA_ZOOM_DISTANCE 5.0f
#define FREE_CAMERA_NARROWEST DEGREES_TO_RADIANS(8.0f)
#define FREE_CAMERA_ZOOM_TIME 0.3f
/* the cinematic camera: how long the player may be hidden from it (seconds)
before it is placed behind them again, and how long after they spawn it is
placed at once if they are (seconds) */
#define CINEMATIC_HIDDEN_TIME 0.2f
#define CINEMATIC_SPAWN_TIME 0.5f
/* the keyframes: one every 10 seconds of the game, as the playback first
reaches it, compressed in memory (zlib's fastest level), up to this much
memory in all (and this many); and how long after one d-pad left goes to
the one before it instead */
#define REPLAY_KEYFRAME_TICKS (10 * TICKS_PER_SECOND)
#define REPLAY_MAXIMUM_KEYFRAMES 1024
#define REPLAY_KEYFRAME_MEMORY (256UL * 1024 * 1024)
#define REPLAY_REWIND_GRACE_TICKS (2 * TICKS_PER_SECOND)
/* d-pad right with no keyframe ahead: the game played ten seconds on this
many times as fast, and no more ticks than this a frame (the controls still
read, and its tick, and a keyframe's, never passed); the frame it was asked
in held meanwhile (playback_skip), or drawn as it plays if it was not held
within this many frames */
#define REPLAY_SKIP_SPEED 50.0f
#define REPLAY_SKIP_TICKS_PER_FRAME 60
#define REPLAY_SKIP_HOLD_FRAMES 3
/* a d-pad direction held this long (milliseconds) scrubs: the skip's
target, the marker on the seek bar, slides that way, this many ticks of the
game a second at first, faster over this long (milliseconds) until it
crosses the whole bar in this many seconds (never slower than at first) */
#define REPLAY_SCRUB_HOLD_MILLISECONDS 300
#define REPLAY_SCRUB_SLOW_TICKS (10 * TICKS_PER_SECOND)
#define REPLAY_SCRUB_RAMP_MILLISECONDS 1500
#define REPLAY_SCRUB_CROSSING_SECONDS 4.0f
/* the text on the screen: how far from its top edge the top line is (and
twice how far from the bottom the hints' bar), the hints' height (a button's
picture's), the bar's room either side of them and its colour */
#define REPLAY_SCREEN_MARGIN 12
#define REPLAY_HINT_HEIGHT 26
#define REPLAY_HINT_PADDING 10
#define REPLAY_HINT_BAR_COLOR 0xA0000000
/* the room between the buttons' bar and the camera's above it */
#define REPLAY_HINT_ROW_GAP 4
/* a button's picture's size, the room after it, and between two hints */
#define REPLAY_HINT_ICON_SIZE 18
#define REPLAY_HINT_ICON_GAP 3
#define REPLAY_HINT_SPACING 12
/* the seek bar under the hints (playback_draw_seek): its line's height, how
far it is from the screen's sides, the room either side of the line for
the times (mm:ss), the line's thickness and the playhead's radius, and its
colours: the Halo blue of the menus' text, the line ahead of the playhead
dimmer */
#define REPLAY_SEEK_HEIGHT 20
#define REPLAY_SEEK_INSET 40
#define REPLAY_SEEK_TIME_WIDTH 44
#define REPLAY_SEEK_THICKNESS 4
#define REPLAY_SEEK_HEAD_RADIUS 6
#define REPLAY_SEEK_COLOR 0xFF2896FF
#define REPLAY_SEEK_AHEAD_COLOR 0x602896FF
/* ... while a skip ahead plays: the playhead throbs (its radius up this
much more, its glow this much wider and its alpha up to this) once in this
long (milliseconds, the clock's: the game's time races), and where the skip
goes is a tick on the line */
#define REPLAY_SEEK_THROB_GROWTH 2
#define REPLAY_SEEK_GLOW_WIDTH 7
#define REPLAY_SEEK_GLOW_ALPHA 0x70
#define REPLAY_SEEK_THROB_MILLISECONDS 700
#define REPLAY_SEEK_TARGET_COLOR 0xC0FFFFFF
/* the screen's dimming under the pause menu */
#define REPLAY_PAUSE_DIM_COLOR 0x80000000
/* on a touchscreen, the buttons are boxes a finger presses (the touch
controls press the controller's button under it): their height, the room
between two and how wide one is at most; and their fill, held */
#define REPLAY_TOUCH_HEIGHT 40
#define REPLAY_TOUCH_GAP 4
#define REPLAY_TOUCH_MAXIMUM_WIDTH 130
/* ... and in two rows past this many (one row of them all left each too
narrow for its word) */
#define REPLAY_TOUCH_ONE_ROW 6
#define REPLAY_TOUCH_HELD_COLOR 0xC02896FF
/* a stick's or a trigger's dead middle, of its whole */
#define STICK_DEAD_ZONE 0.15f

/* a hint at the bottom: its button, or two (NONE for none), and what it
does */
struct playback_hint
{
	short buttons[2];
	wchar_t const *action;
};

/* the sticks, in a hint (no button: past the gamepad's) */
enum
{
	_replay_hint_stick_left = NUMBER_OF_GAMEPAD_BUTTONS,
	_replay_hint_stick_right,
};

/* a skip ahead's held frame (skip_hold) */
enum
{
	_replay_hold_wanted,
	_replay_hold_taken,
	_replay_hold_none,
};

/* the views Y goes round */
enum
{
	_replay_view_behind,
	_replay_view_eyes,
	_replay_view_free,
	_replay_view_cinematic,
	NUMBER_OF_REPLAY_VIEWS
};

/* the speeds the shoulder buttons go through (0 a pause), and the
normal one's */
static real const replay_speeds[] = { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
#define REPLAY_NORMAL_SPEED 3

static struct
{
	/* the window the replay's records are read through (they are kept in
	the host's memory: host_replay.c; NULL when no replay is), the records'
	size (the file's header with them) and where the next record is */
	byte *window;
	long size;
	long next;
	long start_tick;
	/* the tick of its last record: where it ends (a file a crash cut short,
	where it was cut) */
	long end_tick;
	/* the replay is being set up (the old client's disposal does not stop
	it: network_replay_playback_stop) */
	boolean setting_up;
	/* the game is to start at the client's next idle; it has */
	boolean starting;
	boolean playing;
	/* when the replay (or the game) ended, for the return to the menu; 0
	before then; and whether the return has been asked for */
	unsigned long ended;
	boolean leaving;
	/* the first tick's records handed over (the host's objects all told) */
	boolean synchronized;
	/* the player watched (a datum index), NONE before one is */
	long watched;
	/* the network version it was recorded in (older ones: replay_changed_messages) */
	unsigned long network_version;
	/* a client's recording, and its own players (by absolute index): only
	theirs is a view through the eyes, the others' it never had */
	boolean client;
	boolean recorders[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* every player's facings through the replay (playback_index_facings),
	each player's (by absolute index) together, in the order they were
	read: the first of them and how many */
	struct replay_facing *facings;
	long facing_first[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	long facing_count[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* the eyes view's place as it was last made: the unit, the moment
	(playback_eyes_time), where its eyes were drawn, and what of the host's
	corrections of them is still being glided over (playback_eyes_glide) */
	long eyes_unit;
	real eyes_time;
	real_point3d eyes_drawn;
	real_vector3d eyes_offset;
	/* the buttons held at the last idle (each press counted once) */
	boolean held[NUMBER_OF_GAMEPAD_BUTTONS];
	/* the view, the speed (replay_speeds'), and whether the scores are
	shown (Back held); the lines over the screen hidden (B); the pause
	menu up (Start: the game stopped, the camera kept where it was) */
	short view;
	short speed;
	boolean scores;
	boolean hidden;
	boolean paused;
	/* the camera, made once a frame; whether it is the watched player's own
	first-person view (their arms and weapon drawn: first_person_weapons.c);
	behind's turn; the free camera's place and its turns */
	boolean camera_valid;
	boolean first_person;
	/* the free camera: chosen and not yet placed (it is placed behind the
	player, as behind's would be); turning to keep the player in sight
	(R3 toggles it; the right stick takes the aim); not moved since it was
	placed (zooming on the player as they go further); its view's width;
	and where the player is aimed at (kept once they die), if anywhere */
	boolean free_placing;
	boolean free_tracking;
	boolean free_still;
	/* the cinematic camera: how long the player has been hidden from it
	(seconds), whether they have died since it was placed, and what is left
	of the CINEMATIC_SPAWN_TIME after they spawned (seconds) */
	real cinematic_hidden;
	boolean cinematic_died;
	real cinematic_spawned;
	/* when D-pad Down last put the free camera back behind the
	player (system_milliseconds: REPLAY_CAMERA_RESET_MILLISECONDS) */
	unsigned long free_reset_at;
	real free_field_of_view;
	boolean free_focus_valid;
	real_point3d free_focus;
	/* behind: flown round the player (the right stick touched since the
	view was chosen), its pitch then, and the left stick's zoom (a scale of
	the distance behind) */
	boolean behind_orbiting;
	real behind_pitch;
	real behind_zoom;
	real yaw;
	real_point3d free_position;
	real free_yaw;
	real free_pitch;
	/* ... its move (of the left stick's, as it follows it:
	playback_free_camera_speed) and how far up a trigger held has taken
	its rise (0 to 1, and which way: playback_free_camera_rise) */
	real free_move_x;
	real free_move_y;
	real free_rise;
	struct observer_result camera;
	/* the touch controls' view swipe this frame (radians: the free camera's
	aim, behind's orbit) */
	real touch_yaw;
	real touch_pitch;
	/* the keyframes, by their ticks; the memory they take; where a
	keyframe is compressed (as large as the largest it can be); and the
	skip's target, the tick the playback goes to, fast (the marker on the
	seek bar; NONE when not skipping) */
	struct replay_keyframe keyframes[REPLAY_MAXIMUM_KEYFRAMES];
	long keyframe_count;
	unsigned long keyframe_memory;
	byte *keyframe_scratch;
	unsigned long keyframe_scratch_size;
	boolean keyframes_full;
	/* the newest keyframe's tick, taken (or being compressed), NONE before
	the first */
	long keyframe_frontier;
	long skip_tick;
	/* the skip's held frame: to be copied (the frame drawn next, which runs
	no tick), copied (each frame's view until the skip ends: network_replay_frozen),
	or not (the game drawn as it plays); and the frames waited for it */
	short skip_hold;
	short skip_hold_frames;
	/* the d-pad's direction held (NONE when neither is: the other is not
	heard meanwhile) and when it was pressed (the clock's milliseconds);
	whether it is held long enough to scrub, the marker where it has slid
	to (a tick, in part) and when it last slid */
	short scrub_button;
	unsigned long scrub_pressed;
	boolean scrubbing;
	real scrub_marker;
	unsigned long scrub_moved;
	/* a record's bytes, aligned as the handlers read them */
	word message[0x10000 / sizeof(word)];
	/* where in the records the window starts, and how much of them it has */
	long window_start;
	long window_length;
} playback = { NULL, 0, 0, 0, FALSE, FALSE, FALSE, 0, FALSE, FALSE, NONE };

static unsigned long replay_get_long(
	byte const *source)
{
	return (unsigned long)source[0] | (unsigned long)source[1] << 8 | (unsigned long)source[2] << 16 |
		(unsigned long)source[3] << 24;
}

/* the record at offset in the replay's records, through the window (read
again from the host where it does not have all of it), and its size; NULL
where none is whole (the records' end, or a file a crash cut short). The
pointer holds until the next call */
static byte const *playback_record(
	long offset,
	word *size)
{
	long length;

	if (offset < 0 || offset + REPLAY_RECORD_HEADER_SIZE > playback.size)
		return NULL;
	length = REPLAY_RECORD_HEADER_SIZE;
	for (;;)
	{
		if (offset < playback.window_start || offset + length > playback.window_start + playback.window_length)
		{
			playback.window_start = offset;
			playback.window_length = (long)host_replay_store_read((unsigned int)offset,
				(unsigned int)(uintptr_t)playback.window, REPLAY_WINDOW_SIZE);
			if (length > playback.window_length)
				return NULL;
		}
		if (length > REPLAY_RECORD_HEADER_SIZE)
			return playback.window + (offset - playback.window_start);
		{
			byte const *header = playback.window + (offset - playback.window_start);

			*size = (word)(header[2] | header[3] << 8);
		}
		length = REPLAY_RECORD_HEADER_SIZE + *size;
		if (offset + length > playback.size)
			return NULL;
	}
}

/* the replay's records read from the file (after its header, which is
kept already) into the host's memory (host_replay.c): as they are (formats
before 4), or inflated (one zlib stream, format 4 on), as much of them as
there is of a file a crash cut short. The guest's memory, below 4 GB, holds
none of them but its window's: a long match's replay of 46 MB inflates to
152 MB, which the Switch's guest could not be given in one piece.
REPLAY_LOADED, REPLAY_LOAD_UNREADABLE (not even part of a stream), or
REPLAY_LOAD_NO_MEMORY (the host's) */
enum
{
	REPLAY_LOADED,
	REPLAY_LOAD_UNREADABLE,
	REPLAY_LOAD_NO_MEMORY,
};

/* records kept; FALSE if the host has not the memory for them */
static boolean playback_keep(
	byte const *data,
	unsigned long size,
	boolean *capped)
{
	if ((unsigned long)playback.size + size > REPLAY_MAXIMUM_RECORDS_SIZE)
	{
		size = REPLAY_MAXIMUM_RECORDS_SIZE - (unsigned long)playback.size;
		*capped = TRUE;
	}
	if (size && !host_replay_store_append((unsigned int)(uintptr_t)data, (unsigned int)size))
		return FALSE;
	playback.size += (long)size;
	return TRUE;
}

static int playback_load_records(
	FILE *file,
	unsigned long header_size,
	boolean compressed)
{
	byte *in = playback.window;
	byte *out = playback.window + REPLAY_LOAD_PART;
	z_stream stream;
	int result = Z_OK;
	boolean capped = FALSE;
	boolean ended;

	if (compressed)
	{
		csmemset(&stream, 0, sizeof(stream));
		if (inflateInit(&stream) != Z_OK)
			return REPLAY_LOAD_NO_MEMORY;
	}
	while (!capped && result == Z_OK)
	{
		size_t read = fread(in, 1, REPLAY_LOAD_PART, file);

		if (!read)
			break;
		if (!compressed)
		{
			if (!playback_keep(in, read, &capped))
				return REPLAY_LOAD_NO_MEMORY;
			continue;
		}
		stream.next_in = in;
		stream.avail_in = (uInt)read;
		do
		{
			stream.next_out = out;
			stream.avail_out = REPLAY_LOAD_PART;
			result = inflate(&stream, Z_NO_FLUSH);
			/* (none to be made of what is left of this part: the next's) */
			if (result == Z_BUF_ERROR)
				result = Z_OK;
			if (result != Z_OK && result != Z_STREAM_END)
				break;
			if (!playback_keep(out, REPLAY_LOAD_PART - stream.avail_out, &capped))
			{
				inflateEnd(&stream);
				return REPLAY_LOAD_NO_MEMORY;
			}
		} while (!capped && result == Z_OK && !stream.avail_out);
	}
	ended = !compressed || result == Z_STREAM_END;
	if (compressed)
		inflateEnd(&stream);
	if ((unsigned long)playback.size <= header_size)
		return REPLAY_LOAD_UNREADABLE;
	if (capped)
		error(_error_log, "replay: its records are more than %d MB: the first %d MB kept",
			REPLAY_MAXIMUM_RECORDS_SIZE >> 20, REPLAY_MAXIMUM_RECORDS_SIZE >> 20);
	else if (!ended)
		error(_error_log, "replay: the file ends before its stream does (cut short): %lu bytes of records kept",
			(unsigned long)playback.size - header_size);
	error(_error_log, "replay: %lu MB of records kept in the host's memory",
		(unsigned long)playback.size >> 20);
	return REPLAY_LOADED;
}

static boolean playback_keyframe_collect(boolean wait);

/* a skip ahead ended, reached or not: the frames drawn and the sound heard
again (the speed is the caller's) */
static void playback_skip_stop(
	void)
{
	if (playback.skip_tick == NONE)
		return;
	playback.skip_tick = NONE;
	playback.skip_hold = _replay_hold_wanted;
	playback.skip_hold_frames = 0;
	sound_pause(FALSE);
}

/* a skip and a scrub ended where the playback is, the d-pad held or not
(Start, the speed buttons): a press again starts anew */
static void playback_seek_stop(
	void)
{
	playback_skip_stop();
	playback.scrub_button = NONE;
	playback.scrubbing = FALSE;
}

/* every keyframe let go */
static void playback_keyframes_free(
	void)
{
	long index;

	/* (one being compressed done with first, and dropped; the buffers kept
	for the next replay. The game's free takes no NULL: debug_memory.c) */
	playback_keyframe_collect(TRUE);
	for (index = 0; index < playback.keyframe_count; index++)
		free(playback.keyframes[index].data);
	playback.keyframe_count = 0;
	playback.keyframe_memory = 0;
	playback.keyframe_frontier = NONE;
	playback.keyframes_full = FALSE;
	playback_seek_stop();
}

static void playback_free(
	void)
{
	/* (the game's free takes no NULL: debug_memory.c) */
	if (playback.window)
		free(playback.window);
	playback.window = NULL;
	playback.window_start = 0;
	playback.window_length = 0;
	host_replay_store_free();
	playback.size = 0;
	playback.next = 0;
	playback.starting = FALSE;
	playback.playing = FALSE;
	playback.ended = 0;
	playback.leaving = FALSE;
	playback.synchronized = FALSE;
	playback.watched = NONE;
	playback.client = FALSE;
	csmemset(playback.recorders, 0, sizeof(playback.recorders));
	if (playback.facings)
		free(playback.facings);
	playback.facings = NULL;
	csmemset(playback.facing_first, 0, sizeof(playback.facing_first));
	csmemset(playback.facing_count, 0, sizeof(playback.facing_count));
	playback.eyes_unit = NONE;
	playback.view = _replay_view_behind;
	playback.speed = REPLAY_NORMAL_SPEED;
	playback.scores = FALSE;
	playback.hidden = FALSE;
	playback.paused = FALSE;
	playback.camera_valid = FALSE;
	playback_keyframes_free();
	playback.behind_orbiting = FALSE;
	playback.behind_zoom = 1.0f;
}

/* whether the replay is a client's, and its own players: the flag in the
header (format 4), and the players of its own input's records (an older
client's recording has those alone) */
static void playback_find_recorders(
	unsigned long format,
	byte const *header,
	unsigned long header_size)
{
	long next = playback.next;
	byte const *record;
	word size;

	playback.client = format >= 4 && header_size >= REPLAY_FLAGS_OFFSET + 4 &&
		(replay_get_long(header + REPLAY_FLAGS_OFFSET) & REPLAY_FLAG_CLIENT) != 0;
	csmemset(playback.recorders, 0, sizeof(playback.recorders));
	playback.end_tick = playback.start_tick;
	while ((record = playback_record(next, &size)) != NULL)
	{
		playback.end_tick = MAX(playback.end_tick, (long)replay_get_long(record + 4));
		if (record[0] == _network_replay_own_actions)
		{
			short player_index = network_distributed_own_actions_player(record + REPLAY_RECORD_HEADER_SIZE, size);

			if (player_index >= 0 && player_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
			{
				playback.client = TRUE;
				playback.recorders[player_index] = TRUE;
			}
		}
		next += REPLAY_RECORD_HEADER_SIZE + size;
	}
}

/* the players' facings through the whole replay, as the client read them:
the host's relayed input (a far player's every second to sixth tick), and a
client's recording's own input (every tick), for the eyes view to turn
smoothly between (playback_eyes_facing). Counted, then kept; a facing older
than one the player already had is dropped, as the client drops it
(distributed_add_new_ticks), and a second one read at the same tick takes the
first's place */
struct playback_facing_scan
{
	boolean counting;
	long tick;
	long counts[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	long newest[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	boolean any[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
};

static void playback_facing_found(
	void *context,
	short player_index,
	long tick,
	short yaw,
	short pitch)
{
	struct playback_facing_scan *scan = (struct playback_facing_scan *)context;
	struct replay_facing *facing;
	long count;

	if (player_index < 0 || player_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		return;
	if (scan->any[player_index] && tick <= scan->newest[player_index])
		return;
	scan->any[player_index] = TRUE;
	scan->newest[player_index] = tick;
	if (scan->counting)
	{
		scan->counts[player_index]++;
		return;
	}
	count = playback.facing_count[player_index];
	facing = playback.facings + playback.facing_first[player_index] + count;
	if (count > 0 && facing[-1].tick == scan->tick)
		facing--;
	else if (count < scan->counts[player_index])
		playback.facing_count[player_index]++;
	else
		return;
	facing->tick = scan->tick;
	facing->yaw = yaw;
	facing->pitch = pitch;
}

static void playback_scan_facings(
	struct playback_facing_scan *scan)
{
	long next = playback.next;

	byte const *record;
	word size;

	csmemset(scan->newest, 0, sizeof(scan->newest));
	csmemset(scan->any, 0, sizeof(scan->any));
	while ((record = playback_record(next, &size)) != NULL)
	{
		scan->tick = (long)replay_get_long(record + 4);
		switch (record[0])
		{
		case _network_replay_datagram:
		case _network_replay_reliable:
		case _network_replay_reliable_to_machine:
			network_distributed_replay_facings(record + REPLAY_RECORD_HEADER_SIZE, size, FALSE,
				playback_facing_found, scan);
			break;
		case _network_replay_own_actions:
			network_distributed_replay_facings(record + REPLAY_RECORD_HEADER_SIZE, size, TRUE,
				playback_facing_found, scan);
			break;
		}
		next += REPLAY_RECORD_HEADER_SIZE + size;
	}
}

static void playback_index_facings(
	void)
{
	struct playback_facing_scan scan;
	long total = 0;
	short player_index;

	csmemset(&scan, 0, sizeof(scan));
	scan.counting = TRUE;
	playback_scan_facings(&scan);
	for (player_index = 0; player_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; player_index++)
	{
		playback.facing_first[player_index] = total;
		playback.facing_count[player_index] = 0;
		total += scan.counts[player_index];
	}
	/* (none: the eyes view turns with the players' ticks) */
	if (!total || !(playback.facings = (struct replay_facing *)malloc((size_t)total * sizeof(struct replay_facing))))
		return;
	scan.counting = FALSE;
	playback_scan_facings(&scan);
	error(_error_log, "replay: %ld facings of the players kept for the eyes view (%lu KB)", total,
		(unsigned long)(total * sizeof(struct replay_facing) / 1024));
}

boolean network_replay_watch(
	char const *path,
	char *why,
	long why_size)
{
	FILE *file;
	long size;
	unsigned long format, network_version, header_size;
	byte start[32];
	byte *header;
	byte const *record;
	word record_size;
	boolean header_capped = FALSE;
	int loaded;
	struct network_game game;

	why[0] = 0;
	playback_free();
	file = platform_fopen_host(path, "rb");
	if (!file)
	{
		snprintf(why, (size_t)why_size, "The replay cannot be opened.");
		return FALSE;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || size > REPLAY_MAXIMUM_FILE_SIZE ||
		fseek(file, 0, SEEK_SET) != 0 || (size >= 32 && fread(start, sizeof(start), 1, file) != 1))
	{
		fclose(file);
		snprintf(why, (size_t)why_size, "The replay cannot be read.");
		return FALSE;
	}
	format = size >= 32 ? replay_get_long(start + 8) : 0;
	network_version = size >= 32 ? replay_get_long(start + 12) : 0;
	header_size = size >= 32 ? replay_get_long(start + 28) : 0;
	if (size < 32 || memcmp(start, REPLAY_MAGIC, 8) || header_size < 32 ||
		header_size + REPLAY_RECORD_HEADER_SIZE > (unsigned long)size)
	{
		fclose(file);
		snprintf(why, (size_t)why_size, "This is not a replay.");
		return FALSE;
	}
	if (format < REPLAY_PLAYABLE_FORMAT)
	{
		fclose(file);
		snprintf(why, (size_t)why_size, "This replay was recorded by a build that could not play it back.");
		return FALSE;
	}
	if (!network_replay_network_version_playable(network_version))
	{
		fclose(file);
		snprintf(why, (size_t)why_size, "This replay is of network version %lu; this build plays %d.",
			network_version, HALO_PORT_NETWORK_VERSION);
		return FALSE;
	}
	/* (the header kept as the records' start, as the file has it; the
	records after it into the host's memory, through the window) */
	if (!(header = (byte *)malloc((size_t)header_size)) ||
		!(playback.window = (byte *)malloc(REPLAY_WINDOW_SIZE)))
	{
		fclose(file);
		if (header)
			free(header);
		snprintf(why, (size_t)why_size, "There is not memory enough to play this replay.");
		return FALSE;
	}
	csmemcpy(header, start, sizeof(start));
	if (header_size > sizeof(start) && fread(header + sizeof(start), header_size - sizeof(start), 1, file) != 1)
		loaded = REPLAY_LOAD_UNREADABLE;
	else if (!playback_keep(header, header_size, &header_capped))
		loaded = REPLAY_LOAD_NO_MEMORY;
	else
		loaded = playback_load_records(file, header_size, format >= REPLAY_COMPRESSED_FORMAT);
	fclose(file);
	if (loaded != REPLAY_LOADED)
	{
		free(header);
		if (loaded == REPLAY_LOAD_NO_MEMORY)
			snprintf(why, (size_t)why_size, "There is not memory enough to play this replay (%lu MB read).",
				(unsigned long)playback.size >> 20);
		else
			snprintf(why, (size_t)why_size, "The replay cannot be read.");
		playback_free();
		return FALSE;
	}
	playback.network_version = network_version;
	/* (its first record is the game) */
	record = playback_record((long)header_size, &record_size);
	if (!record || record[0] != _network_replay_game || record_size != sizeof(game))
	{
		free(header);
		snprintf(why, (size_t)why_size, "This replay has no game to play.");
		playback_free();
		return FALSE;
	}
	csmemcpy(&game, record + REPLAY_RECORD_HEADER_SIZE, sizeof(game));
	playback.start_tick = (long)replay_get_long(header + 24);
	playback.next = (long)(header_size + REPLAY_RECORD_HEADER_SIZE + sizeof(game));
	playback_find_recorders(format, header, header_size);
	free(header);
	playback_index_facings();
	/* (as a client joins a game: none of this machine's own network game is
	left, benchmark.c's way) */
	playback.setting_up = TRUE;
	dispose_global_network_game_server();
	dispose_global_network_game_client();
	if (!create_global_network_game_client() || !network_game_client_port_replay_begin(&game))
	{
		snprintf(why, (size_t)why_size, "The replay's game cannot be set up.");
		dispose_global_network_game_client();
		playback.setting_up = FALSE;
		playback_free();
		return FALSE;
	}
	playback.setting_up = FALSE;
	playback.starting = TRUE;
	error(_error_log, "replay: playing %s (%s), from tick %ld", path, game.map.name, playback.start_tick);
	return TRUE;
}

boolean network_replay_network_version_playable(
	unsigned long network_version)
{
	return network_version >= REPLAY_OLDEST_NETWORK_VERSION && network_version <= HALO_PORT_NETWORK_VERSION;
}

boolean network_replay_skips_message(
	byte type)
{
	short index;

	if (!network_replay_playing())
		return FALSE;
	for (index = 0; index < (short)NUMBEROF(replay_changed_messages); index++)
	{
		if (replay_changed_messages[index].type == type &&
			playback.network_version < replay_changed_messages[index].changed_in)
		{
			return TRUE;
		}
	}
	return FALSE;
}

boolean network_replay_playing(
	void)
{
	return playback.window != NULL && (playback.starting || playback.playing);
}

boolean network_replay_hold_wanted(
	void)
{
	return playback.playing && playback.skip_tick != NONE && playback.skip_hold == _replay_hold_wanted;
}

void network_replay_hold_taken(
	boolean held)
{
	if (network_replay_hold_wanted())
		playback.skip_hold = held ? _replay_hold_taken : _replay_hold_none;
}

boolean network_replay_frozen(
	void)
{
	return playback.playing && playback.skip_tick != NONE && playback.skip_hold == _replay_hold_taken;
}

long network_replay_ticks_per_frame(
	long ticks)
{
	long now = game_time_get();
	long most;

	if (!playback.playing || playback.skip_tick == NONE)
		return ticks;
	/* (none until the frame it was asked in is held: it is that frame) */
	if (playback.skip_hold == _replay_hold_wanted)
		return 0;
	most = MIN(REPLAY_SKIP_TICKS_PER_FRAME, playback.skip_tick - now);
	/* (and none past the next keyframe's tick, taken there) */
	if (!playback.keyframes_full && playback.keyframe_frontier != NONE &&
		playback.keyframe_frontier + REPLAY_KEYFRAME_TICKS > now)
	{
		most = MIN(most, playback.keyframe_frontier + REPLAY_KEYFRAME_TICKS - now);
	}
	return MAX(0, most);
}

static void playback_feed(struct network_game_client *client);

void network_replay_playback_tick(
	void)
{
	struct network_game_client *client = global_network_game_client_get();

	if (playback.playing && client)
		playback_feed(client);
}

boolean network_replay_playback_starting(
	void)
{
	return playback.window != NULL && playback.starting;
}

void network_replay_playback_started(
	void)
{
	playback.starting = FALSE;
	playback.playing = TRUE;
	game_time_set_distributed(playback.start_tick);
	error(_error_log, "replay: the game is loaded; playing from tick %ld", playback.start_tick);
}

/* a replay stopped, for the main menu to open the Theater again
(network_replay_theater_wanted) */
static boolean playback_theater_wanted;

void network_replay_playback_stop(
	void)
{
	if (playback.setting_up)
		return;
	if (playback.window)
	{
		error(_error_log, "replay: stopped");
		playback_theater_wanted = TRUE;
	}
	playback_free();
}

boolean network_replay_theater_wanted(
	void)
{
	boolean wanted = playback_theater_wanted;

	playback_theater_wanted = FALSE;
	return wanted;
}

/* whether the button was pressed on any controller since the last idle */
static boolean playback_pressed(
	short button)
{
	boolean down = FALSE;
	boolean pressed;
	short controller;

	for (controller = 0; controller < MAXIMUM_GAMEPADS; controller++)
	{
		struct gamepad_state const *gamepad = input_get_gamepad_state(controller);

		down |= gamepad && gamepad->buttons[button] > 0;
	}
	pressed = down && !playback.held[button];
	playback.held[button] = down;
	return pressed;
}

/* the player after (or before) `after` that has a unit, wrapping round;
`after` itself if it is the only one, NONE if nobody has one */
static long playback_next_player(
	long after,
	boolean backwards)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long previous = NONE;
	long first = NONE;
	long last = NONE;
	boolean passed = after == NONE;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		if (iterator.datum_index == after)
		{
			if (backwards && previous != NONE)
				return previous;
			passed = TRUE;
			continue;
		}
		if (player->unit_index == NONE)
			continue;
		if (passed && !backwards)
			return iterator.datum_index;
		if (first == NONE)
			first = iterator.datum_index;
		previous = iterator.datum_index;
		last = iterator.datum_index;
	}
	if (backwards)
		return last != NONE ? last : after;
	return first != NONE ? first : after;
}

/* the records of the ticks the clock has reached, to the client's
handlers as it reads them from its host */
static void playback_feed(
	struct network_game_client *client)
{
	struct transport_address host;

	csmemset(&host, 0, sizeof(host));
	while (playback.next < playback.size)
	{
		word size;
		byte const *record = playback_record(playback.next, &size);
		byte kind;
		long tick;

		if (!record)
		{
			/* (a file cut short: its end) */
			playback.next = playback.size;
			break;
		}
		kind = record[0];
		tick = (long)replay_get_long(record + 4);
		if (tick > game_time_get())
			break;
		/* (the game over: none of the records after it, the host's
		switch to the next game's pregame among them, which a client with
		no host to talk to takes for the host gone. A keyframe put back
		has the client in the game again) */
		if (network_game_client_port_in_postgame(client))
			break;
		playback.next += REPLAY_RECORD_HEADER_SIZE + size;
		if (size < sizeof(message_header))
			continue;
		csmemcpy(playback.message, record + REPLAY_RECORD_HEADER_SIZE, size);
		switch (kind)
		{
		case _network_replay_datagram:
			network_connection_port_set_last_read_unreliable(TRUE);
			network_distributed_handle_message(NONE, playback.message, size);
			break;
		case _network_replay_reliable:
		case _network_replay_reliable_to_machine:
			network_connection_port_set_last_read_unreliable(FALSE);
			network_distributed_handle_message(NONE, playback.message, size);
			break;
		case _network_replay_game_message:
			network_connection_port_set_last_read_unreliable(FALSE);
			if (size == GET_MESSAGE_SIZE(playback.message[0]))
				network_game_client_handle_message(client, playback.message, (short)size, &host);
			break;
		case _network_replay_own_actions:
			/* (a client's recording: its own player's input, as the host
			relays another's) */
			network_distributed_own_actions((byte const *)playback.message, size);
			break;
		}
	}
	network_connection_port_set_last_read_unreliable(FALSE);
}

/* ---------- keyframes */

/* what a keyframe holds, in order: the game state's parts in use, then the
simulation's and the netcode's variables beside it */
static short playback_keyframe_regions(
	byte **regions,
	unsigned long *sizes)
{
	void *cpu, *gpu;
	long cpu_size, gpu_size;

	game_state_port_used(&cpu, &cpu_size, &gpu, &gpu_size);
	regions[0] = (byte *)cpu;
	sizes[0] = (unsigned long)cpu_size;
	regions[1] = (byte *)gpu;
	sizes[1] = (unsigned long)gpu_size;
	regions[2] = __replay_data_start;
	sizes[2] = (unsigned long)(__replay_data_end - __replay_data_start);
	regions[3] = __replay_bss_start;
	sizes[3] = (unsigned long)(__replay_bss_end - __replay_bss_start);
	return 4;
}

/* the keyframes' compressor: a thread of its own, so that the game goes on
while a keyframe is compressed. The game is copied for it in one go, in
the game's thread (staging, at the keyframe's tick), and its stream made
there (zlib allocates only as a stream is made and ended: the game's
memory is the game's thread's); the compressor only deflates the copy, into
scratch, and the game's thread keeps the result at its next idle
(playback_keyframe_collect) */
static struct
{
	boolean started;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	pthread_cond_t done;
	/* the copy is the compressor's (from when it is woken to when it is
	done), and what it made of it */
	boolean busy;
	boolean finished;
	boolean failed;
	unsigned long milliseconds;
	z_stream stream;
	byte *staging;
	unsigned long staging_size;
	unsigned long staged;
	/* the keyframe's tick and the file's place then */
	long tick;
	long next;
} compressor;

static void *playback_compressor_main(
	void *unused)
{
	(void)unused;
	pthread_mutex_lock(&compressor.lock);
	for (;;)
	{
		unsigned long started;
		int status;

		while (!compressor.busy || compressor.finished)
			pthread_cond_wait(&compressor.wake, &compressor.lock);
		pthread_mutex_unlock(&compressor.lock);
		started = system_milliseconds();
		compressor.stream.next_in = compressor.staging;
		compressor.stream.avail_in = (uInt)compressor.staged;
		compressor.stream.next_out = playback.keyframe_scratch;
		compressor.stream.avail_out = (uInt)playback.keyframe_scratch_size;
		status = deflate(&compressor.stream, Z_FINISH);
		pthread_mutex_lock(&compressor.lock);
		compressor.failed = status != Z_STREAM_END;
		compressor.milliseconds = system_milliseconds() - started;
		compressor.finished = TRUE;
		pthread_cond_broadcast(&compressor.done);
	}
	return NULL;
}

/* the keyframe the compressor finished kept, if it has (waiting for it if
wait); FALSE while it is still at it */
static boolean playback_keyframe_collect(
	boolean wait)
{
	struct replay_keyframe *keyframe;
	boolean failed;

	if (!compressor.started)
		return TRUE;
	pthread_mutex_lock(&compressor.lock);
	if (!compressor.busy)
	{
		pthread_mutex_unlock(&compressor.lock);
		return TRUE;
	}
	while (wait && !compressor.finished)
		pthread_cond_wait(&compressor.done, &compressor.lock);
	if (!compressor.finished)
	{
		pthread_mutex_unlock(&compressor.lock);
		return FALSE;
	}
	failed = compressor.failed;
	pthread_mutex_unlock(&compressor.lock);
	keyframe = &playback.keyframes[playback.keyframe_count];
	keyframe->size = (unsigned long)compressor.stream.total_out;
	deflateEnd(&compressor.stream);
	if (failed || !(keyframe->data = (byte *)malloc(keyframe->size)))
	{
		error(_error_log, "replay: the keyframe at tick %ld cannot be kept", compressor.tick);
		playback.keyframes_full = TRUE;
	}
	else
	{
		csmemcpy(keyframe->data, playback.keyframe_scratch, keyframe->size);
		keyframe->tick = compressor.tick;
		keyframe->next = compressor.next;
		playback.keyframe_count++;
		playback.keyframe_memory += keyframe->size;
		error(_error_log, "replay: keyframe %ld at tick %ld, %lu KB of %lu KB, compressed in %lu ms",
			playback.keyframe_count - 1, keyframe->tick, keyframe->size / 1024, compressor.staged / 1024,
			compressor.milliseconds);
	}
	pthread_mutex_lock(&compressor.lock);
	compressor.busy = FALSE;
	compressor.finished = FALSE;
	pthread_mutex_unlock(&compressor.lock);
	return TRUE;
}

/* a keyframe of now, after the last: copied, and handed to the compressor
(FALSE, said once, when there is no more room for one) */
static boolean playback_keyframe_take(
	void)
{
	byte *regions[4];
	unsigned long sizes[4];
	unsigned long total = 0;
	unsigned long started = system_milliseconds();
	short count = playback_keyframe_regions(regions, sizes);
	short index;

	if (playback.keyframe_count >= REPLAY_MAXIMUM_KEYFRAMES || playback.keyframe_memory >= REPLAY_KEYFRAME_MEMORY)
	{
		if (!playback.keyframes_full)
			error(_error_log, "replay: no room for more keyframes (%ld, %lu KB)", playback.keyframe_count,
				playback.keyframe_memory / 1024);
		playback.keyframes_full = TRUE;
		return FALSE;
	}
	for (index = 0; index < count; index++)
		total += sizes[index];
	if (!compressor.started)
	{
		pthread_attr_t attributes;
		boolean made;

		pthread_mutex_init(&compressor.lock, NULL);
		pthread_cond_init(&compressor.wake, NULL);
		pthread_cond_init(&compressor.done, NULL);
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, 64 * 1024);
		made = pthread_create(&compressor.thread, &attributes, playback_compressor_main, NULL) == 0;
		pthread_attr_destroy(&attributes);
		if (!made)
		{
			error(_error_log, "replay: no thread for keyframes");
			playback.keyframes_full = TRUE;
			return FALSE;
		}
		pthread_detach(compressor.thread);
		compressor.started = TRUE;
	}
	/* (the buffers, kept from one replay to the next while the game state
	is no larger: a map's is the same throughout) */
	if (compressor.staging_size < total)
	{
		if (compressor.staging)
			free(compressor.staging);
		if (playback.keyframe_scratch)
			free(playback.keyframe_scratch);
		compressor.staging_size = total;
		compressor.staging = (byte *)malloc(total);
		/* (the most deflate can make of it: zlib 1.1.3's, which has no
		compressBound, is a thousandth more and 12 bytes) */
		playback.keyframe_scratch_size = total + total / 1000 + 12 + 1024;
		playback.keyframe_scratch = (byte *)malloc(playback.keyframe_scratch_size);
		if (!compressor.staging || !playback.keyframe_scratch)
		{
			if (compressor.staging)
				free(compressor.staging);
			if (playback.keyframe_scratch)
				free(playback.keyframe_scratch);
			compressor.staging = NULL;
			compressor.staging_size = 0;
			playback.keyframe_scratch = NULL;
			playback.keyframe_scratch_size = 0;
			playback.keyframes_full = TRUE;
			error(_error_log, "replay: no memory for keyframes");
			return FALSE;
		}
	}
	compressor.staged = 0;
	for (index = 0; index < count; index++)
	{
		csmemcpy(compressor.staging + compressor.staged, regions[index], sizes[index]);
		compressor.staged += sizes[index];
	}
	csmemset(&compressor.stream, 0, sizeof(compressor.stream));
	if (deflateInit(&compressor.stream, 1) != Z_OK)
		return FALSE;
	compressor.tick = game_time_get();
	compressor.next = playback.next;
	playback.keyframe_frontier = compressor.tick;
	pthread_mutex_lock(&compressor.lock);
	compressor.busy = TRUE;
	compressor.finished = FALSE;
	pthread_cond_signal(&compressor.wake);
	pthread_mutex_unlock(&compressor.lock);
	if (playback.keyframe_count < 2)
		error(_error_log, "replay: keyframe at tick %ld copied in %lu ms", compressor.tick, system_milliseconds() - started);
	return TRUE;
}

/* a keyframe being put back: its stream, and whether it has gone wrong */
struct playback_keyframe_reading
{
	z_stream stream;
	byte *regions[4];
	unsigned long sizes[4];
	boolean failed;
};

/* the next region of a keyframe, inflated in place */
static void playback_keyframe_read_region(
	struct playback_keyframe_reading *reading,
	short index)
{
	int status;

	if (reading->failed)
		return;
	reading->stream.next_out = reading->regions[index];
	reading->stream.avail_out = (uInt)reading->sizes[index];
	status = inflate(&reading->stream, Z_SYNC_FLUSH);
	if ((status != Z_OK && status != Z_STREAM_END) || reading->stream.avail_out)
		reading->failed = TRUE;
}

/* (the game state's, between the procedures of a core's load) */
static void playback_keyframe_read_game_state(
	void *context)
{
	struct playback_keyframe_reading *reading = (struct playback_keyframe_reading *)context;

	playback_keyframe_read_region(reading, 0);
	playback_keyframe_read_region(reading, 1);
}

/* the game as it was at a keyframe: the game state (as a core is loaded),
then the variables beside it (after the load's procedures, which reset
some of them), the file where it was then, and what is kept of this
machine rather than of the game made right again: the clock's times, the
frame's interpolation, the first-person weapon; the playback's speed, or a
skip's (seeking: the skip goes on from it, its frame still held) */
static void playback_keyframe_restore(
	long keyframe_index,
	boolean seeking)
{
	struct replay_keyframe const *keyframe = &playback.keyframes[keyframe_index];
	struct playback_keyframe_reading reading;

	csmemset(&reading, 0, sizeof(reading));
	playback_keyframe_regions(reading.regions, reading.sizes);
	if (inflateInit(&reading.stream) != Z_OK)
		return;
	reading.stream.next_in = keyframe->data;
	reading.stream.avail_in = (uInt)keyframe->size;
	game_state_port_restore(playback_keyframe_read_game_state, &reading);
	playback_keyframe_read_region(&reading, 2);
	playback_keyframe_read_region(&reading, 3);
	inflateEnd(&reading.stream);
	if (reading.failed)
	{
		/* (half a game: there is no going on from it) */
		error(_error_log, "replay: keyframe %ld cannot be read back; leaving", keyframe_index);
		playback.leaving = TRUE;
		network_game_abort();
		return;
	}
	playback.next = keyframe->next;
	playback.ended = 0;
	if (!seeking)
		playback_skip_stop();
	network_distributed_replay_restored();
	render_interpolation_reset();
	first_person_weapons_initialize_for_new_map();
	game_time_set_speed(seeking ? REPLAY_SKIP_SPEED : replay_speeds[playback.speed]);
	error(_error_log, "replay: to keyframe %ld, tick %ld", keyframe_index, keyframe->tick);
}

/* the keyframe latest at or before the tick, NONE if none is */
static long playback_keyframe_before(
	long tick)
{
	long index;

	for (index = playback.keyframe_count - 1; index >= 0; index--)
	{
		if (playback.keyframes[index].tick <= tick)
			return index;
	}
	return NONE;
}

/* the skip's target set, the skip started if it was not: the frame it is
asked in held (network_replay_frozen) and nothing heard; the keyframes on
the way taken at their own ticks (network_replay_ticks_per_frame) */
static void playback_skip_to(
	long target)
{
	if (playback.skip_tick == NONE)
	{
		playback.skip_hold = _replay_hold_wanted;
		playback.skip_hold_frames = 0;
		sound_pause(TRUE);
	}
	playback.skip_tick = target;
	game_time_set_speed(REPLAY_SKIP_SPEED);
}

/* a tap of the d-pad. Left: back to the last keyframe (the one before it,
just after one). Right: on to the keyframe ahead, put back at once, else
the target ten seconds on, played to fast. Taps add up: during a skip each
goes on from its target, not from where the playback is */
static void playback_skip(
	boolean forward)
{
	long now = game_time_get();
	long from = playback.skip_tick != NONE ? playback.skip_tick : now;
	long target;
	long index;

	/* (one still being compressed waited for: it may be the one wanted) */
	playback_keyframe_collect(TRUE);
	if (!playback.keyframe_count)
		return;
	if (!forward)
	{
		index = playback_keyframe_before(from - REPLAY_REWIND_GRACE_TICKS);
		playback_keyframe_restore(index != NONE ? index : 0, FALSE);
		return;
	}
	for (index = 0; index < playback.keyframe_count; index++)
	{
		if (playback.keyframes[index].tick > from)
			break;
	}
	if (index < playback.keyframe_count)
	{
		/* (not skipping: there at once, as before. Skipping: the skip's
		new target, put back by playback_seek_chase) */
		if (playback.skip_tick == NONE)
		{
			playback_keyframe_restore(index, FALSE);
			return;
		}
		target = playback.keyframes[index].tick;
	}
	else
	{
		if (playback.ended)
			return;
		target = MIN(from + REPLAY_KEYFRAME_TICKS, playback.end_tick);
		if (target <= now)
			return;
	}
	playback_skip_to(target);
}

/* a d-pad direction held: past REPLAY_SCRUB_HOLD_MILLISECONDS it scrubs,
the marker sliding from the skip's target (or from where the playback is)
that way, slowly at first and faster as it is held; the skip's target is
the marker. Let go, the skip goes on to it */
static void playback_scrub(
	unsigned long now)
{
	real length = (real)(playback.end_tick - playback.start_tick);
	real seconds;
	real ramp;
	real fast;
	real speed;

	if (playback.scrub_button == NONE)
		return;
	if (!playback.held[playback.scrub_button])
	{
		playback.scrub_button = NONE;
		playback.scrubbing = FALSE;
		return;
	}
	if (!playback.scrubbing)
	{
		if (now - playback.scrub_pressed < REPLAY_SCRUB_HOLD_MILLISECONDS)
			return;
		/* (the replay over: nothing ahead to scrub to) */
		if (playback.scrub_button == _gamepad_binary_button_dpad_right && playback.ended)
			return;
		playback.scrubbing = TRUE;
		playback.scrub_moved = now;
		playback.scrub_marker = (real)(playback.skip_tick != NONE ? playback.skip_tick : game_time_get());
		playback_skip_to((long)playback.scrub_marker);
		return;
	}
	seconds = (real)(now - playback.scrub_moved) / 1000.0f;
	playback.scrub_moved = now;
	ramp = MIN(1.0f, (real)(now - playback.scrub_pressed - REPLAY_SCRUB_HOLD_MILLISECONDS) /
		(real)REPLAY_SCRUB_RAMP_MILLISECONDS);
	fast = MAX((real)REPLAY_SCRUB_SLOW_TICKS, length / REPLAY_SCRUB_CROSSING_SECONDS);
	speed = (real)REPLAY_SCRUB_SLOW_TICKS + (fast - (real)REPLAY_SCRUB_SLOW_TICKS) * ramp;
	if (playback.scrub_button == _gamepad_binary_button_dpad_left)
		speed = -speed;
	playback.scrub_marker = PIN(playback.scrub_marker + speed * seconds,
		(real)playback.start_tick, (real)playback.end_tick);
	playback.skip_tick = (long)playback.scrub_marker;
}

/* a skip on its way to its target over what has been played already: the
latest keyframe before the target put back when it is ahead of the
playback, or when the target is behind it (once left is let go: a scrub
back freezes the playback until then), and played on from there. Not
before the skip's frame is held: the frame put back would be the one held */
static void playback_seek_chase(
	void)
{
	long now = game_time_get();
	long index;

	if (playback.skip_tick == NONE || playback.skip_hold == _replay_hold_wanted ||
		playback.scrub_button == _gamepad_binary_button_dpad_left)
	{
		return;
	}
	index = playback_keyframe_before(playback.skip_tick);
	if (index == NONE)
		return;
	if (playback.keyframes[index].tick > now || playback.skip_tick < now)
		playback_keyframe_restore(index, TRUE);
}

boolean network_replay_playback_idle(
	struct network_game_client *client)
{
	unsigned long now = system_milliseconds();

	if (!playback.playing)
		return TRUE;
	playback_feed(client);
	/* (the host's objects, all of them, came at the game's first tick: as a
	client is told once the host has sent it them all, its own objects the
	host has not are let go, network_objects.c) */
	if (!playback.synchronized)
	{
		playback.synchronized = TRUE;
		network_objects_handle_synchronized();
	}
	if (playback.leaving)
		return TRUE;
	/* (a keyframe each 10 seconds, the first at the start: the game as the
	records fed so far have made it, at a tick's end; and a skip ahead
	ending at the one it played to, taken just now) */
	if (playback_keyframe_collect(FALSE) && !playback.ended && !playback.keyframes_full &&
		(playback.keyframe_frontier == NONE || game_time_get() >= playback.keyframe_frontier + REPLAY_KEYFRAME_TICKS))
	{
		playback_keyframe_take();
	}
	/* (a skip ahead's frame not held in a few frames, as Smooth Motion off
	can leave it undrawn: the game drawn as it plays) */
	if (playback.skip_tick != NONE && playback.skip_hold == _replay_hold_wanted &&
		++playback.skip_hold_frames > REPLAY_SKIP_HOLD_FRAMES)
	{
		playback.skip_hold = _replay_hold_none;
	}
	/* (a skip on its way: a keyframe on the way put back) */
	playback_seek_chase();
	if (playback.leaving)
		return TRUE;
	/* (a skip there, not scrubbing still: the game drawn again, at its
	normal speed. The keyframes' memory full, it goes on, taking none) */
	if (playback.skip_tick != NONE && !playback.scrubbing &&
		(game_time_get() >= playback.skip_tick || playback.ended))
	{
		playback_skip_stop();
		playback.speed = REPLAY_NORMAL_SPEED;
		game_time_set_speed(replay_speeds[playback.speed]);
	}
	/* (the pause menu: Start puts it up, the game stopped, and takes it
	down again, the game at its speed; B in it leaves the replay. Nothing
	else while it is up: its end waits too, its seconds again from the
	resume) */
	if (playback_pressed(_gamepad_binary_button_start))
	{
		/* (a skip or a scrub ended where the playback is, which resumes at
		normal speed) */
		if (playback.skip_tick != NONE)
			playback.speed = REPLAY_NORMAL_SPEED;
		playback.paused = !playback.paused;
		playback_seek_stop();
		playback.scores = FALSE;
		game_time_set_speed(playback.paused ? 0.0f : replay_speeds[playback.speed]);
		if (!playback.paused && playback.ended)
			playback.ended = now ? now : 1;
	}
	if (playback.paused)
	{
		/* (the north button: the hints' bars shown or hidden, kept in
		config.toml: network.replay_hints) */
		if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y))
			config_write_boolean("network.replay_hints", !config_boolean("network.replay_hints"));
		if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b))
		{
			error(_error_log, "replay: left at tick %ld", game_time_get());
			playback.leaving = TRUE;
			network_game_abort();
		}
		return TRUE;
	}
	if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a))
		playback.watched = playback_next_player(playback.watched, FALSE);
	if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x))
		playback.watched = playback_next_player(playback.watched, TRUE);
	/* (the d-pad: a tap skips, held it scrubs; while one way is held the
	other is not heard) */
	{
		boolean left = playback_pressed(_gamepad_binary_button_dpad_left);
		boolean right = playback_pressed(_gamepad_binary_button_dpad_right);

		if (playback.scrub_button == NONE && (left || right))
		{
			playback_skip(right);
			if (playback.leaving)
				return TRUE;
			playback.scrub_button = right ? _gamepad_binary_button_dpad_right : _gamepad_binary_button_dpad_left;
			playback.scrub_pressed = now;
		}
		playback_scrub(now);
	}
	if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y))
	{
		playback.view = (short)((playback.view + 1) % NUMBER_OF_REPLAY_VIEWS);
		/* (the free camera follows the player until it is flown; behind
		turns with them until it is, at its distance) */
		if (playback.view == _replay_view_free || playback.view == _replay_view_cinematic)
			playback.free_placing = TRUE;
		if (playback.view == _replay_view_behind)
		{
			playback.behind_orbiting = FALSE;
			playback.behind_zoom = 1.0f;
		}
	}
	/* (the right stick's click, or D-pad Up: behind turns with the player
	again, at its zoom; the free camera tracks them from where it is, or
	holds its aim. Both asked, so each one's press is seen) */
	if (playback_pressed(_gamepad_binary_button_right_thumb) | playback_pressed(_gamepad_binary_button_dpad_up))
	{
		if (playback.view == _replay_view_behind)
			playback.behind_orbiting = FALSE;
		else if (playback.view == _replay_view_free && !playback.free_placing)
			playback.free_tracking = !playback.free_tracking;
	}
	/* (D-pad Down, or L3: the free camera placed behind the player again, as when
	it is chosen, and left there tracking them, whether it tracked them or
	held its aim; once a press, the next heard no sooner than
	REPLAY_CAMERA_RESET_MILLISECONDS after) */
	if ((playback_pressed(_gamepad_binary_button_dpad_down) | playback_pressed(_gamepad_binary_button_left_thumb)) &&
		playback.view == _replay_view_free && !playback.free_placing &&
		now - playback.free_reset_at >= REPLAY_CAMERA_RESET_MILLISECONDS)
	{
		playback.free_placing = TRUE;
		playback.free_reset_at = now;
	}
	/* (the right shoulder faster, the left slower: the Xbox's black and
	white, xinput_sdl.c) */
	if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black) &&
		playback.speed < (short)NUMBEROF(replay_speeds) - 1)
	{
		playback_seek_stop();
		game_time_set_speed(replay_speeds[++playback.speed]);
	}
	if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white) && playback.speed > 0)
	{
		playback_seek_stop();
		game_time_set_speed(replay_speeds[--playback.speed]);
	}
	playback_pressed(_gamepad_binary_button_back);
	playback.scores = playback.held[_gamepad_binary_button_back];
	/* (B: the lines over the screen hidden, or shown again) */
	if (playback_pressed(FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b))
		playback.hidden = !playback.hidden;
	/* (its end: the file's, or the game's) */
	if (!playback.ended && (playback.next >= playback.size ||
		network_game_client_port_in_postgame(client)))
	{
		error(_error_log, "replay: its end, at tick %ld", game_time_get());
		playback.ended = now ? now : 1;
	}
	if (playback.ended && now - playback.ended >= REPLAY_END_MILLISECONDS)
	{
		playback.leaving = TRUE;
		network_game_abort();
	}
	return TRUE;
}

/* ---------- the view */

/* the camera at position looking along forward (made unit length), up
square to it */
static void playback_camera_set(
	real_point3d const *position,
	real_vector3d const *forward)
{
	real_vector3d look = *forward;
	real length = (real)sqrt(look.i * look.i + look.j * look.j + look.k * look.k);
	real along;

	if (length < 0.0001f)
		return;
	look.i /= length;
	look.j /= length;
	look.k /= length;
	csmemset(&playback.camera, 0, sizeof(playback.camera));
	playback.camera.position = *position;
	scenario_location_from_point(&playback.camera.location, &playback.camera.position);
	playback.camera.forward = look;
	/* (up: the world's, less what of it is along forward) */
	along = look.k;
	playback.camera.up.i = -look.i * along;
	playback.camera.up.j = -look.j * along;
	playback.camera.up.k = 1.0f - look.k * along;
	length = (real)sqrt(playback.camera.up.i * playback.camera.up.i + playback.camera.up.j * playback.camera.up.j +
		playback.camera.up.k * playback.camera.up.k);
	if (length < 0.0001f)
	{
		playback.camera.up.i = 1.0f;
		playback.camera.up.j = 0.0f;
		playback.camera.up.k = 0.0f;
	}
	else
	{
		playback.camera.up.i /= length;
		playback.camera.up.j /= length;
		playback.camera.up.k /= length;
	}
	playback.camera.field_of_view = CAMERA_FIELD_OF_VIEW;
	playback.camera_valid = TRUE;
}

/* a stick's part, or a trigger, of -1 to 1 (0 to 1), its dead middle 0 */
static real playback_axis(
	real value)
{
	if (value > -STICK_DEAD_ZONE && value < STICK_DEAD_ZONE)
		return 0.0f;
	return value > 0.0f ? (value - STICK_DEAD_ZONE) / (1.0f - STICK_DEAD_ZONE) :
		(value + STICK_DEAD_ZONE) / (1.0f - STICK_DEAD_ZONE);
}

/* every controller's sticks (the left's, the right's) and triggers (the
right's less the left's), each 0 in its dead middle, added together; and
the touch controls' stick, the left's (it moves a player as their input,
touch_input_move, not as the controller's stick) */
static void playback_sticks(
	real *move_x,
	real *move_y,
	real *turn_x,
	real *turn_y,
	real *rise)
{
	short controller;

	*move_x = *move_y = *turn_x = *turn_y = *rise = 0.0f;
	for (controller = 0; controller < MAXIMUM_GAMEPADS; controller++)
	{
		struct gamepad_state const *gamepad = input_get_gamepad_state(controller);

		if (!gamepad)
			continue;
		*move_x += playback_axis(gamepad->sticks[_gamepad_stick_left].x / 32767.0f);
		*move_y += playback_axis(gamepad->sticks[_gamepad_stick_left].y / 32767.0f);
		*turn_x += playback_axis(gamepad->sticks[_gamepad_stick_right].x / 32767.0f);
		*turn_y += playback_axis(gamepad->sticks[_gamepad_stick_right].y / 32767.0f);
		*rise += playback_axis(gamepad->analog_buttons[_gamepad_analog_button_right_trigger] / 255.0f) -
			playback_axis(gamepad->analog_buttons[_gamepad_analog_button_left_trigger] / 255.0f);
	}
#ifdef REPLAY_TOUCH
	{
		float forward, strafe;

		if (touch_input_move(&forward, &strafe))
		{
			*move_x -= strafe;
			*move_y += forward;
		}
	}
#endif
}

/* the free camera's move from the left stick: its tilt to
FREE_CAMERA_MOVE_POWER, which the move follows over the seconds, a full
speed's change in FREE_CAMERA_ACCELERATION_TIME speeding up and in
FREE_CAMERA_BRAKE_TIME slowing down */
static void playback_free_camera_speed(
	real move_x,
	real move_y,
	real seconds)
{
	real tilt = (real)sqrt(move_x * move_x + move_y * move_y);
	real target_x = 0.0f;
	real target_y = 0.0f;
	real difference_x;
	real difference_y;
	real difference;
	real step;

	if (tilt > 0.0f)
	{
		real speed = (real)pow(MIN(tilt, 1.0f), FREE_CAMERA_MOVE_POWER);

		target_x = move_x / tilt * speed;
		target_y = move_y / tilt * speed;
	}
	difference_x = target_x - playback.free_move_x;
	difference_y = target_y - playback.free_move_y;
	difference = (real)sqrt(difference_x * difference_x + difference_y * difference_y);
	step = seconds / (target_x * target_x + target_y * target_y >=
		playback.free_move_x * playback.free_move_x + playback.free_move_y * playback.free_move_y ?
		FREE_CAMERA_ACCELERATION_TIME : FREE_CAMERA_BRAKE_TIME);
	if (difference <= step)
	{
		playback.free_move_x = target_x;
		playback.free_move_y = target_y;
	}
	else
	{
		playback.free_move_x += difference_x / difference * step;
		playback.free_move_y += difference_y / difference * step;
	}
}

/* the free camera's rise from the triggers (the right's less the left's):
no more than a trigger held has taken it to, from none to full in
FREE_CAMERA_RISE_TIME; let go, or the other one, and it starts from none */
static real playback_free_camera_rise(
	real rise,
	real seconds)
{
	real held;

	if (rise == 0.0f || (rise > 0.0f) != (playback.free_rise > 0.0f))
		playback.free_rise = 0.0f;
	if (rise == 0.0f)
		return 0.0f;
	held = MIN((real)fabs(playback.free_rise) + seconds / FREE_CAMERA_RISE_TIME, 1.0f);
	playback.free_rise = rise > 0.0f ? held : -held;
	return rise > 0.0f ? MIN(rise, held) : MAX(rise, -held);
}

/* the free camera moved by move, but never nearer a wall than
FREE_CAMERA_WALL_MARGIN (further than the view's near clip, so it never
sees into one): each part of the move is looked along as far again as the
margin, and goes only as far as keeps it the margin off the wall it finds
there, the rest of it along the wall, as far as the next. Tested only up to
the move's end, a short move went past the margin into the wall's near
clip and the next was put back out: the view in and out of the wall each
frame. Only the walls facing it stop it, so one that starts inside
something can still leave it */
static void playback_free_camera_move(
	real_vector3d const *move)
{
	real_vector3d left = *move;
	short slide;

	for (slide = 0; slide < FREE_CAMERA_SLIDES; slide++)
	{
		struct collision_result collision;
		real_vector3d const *normal;
		real_vector3d direction;
		real_vector3d probe;
		real length = (real)sqrt(left.i * left.i + left.j * left.j + left.k * left.k);
		real approach;
		real distance;
		real travel;
		real across;

		if (length < 1.0e-4f)
			return;
		direction.i = left.i / length;
		direction.j = left.j / length;
		direction.k = left.k / length;
		probe.i = direction.i * (length + FREE_CAMERA_WALL_MARGIN);
		probe.j = direction.j * (length + FREE_CAMERA_WALL_MARGIN);
		probe.k = direction.k * (length + FREE_CAMERA_WALL_MARGIN);
		normal = &collision.plane.n;
		if (!collision_test_vector(FREE_CAMERA_COLLISION_FLAGS, &playback.free_position, &probe, NONE, &collision) ||
			(approach = -(direction.i * normal->i + direction.j * normal->j + direction.k * normal->k)) < 1.0e-4f)
		{
			playback.free_position.x += left.i;
			playback.free_position.y += left.j;
			playback.free_position.z += left.k;
			return;
		}
		/* (how far off the wall it is, and how far along the move it may go
		to be the margin off it: none if it is nearer already) */
		distance = (playback.free_position.x - collision.point.x) * normal->i +
			(playback.free_position.y - collision.point.y) * normal->j +
			(playback.free_position.z - collision.point.z) * normal->k;
		travel = PIN((distance - FREE_CAMERA_WALL_MARGIN) / approach, 0.0f, length);
		playback.free_position.x += direction.i * travel;
		playback.free_position.y += direction.j * travel;
		playback.free_position.z += direction.k * travel;
		/* (what is left of the move, along the wall: none of it into it) */
		left.i = direction.i * (length - travel);
		left.j = direction.j * (length - travel);
		left.k = direction.k * (length - travel);
		across = left.i * normal->i + left.j * normal->j + left.k * normal->k;
		if (across < 0.0f)
		{
			left.i -= normal->i * across;
			left.j -= normal->j * across;
			left.k -= normal->k * across;
		}
	}
}

static void playback_focus(long unit_index, real_point3d *focus);
static void playback_behind_camera(long unit_index, real seconds);

/* how much of the way to where it is going a value eased over time
(seconds) goes in seconds */
static real playback_ease(
	real seconds,
	real time)
{
	return 1.0f - (real)exp(-seconds / time);
}

/* the free camera's aim eased toward the player's focus, as a tripod's
turned by hand: FREE_CAMERA_TRACK_TIME behind it */
static void playback_free_camera_track(
	real seconds)
{
	real_vector3d to;
	real across;
	real yaw, pitch, turn;
	real ease = playback_ease(seconds, FREE_CAMERA_TRACK_TIME);

	to.i = playback.free_focus.x - playback.free_position.x;
	to.j = playback.free_focus.y - playback.free_position.y;
	to.k = playback.free_focus.z - playback.free_position.z;
	across = (real)sqrt(to.i * to.i + to.j * to.j);
	if (across < 0.01f && (real)fabs(to.k) < 0.01f)
		return;
	yaw = (real)atan2(to.j, to.i);
	pitch = PIN((real)atan2(to.k, across), -FREE_CAMERA_MAXIMUM_PITCH, FREE_CAMERA_MAXIMUM_PITCH);
	turn = yaw - playback.free_yaw;
	while (turn > _pi)
		turn -= 2.0f * _pi;
	while (turn < -_pi)
		turn += 2.0f * _pi;
	playback.free_yaw += turn * ease;
	playback.free_pitch += (pitch - playback.free_pitch) * ease;
}

/* the free camera's view's width: while it has not moved since it was
placed and tracks the player, narrowed as they go further than
FREE_CAMERA_ZOOM_DISTANCE so they stay the size they were (to
FREE_CAMERA_NARROWEST); held while it has not moved but does not track;
moved, eased back to the camera's own, and never narrowed again until it is
placed anew */
static void playback_free_camera_zoom(
	real seconds)
{
	real wanted = playback.free_field_of_view;

	if (!playback.free_still)
		wanted = CAMERA_FIELD_OF_VIEW;
	else if (playback.free_tracking && playback.free_focus_valid)
	{
		real_vector3d to;
		real distance;

		to.i = playback.free_focus.x - playback.free_position.x;
		to.j = playback.free_focus.y - playback.free_position.y;
		to.k = playback.free_focus.z - playback.free_position.z;
		distance = (real)sqrt(to.i * to.i + to.j * to.j + to.k * to.k);
		wanted = CAMERA_FIELD_OF_VIEW;
		if (distance > FREE_CAMERA_ZOOM_DISTANCE)
		{
			wanted = 2.0f * (real)atan(tan(CAMERA_FIELD_OF_VIEW / 2.0f) * FREE_CAMERA_ZOOM_DISTANCE / distance);
			wanted = MAX(wanted, FREE_CAMERA_NARROWEST);
		}
	}
	playback.free_field_of_view += (wanted - playback.free_field_of_view) *
		playback_ease(seconds, FREE_CAMERA_ZOOM_TIME);
}

/* the cinematic camera's sight of the player: hidden from it by the level,
its scenery or its doors for CINEMATIC_HIDDEN_TIME (at all, in the
CINEMATIC_SPAWN_TIME after they spawn), it is placed behind them again, as
L3 places the free camera */
static void playback_cinematic_sight(
	long unit_index,
	real seconds)
{
	real_point3d focus;
	real_vector3d to;
	struct collision_result collision;

	playback_focus(unit_index, &focus);
	to.i = focus.x - playback.free_position.x;
	to.j = focus.y - playback.free_position.y;
	to.k = focus.z - playback.free_position.z;
	if (collision_test_vector(FREE_CAMERA_COLLISION_FLAGS, &playback.free_position, &to, NONE, &collision))
	{
		playback.cinematic_hidden += seconds;
		if (playback.cinematic_spawned > 0.0f || playback.cinematic_hidden >= CINEMATIC_HIDDEN_TIME)
			playback.free_placing = TRUE;
	}
	else
		playback.cinematic_hidden = 0.0f;
	playback.cinematic_spawned = MAX(playback.cinematic_spawned - seconds, 0.0f);
}

/* the free camera, a tripod: placed behind the player when it is chosen (as
behind's would be, turned as they face), tracking them (R3 toggles it, the
right stick takes the aim), moved by the left stick and the triggers, still
tracking; FALSE, the camera left alone, while it cannot be placed (nobody
to place it behind, and no camera before). The cinematic camera is this,
its sticks and swipes not heard */
static boolean playback_free_camera(
	long unit_index,
	real seconds)
{
	real move_x, move_y, turn_x, turn_y, rise;
	real_vector3d forward;
	real_vector3d move;

	playback_sticks(&move_x, &move_y, &turn_x, &turn_y, &rise);
	if (playback.view == _replay_view_cinematic)
	{
		move_x = move_y = turn_x = turn_y = rise = 0.0f;
		playback.touch_yaw = playback.touch_pitch = 0.0f;
		/* (a player who died: hidden as they spawn, followed at once) */
		if (unit_index == NONE)
			playback.cinematic_died = TRUE;
		else if (playback.cinematic_died)
		{
			playback.cinematic_died = FALSE;
			playback.cinematic_spawned = CINEMATIC_SPAWN_TIME;
		}
		if (!playback.free_placing && unit_index != NONE)
			playback_cinematic_sight(unit_index, seconds);
	}
	if (playback.free_placing)
	{
		if (unit_index != NONE)
		{
			struct object_datum *object = object_get(unit_index);
			long followed = object->object.parent_object_index != NONE ? object->object.parent_object_index : unit_index;

			object = object_get(followed);
			playback.yaw = (real)atan2(object->object.forward.j, object->object.forward.i);
			playback_behind_camera(unit_index, 0.0f);
		}
		if (!playback.camera_valid)
			return FALSE;
		playback.free_placing = FALSE;
		playback.free_tracking = TRUE;
		playback.free_still = TRUE;
		playback.cinematic_hidden = 0.0f;
		playback.cinematic_died = FALSE;
		playback.cinematic_spawned = 0.0f;
		playback.free_field_of_view = CAMERA_FIELD_OF_VIEW;
		playback.free_focus_valid = FALSE;
		playback.free_move_x = playback.free_move_y = playback.free_rise = 0.0f;
		playback.free_position = playback.camera.position;
		playback.free_yaw = (real)atan2(playback.camera.forward.j, playback.camera.forward.i);
		playback.free_pitch = (real)asin(PIN(playback.camera.forward.k, -1.0f, 1.0f));
	}
	/* (the player's focus; where they died, while they are dead) */
	if (unit_index != NONE)
	{
		playback_focus(unit_index, &playback.free_focus);
		playback.free_focus_valid = TRUE;
	}
	/* (the right stick, or a swipe, takes the aim) */
	if (turn_x != 0.0f || turn_y != 0.0f || playback.touch_yaw != 0.0f || playback.touch_pitch != 0.0f)
		playback.free_tracking = FALSE;
	if (playback.free_tracking && playback.free_focus_valid)
		playback_free_camera_track(seconds);
	else
	{
		playback.free_yaw += playback.touch_yaw - turn_x * FREE_CAMERA_TURN * seconds;
		playback.free_pitch = PIN(playback.free_pitch + playback.touch_pitch + turn_y * FREE_CAMERA_TURN * seconds,
			-FREE_CAMERA_MAXIMUM_PITCH, FREE_CAMERA_MAXIMUM_PITCH);
	}
	forward.i = (real)(cos(playback.free_yaw) * cos(playback.free_pitch));
	forward.j = (real)(sin(playback.free_yaw) * cos(playback.free_pitch));
	forward.k = (real)sin(playback.free_pitch);
	/* (as the game's debug flying camera, flying_camera.c: the stick moves it
	level, forward and back toward where it looks across the ground whatever
	its pitch, and only the triggers change its height; both eased:
	playback_free_camera_speed, playback_free_camera_rise) */
	if (move_x != 0.0f || move_y != 0.0f || rise != 0.0f)
		playback.free_still = FALSE;
	playback_free_camera_speed(move_x, move_y, seconds);
	rise = playback_free_camera_rise(rise, seconds);
	move.i = ((real)cos(playback.free_yaw) * playback.free_move_y + (real)sin(playback.free_yaw) * playback.free_move_x) *
		FREE_CAMERA_SPEED * seconds;
	move.j = ((real)sin(playback.free_yaw) * playback.free_move_y - (real)cos(playback.free_yaw) * playback.free_move_x) *
		FREE_CAMERA_SPEED * seconds;
	move.k = rise * FREE_CAMERA_SPEED * seconds;
	playback_free_camera_move(&move);
	playback_free_camera_zoom(seconds);
	playback_camera_set(&playback.free_position, &forward);
	playback.camera.field_of_view = playback.free_field_of_view;
	return TRUE;
}

/* the moment the frame draws, in ticks: between the last two (render
interpolation's), the tick just run's at 1 */
static real playback_eyes_time(
	void)
{
	return (real)game_time_get() - 1.0f + render_interpolation_fraction();
}

/* the angle from one to the other, the short way round */
static real playback_angle_between(
	real from,
	real to)
{
	real difference = (real)fmod(to - from, 2.0f * _pi);

	if (difference > _pi)
		difference -= 2.0f * _pi;
	else if (difference < -_pi)
		difference += 2.0f * _pi;
	return difference;
}

/* the player's (an absolute index) facing at the moment, turned smoothly
from the facing their ticks run to the next one the replay has
(playback_index_facings); FALSE when it has none of theirs yet. A facing
read at a tick is what the client's next tick runs: it is theirs from then
(its tick and 1). */
static boolean playback_eyes_facing(
	short player_index,
	real time,
	real_euler_angles2d *facing)
{
	struct replay_facing const *facings;
	long low = 0;
	long high;
	real from;
	real to;
	real fraction;

	if (!playback.facings || player_index < 0 || player_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ||
		!playback.facing_count[player_index])
	{
		return FALSE;
	}
	facings = playback.facings + playback.facing_first[player_index];
	high = playback.facing_count[player_index];
	/* (the last whose moment the time has reached) */
	if ((real)(facings[0].tick + 1) > time)
		return FALSE;
	while (high - low > 1)
	{
		long middle = (low + high) / 2;

		if ((real)(facings[middle].tick + 1) <= time)
			low = middle;
		else
			high = middle;
	}
	facing->yaw = distributed_angle_unpack(facings[low].yaw, FALSE);
	facing->pitch = distributed_angle_unpack(facings[low].pitch, TRUE);
	if (low + 1 >= playback.facing_count[player_index] ||
		facings[low + 1].tick - facings[low].tick > EYES_FACING_MAXIMUM_TICKS)
	{
		return TRUE;
	}
	fraction = (time - (real)(facings[low].tick + 1)) / (real)(facings[low + 1].tick - facings[low].tick);
	fraction = PIN(fraction, 0.0f, 1.0f);
	from = facing->yaw;
	to = distributed_angle_unpack(facings[low + 1].yaw, FALSE);
	facing->yaw = from + playback_angle_between(from, to) * fraction;
	from = facing->pitch;
	to = distributed_angle_unpack(facings[low + 1].pitch, TRUE);
	facing->pitch = from + (to - from) * fraction;
	return TRUE;
}

/* where the player aims at the moment: their smooth facing
(playback_eyes_facing), which is where a player looks, as their own view
is. Not the unit's aim: a unit turns toward its input's facing a little a
tick, so it lags each new facing by ticks, and between a far player's it
would step and swing. The unit's aim where the replay has no facing of
theirs, or theirs is far from it (dead, or the facing not the unit's). */
static void playback_eyes_aim(
	long unit_index,
	short player_index,
	real time,
	real_vector3d *aim)
{
	struct unit_datum *unit = unit_get(unit_index);
	real_euler_angles2d smooth;
	real_vector3d facing;

	*aim = unit->unit.aiming_vector;
	if (!playback_eyes_facing(player_index, time, &smooth))
		return;
	facing.i = (real)(cos(smooth.pitch) * cos(smooth.yaw));
	facing.j = (real)(cos(smooth.pitch) * sin(smooth.yaw));
	facing.k = (real)sin(smooth.pitch);
	/* (more than EYES_FACING_MAXIMUM_TURN apart: cos 90 degrees) */
	if (facing.i * aim->i + facing.j * aim->j + facing.k * aim->k > (real)cos(EYES_FACING_MAXIMUM_TURN))
		*aim = facing;
}

/* the eyes moved past the host's corrections of where the player is: what a
frame's move differs from the player's speed (or the vehicle's they ride)
by is a correction the client was sent, glided over (EYES_CORRECTION_TIME)
rather than jumped (a far player's come every second to sixth tick); a
larger one, a teleport or a respawn, is taken at once, as is any after
another player or view, or a jump in the replay's time (a keyframe, a
skip, a fast speed) */
static void playback_eyes_glide(
	long unit_index,
	real time,
	real_point3d *eyes)
{
	real elapsed = time - playback.eyes_time;
	struct object_datum *mover = object_get(object_get_ultimate_parent(unit_index));
	real_vector3d error;
	real distance;

	if (playback.eyes_unit != unit_index || elapsed < 0.0f || elapsed > 3.0f)
	{
		playback.eyes_unit = unit_index;
		playback.eyes_time = time;
		playback.eyes_drawn = *eyes;
		csmemset(&playback.eyes_offset, 0, sizeof(playback.eyes_offset));
		return;
	}
	error.i = eyes->x - playback.eyes_drawn.x - mover->object.translational_velocity.i * elapsed;
	error.j = eyes->y - playback.eyes_drawn.y - mover->object.translational_velocity.j * elapsed;
	error.k = eyes->z - playback.eyes_drawn.z - mover->object.translational_velocity.k * elapsed;
	distance = (real)sqrt(error.i * error.i + error.j * error.j + error.k * error.k);
	playback.eyes_time = time;
	playback.eyes_drawn = *eyes;
	if (distance >= EYES_CORRECTION_MAXIMUM)
		csmemset(&playback.eyes_offset, 0, sizeof(playback.eyes_offset));
	else if (distance > EYES_CORRECTION_MINIMUM)
	{
		playback.eyes_offset.i -= error.i;
		playback.eyes_offset.j -= error.j;
		playback.eyes_offset.k -= error.k;
	}
	/* (to a third in EYES_CORRECTION_TIME of the game) */
	{
		real kept = (real)exp(-elapsed / (EYES_CORRECTION_TIME * TICKS_PER_SECOND));

		playback.eyes_offset.i *= kept;
		playback.eyes_offset.j *= kept;
		playback.eyes_offset.k *= kept;
	}
	eyes->x += playback.eyes_offset.i;
	eyes->y += playback.eyes_offset.j;
	eyes->z += playback.eyes_offset.k;
}

/* the view through the player's eyes, where the frame draws them: their
aim (playback_eyes_aim) and their place (playback_eyes_glide) smooth */
static void playback_eyes_camera(
	long unit_index,
	short player_index)
{
	real time = playback_eyes_time();
	real_point3d eyes;
	real_point3d origin;
	real_point3d drawn;
	real_vector3d aim;

	unit_get_camera_position(unit_index, &eyes);
	/* (moved as the frame draws the unit) */
	object_get_origin(unit_index, &origin);
	drawn = origin;
	render_interpolation_object_point(unit_index, &drawn);
	eyes.x += drawn.x - origin.x;
	eyes.y += drawn.y - origin.y;
	eyes.z += drawn.z - origin.z;
	playback_eyes_glide(unit_index, time, &eyes);
	playback_eyes_aim(unit_index, player_index, time, &aim);
	playback_camera_set(&eyes, &aim);
}

/* the view the unit's player has in a seat with a third-person camera:
the game's own (following_camera_unit_view), moved as the frame draws the
vehicle and kept on this side of walls */
static void playback_seat_camera(
	long unit_index)
{
	long vehicle_index = object_get(unit_index)->object.parent_object_index;
	real_point3d focus;
	real_point3d origin;
	real_point3d drawn;
	real_point3d position;
	real_vector3d forward;
	real_vector3d back;
	real distance;
	struct collision_result collision;

	following_camera_unit_view(unit_index, &focus, &forward, &distance);
	object_get_origin(vehicle_index, &origin);
	drawn = origin;
	render_interpolation_object_point(vehicle_index, &drawn);
	focus.x += drawn.x - origin.x;
	focus.y += drawn.y - origin.y;
	focus.z += drawn.z - origin.z;
	back.i = -forward.i * distance;
	back.j = -forward.j * distance;
	back.k = -forward.k * distance;
	if (collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&focus, &back, NONE, &collision))
	{
		real kept = collision.t * distance > CAMERA_WALL_MARGIN ?
			(collision.t * distance - CAMERA_WALL_MARGIN) / distance : 0.0f;

		back.i *= kept;
		back.j *= kept;
		back.k *= kept;
	}
	position.x = focus.x + back.i;
	position.y = focus.y + back.j;
	position.z = focus.z + back.k;
	playback_camera_set(&position, &forward);
}

/* what the cameras look at: a little above what the unit is (on foot, or
the vehicle it rides), where it is drawn this frame */
static void playback_focus(
	long unit_index,
	real_point3d *focus)
{
	struct object_datum *object = object_get(unit_index);
	long followed = object->object.parent_object_index != NONE ? object->object.parent_object_index : unit_index;

	object_get_origin(followed, focus);
	render_interpolation_object_point(followed, focus);
	focus->z += followed == unit_index ? CAMERA_HEIGHT : CAMERA_VEHICLE_HEIGHT;
}

/* behind what the unit is (on foot, or the vehicle it rides): turned
with it, or, in the behind view once its right stick is touched, flown
round it (across with the stick's sideways, up to above it and down with
its up and down), at the left stick's zoom (its up nearer, down further).
The free camera is placed where this is */
static void playback_behind_camera(
	long unit_index,
	real seconds)
{
	struct object_datum *object = object_get(unit_index);
	long followed = object->object.parent_object_index != NONE ? object->object.parent_object_index : unit_index;
	real distance = followed == unit_index ? CAMERA_DISTANCE : CAMERA_VEHICLE_DISTANCE;
	real_point3d focus;
	real_point3d position;
	real_vector3d back;
	real_vector3d forward;
	struct collision_result collision;
	real yaw, turn;
	real pitch = CAMERA_PITCH;

	if (playback.view == _replay_view_behind)
	{
		real move_x, move_y, turn_x, turn_y, rise;
		boolean swiped = playback.touch_yaw != 0.0f || playback.touch_pitch != 0.0f;

		playback_sticks(&move_x, &move_y, &turn_x, &turn_y, &rise);
		if (!playback.behind_orbiting && playback.camera_valid && (turn_x != 0.0f || turn_y != 0.0f || swiped))
		{
			playback.behind_orbiting = TRUE;
			playback.behind_pitch = CAMERA_PITCH;
		}
		/* (a swipe as the right stick: across round them, up over them) */
		if (playback.behind_orbiting)
		{
			playback.yaw += playback.touch_yaw - turn_x * CAMERA_ORBIT_TURN * seconds;
			playback.behind_pitch = PIN(playback.behind_pitch - playback.touch_pitch -
				turn_y * CAMERA_ORBIT_TURN * seconds, CAMERA_ORBIT_HIGHEST, CAMERA_ORBIT_LOWEST);
			pitch = playback.behind_pitch;
		}
		playback.behind_zoom = PIN(playback.behind_zoom * (real)exp(-move_y * CAMERA_ZOOM_RATE * seconds),
			CAMERA_ZOOM_NEAREST, CAMERA_ZOOM_FARTHEST);
		distance *= playback.behind_zoom;
	}

	object = object_get(followed);
	playback_focus(unit_index, &focus);
	/* (eased round behind it, unless flown) */
	yaw = (real)atan2(object->object.forward.j, object->object.forward.i);
	if (!playback.camera_valid)
		playback.yaw = yaw;
	if (!playback.behind_orbiting || playback.view != _replay_view_behind)
	{
		turn = yaw - playback.yaw;
		while (turn > _pi)
			turn -= 2.0f * _pi;
		while (turn < -_pi)
			turn += 2.0f * _pi;
		playback.yaw += turn * CAMERA_TURN;
	}
	forward.i = (real)(cos(playback.yaw) * cos(pitch));
	forward.j = (real)(sin(playback.yaw) * cos(pitch));
	forward.k = (real)sin(pitch);
	back.i = -forward.i * distance;
	back.j = -forward.j * distance;
	back.k = -forward.k * distance;
	/* (no further than the first wall behind it) */
	if (collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&focus, &back, NONE, &collision))
	{
		real kept = collision.t * distance > CAMERA_WALL_MARGIN ?
			(collision.t * distance - CAMERA_WALL_MARGIN) / distance : 0.0f;

		back.i *= kept;
		back.j *= kept;
		back.k *= kept;
	}
	position.x = focus.x + back.i;
	position.y = focus.y + back.j;
	position.z = focus.z + back.k;
	playback_camera_set(&position, &forward);
}

void network_replay_playback_frame(
	void)
{
	struct player_datum *player;
	long unit_index;

	if (!playback.playing)
		return;
	/* (paused: the camera where it was, its sticks let be) */
	if (playback.paused && playback.camera_valid)
		return;
	/* (the touch controls' view swipe, which no player reads in a replay:
	the cameras', as the right stick's) */
	playback.touch_yaw = playback.touch_pitch = 0.0f;
#ifdef REPLAY_TOUCH
	{
		real gyro_yaw, gyro_pitch;

		halo_linux_touch_look(0, &playback.touch_yaw, &playback.touch_pitch, &gyro_yaw, &gyro_pitch);
	}
#endif
	player = playback.watched != NONE ? player_try_and_get(playback.watched) : NULL;
	/* (the first player alive, before anyone is watched, or once the one
	watched has left; one who died is watched again when they spawn, the
	camera where it was until then) */
	if (!player || (player->unit_index == NONE && !playback.camera_valid))
	{
		playback.watched = playback_next_player(NONE, FALSE);
		player = playback.watched != NONE ? player_try_and_get(playback.watched) : NULL;
	}
	unit_index = player ? player->unit_index : NONE;
	playback.first_person = FALSE;
	/* (the free camera, until it is placed, is the view before it; the
	cinematic one's too) */
	if ((playback.view == _replay_view_free || playback.view == _replay_view_cinematic) &&
		playback_free_camera(unit_index, main_get_seconds_elapsed()))
		;
	else if (unit_index == NONE)
		return;
	else if (playback.view == _replay_view_eyes)
	{
		director_perspective perspective;

		/* (what the player saw: a seat with a third-person camera's view, a
		Warthog's or a Ghost's, else their own eyes with their arms and
		weapon, as the director chooses for a player: director.c) */
		if (director_desired_perspective(unit_index, &perspective) &&
			object_get(unit_index)->object.parent_object_index != NONE)
		{
			playback_seat_camera(unit_index);
		}
		else
		{
			playback_eyes_camera(unit_index, (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(playback.watched));
			playback.first_person = TRUE;
		}
	}
	else
		playback_behind_camera(unit_index, main_get_seconds_elapsed());
}

long network_replay_watched_player(
	void)
{
	return playback.playing ? playback.watched : NONE;
}

boolean network_replay_viewing_eyes(
	void)
{
	return playback.playing && playback.view == _replay_view_eyes && playback.first_person;
}

struct observer_result const *network_replay_camera(
	void)
{
	return playback.playing && playback.camera_valid ? &playback.camera : NULL;
}

/* the hints' and the top line's font: the menus' small one (as the overlay's
small text), else the HUD's */
static long playback_font(
	void)
{
	long font_index = tag_loaded(FONT_GROUP_TAG, "ui\\small_ui");

	return font_index != NONE ? font_index : hud_get_font_index();
}

/* at the top, who is watched, the view and the speed; along the bottom
edge, on a dark bar, the buttons, with the controller's pictures of them
(the game's prompts' %a-button and so on: ui_widget.c's
draw_string_and_hack_in_icons; the Switch's and the PS5's own, swapped as a
playback's buttons are, hud_hires_tags.c); and with Back held, the scores */
/* a button in the hints: its picture (hud_hires_prompt_draw: Kenney's of
the controller), and its name on the controller where there is none (the
Switch's, the PS5's, else the Xbox's; the Switch's A and B as its menus
have them, as a replay does: xinput_sdl.c) */
static char const *playback_button_prompt(
	short button)
{
	switch (button)
	{
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a: return "a";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b: return "b";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x: return "x";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y: return "y";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black: return "black";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white: return "white";
	case _gamepad_binary_button_dpad_left: return "dpad_horizontal";
	case _gamepad_binary_button_dpad_up: return "dpad_up";
	case _gamepad_binary_button_dpad_down: return "dpad_down";
	case _gamepad_binary_button_back: return "back";
	case _gamepad_binary_button_start: return "start";
	case _gamepad_binary_button_right_thumb: return "stick_right_press";
	case _gamepad_binary_button_left_thumb: return "stick_left_press";
	case _replay_hint_stick_left: return "stick_left";
	case _replay_hint_stick_right: return "stick_right";
	}
	return "";
}

static wchar_t const *playback_button_label(
	short button)
{
#if defined(HALO_SWITCH)
	boolean swapped = config_boolean("controls.nintendo_menu_buttons") != 0;
#elif defined(HALO_PS5)
	boolean xbox_buttons = config_boolean("display.xbox_buttons") != 0;
#endif

	switch (button)
	{
	case _gamepad_binary_button_dpad_left: return L"D-PAD";
	case _gamepad_binary_button_dpad_up: return L"UP";
	case _gamepad_binary_button_dpad_down: return L"DOWN";
#if defined(HALO_SWITCH)
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a: return swapped ? L"A" : L"B";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b: return swapped ? L"B" : L"A";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x: return L"Y";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y: return L"X";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white: return L"L";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black: return L"R";
	case _gamepad_binary_button_back: return L"-";
	case _gamepad_binary_button_start: return L"+";
	case _gamepad_binary_button_right_thumb: return L"RS";
	case _gamepad_binary_button_left_thumb: return L"LS";
	case _replay_hint_stick_left: return L"LEFT STICK";
	case _replay_hint_stick_right: return L"RIGHT STICK";
#elif defined(HALO_PS5)
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a: return xbox_buttons ? L"A" : L"CROSS";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b: return xbox_buttons ? L"B" : L"CIRCLE";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x: return xbox_buttons ? L"X" : L"SQUARE";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y: return xbox_buttons ? L"Y" : L"TRIANGLE";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white: return xbox_buttons ? L"LB" : L"L1";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black: return xbox_buttons ? L"RB" : L"R1";
	case _gamepad_binary_button_back: return xbox_buttons ? L"BACK" : L"CREATE";
	case _gamepad_binary_button_start: return xbox_buttons ? L"START" : L"OPTIONS";
	case _gamepad_binary_button_right_thumb: return xbox_buttons ? L"RS" : L"R3";
	case _gamepad_binary_button_left_thumb: return xbox_buttons ? L"LS" : L"L3";
	case _replay_hint_stick_left: return L"LEFT STICK";
	case _replay_hint_stick_right: return L"RIGHT STICK";
#else
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a: return L"A";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b: return L"B";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x: return L"X";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y: return L"Y";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white: return L"LB";
	case FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black: return L"RB";
	case _gamepad_binary_button_back: return L"BACK";
	case _gamepad_binary_button_start: return L"START";
	case _gamepad_binary_button_right_thumb: return L"RS";
	case _gamepad_binary_button_left_thumb: return L"LS";
	case _replay_hint_stick_left: return L"LEFT STICK";
	case _replay_hint_stick_right: return L"RIGHT STICK";
#endif
	}
	return L"?";
}

/* a text of the hints at x, drawn or only measured; where it ends (3 short
of where the text says: render_state_text) */
static short playback_draw_hint_text(
	long font_index,
	rectangle2d const *line,
	short x,
	wchar_t const *text,
	boolean visible)
{
	real_argb_color color = *global_real_argb_white;
	rectangle2d bounds = *line;

	color.alpha = visible ? 1.0f : 0.0f;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, &color);
	bounds.x0 = x;
	bounds.x1 = (short)(bounds.x0 + 4000);
	draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, text, TRUE);
	return (short)(draw_string_and_hack_in_icons_port_end_x + 3);
}

/* a button of the hints at x, its picture centred on the text's capitals
(middle), else its name; where it ends */
static short playback_draw_hint_button(
	long font_index,
	rectangle2d const *line,
	short x,
	short middle,
	short button,
	boolean visible)
{
	char const *prompt = playback_button_prompt(button);

	if (hud_hires_prompt_draw(prompt, 0.0f, 0.0f, 0.0f, 0, FALSE))
	{
		if (visible)
		{
			hud_hires_prompt_draw(prompt, (real)x, (real)(middle - REPLAY_HINT_ICON_SIZE / 2),
				(real)REPLAY_HINT_ICON_SIZE, 0xFFFFFFFF, TRUE);
		}
		return (short)(x + REPLAY_HINT_ICON_SIZE + REPLAY_HINT_ICON_GAP);
	}
	return playback_draw_hint_text(font_index, line, x, playback_button_label(button), visible);
}

/* the hints along a line from x, drawn or only measured: each a button or
two (A/X) and what it does; where they end */
static short playback_draw_hint_line(
	long font_index,
	rectangle2d const *line,
	short x,
	struct playback_hint const *hints,
	short count,
	boolean visible)
{
	real_argb_color color = *global_real_argb_white;
	short middle;
	short index;

	draw_string_set_draw_mode(font_index, NONE, 0, 0, &color);
	middle = draw_unicode_string_capital_middle(line, L"");
	for (index = 0; index < count; index++)
	{
		wchar_t action[32];

		if (index)
			x = (short)(x + REPLAY_HINT_SPACING);
		x = playback_draw_hint_button(font_index, line, x, middle, hints[index].buttons[0], visible);
		if (hints[index].buttons[1] != NONE)
		{
			x = playback_draw_hint_text(font_index, line, x, L"/", visible);
			x = playback_draw_hint_button(font_index, line, x, middle, hints[index].buttons[1], visible);
		}
		usnprintf(action, NUMBEROF(action) - 1, L": %s", hints[index].action);
		action[NUMBEROF(action) - 1] = 0;
		x = playback_draw_hint_text(font_index, line, x, action, visible);
	}
	return x;
}

/* a tick of the replay as minutes and seconds from its start (mm:ss) */
static void playback_time_text(
	long tick,
	wchar_t *text,
	short size)
{
	long seconds = MAX(0, tick - playback.start_tick) / TICKS_PER_SECOND;

	usnprintf(text, size - 1, L"%02ld:%02ld", seconds / 60, seconds % 60);
	text[size - 1] = 0;
}

/* a filled circle, a row of pixels at a time */
static void playback_draw_circle(
	short x,
	short y,
	short radius,
	pixel32 color)
{
	short row;

	for (row = (short)-radius; row < radius; row++)
	{
		real across = (real)row + 0.5f;
		short half = (short)(sqrt((real)(radius * radius) - across * across) + 0.5f);
		rectangle2d span;

		span.x0 = (short)(x - half);
		span.x1 = (short)(x + half);
		span.y0 = (short)(y + row);
		span.y1 = (short)(span.y0 + 1);
		if (span.x1 > span.x0)
			draw_quad(&span, color);
	}
}

/* the seek bar, at the screen's foot under the hints: where the replay is
(the time at the left, its length at the right, and between them the line,
Halo blue to the playhead, a circle, and dimmer after it). It only shows:
nothing on it is chosen */
static void playback_draw_seek(
	long font_index)
{
	struct font_header *font = font_definition_get(font_index);
	short height = (short)(font->ascending_height + font->descending_height);
	rectangle2d window = render.camera.window_bounds;
	rectangle2d box;
	rectangle2d text_bounds;
	rectangle2d line;
	real_argb_color color = *global_real_argb_white;
	wchar_t text[16];
	long length = MAX(1, playback.end_tick - playback.start_tick);
	real fraction = PIN((real)(game_time_get() - playback.start_tick) / (real)length, 0.0f, 1.0f);
	short middle;
	short head;

	box.x0 = (short)(window.x0 + REPLAY_SEEK_INSET);
	box.x1 = (short)(window.x1 - REPLAY_SEEK_INSET);
	box.y1 = (short)(window.y1 - REPLAY_SCREEN_MARGIN / 2);
	box.y0 = (short)(box.y1 - MAX(height, REPLAY_SEEK_HEIGHT) - 4);
	draw_quad(&box, REPLAY_HINT_BAR_COLOR);
	middle = (short)((box.y0 + box.y1) / 2);
	/* (the times, either side) */
	text_bounds = box;
	text_bounds.y0 = (short)(middle - height / 2);
	text_bounds.y1 = (short)(text_bounds.y0 + height + 4);
	text_bounds.x0 = (short)(box.x0 + REPLAY_HINT_PADDING);
	text_bounds.x1 = (short)(text_bounds.x0 + REPLAY_SEEK_TIME_WIDTH);
	playback_time_text(game_time_get(), text, NUMBEROF(text));
	draw_string_set_draw_mode(font_index, NONE, 0, 0, &color);
	rasterizer_draw_unicode_string(&text_bounds, NULL, NULL, 0, text);
	text_bounds.x1 = (short)(box.x1 - REPLAY_HINT_PADDING);
	text_bounds.x0 = (short)(text_bounds.x1 - REPLAY_SEEK_TIME_WIDTH);
	playback_time_text(playback.end_tick, text, NUMBEROF(text));
	draw_string_set_draw_mode(font_index, NONE, 1, 0, &color);
	rasterizer_draw_unicode_string(&text_bounds, NULL, NULL, 0, text);
	/* (the line, played and to come, and the playhead on it) */
	line.x0 = (short)(box.x0 + REPLAY_HINT_PADDING + REPLAY_SEEK_TIME_WIDTH + REPLAY_SEEK_HEAD_RADIUS);
	line.x1 = (short)(box.x1 - REPLAY_HINT_PADDING - REPLAY_SEEK_TIME_WIDTH - REPLAY_SEEK_HEAD_RADIUS);
	if (line.x1 <= line.x0)
		return;
	line.y0 = (short)(middle - REPLAY_SEEK_THICKNESS / 2);
	line.y1 = (short)(line.y0 + REPLAY_SEEK_THICKNESS);
	head = (short)(line.x0 + (short)(fraction * (real)(line.x1 - line.x0) + 0.5f));
	{
		rectangle2d part = line;

		part.x1 = head;
		if (part.x1 > part.x0)
			draw_quad(&part, REPLAY_SEEK_COLOR);
		part.x0 = head;
		part.x1 = line.x1;
		if (part.x1 > part.x0)
			draw_quad(&part, REPLAY_SEEK_AHEAD_COLOR);
	}
	if (playback.skip_tick == NONE)
	{
		playback_draw_circle(head, middle, REPLAY_SEEK_HEAD_RADIUS, REPLAY_SEEK_COLOR);
		return;
	}
	/* (a skip ahead: where it goes, a tick on the line, and the playhead
	throbbing in a glow of its blue, two rings of it, fainter outside) */
	{
		real throb = 0.5f - 0.5f * (real)cos(2.0f * _pi *
			(real)(system_milliseconds() % REPLAY_SEEK_THROB_MILLISECONDS) / (real)REPLAY_SEEK_THROB_MILLISECONDS);
		real target = PIN((real)(playback.skip_tick - playback.start_tick) / (real)length, 0.0f, 1.0f);
		short glow = (short)(REPLAY_SEEK_HEAD_RADIUS + REPLAY_SEEK_GLOW_WIDTH * (0.5f + 0.5f * throb) + 0.5f);
		pixel32 alpha = (pixel32)(REPLAY_SEEK_GLOW_ALPHA * (0.4f + 0.6f * throb));
		rectangle2d mark;

		mark.x0 = (short)(line.x0 + (short)(target * (real)(line.x1 - line.x0) + 0.5f) - 1);
		mark.x1 = (short)(mark.x0 + 2);
		mark.y0 = (short)(middle - REPLAY_SEEK_HEAD_RADIUS);
		mark.y1 = (short)(middle + REPLAY_SEEK_HEAD_RADIUS);
		draw_quad(&mark, REPLAY_SEEK_TARGET_COLOR);
		playback_draw_circle(head, middle, glow, (alpha / 2) << 24 | (REPLAY_SEEK_COLOR & 0xFFFFFF));
		playback_draw_circle(head, middle, (short)((glow + REPLAY_SEEK_HEAD_RADIUS) / 2),
			alpha << 24 | (REPLAY_SEEK_COLOR & 0xFFFFFF));
		playback_draw_circle(head, middle, (short)(REPLAY_SEEK_HEAD_RADIUS + REPLAY_SEEK_THROB_GROWTH * throb + 0.5f),
			REPLAY_SEEK_COLOR);
	}
}

/* whether the buttons are a touchscreen's: the touch controls show
(touch_input.c), and press the buttons drawn for them */
static boolean playback_touch(
	void)
{
#ifdef REPLAY_TOUCH
	return touch_input_screen_buttons() != 0;
#else
	return FALSE;
#endif
}

/* on a touchscreen, the hints as boxes in a row above the seek bar (two,
the first above, past REPLAY_TOUCH_ONE_ROW), each
its action's word, brighter while its button is held; and where each is,
for the touch controls to press its button under a finger
(touch_input_replay_targets: their places as parts of the screen) */
static void playback_draw_touch_hints(
	long font_index,
	struct playback_hint const *hints,
	short count)
{
	struct font_header *font = font_definition_get(font_index);
	short height = (short)(font->ascending_height + font->descending_height);
	rectangle2d window = render.camera.window_bounds;
	real_argb_color color = *global_real_argb_white;
	int targets[REPLAY_TOUCH_MAXIMUM_TARGETS * REPLAY_TOUCH_TARGET_FIELDS];
	/* (the targets are parts of the viewport, which fills the screen as a
	finger's place is of it, not of the window drawn in: inside it, and
	smaller on Android, which put them below the boxes, as the text chat's
	keyboard's: network_chat.c) */
	rectangle2d viewport = render.camera.viewport_bounds;
	long screen_width = MAX(1, viewport.x1 - viewport.x0);
	long screen_height = MAX(1, viewport.y1 - viewport.y0);
	short shown = (short)MIN(count, REPLAY_TOUCH_MAXIMUM_TARGETS);
	short per_row = shown > REPLAY_TOUCH_ONE_ROW ? (short)((shown + 1) / 2) : shown;
	short rows;
	short bottom;
	short width;
	short x = 0;
	short index;
	rectangle2d box;

	if (shown <= 0)
	{
#ifdef REPLAY_TOUCH
		touch_input_replay_targets(targets, 0);
#endif
		return;
	}
	rows = (short)((shown + per_row - 1) / per_row);
	width = (short)((window.x1 - window.x0 - 2 * REPLAY_SEEK_INSET - (per_row - 1) * REPLAY_TOUCH_GAP) / per_row);
	width = (short)MIN(width, REPLAY_TOUCH_MAXIMUM_WIDTH);
	/* (where the controller's bar is: above the seek bar; the last row's
	bottom there, the first above it) */
	bottom = (short)(window.y1 - REPLAY_SCREEN_MARGIN / 2 - MAX(height, REPLAY_SEEK_HEIGHT) - 4 - REPLAY_SCREEN_MARGIN / 2);
	for (index = 0; index < shown; index++)
	{
		short button = hints[index].buttons[0];
		int *target = targets + index * REPLAY_TOUCH_TARGET_FIELDS;
		short row = (short)(index / per_row);
		rectangle2d text;

		/* (a row's first: its boxes centred, the row's own count of them) */
		if (index % per_row == 0)
		{
			short in_row = (short)MIN(per_row, shown - index);

			x = (short)((window.x0 + window.x1 - (in_row * width + (in_row - 1) * REPLAY_TOUCH_GAP)) / 2);
			box.y1 = (short)(bottom - (rows - 1 - row) * (REPLAY_TOUCH_HEIGHT + REPLAY_TOUCH_GAP));
			box.y0 = (short)(box.y1 - REPLAY_TOUCH_HEIGHT);
		}

		box.x0 = x;
		box.x1 = (short)(x + width);
		x = (short)(box.x1 + REPLAY_TOUCH_GAP);
		draw_quad(&box, playback.held[button] ? REPLAY_TOUCH_HELD_COLOR : REPLAY_HINT_BAR_COLOR);
		text = box;
		text.y0 = (short)(box.y0 + (REPLAY_TOUCH_HEIGHT - height) / 2);
		text.y1 = (short)(text.y0 + height + 4);
		draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
		rasterizer_draw_unicode_string(&text, NULL, NULL, 0, hints[index].action);
		target[0] = (int)((box.x0 - viewport.x0) * 10000L / screen_width);
		target[1] = (int)((box.y0 - viewport.y0) * 10000L / screen_height);
		target[2] = (int)((box.x1 - viewport.x0) * 10000L / screen_width);
		target[3] = (int)((box.y1 - viewport.y0) * 10000L / screen_height);
		target[4] = button;
	}
#ifdef REPLAY_TOUCH
	touch_input_replay_targets(targets, shown);
#endif
}

/* the buttons' hints on a dark bar that fits them, above the seek bar at
the bottom edge (their width from a pass drawn unseen first); a
touchscreen's as boxes to press (playback_draw_touch_hints) */
/* a bar of hints, centred, its bottom at bottom; its top */
static short playback_draw_hint_bar(
	long font_index,
	struct playback_hint const *hints,
	short count,
	short bottom)
{
	struct font_header *font = font_definition_get(font_index);
	short height = (short)(font->ascending_height + font->descending_height);
	rectangle2d window = render.camera.window_bounds;
	rectangle2d line;
	rectangle2d bar;
	short width;

	line = window;
	line.y0 = (short)(window.y1 - REPLAY_HINT_HEIGHT);
	width = (short)(playback_draw_hint_line(font_index, &line, window.x0, hints, count, FALSE) - window.x0);
	bar.x0 = (short)((window.x0 + window.x1 - width) / 2 - REPLAY_HINT_PADDING);
	bar.x1 = (short)(bar.x0 + width + 2 * REPLAY_HINT_PADDING);
	bar.y1 = bottom;
	bar.y0 = (short)(bar.y1 - MAX(height, REPLAY_HINT_HEIGHT) - 6);
	draw_quad(&bar, REPLAY_HINT_BAR_COLOR);
	line.y0 = (short)(bar.y0 + 3);
	line.y1 = bar.y1;
	playback_draw_hint_line(font_index, &line, (short)(bar.x0 + REPLAY_HINT_PADDING), hints, count, TRUE);
	return bar.y0;
}

/* the buttons' hints on a dark bar, and the camera's (the view's sticks
and buttons: none for a touchscreen, whose own controls they are) on
another above it */
static void playback_draw_hints(
	long font_index,
	struct playback_hint const *hints,
	short count,
	struct playback_hint const *camera,
	short camera_count)
{
	struct font_header *font = font_definition_get(font_index);
	short height = (short)(font->ascending_height + font->descending_height);
	rectangle2d window = render.camera.window_bounds;
	short top;

	if (playback_touch())
	{
		playback_draw_touch_hints(font_index, hints, count);
		playback_draw_seek(font_index);
		return;
	}
	/* (above the seek bar: playback_draw_seek) */
	top = playback_draw_hint_bar(font_index, hints, count,
		(short)(window.y1 - REPLAY_SCREEN_MARGIN / 2 - MAX(height, REPLAY_SEEK_HEIGHT) - 4 - REPLAY_SCREEN_MARGIN / 2));
	if (camera_count > 0)
		playback_draw_hint_bar(font_index, camera, camera_count, (short)(top - REPLAY_HINT_ROW_GAP));
	playback_draw_seek(font_index);
}

/* the pause menu: the screen dimmed, PAUSED in its middle, and its
buttons' bar (resume, the hints shown or hidden, leave) */
static void playback_draw_pause_menu(
	long font_index)
{
	struct playback_hint const hints[] = {
		{ { _gamepad_binary_button_start, NONE }, L"RESUME" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y, NONE },
			config_boolean("network.replay_hints") ? L"HIDE HINTS" : L"SHOW HINTS" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b, NONE }, L"LEAVE" },
	};
	struct font_header *font = font_definition_get(font_index);
	short height = (short)(font->ascending_height + font->descending_height);
	real_argb_color color = *global_real_argb_white;
	rectangle2d window = render.camera.window_bounds;
	rectangle2d bounds;

	draw_quad(&window, REPLAY_PAUSE_DIM_COLOR);
	bounds = window;
	bounds.y0 = (short)((window.y0 + window.y1 - height) / 2);
	bounds.y1 = (short)(bounds.y0 + height + 4);
	draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, L"PAUSED");
	playback_draw_hints(font_index, hints, NUMBEROF(hints), NULL, 0);
}

void network_replay_playback_draw(
	void)
{
	static wchar_t const *const view_names[NUMBER_OF_REPLAY_VIEWS] = { L"BEHIND", L"EYES", L"FREE CAMERA", L"CINEMATIC" };
	/* (the third, R3: behind flown round the player, how to have it turn
	with them again; the free camera, how to have it track them, or stop;
	left out otherwise) */
	static struct playback_hint const hints[] = {
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a,
			FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x }, L"PLAYER" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y, NONE }, L"VIEW" },
		{ { _gamepad_binary_button_right_thumb, NONE }, L"RESET" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white,
			FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black }, L"SPEED" },
		{ { _gamepad_binary_button_dpad_left, NONE }, L"SEEK" },
		{ { _gamepad_binary_button_back, NONE }, L"SCORES" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b, NONE }, L"HIDE" },
		{ { _gamepad_binary_button_start, NONE }, L"PAUSE" },
	};
	/* (a touchscreen's: a box a button, both ways for the player and the
	seek; the fourth, R3, as the controller's third) */
	static struct playback_hint const touch_hints[] = {
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_x, NONE }, L"PREV" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_a, NONE }, L"NEXT" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y, NONE }, L"VIEW" },
		{ { _gamepad_binary_button_right_thumb, NONE }, L"RESET" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_white, NONE }, L"SLOWER" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_black, NONE }, L"FASTER" },
		{ { _gamepad_binary_button_dpad_left, NONE }, L"<<" },
		{ { _gamepad_binary_button_dpad_right, NONE }, L">>" },
		{ { _gamepad_binary_button_back, NONE }, L"SCORES" },
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b, NONE }, L"HIDE" },
		{ { _gamepad_binary_button_start, NONE }, L"PAUSE" },
	};
	/* (a controller's camera bar, above the buttons': the free camera's
	sticks, its tracking and its return behind the player; behind's) */
	static struct playback_hint const free_camera_hints[] = {
		{ { _replay_hint_stick_left, NONE }, L"MOVE" },
		{ { _replay_hint_stick_right, NONE }, L"AIM" },
		{ { _gamepad_binary_button_right_thumb, _gamepad_binary_button_dpad_up }, L"TRACK" },
		{ { _gamepad_binary_button_left_thumb, _gamepad_binary_button_dpad_down }, L"FOLLOW PLAYER" },
	};
	static struct playback_hint const behind_camera_hints[] = {
		{ { _replay_hint_stick_left, NONE }, L"ZOOM" },
		{ { _replay_hint_stick_right, NONE }, L"ORBIT" },
		{ { _gamepad_binary_button_right_thumb, _gamepad_binary_button_dpad_up }, L"RESET" },
	};
	static struct playback_hint const touch_show[] = {
		{ { FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_b, NONE }, L"SHOW" },
	};
	/* (the replay over: back to the last keyframe, before it returns to
	the menu) */
	static struct playback_hint const touch_ended[] = {
		{ { _gamepad_binary_button_dpad_left, NONE }, L"<<" },
	};
	struct playback_hint shown[NUMBEROF(touch_hints)];
	short shown_count = 0;
	short index;
	long font_index;
	struct font_header *font;
	real_argb_color color;
	rectangle2d window;
	rectangle2d bounds;
	short height;
	wchar_t text[64];
	wchar_t speed[16];
	struct player_datum *player;

	if (!playback.playing)
		return;
	player = playback.watched != NONE ? player_try_and_get(playback.watched) : NULL;
	if (playback.scores && player)
	{
		game_engine_port_rasterize_scores(playback.watched);
		return;
	}
	font_index = playback_font();
	if (font_index == NONE)
		return;
	/* (a touchscreen's lines hidden: a box to show them again, B's) */
	if (playback.hidden && !playback.paused)
	{
		if (playback_touch())
			playback_draw_touch_hints(font_index, touch_show, NUMBEROF(touch_show));
		return;
	}
	if (playback.paused)
	{
		playback_draw_pause_menu(font_index);
		return;
	}
	if (playback.skip_tick != NONE)
		speed[0] = 0;
	else if (playback.speed == 0)
		usnprintf(speed, NUMBEROF(speed) - 1, L"PAUSED");
	else if (playback.speed == REPLAY_NORMAL_SPEED)
		speed[0] = 0;
	else if (replay_speeds[playback.speed] < 1.0f)
		usnprintf(speed, NUMBEROF(speed) - 1, L"1/%dx", (int)(1.0f / replay_speeds[playback.speed] + 0.5f));
	else
		usnprintf(speed, NUMBEROF(speed) - 1, L"%dx", (int)replay_speeds[playback.speed]);
	speed[NUMBEROF(speed) - 1] = 0;
	if (playback.ended)
		usnprintf(text, NUMBEROF(text) - 1, L"REPLAY OVER");
	else
	{
		/* (a client's recording's other player through their eyes: their
		aim between the host's messages, which came every second to sixth
		tick, is the smooth turn between them, not theirs) */
		boolean smoothed = playback.view == _replay_view_eyes && playback.client && player &&
			!playback.recorders[DATUM_INDEX_TO_ABSOLUTE_INDEX(playback.watched)];

		/* (the free camera names the player only while it tracks them) */
		boolean tracking = playback.view == _replay_view_free && playback.free_tracking && !playback.free_placing;

		usnprintf(text, NUMBEROF(text) - 1, L"%.12s   %s   %s",
			(playback.view == _replay_view_free && !tracking) || !player ? L"REPLAY" : player->name,
			smoothed ? L"EYES (SMOOTHED)" : tracking ? L"FREE CAMERA (TRACKING)" : view_names[playback.view], speed);
	}
	text[NUMBEROF(text) - 1] = 0;
	font = font_definition_get(font_index);
	height = (short)(font->ascending_height + font->descending_height);
	color = *global_real_argb_white;
	window = render.camera.window_bounds;

	/* (the top line, centred: draw mode 2) */
	bounds = window;
	bounds.y0 = (short)(window.y0 + REPLAY_SCREEN_MARGIN);
	bounds.y1 = (short)(bounds.y0 + height + 4);
	draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	if (playback.ended)
	{
		if (playback_touch())
			playback_draw_touch_hints(font_index, touch_ended, NUMBEROF(touch_ended));
		return;
	}
	if (playback_touch())
	{
		for (index = 0; index < (short)NUMBEROF(touch_hints); index++)
		{
			if (index != 3 || (playback.view == _replay_view_behind && playback.behind_orbiting))
				shown[shown_count++] = touch_hints[index];
			else if (playback.view == _replay_view_free)
			{
				shown[shown_count] = touch_hints[index];
				shown[shown_count++].action = L"TRACK";
			}
		}
		playback_draw_hints(font_index, shown, shown_count, NULL, 0);
		return;
	}
	/* (a controller's hints hidden from the pause menu: the seek bar alone,
	network.replay_hints) */
	if (!config_boolean("network.replay_hints"))
	{
		playback_draw_seek(font_index);
		return;
	}
	/* (a controller's: R3 is in the camera's bar above, the view's) */
	for (index = 0; index < (short)NUMBEROF(hints); index++)
	{
		if (index != 2)
			shown[shown_count++] = hints[index];
	}
	if (playback.view == _replay_view_free)
	{
		/* (R3 and D-pad Up toggle the tracking: what it does now) */
		struct playback_hint camera[NUMBEROF(free_camera_hints)];

		csmemcpy(camera, free_camera_hints, sizeof(camera));
		if (playback.free_tracking && !playback.free_placing)
			camera[2].action = L"FREE";
		playback_draw_hints(font_index, shown, shown_count, camera, NUMBEROF(camera));
	}
	else if (playback.view == _replay_view_behind)
		playback_draw_hints(font_index, shown, shown_count, behind_camera_hints, NUMBEROF(behind_camera_hints));
	else
		playback_draw_hints(font_index, shown, shown_count, NULL, 0);
}

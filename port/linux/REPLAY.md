# Replays

Recording a network game so that it can be watched again later, from any
player's view or a free camera. This file holds the plan and, phase by
phase, what was built. It answers "Proposal: Lightweight Network State
Replay System", which suggested recording the netcode's state, compressing
it, adding keyframes for seeking, and putting the version in a header. That
direction is right. What follows fits it to this netcode
([NETCODE.md](NETCODE.md)).

## What a replay is here

The Xbox's films replayed each tick's input, which works only because the
Xbox played in lockstep: every machine simulated the whole game from the
same inputs. The native builds no longer do (NETCODE.md, "Distributed
netcode"), and `source/saved films/saved_films.c` is empty. So a replay
records the netcode's messages instead.

A client does not just draw what the host sends. It simulates the game
itself. It drives remote players with the input the host relays, and their
shots, projectiles and effects come from that. The host's state then pulls
each object back toward the host's copy. So **playing a replay is running
the game as a client with no player of its own, its messages read from a
file instead of a socket**. Replays reuse the decoder the game already has:
`network_objects.c`, `network_actors.c`, the damage, statistics and co-op
handlers.

What the replay records is what the host would send such a client: a
**recorder**, a client machine that has no players. The host already sends
a machine whose players have never been alive every player's unit and
input every tick (NETCODE.md, "The players each client needs"), and the
objects and actors as often as they are near anyone's players. That is
what a free camera needs. It is about one client's traffic at its fullest,
not the host's traffic to all of its clients.

Where the proposal differs from this netcode:

- **Size.** The proposal's ~50 MB for a host's recording assumes the
  host keeps every client's stream. One recorder's stream is what is
  recorded, so a host's file is about one client's size.
- **Culling.** No player is ever left out of a client's updates here; far
  and unseen players are sent less often. A recording made on a client
  would be complete for its own view, only coarser for far players.
- **Quantization and deltas.** The wire format does most of it already:
  16 bits a vector part, parts at their default left out, buttons sent
  only where they changed, what a unit carries only when it changes. The
  file records the encoded messages, and compression comes later, when
  phase 0 has measured what takes the room.
- **Out-of-order packets.** The host records what it sends, before the
  transport, so nothing is lost or reordered.
- **Versions.** The messages can change with a network version (41 now).
  The header says which a replay was recorded in, with the map and the
  game variant. A replay plays on a build of its network version or of a
  later one whose messages are its own but for some kinds, which are not
  read from it (`replay_changed_messages`, from
  `port/linux/network_format.txt`'s fingerprints: 40 changed only a
  client's join request, no replay's; 41 a vote's status). The oldest
  played is 39, the first whose replays could be; older ones are refused
  with a message. A version that changes what a replay has more deeply
  sets the oldest to itself. A Custom Edition map's replay needs that map
  (map sharing's hashes can check it).

## The plan

| Phase | What | Done when |
| --- | --- | --- |
| 0 | The host writes the recorder's messages to a file. `tools/replay_stats.py` says how large it is, raw and through zlib, and which messages take the room. | A 16-player game of bots is measured. |
| 1 | Playback, start to end. A client with no player reads a file instead of the host. A spectator camera follows any player, or flies free. | A recorded game plays to its end and looks like the game did. |
| 2 | Seeking. A keyframe every few seconds: what a machine joining the game in progress is given (objects, statistics, the game type's state). A seek backwards loads the map again, applies the keyframe before the target and runs ahead to it as fast as it can. | Seeking anywhere in a 10-minute game takes a few seconds on a PC, and is measured on the Switch. |
| 3 | The files and the menu. Compression (zlib in the game, maybe the messages split by type if phase 0 shows it pays). A Replays menu lists the files, with each one's map, game type, players and length, and plays one. Old network versions are refused with a message. | A replay is recorded, found and watched from the menus on every platform. |
| 4 | Further, if wanted: a client's recording of its own view, sharing replays, co-op levels' scripts and cutscenes in playback, voice. | |

## Phase 0: recording

`network.record_replays = true` (`HALO_NET_RECORD_REPLAYS=1`) makes a
host record each network game it hosts, a player's or a dedicated
server's, to `replays/` in the data folder. It is off by default: nothing
plays the files yet. The overlay's System folder has it (Record Replays).
It is read at a game's first tick, so a game is recorded whole or not at
all: turned on during a game, it records from the next. The main menu's
Theater has it too (RECORD REPLAYS), says how many replays there are and
where, and on Android copies them out of the app's folder (EXPORT
REPLAYS: the system's folder picker, `HaloActivity.exportReplays`) and
others into it (IMPORT: the system's file picker,
`HaloActivity.importReplays`).

### What is recorded

`port/linux/game/network_replay.c` opens the file at the host's first tick
of a game (`network_distributed_tick`), and closes it when the map is
unloaded (`game_dispose_from_old_map`) or a new game starts.

The recorder is a machine slot that no client has, joined or still
joining, and no player is at, a bot's either (the bots take slots from 65
up, `bots.h`). It is the highest such slot, found again each tick
(`distributed_recorder_find` in `network_distributed.c`), so a client or a
bot taking it moves it. It has no players, so:

- `distributed_host_plan_players` sends it every player's unit and input,
  every tick;
- the moving objects (`network_objects.c`), what units carry, and the
  actors (`network_actors.c`) go to it as often as they are near any
  player, as to a client whose players are dead;
- every damage event goes to it (`distributed_event_goes_to`);
- an object at rest goes to it only when its state differs from the last
  it was sent. A client is sent each object at rest again and again (as it
  comes to rest, and four a tick round them all), in case a datagram was
  lost; the recorder loses none. `network_objects.c` keeps a checksum of
  each object's last state sent to the recorder (4 bytes an object), not
  the state itself (44 bytes, for up to 8192 objects).

The loops that decide what each client is sent take it through
`distributed_viewing_machines`, which is `distributed_client_machines`
with the recorder added. Everything that counts machines as players
(votes to kick or to skip a cutscene, pings, chat, voice, map sharing)
still uses `distributed_client_machines`, and never sees it.

What is written:

| Record | What |
| --- | --- |
| datagram | The recorder's batch of the tick's unreliable messages (`distributed_batch_flush`), as a client would receive it |
| reliable | A distributed message sent to every client reliably (`_distributed_to_clients_reliably`): objects created and deleted, kills, the game type's state... |
| reliable to the recorder | One sent to the recorder alone, reliably (none yet: it never loads late) |
| game message | A message of the game's own sent to every machine in the game (`network_game_server_send_message_to_all_machines`): a player joining or leaving, the game's end |

The clients are sent what they were before; the recorder adds a datagram a
tick to the host's work and nothing to the network.

### The file

Little-endian. A header of 384 bytes (format 4; formats 1 to 3's ended at
280, after the version text; format 0's was 216, its version text 32
bytes, too few for `OpenCE Android | local build (<commit>)`):

| Offset | Size | What |
| --- | --- | --- |
| 0 | 8 | `OCREPLAY` |
| 8 | 4 | The format, 4 (3 had no length, server or player; 2's records were not compressed; 1 had no game record; 0 a 216-byte header) |
| 12 | 4 | The network version (`HALO_PORT_NETWORK_VERSION`) |
| 16 | 4 | The build's number |
| 20 | 4 | Ticks a second (30) |
| 24 | 4 | The host's tick at the start |
| 28 | 4 | The header's size (384): records start there |
| 32 | 8 | When it started, seconds since 1970 |
| 40 | 128 | The map's path, as the game names it (`levels\test\bloodgulch\bloodgulch`) |
| 168 | 16 | The game variant's name |
| 184 | 96 | The build's version text |
| 280 | 4 | The game's length in ticks: written again at each of the file's syncs and at its end, so a file a crash cut short has its length to its last sync |
| 284 | 4 | Flags: 1, a client recorded it (else its host) |
| 288 | 64 | The game's name, the server's as its players see it (UTF-8) |
| 352 | 32 | The name of the recording machine's first player (UTF-8; none for a dedicated server) |

Then records to the end of the file; from format 3 they are one zlib
stream (zlib's fastest level, about a fifth of their size), synced every
five seconds, so that a file a crash cut short reads to its last sync.
From format 2 the first is the game (kind 5): `struct network_game` as the host had it at its first
tick, its map, game type, players, random seed and PC options, which a
playback loads the game from. Each record is a byte for the kind (1 to 5,
the table above and the game), a byte 0, the size (16 bits), the host's tick (32 bits), then
the bytes as they would have been sent, before the transport swaps their
header. A distributed message is its 8-byte header (`struct
distributed_message_header`: the message header, the type, the count, the
tick) and its entries. A datagram is a batch (`_distributed_message_batch`),
whose messages each follow as their length (16 bits), their header past
the message header, and their entries. A game message is bit-packed by
`network_messages.c`.

The file is written through a 64 KB buffer, and flushed every five seconds:
a crash loses no more, and `tools/replay_stats.py` reads a file cut short.

### Measuring

```
python3 tools/replay_stats.py replays/20261027-201500_bloodgulch.ocreplay
```

prints the game's length; the file's size raw and through zlib at levels
1, 6 and 9, each per minute and for ten minutes; and the bytes by record
kind and by message type. `tools/test_replay_stats.py` tests it.

To record a game of bots without playing, set in `config.toml` (or the
`HALO_` variables on a desktop):

```
[network]
record_replays = true
[bots]
count = 15
[debug]
network_test = "local:bloodgulch:slayer"
```

`network_test`'s local game is hosted as a network game, so the recorder
runs; with 15 bots it is a game of 16 players. Copy the file from
`replays/` and run `tools/replay_stats.py` on it.

### Results

The first recording, from Android (format 0, before objects at rest were
left out): a Slayer game on Beaver Creek, three players (a player and two
bots added at ticks 145 and 171), 50 seconds. Every one of its 1,512 ticks
has its datagram, and the file was closed when the map was unloaded.

| | 50 seconds | A minute | 10 minutes |
| --- | --- | --- | --- |
| Raw | 594 KB | 707 KB | 6.9 MB |
| zlib, level 1 | 135 KB | 161 KB | 1.6 MB |
| zlib, level 9 | 121 KB | 144 KB | 1.4 MB |

zlib takes it to a fifth, and its fastest level is nearly as good as its
best: compressing in the game (phase 3) can be cheap.

| Message | Share | |
| --- | --- | --- |
| object_states | 50% | about four a tick, 44 bytes each: most were objects at rest, sent again in their turn (now left out) |
| unit_states | 26% | the players: with their input, about 58 bytes a player a tick |
| relayed_actions | 12% | the players' input |
| the rest | 12% | the batches' and records' headers, inventories, objects made and deleted, statistics, the game type's state, damage |

The second, from Android (format 1, objects at rest left out unless they
change): Slayer on Rat Race, three players again, 50 seconds, every one of
its 1,502 ticks there.

| | Beaver Creek (format 0) | Rat Race (format 1) |
| --- | --- | --- |
| Raw | 594 KB (707 KB a minute) | 341 KB (409 KB a minute) |
| zlib, level 1 | 135 KB | 122 KB |
| zlib, level 9 | 121 KB | 110 KB |
| object_states | 295 KB, 50% | 46 KB, 14% |
| Ticks with no object state | none | 3 in 4 |

Leaving the objects at rest out took the raw file down by 43%, but the
zlibbed one by 9% only: zlib had found most of those repeats itself. The
players are now most of the file (unit_states 43%, relayed_actions 20%),
the same 58 bytes a player a tick as before.

What grows with the players is about 58 bytes a player a tick: 16 players
would be about 17 MB raw in ten minutes for them alone, about 3.5 MB
zlibbed, beside what the objects take. Getting a 16-player game well under
that needs what is particular to these messages (phase 3): states as their
difference from the last one of the same object or player, and the
messages split by type before they are compressed.

## Phase 1: playback

The Theater (under Multiplayer in the main menu) lists the replays, a row
each, the newest first (its map, when and length, from its header, read
once for each new file), scrolling over more; A plays the one chosen, and
the panel says what it is, or why it cannot be played (a replay from
before format 2, or of another network version). Its buttons are RECORD,
DELETE (pressed twice), IMPORT and EXPORT (Android) and BACK. A recording shorter than
15 seconds is deleted as it ends.
`port/linux/game/network_replay_playback.c`:

- **As a client with no player and no host.** The file is read whole.
  The global client is made one joined to the game of its first record
  (`network_game_client_port_replay_begin`, `network_client_manager.c`),
  with no machine of its own, and its next idle loads that game as a
  client loads the game it joins (`network_game_client_game_has_started`).
  Its clock starts at the replay's first tick, released at once: no host
  sends the first game update it waits for.
- **The records, at their ticks.** Each frame, before its ticks, the
  records of the ticks the clock has reached go where a client's reads
  of its host go: the datagrams and the reliable messages to
  `network_distributed_handle_message`, marked as having come unreliably
  or not (`network_connection_port_set_last_read_unreliable`), and the
  game's own to `network_game_client_handle_message`. Once the first
  tick's are handed over (the host's objects, all of them) the client is
  synchronized, as a client the host has told them all is. The client
  sends nothing: `network_game_client_write` drops it, and the in-game
  idle asks for no connection.
- **The view.** The game has no local player, so the frame's first
  window is local player 0's (as the Xbox's film playback's was), with
  the playback's camera, made once a frame (`main.c`), which the sound
  listens from too (`sound_manager.c`); no HUD is drawn
  (`interface_draw_screen`). Y goes round four views: behind the player
  watched (or the vehicle they ride), where the frame draws them (render
  interpolation), turning round behind them, kept this side of walls,
  until the right stick flies it round them (across, and up to straight
  above them; the right stick's click turns it with them again), the left
  stick's up and down taking it nearer and further;
  what they saw, where they aim: through their eyes with their arms and
  weapon (`first_person_weapons.c` drawing them as local player 0's), or,
  in a seat with a third-person camera (a Warthog's, a Ghost's), the
  game's view from behind it (`following_camera_unit_view`); a free
  camera that flies as the game's debug camera does (the left stick moves
  it level, toward where it looks across the ground, as its tilt squared,
  so a slight tilt moves it slowly, its speed eased up in 0.2 seconds and
  down in 0.08; the right one turns it; the left trigger takes it down and
  the right one up, from still to full in a quarter of a second, so a tap
  of the Switch's ZL or ZR, which are buttons, is a small step; it slides
  along the level's walls, its scenery and its doors rather than flying
  through them, `playback_free_camera_move`). It is a tripod: chosen, it is
  placed where the view behind the player would be, and tracks them, its
  aim turning after them a little behind (0.15 seconds), at their upper
  body (`playback_free_camera_track`); the left stick and the triggers move
  it and it goes on tracking, the right stick takes the aim and it stops,
  and R3 starts it again from where it is, or stops it, holding its aim.
  It tracks the next player A or X chooses, aims at where the player died
  until they spawn again, and the top line says FREE CAMERA (TRACKING).
  Until it first moves, it zooms on the player as they go further than 5
  units, so they stay the size they were (down to 8 degrees of view);
  once moved it eases back to the usual view, and does not zoom again until
  it is chosen again (`playback_free_camera_zoom`). And a cinematic camera:
  the free camera with no controls (its sticks, R3, L3 and swipes not
  heard, and no camera bar above the buttons'), placed and tracking and
  zooming as it is when chosen; whenever the level, its scenery or its
  doors hide the player from it for 0.2 seconds, it is placed behind them
  again, as L3 places the free camera, and tracks them from there
  (`playback_cinematic_sight`); hidden from it in the half second after
  they spawn, it is placed behind them at once. The top line says CINEMATIC. Through the eyes, the aim
  turns smoothly between the facings the replay has of the player (all
  of them indexed as it loads, `playback_index_facings`: the host's
  relayed input and a client's own input's records), the aim the
  facing at the frame's moment (`playback_eyes_aim`: a unit's own aim
  lags each new facing by ticks), and the host's
  corrections of where they are are glided over in a tenth of a second
  rather than jumped (`playback_eyes_glide`). Every player's eyes can be
  watched: a client's recording has the players far from its own, or out
  of their sight, every second to sixth tick, so their aim between those
  is the smooth turn, not theirs, and the top line says EYES (SMOOTHED).
- **The controls.** A watches the next player alive, X the one before;
  one who dies is watched again when they spawn. The d-pad's left goes
  back to the last keyframe (the one before it, within two seconds of
  one) and its right on to the next; held, either scrubs (see
  "Keyframes" and "Scrubbing"). The right
  shoulder button (RB, the Switch's R, the PS5's R1) plays faster (2, 4,
  8 times) and the left one slower (a half, a quarter, then a pause:
  `game_time_set_speed`). Back held shows the scores, the watched
  player's row as a player's own. B hides the lines over the screen
  and shows them again. Start puts up the pause menu: the game stopped
  (`game_time_set_speed`), the camera kept where it is, the screen
  dimmed with PAUSED in its middle; in it B leaves the replay and Start
  again resumes it at its speed. A playback is a client of the
  distributed netcode, whose host's rules put a client's game speed back
  each tick (`cheats_network_client_enforce`); a playback's is its own.
  A line at the top says who is watched, the view and the speed. Along
  the bottom edge a seek bar shows where the replay is: the time (mm:ss)
  at its left, the replay's length (to its last record) at its right, and
  between them a Halo blue line, the playhead a circle on it, dimmer
  after it; it only shows (`playback_draw_seek`). Above it a dark bar
  shows the buttons, in the menus' small
  font, each as a picture of the controller's button: Kenney's Input
  Prompts, the Xbox Series' in common and the Switch's and the PS5's on
  them (`hud_hires_prompt_draw`; port/assets/buttons/README.md, "Replay
  prompts"), as the game's own message icons have no d-pad, Back or Start
  (its d-pad's are the A's). Without a picture, a button's name on the
  controller is written instead (the Switch's L, R, - and +, the PS5's
  L1, R1, CREATE and OPTIONS, the Xbox's LB, RB, BACK and START).
- **On a touchscreen** (Android, while the touch controls show:
  `touch_input_screen_buttons`) the bar's buttons are boxes in a row, a
  word each: PREV, NEXT, VIEW, RESET or TRACK (as R3's hint), SLOWER,
  FASTER, << and >> (the d-pad's left and right: held, they scrub),
  SCORES (held), HIDE and PAUSE; the pause menu's RESUME and LEAVE; with
  the lines hidden, SHOW; at the replay's end, <<. The playback says where
  they are (`touch_input_replay_targets`, `host_touch_targets`), and the
  touch controls, in a replay (`_touch_scene_replay`), show only their
  stick and press the controller's button of the box under a finger
  (`TouchControls.java`'s targets), so the playback reads them as a
  controller's. Their stick is the left stick (the free camera's move,
  behind's zoom), and a swipe elsewhere is the right one: the free
  camera's aim, behind's orbit (`halo_linux_touch_look`, which no player
  reads in a replay).
- **The end.** The replay's end, or the game's, returns to the Theater
  half a second later (`network_game_abort`), unless the d-pad's left
  goes back first: the main menu loads, and opens the Theater over it
  (`network_replay_theater_wanted`, as a replay left from its pause menu
  does too), B going back to the main menu. The records after the game's end are not handed to
  the client: the host's switch to the next game's pregame among them
  would leave it in a pregame with no host, which it reports as the host
  gone (`playback_feed`).
- **Keyframes.** Every 10 seconds of the game, as the playback first
  reaches them (the first at its start), `playback_keyframe_take` keeps
  the game as it is in memory, copied in one go and compressed on a
  thread of its own with zlib's fastest level (the game goes on; the
  copy and the room it is compressed into, each the size of what is
  kept, stay for the next replay): the
  game state's parts in use (`game_state_port_used`) and the variables of
  the simulation and of the netcode's client that are not in it, which
  the guest image's linker script gathers into one block
  (`port/android/guest/guest.ld`'s `.replay_data` and `.replay_bss`: the
  object files of `source/game`, `objects`, `units`, `items`, `effects`,
  `physics` and `devices`, the client's in `source/networking`, and the
  port's network, ragdoll and game mode modules), with where the file's
  records were. Going back puts one back as a core is loaded
  (`game_state_port_restore`, its procedures before and after), then the
  block, then sets right what is this machine's rather than the game's
  (the host's last message's time, the frame's interpolation, the
  first-person weapon, the playback's speed). With no keyframe ahead, it
  plays ten seconds on from where it is (2:34 to 2:44) at 50 times, up to
  60 ticks a frame and never past it nor past the next keyframe's tick,
  where that keyframe is taken (2:40: `network_replay_ticks_per_frame`).
  The frame it was asked in is held: drawn again with no tick run, its 3D
  view copied (`rasterizer_port_hold_capture`), and drawn back in place of
  the view each frame until the skip is there (`rasterizer_port_hold_draw`),
  the replay's lines over it as they change (the seek bar's playhead
  moving), and the sound paused; then the playback is at normal speed. The replay's records go to the game before each
  tick (`network_replay_playback_tick`), as a client reads its host's
  between ticks, so a frame of many ticks plays as the recording did. They
  take up to 256 MB (a log line says each one's size and how long its
  compression took); past that, none more are taken.

- **The announcer** speaks to the player watched as to a local player
  (kills, sprees, the respawn countdown: `game_engine.c`'s
  `game_engine_player_heard`), and through their eyes their own unit is
  not drawn (`render_objects.c`'s `object_is_first_person_camera`).

The Theater keeps the newest `network.replays_kept` replays (30; the
oldest are deleted as a recording starts, 0 keeps them all), and DELETE
deletes the one chosen, at a second press.

**A client's recordings.** A client of a network game records too, with
the same setting (`network_replay_client_tick`, from its first tick): what
it reads of its host, as it reads it (`network_replay_record_received`,
`network_client_manager.c`: the host's datagrams to it, its reliable
messages and the game's own), and its own players' input, which the host
never sends it (kind 6: each tick's input as the host relays a player's,
the client's tick its number: `network_distributed.c`'s
`distributed_record_own_action`). Its game record is the game as it
joined it. A playback gives that input to its player as the host's
relayed input (`network_distributed_own_actions`), so the client's player
is driven as the others are. Such a replay is the game as that client had
it: the players far from its own, or out of sight, sent every second to
sixth tick, and only the events the host sent it (damage near its
players, every kill); smaller than a host's. A host of this build with
`network.full_detail_to_clients` on (off by default) sends every client
every player every tick and every damage event, as its own recorder is
sent (`distributed_host_plan_players`, `distributed_event_goes_to`), so a
client's replay of its game holds all of the players; an upstream host, or
this one with it off, sends the usual.

**Taking them off the console, and putting others on.** Android's
EXPORT REPLAYS copies them through the system's folder picker, and IMPORT
copies the files chosen in the system's file picker into the folder, as the
consoles' page takes an upload: each into `<name>.part`, kept only once it
is whole and begins as a replay does, its name's other characters made `_`,
and a name the folder has already left as it is. The Switch
and the PS5 have no file manager another app can open, so while the
Theater is up they serve a page on the local network
(`port/linux/game/network_replay_page.c`, from port 8090; shown in the
Theater as a QR code and an address, as Direct Link's page is, and off with
`network.link_page`): the replays, each a download, and an upload (the
page's script sends each file with `PUT /upload/<name>`). One connection at
a time; a download is sent and an upload read a piece at a time over the
frames (512 KB a frame at most), the upload into `<name>.part`, which
becomes the replay only once it is whole and begins as a replay does; a
name the folder has already is refused.

Not yet: co-op levels' scripts have not been tried.

## Scrubbing

The d-pad's skips are a seek: the playback has one target, a marker on
the seek bar, and taps and holds move it; the skip ahead (50 times, the
frame asked in held, the sound paused, the playhead throbbing) takes the
playback there (`playback_skip`, `playback_scrub`, `playback_seek_chase`).
Keyframes stay in memory only.

- **Taps.** Right goes to the keyframe ahead if there is one, else sets
  the target ten seconds on, exactly (2:34 to 2:44), and plays to it.
  Left goes back to a keyframe, as now. Taps add up: a tap during a skip
  moves the target on from where it was (three taps of right, 30
  seconds), not from where the playback is.
- **Holding right.** The press does what a tap does; held past about 0.3
  seconds, the marker slides on, slowly at first (about ten seconds of
  the game a second), faster over about 1.5 seconds, to a speed that
  crosses the whole bar in about four. The playback chases it live
  behind the held frame: where the marker is over what has been played
  already, the last keyframe before it is put back, and from there it
  plays to it fast, the playhead moving and throbbing. Let go, the marker
  stops and the skip goes on to it. It cannot be cancelled.
- **Holding left.** The press goes back to the keyframe to the left, as a
  tap does; held, the marker slides back as it slides on. Let go, the
  keyframe at or before the marker is put back and the playback plays
  to the marker, fast, as a skip ahead does.
- The target stays between the start and the replay's length. While a
  direction is held, the other is not heard.
- **Start** during a skip or a scrub ends it: the marker goes, the pause
  menu comes up, and a resume plays on from wherever the playback had
  got to, at normal speed.
- With the keyframes' memory full (256 MB), a skip goes on to its
  target, taking none more, rather than ending at once.

# PS5

Halo: Combat Evolved on a jailbroken PlayStation 5, as a homebrew title
(`PPSA99500`): the menus, the campaign and multiplayer at 120 frames a
second on a TV that takes it, up to native 4K, split screen with a second
pad, aiming with the DualSense's gyroscope, voice chat through the pad's
microphone, and system link and internet play with the other builds. It
needs your own copy of the game's Xbox disc image, from which the first
start extracts the maps. This README is for players first; building and
developing are at its end.

## What the console needs

- **System software 13.60 or lower.** It was made and tested on 13.59;
  PS5_Vulkan, whose toolchain builds it, was proved on 12.02 to 12.70.
  Nothing yet jailbreaks 14.00 or later: keep a console you want to run
  homebrew on from updating.
- **A kernel exploit and kstuff**: the Relapse exploit chain (7.00 to
  13.60, run from the console's browser) or another for your firmware,
  then kstuff (kstuff-lite 1.07 or later). They do not survive a reboot:
  run them again after each one.
- **ShadowMountPlus 1.7beta5 or later**, which puts the folders in
  `/data/homebrew` on the home screen (1.7beta5 repaired a hook that could
  be lost after a game's launch or rest mode).
- **An FTP server** on the console, such as ftpsrv (port 2121), to copy
  files to it from a computer.
- etaHEN was loaded on the test console as well. The title does not need
  it, and ShadowMountPlus's 1.7beta5 notes advise against it; it worked
  here.
- A TV or monitor; 4K and 120 Hz are used when the console's video output
  is set to them (Settings, Screen and Video).

## Installing from the release

1. Download `halo-ps5-release.zip` from the repository's latest release
   (https://github.com/thelinkin3000/SlipstreamCE/releases/latest)
   and unzip it on your computer.
2. Over FTP, copy its `PPSA99500` folder into `/data/homebrew` on the
   console.
3. Make the folder `/data/halo` and copy the game's disc image (`.iso` or
   `.xiso`) into it.
4. Start SlipstreamCE from the home screen.

`README.txt` in the zip says the same in a few lines. The zip also holds
`COPYING` and `licenses/`, the licences of what the title is built from;
`NOTICE.txt` in the title's folder says where its source is.

## Game data

The game's data folder is `/data/halo`; make it over FTP and put the game's
Xbox disc image (`.iso` or `.xiso`) in it. On the first start the game finds
no `maps` folder there and extracts it from the image (about 1.8 GB, half a
minute): the desktop game asks whether to and which image, and on the PS5
the host answers "Yes" and the first `.iso` or `.xiso` in `/data/halo`.
Nothing is drawn meanwhile; the console's notifications say what goes on:
one when the extraction starts (the image's name), one at every 10 %, and
one when the maps are in place, after which the menu comes up. With no
`maps` folder and no disc image, one notification says to put the image in
`/data/halo` over FTP and start again, and the title ends. After that the
folder holds:

- `maps/`, the extracted maps (the image can then be removed);
- `config.toml`, the game's settings, written with the defaults on the
  first start (`port/linux/README.md` describes them);
- `save/`, the saves;
- `debug.txt`, the game's own log, and `halo.log`, the host's;
- `vk_cache/` and `radv-shader-cache/`, the renderer's caches (below, "The
  picture").

## Updates

A release's build looks for a newer release on GitHub each time it starts,
about ten seconds in, while you play. When there is one it downloads it
(about 50 MB) and installs it in the title's folder, with notifications
("SlipstreamCE: downloading build N", every quarter of the way, then "SlipstreamCE: build N
is installed: close the game and start it again to use it"). The game goes
on as it was until you close it; the next start is the new build. If the
download or the install does not finish, a notification says why and
nothing is changed. `update.auto = false` in `/data/halo/config.toml` turns
the look off. To update by hand, copy a newer release's `PPSA99500` folder
over the old one (with the game closed).

## Settings

The game's settings are in its menus (Settings), as the PS5's: Video Setup's
MAXIMUM RESOLUTION, Gamepads' GYRO AIM and the rest (below, "The menus"),
and in `/data/halo/config.toml`, which you can edit over FTP with the game
closed (`docs/settings.md` describes every setting). Two defaults are a
computer's rather than a console's: voice chat's push-to-talk is the `V`
key (`controls.push_to_talk`: set it to `"Gamepad LB"`, say, to talk with the
pad), and the frame rate is not capped (`display.max_fps = 0`; the display's
59.94 or 119.88 Hz paces it anyway).

Everything in `/data/halo` is yours over FTP: the title makes each file and
folder it writes there readable and writable by everyone, so the console's
FTP server can delete the maps (the next start extracts them again), edit
`config.toml` or remove the caches.

## The log

The host's log is `/data/halo/halo.log` (or `halo.log` in the title's folder
when there is no `/data/halo`), and every line goes to the kernel log too,
prefixed `halo: `. `halo_log = false` in the `[debug]` section of
`config.toml` (the overlay's **Write halo.log**) writes the kernel log
only, from the next start; `log_files = false` (**Write debug.txt**)
writes no `debug.txt`. It has the ranges of memory the game was given, the
game's own output (`game: `), the files the extraction writes, the maps
opened, and every 5 s a heartbeat: the frames swapped, their mean and
longest time, how many took over 20 and over 33 ms (and over 10 and 17 ms
at 120 Hz), what the log's lines cost, and every minute the host's heap
and the console's free direct memory. A line costs its writer 2 to 13 ms
on the console (most of it the kernel log's), so a thread of the log's own
writes them; errors, the end and a crash report are written at once. When the game ends it lists the system calls it asked for
and did not get, the OpenGL names it called, and the memory figures, then
`the host ends`. After that the kernel log reports a `SIGSYS` for the
title: every PS5 title that exits gets one (`PS5.md`, phase 0 B), and it
means nothing more.

A crash is reported in the log with the signal, the registers and the
return addresses of the stack; an address in the game is looked up with
`addr2line -e build/ps5/halo_guest_vk.elf`, one in the host in
`build/ps5/host/eboot.elf.map`.

## The menus

The game's menus are the PS5's own (`ps5` in the menu files' `platform`
lists): Video Setup has MAXIMUM RESOLUTION, V-SYNC, SMOOTH MOTION,
GRAPHICS and FOV AND VIEWMODELS, with nothing of a window's; Graphics has
the anti-aliasing the console's renderer draws (off, FXAA, MSAA 2X and
4X); Audio Setup starts at the volumes, with no device rows (the console
has one output and one microphone); Gamepads ends with GYRO AIM; Network
Setup has internet play, UPnP and map sharing, but no clipboard or
updates rows (neither is a console's).

## The picture

The game draws through the Vulkan renderer of the Android port
(`port/android/VULKAN.md`), whose host half is compiled for the console as it
is, on RADV, the Vulkan driver PS5_Vulkan builds from PS5_Mesa, linked into
the title. The log's `renderer:` line names it; a console where it does not
come up ends the title (there is no other renderer), saying why.

The game draws at its maximum resolution, Video Setup's MAXIMUM
RESOLUTION (`display.resolution_limit`, `port/linux/README.md`): 2160P
(3840x2160, the default), 1440P (2560x1440) or 1080P (1920x1080), no more
than the console's video output shows (Settings, Screen and Video: on a
1080p output, 2160P draws at 1920x1080), and the picture is scaled to the
display. A change applies at once, in the menus or in a level: the log says
`maximum resolution: "1440" (config.toml, changed in play); the display is
3840x2160: the game draws at 2560x1440`. Each holds 119.88 frames a second
(the figures below). The display runs at 59.94 Hz, or at 119.88 Hz: the title declares
high-frame-rate output (`param.json`), and RADV takes the 120 Hz mode when
the console's output does (Settings, Screen and Video, Video Output, Enable
120 Hz Output, with a TV that takes it). RADV checks the switch once and
sometimes logs `59.94 Hz restored` while the display goes on at 120 Hz; the
heartbeat's frames a second say which it is. The system's menu may bring
the display back to 59.94 Hz for the rest of the run.

The renderer compiles the game's shaders on a thread of its own the first
time it meets them, below the game's priority (732 against the game's
700, in the console's range of 256, the highest, to 767), and keeps them,
with its pipelines, in `/data/halo/vk_cache`; RADV keeps its own in
`/data/halo/radv-shader-cache`. A pipeline not yet made is made by the
draw that needs it: on a first start its first sight costs a frame (a
match's first minute had about a dozen frames over 33 ms more than with
the caches). A second start compiles nothing it has met before. Removing
either folder only makes the next start compile again.

For testing, through `environment.txt` (`tools/ps5_deploy.py --env`):

- `HALO_VK_SELF_TEST=1`: the renderer's and the game's self-tests at start
  (`clears self-test: ok`, `data self-test: ... ok`);
- `HALO_VK_PRESENT_MARKER=1`: a red and a green square at the picture's top
  corners, to see which way up it is;
- `HALO_GPU_STATS=1`: the draws and what skipped them, every second;
- `HALO_PS5_SCREENSHOT=<seconds>`: the picture as it went to the TV, written
  that often to `/data/halo/screenshots` (the last 200 kept);
  `tools/ps5_deploy.py`'s `fetch` brings them to the run's folder as PNG
  and deletes them from the console.

RADV's own variables (`RADV_DEBUG`, `RADV_PERFTEST`, `MESA_VK_*`) reach the
driver as they are; `MESA_VK_ABORT_ON_DEVICE_LOSS=1` stops at a GPU fault
with Mesa's message in the kernel log, where RADV's lines go, prefixed
`[radv]`. The game's own frame counter is a console command: a line
`display_framerate 1` in `/data/halo/init.txt`.

## The pad and the sound

The game's pads are SDL2's game controllers: the DualSense, listed under
the signed-in user's name, as a PS5 controller. The console also lists its
own "PS5 Remote Control" as a controller; the game is never given it, and
the log says so (`gamepads: ... left out`). A pad switched off and on is
listed again (`SDL: gamepad removed`, `added`); when SDL2 says nothing of
it, the host compares the pads it lists once a second and tells the game
itself (`gamepads: ... (the poll)`). A second DualSense is a second
player: switched on, the console signs it in as a guest ("Invitado1" on a
Spanish console), no account needed, and the game opens it on the next
port, so two players play split screen from the menus. The pad rumbles through
SDL2. Every minute in which the pad was used, a `pad:` line lists the
buttons pressed, each stick's and trigger's range and the rumble asked for.

**Gyro Aim** (Settings, Gamepads, Gyro Aim: ON, OFF or ONLY ON ZOOM, the
two sensitivities and inverts; `input.gyro_*` in `config.toml`) aims with
the DualSense's gyroscope, each local player with their own pad: steer
the pad like a wheel to turn the view sideways, tip its top away to look
up. Each sensitivity turns the view as far as it does on the Switch (5
felt right in testing). The host finds each pad's user by the name SDL2 gives it,
turns the pad's motion sensor on and reads the pad's state every frame
(`gyro: pad 1 ("thelinkin3000") is user ...'s, ... the motion sensor on`);
the `pad:` line adds the fresh samples a second (120 at 120 frames a
second), the largest turn of a frame and the turn in all.

The sound is SDL2's device on the console's audio output: 48 kHz, float,
stereo, 1024 frames a callback (21.3 ms), the format the game asks for.
SDL2 calls for sound on a thread that cannot run the game, so each call is
handed to a thread that can. Every minute an `audio:` line gives the
callbacks, the gaps between them (one longer than two periods is late)
and the time the hand-off took; the end of the run gives the totals.

**Voice chat's microphone** is the signed-in player's: the DualSense's
own, or a headset in its jack. SDL2 records nothing on the console, so the
host reads the console's audio input itself (libSceAudioIn, loaded at the
start: `microphone: libSceAudioIn: ... 0x00000000`), 48 kHz mono, the rate
voice chat speaks at. The pad's mute button silences it: the input then
gives only zeros, which are dropped (`microphone: the input gives zeros`),
as are the second or two of zeros while a headset is plugged in. Every
minute it is open an `audio: the microphone:` line gives its buffers a
second (187) and the seconds of zeros.

SDL2's video is never started: its PS5 driver crashes a title (it calls
the keyboard library, which a title does not load). The log's `SDL_Init`
line says `video driver none`.

The PS button's menu and the Quick Menu do not stop the game; the host
reads the system's flags and gives the game "focus lost" while either is
up, and "focus gained" after, as a desktop window would (`overlay:` lines).
In a campaign level (the loaded map is a solo one) the menu coming up also
opens the pause screen, as the Start button does: the host holds Start on
the first pad for 100 ms. In a match or the menus nothing pauses (a match
cannot), and the log says which (`overlay: the map is ratrace (type 1,
multiplayer ...): no pause`). The system sometimes sets the flag for about
6 s after the title is launched; that start-up overlay is told apart and
pauses nothing. The console's rest mode with the title running was not
tried: if it ends the title, what survives is the game's saves and the
renderer's caches as last written.

## The network

The console plays system link and internet play with the Linux, Windows,
Android, Switch and web builds of the same network version, as
`port/linux/README.md` sets them out ("System link", "Internet play"): the
system link list finds the console's games and the console finds theirs
by broadcast; it joins internet games by invite and from the server
browser, and hosts them; it downloads a Custom Edition map it lacks from
the game's players (map sharing). The settings are the desktop's
(`network.*` in `config.toml`; `network.address` and `network.broadcast`
work as there).

- **Sockets** are the console's own (FreeBSD's, in libkernel), through the
  Linux host's `posix_net.c`. A title is refused two things a socket
  normally takes: `fcntl`'s `O_NONBLOCK` and `ioctl`'s `FIONBIO` and
  `FIONREAD` answer `EACCES`, so a socket is made non-blocking with the
  console's own option (`SO_NBIO`). The game's addresses are Winsock's
  layout and the console's BSD's; the host turns each round.
- **Names** (the brokers', STUN's) are looked up with the console's resolver
  (libSceNet), since its C library refuses `getaddrinfo` to a title.
- **This machine's address** is the one its default route leaves from; the
  log's `network:` line gives it beside the console's own word on it
  (`sceNetCtlGetInfo`).
- **Internet play's brokers** are `brokers.txt`, which the title carries
  and writes to `/data/halo` (beside `config.toml`) at every start, so an
  update's list replaces it; name a list of your own in
  `network.brokers_file`.
- **UPnP** asks the router to forward internet play's port as on the
  desktop (miniupnpc). Behind a router that will not, or a second NAT, the
  console reaches hosts behind lenient NATs only: the game says so when it
  starts (`this network's NAT gives each destination its own port`).
- **Browsers** join the console's internet games through DTLS (Mbed TLS,
  with the console's random numbers); the certificate is made at the start,
  and the log gives its hash (`network: DTLS certificate made`).
- **Voice chat** both ways: the other players' voices, and the player's
  through the pad's microphone (above).
- **Map sharing** downloads to `/data/halo/custom_maps` at the host's
  upload limit (`network.map_share_upload`, 1024 KB/s by default: a 500 MB
  map takes eight minutes).
- Not on a console: Discord, the `halo://` link handler and the command
  line, and a clipboard shared with other programs. Server Setup's INVITE
  LINK shows the game's invite link as a QR code for a phone to read (B
  closes it). Join internet games from the server browser, or with an
  invite link sent from a phone: while Join Game >
  Direct Link is open, the game serves a page on the local network, shown
  there as a QR code and an address, where the link is pasted
  (`network.link_page`, as the Switch's: `port/linux/src/link_page.c`).
  Direct Link has no PASTE LINK.

`HALO_PS5_NET_TEST=1` in `environment.txt` runs the console's socket
checks before the game starts (`net test:` lines in the log): a socket,
the route and the console's address, a broadcast, the options UPnP's
search sets, the resolver, a connection to the first broker, a loopback
connection, the random numbers, the interfaces, and the ways of making a
socket non-blocking.

## What runs, and what does not yet

- Runs: the game's start-up, the first start's extraction, the menus,
  the campaign and multiplayer on the TV, paced to the display's 59.94 or
  119.88 Hz; settings, saves and the game's files in `/data/halo`; the pad,
  its rumble and the sound through SDL2; the system's menus as focus events
  (the level goes on under them); system link, internet play, the server
  browser and map sharing (above).
- Also: the menus as the PS5's (below), native 4K drawing and the maximum
  resolution changed in play, Gyro Aim with the DualSense, voice chat's
  microphone, the first start's progress as notifications, split screen
  with a second pad, the pads' arrival and removal.
- Also: the title updates itself from the releases (above, "Updates").
  The PS button's pause in a campaign level and the console's rest mode
  were tried by the user by hand and worked (2026-10-19).
- Not yet: a picture during the first start's extraction (notifications
  only), and a keyboard and mouse. On a new install the first visit to
  each place stutters while its pipelines are made (about 130 ms each,
  none on a second visit); making several at once, ahead of the frame's
  draws, is the plan's next item (`PS5.md`, "What next").

## Figures

On the test console (13.59, 120 Hz output), in a Blood Gulch match the
console hosts with its bot (`HALO_NETWORK_TEST=host:bloodgulch`):

| | Frames a second | Frames over 17 / 20 / 33 ms in 5 s |
|---|---|---|
| 1080p, warm caches | 119.88 | 0 / 0 / 0 (longest 8.4 ms) |
| 2160p, warm caches | 119.88 | 0 / 0 / 0, one over 10 ms in 3 minutes |
| 1080p, split screen (two players) | 119.88 | 0 in 50 of 59 windows |
| 1080p, first minute with no caches | about 116 | a dozen frames over 33 ms in the minute |

The host's heap starts at 160 MB and stays there through a match (192 MB
at 2160p or in split screen). Pacbrew's SDL2 leaks a string in each call
of `SDL_GameControllerTypeForIndex`; the host asks it once per pad and
remembers the answer (it leaked 16 to 32 MB a minute before).
`port/ps5/host/host_alloc_track_ps5.c`, switched on at its top, logs every
minute which callers' heap grew.

For the deploy's tests, two files in the title's folder change a run:
`hold.txt` (`--hold`) keeps the title for 120 s after the game ends, and
`environment.txt`, `NAME=VALUE` lines (`--env NAME=VALUE`, again for each;
an upload without `--env` removes the file), gives the game settings
through its environment (`HALO_EXIT_AFTER=60` ends it after a minute; the
settings' variables are in `port/linux/README.md`).

## Building

The title is built on Linux, by `ninja ps5`, or by the repository's
GitHub workflow, whose `ps5` job builds the release (`.github/workflows/build.yml`).

### What the build needs

- The tools of `ninja linux_x86_64` (Python, ninja, a clang with the x32
  target, the OpenGL headers); `--ps5-guest-cc` selects the clang, which is
  the host's clang 22 by default.
- PS5_Vulkan (github.com/mpereiraesaa/PS5_Vulkan) at the commit
  `tools/ps5_build.py` pins, set up by its
  `tools/setup-native-dependencies.sh`, which puts its payload SDK fork and
  zlib under `.deps/native`. `configure.py` looks for it in `--ps5-vulkan`,
  then `PS5_VULKAN`, then `~/projects/ps5-deps/PS5_Vulkan`.
- PS5_Mesa (github.com/mpereiraesaa/PS5_Mesa) beside it (`--ps5-mesa`), and
  RADV's release archive built from it into PS5_Vulkan's
  `.deps/native/radv-release`: `port/ps5/probe/vk/build.sh` builds it. That
  needs meson 1.4 or newer, LLVM 18 with libclc and the SPIR-V translator,
  and glslang, which `--ps5-hostdeps` (default
  `~/projects/ps5-deps/hostdeps/env.sh`) puts on the path.
- Clang 22 for the host (`--ps5-llvm-config`, default
  `/usr/bin/llvm-config-22`), as RADV's archive is built with it.
- CMake, and a network connection for the first `configure.py --ps5`: it
  downloads musl and SDL3 to `build/ps5/third_party`, glslang (or takes the
  Android build's clone), and pacbrew's SDL2 (346 MB, once, into
  PS5_Vulkan's `.deps/pacbrew`).

### The build

1. Go to the root folder of the repository.
2. Enter `python3 configure.py --ps5`, with the options of your other builds.
3. Enter `ninja ps5`.

The title is `build/ps5/title/PPSA99500`: `eboot.bin` (the host),
`sce_sys/param.json` and `icon0.png`, `sce_module/libc.prx` (PS5_Vulkan's),
`halo_guest.elf` (the game) and `NOTICE.txt`, which names what the title is
built from: PS5_Vulkan, its SDK fork, PS5_Mesa, pacbrew and glslang, at
their commits. PS5_Vulkan and its fork are GPL-3.0-or-later, so the title
is a GPL work as a whole (glslang has its own licences, named in its
repository). The
host's ELF with its symbols, to look a crash's addresses up in, is
`build/ps5/host/eboot.elf`.

### CI

The workflow's `ps5` job (Ubuntu 24.04) clones PS5_Vulkan, its SDK fork
and PS5_Mesa at `tools/ps5_build.py`'s pins (read from the script), sets
them up with PS5_Vulkan's own scripts and caches the result by the pins
(`.deps/native`, 300 MB, and pacbrew's 346 MB), so RADV's build (5 minutes)
is paid once per pin: 11 minutes with nothing cached, 3 with the caches.
Meson reads `LLVM_CONFIG` as its llvm-config, which would give Mesa's
native OpenCL tools LLVM 22 with no SPIR-V translator; the job runs meson
through a wrapper that gives that set-up LLVM 18's, as the build machine's
was. `tools/ci_build.py ps5 release` makes `dist/halo-ps5-release/` (the
title's folder, README.txt, COPYING, licenses/), which the release job
zips stored (`zip -0`), as the updater reads it. A build of a release tag
`v<number>` is given that number (`HALO_BUILD_NUMBER`), which the updater
compares; other builds have none and never look.

The title as built on CI and on the build machine from one commit is the
same file for file but for `eboot.bin`, which embeds the folders it was
built in, and the game image's build time (`__DATE__` and `__TIME__`;
`SOURCE_DATE_EPOCH` gives one).

## Developing

The deploy (`tools/ps5_deploy.py`) needs the console's FTP server on port
2121 and klogsrv on port 3232. To start the title from the PC, PS5_Vulkan's `ps5vkctl` payload
must also be resident (port 9111); load it once after each boot of the
console, with the payload manager (itsPLK's ps5-payload-manager, port 8084)
running:

```
python3 tools/ps5_deploy.py load-ps5vkctl
```

Then one command installs the title, starts it, follows its log and fetches
its files:

```
python3 tools/ps5_deploy.py --run
```

The run's files go to `build/ps5/logs/<time>`, a folder made at the
launch that holds that run alone: `halo.log`, `klog.txt` (the kernel log
of the run), the part of the game's `debug.txt` the run added, and its
screenshots. The follower stops at the host's last line, a crash, or as
soon as the title no longer runs (killed from the console or from another
shell). What an earlier run left on the console and no folder has goes to
a `<time>-leftover` folder first. `--minutes N` makes a long run: it
follows for N minutes, fetching the screenshots every 5 minutes, then
kills the title and fetches. `--host` names another console;
`tools/ps5_deploy.py --help` lists the actions one at a time (`upload`,
`launch`, `kill`, `status`, `follow`, `fetch`).

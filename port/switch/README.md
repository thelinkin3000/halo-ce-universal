# Switch

`ninja switch` builds the game for the Nintendo Switch, as a devkitA64
homebrew program. It needs two toolchains, and neither can be replaced by the
other.

The game is the same guest image as the Android build: the decompilation
compiled as ILP32 AArch64 code, with 32-bit pointers, because the game's data
formats embed pointers of the Xbox's size and they have to keep their
layout. Clang offers exactly one such target, `arm64_32-apple-watchos`, whose
Mach-O output `tools/android_asm_convert.py` rewrites into ELF. The Android
NDK supplies that clang along with `llvm-ar` and `ld.lld`; devkitA64 cannot
build the guest, because it is LP64 only.

The host is the Android host library (`port/android/host`) with three files
replaced and two more added, linked by devkitA64 against libnx, SDL2,
Mesa and deko3d. Refer to [port/android/README.md](../android/README.md) for the guest
and the platform layer, and [port/linux/README.md](../linux/README.md) for
the platform layer the two share.

The host changes are these:

| File | Why |
|---|---|
| `host_main.c` | no JNI and no APK; libnx start-up; the SD card for data |
| `host_sdl2.c` | answers the guest's SDL3 calls with devkitPro's SDL2 |
| `host_sdl3_events.c` | translates SDL2's events into the guest's SDL3 shapes |
| `host_gl.c` | Mesa is linked in, so there is no `libGLESv3.so` to open |
| `host_mman.c` | `mmap`/`mprotect` over the console's memory services |
| `host_futex.c` | futuxes over libnx's mutex and condition variable |
| `host_net.c` | the network calls over libnx's sockets; see Internet play |

## Requirements

You do not need the Xbox SDK. You need the tools of the Linux build
(Python, ninja) and these:

- devkitPro, with `devkitA64`, `libnx`, `switch-sdl2`, `switch-mesa` and
  `switch-libdrm_nouveau`. `configure.py` looks for it in `DEVKITPRO`, then
  in `/opt/devkitpro` and `~/devkitpro`. The option `--devkitpro` selects a
  different one. `DEVKITA64` should point at the compiler; it is found
  without it under `$DEVKITPRO/devkitA64`.
- The Android NDK, for the guest. See port/android/README.md.
- A network connection for the first build: `configure.py` downloads musl
  1.2.5 and SDL 3.4.16 to `build/android/third_party`, which both ports share.

To install the devkitPro packages:

```
sudo dkp-pacman -S --needed devkitA64 libnx switch-sdl2 switch-mesa switch-libdrm_nouveau
```

## Build and install

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja switch`.

That writes two files to `build/switch`:

- `halo.nro`, the program.
- `halo_guest.elf`, the game.

To install, put both on the SD card:

```
sdmc:/switch/halo/halo.nro
sdmc:/switch/halo/halo_guest.elf
```

and start `halo.nro` from the Homebrew Menu. The Homebrew Menu shows the log
while it runs.

The game's own view of the console is that of a 1280x720 handheld or a
1920x1080 docked display; `SDL_GetDesktopDisplayMode` reports which, and the
game is told the width to render at. It resizes on its own if the console is
docked or undocked while the game runs.

## Game data

The game needs the `maps/` folder out of an Xbox disc image (`.xiso` or
`.iso`) of any version of the game. The disc image itself is not included, and
neither is `maps/`.

Put the disc image on the card:

```
sdmc:/switch/halo/halo.iso
```

`halo.xiso`, `maps.iso` and `halo-xbox.iso` are also recognised, as is any
other `.iso` or `.bin` in the same folder. There is no file browser on the
console, so the name and the folder are fixed.

The game unpacks `maps/` itself on the first run. That is about 1.7 GB written
to the card and takes a few minutes, so leave the card in and make sure there
is that much free; the log says which file it is copying and how far it has
got. It reads the image the same way `extract-xiso` does, so all three disc
formats work.

Unpacking it yourself works too. If `maps/` is already there the game uses it
and never looks for an image:

```
sdmc:/switch/halo/maps/
```

The game checks for `sdmc:/switch/halo/maps/ui.map` and, with no image
either, refuses to start and says so. Saves go in `sdmc:/switch/halo/save/`,
and the settings file `config.toml` goes in `sdmc:/switch/halo/`.

Custom Edition maps go in `sdmc:/switch/halo/custom_maps/`, with
`bitmaps.map`, `sounds.map` and `loc.map` in that folder. A map in a
subfolder is listed under the folder's name: campaign maps under CUSTOM
SINGLEPLAYER, multiplayer maps under CUSTOM MULTIPLAYER. One level of
folders is read. A map you do not have is downloaded from the other players
while you wait in the lobby. Refer to the root
[README.md](../../README.md#custom-edition-maps).

## Gyro aiming

The stick aims as it does anywhere else. Open a profile, choose Gamepads, and
then Gyro Aim. That screen is the Switch's only. Gyro Aim is off until it is
set to ON, which aims with the gyroscope all the time, or ONLY ON ZOOM, which
aims with it only while you are zoomed in (a scope), for fine aiming; the
stick aims the rest of the time. Horizontal and vertical sensitivity are how far the view turns
each way (`1` is the sensor's own turn; the screen goes from `0.25` to `10`).
Invert horizontal and invert vertical are separate, and neither follows the
stick's Invert Look. The same five are `input.gyro_aim`,
`input.gyro_sensitivity_horizontal`, `input.gyro_sensitivity_vertical`,
`input.gyro_invert_horizontal` and `input.gyro_invert_vertical` in
`config.toml`. Each local player reads their own controller. A Pro Controller,
the console held in the hands, and a single Joy-Con each use their one sensor;
when two Joy-Con are attached, the right one is read. It is not used in the
menus. In a split-screen match on this console alone, the pause menu's LEAVE
GAME leaves the match for both players and returns to the main menu. Each
player's window is cleared only inside itself, so the second player's clear
does not wipe the first player's view.

## Performance

The console shows the game at 1280x720 in handheld and 1920x1080 docked.
Video Setup's RESOLUTION LIMIT chooses the lines the game draws at, which
are scaled up to that: 480P (the default, the game's own), 720P or 1080P,
no more than the console shows (1080P draws at 720 in handheld). Docking or
undocking switches at once. It is `display.resolution_limit` in
`config.toml` (`"480"`, `"720"` or `"1080"`).

Video Setup also has a PERFORMANCE screen, the Switch's only, with the CPU's
clock: 1.7 GHZ (the default, the fastest Nintendo's own games use), 1 GHZ
(the console's handheld rate: cooler, and the battery lasts longer) or
STOCK (left at the rate the console started the game with). It changes at
once. It is `performance.cpu_clock` in `config.toml` (`"1785"`, `"1020"`
or `"stock"`). The memory and the GPU stay at the rates the game sets for
itself, and every clock goes back to the console's when the game exits.

## The memory model

This is the part of the port that is not like the others, and it is worth
writing down because it shaped the host and because it is the one thing here
that was established by experiment rather than by reading a manual.

Everything the guest can address has to be below 4 GB, because the guest's
pointers are 32 bits. That is not a restriction the console imposes: it is
the same restriction the game's data formats impose, and it is why the
guest image is linked at `HALO_GUEST_IMAGE_BASE` (0x3c000000) with the Xbox
contiguous window at 0x80000000 (port/android/include/halo_android_abi.h).

The answers below were measured on a console running Horizon 21.2.0 with
Atmosphère 1.10.2, by the programs in `port/switch/probe`. They are:

- The address space below 4 GB is free. There is nothing in the way.
- `svcMapPhysicalMemory` (SVC 0x2C), which sounds like the call this needs,
  is refused on this firmware. `svcMapMemory` (SVC 0x05) instead places
  memory at an address the caller chooses.
- No mapping is ever both writable and executable.
  `svcSetMemoryPermission` answers 0xd801 for `Perm_X`, as its documentation
  says it must. Executable memory is a separate kind, made with
  `svcCreateCodeMemory` (0x4B) and `svcControlCodeMemory` (0x4C).
- A code memory object has a writable owner view and a read-execute slave
  view, and the two are one memory: bytes written through one are read
  through the other.

So the guest image cannot be given one writable-and-executable mapping, the
way the Android host gives it one and then `mprotect`s it. `host_mman.c`
splits the image by permission instead, placing each part at the offset the
guest was linked for:

- executable pages become a code memory slave at `HALO_GUEST_IMAGE_BASE` plus their
  offset, filled in through a writable view at a scratch address;
- `.data`, `.bss`, the guest's arenas and the Xbox window become ordinary
  `svcMapMemory` regions.

The guest is not changed by this. It still sees one contiguous range,
because the segments land at exactly the addresses it was linked for.

`mprotect` is correspondingly not cheap here: adding `PROT_EXEC` copies the
range into a code memory object, maps it, and attends to both caches, which
for a large segment is tens of milliseconds. The loader does this a few times
while loading the image and never again; the calls it makes during play are
between read and write, and those are a single syscall on a page.

## SDL2, and why there is a shim

The guest was built against SDL3's headers. There is no SDL3 for the Switch:
devkitPro's SDL has `switch-sdl-3.2` and `switch-sdl-3.4` branches, but no
packaged, working port, and the repository of Switch examples this port
started from uses `pkg-config sdl2` throughout.

So `host_sdl2.c` answers the guest's SDL3 calls with devkitPro's SDL2, which
is the same arrangement the Anbernic port uses for its own reasons. SDL
objects are 64-bit pointers that the guest cannot hold, so the guest gets
small integer handles into a table. Most of the values agree between the two
versions; `host_sdl3_events.c` translates the ones that do not.

Where the graphics come from is worth noting, because it decided the port
early. devkitPro's SDL2 is built with `--disable-filesystem` and links
`switch-mesa`: Mesa 20.1.0 over `libdrm_nouveau`, statically. SDL2's Switch
video driver puts an EGL surface on the console's default `nwindow`, so
`SDL_GL_CreateContext` and `SDL_GL_SwapWindow` — the two calls the renderer
makes — work as they do anywhere else. The port started out drawing that
way. The game now draws with deko3d, the console's native interface,
through its own renderer (`port/switch/guest/d3d8_dk.c` and
`port/switch/host/host_dk.c`; see [DEKO3D.md](DEKO3D.md)); Mesa remains
linked because SDL2 needs it.

## Internet play

`host_net.c` gives the game libnx's BSD sockets, through the same
translation to Winsock that `posix_net.c` does on the other builds, so
system link and internet play work as on Android. The desktop-only parts of
`posix_net.c` (URL schemes, Discord, UPnP) are answered as having nothing
to offer. Voice chat records from a headset in the console's jack
(`host_microphone.c`).

## What the port does not have

Three things in the Android host are absent here, all of them development
aids rather than part of the game, and all for the same reason: libnx
exposes no syscall for installing a signal handler.

- The crash handler in `host_memory.c`, which logged the faulting address,
  the program counter and the guest's frame chain.
- The thread sampler in `host_debug.c`, which `debug.sample_seconds` in
  `config.toml` starts. It is worth having; see each file for how it could be
  built on the console's thread context syscalls instead.
- `host_probe.c`, which walks the memory window looking for the pointers
  that would have to move if the window moved. Its question is already
  answered, and the answer is in port/android/README.md.

## The probes

`port/switch/probe` holds the programs the memory model above was measured
with. They are standalone devkitA64 projects, not part of the port's build,
and they are kept because the answers they gave are the reason the port is
shaped the way it is. Each writes its findings to `sdmc:/`, so they can be
read back off the card:

```
cd port/switch/probe/mem
make
```

`mem` is at its fifth version; the earlier ones found the same things less
directly, and the comments say what each was for.

### Cinematic letterbox

`display.cinematic_letterbox` defaults to `true`, preserving the original cinematic bars. Set it to `false` to reveal the scene behind the bars without zooming or changing the camera.

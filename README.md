<div align="center">

<img src="port/assets/icon/slipstream-logo-2048.png" width="220" alt="The SlipstreamCE logo: a ship flying past a ringworld" />

# SlipstreamCE

**Halo: Combat Evolved for Xbox, on Android, Switch and PS5, with modern renderers and performance in mind**

[![Latest release](https://img.shields.io/github/v/release/thelinkin3000/halo-ce-universal?label=latest&color=2ea44f)](https://github.com/thelinkin3000/halo-ce-universal/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/thelinkin3000/halo-ce-universal/total?color=blue)](https://github.com/thelinkin3000/halo-ce-universal/releases)
[![Android](https://img.shields.io/badge/Android-Vulkan%20%C2%B7%20Turnip%20%C2%B7%20OpenGL%20ES-3ddc84?logo=android&logoColor=white)](#android)
[![Nintendo Switch](https://img.shields.io/badge/Switch-deko3d-e60012)](#nintendo-switch)
[![PlayStation 5](https://img.shields.io/badge/PS5-Vulkan%20%C2%B7%20RADV-003791)](#playstation-5)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/9gqcHyr5km)

[**Download**](#download) ·
[Install](#install) ·
[Graphics](#graphics-vulkan-turnip-and-deko3d) ·
[Multiplayer](#multiplayer) ·
[Controls](#controls) ·
[Building](docs/building.md) ·
[Discord](https://discord.gg/9gqcHyr5km)

<!-- Videos and screenshots from a contributor go here: one large capture,
then a row of three, each taken on a phone, a Switch or a PS5. -->

</div>

It is not an emulator. The game's own code, decompiled from the Xbox build
2342, is compiled for the device's processor (ARM, or the PS5's x86-64).
Its Direct3D 8 rendering is translated to **Vulkan** on Android and the PS5
and to **deko3d**, the Switch's own GPU interface, on the Switch.

> [!TIP]
> **New: the SlipstreamCE overlay.** Hold **L + R** and press **d-pad down** (on
> Android without a controller, tap the screen with three fingers, or four as set in the touch controls' General options) for a
> panel of cheats (god mode, infinite ammo, super jump and more), bots, the
> game's speed, the HUD, the camera and the renderer's switches, without
> leaving the game, and entries of your own. No cheats in multiplayer unless
> you are the host: in a game someone else hosts, the overlay's cheats are
> refused. See [The SlipstreamCE overlay](#the-slipstreamce-overlay).

> [!IMPORTANT]
> **No game data is included.** You need your own **Xbox** disc image of
> Halo: Combat Evolved (`.iso` or `.xiso`, any region). The game unpacks
> its maps from it on the first start.

> [!WARNING]
> **Official sources.** The only official downloads are the
> [releases on this repository](https://github.com/thelinkin3000/halo-ce-universal/releases). Builds,
> "updates" or donation requests offered anywhere else under this project's
> name are not from us.

<div align="center">

[![Halo Combat Evolved - Nintendo Switch: watch on YouTube](https://img.youtube.com/vi/imNdnKszCVI/maxresdefault.jpg)](https://www.youtube.com/watch?v=imNdnKszCVI)

*Halo: Combat Evolved on the Nintendo Switch (video by puginatux).*

</div>

## Contents

- [What works](#what-works) · [Known issues](#known-issues) · [What's improved](#whats-improved)
- [Download](#download) · [Install](#install): [Android](#android), [Nintendo Switch](#nintendo-switch), [PlayStation 5](#playstation-5), [updating](#updating)
- [Graphics: Vulkan, Turnip and deko3d](#graphics-vulkan-turnip-and-deko3d)
- [Multiplayer](#multiplayer) · [Controls](#controls) · [The SlipstreamCE overlay](#the-slipstreamce-overlay)
- [Building](docs/building.md) · [Credits](#credits)

## What works

| | |
| --- | --- |
| 🎮 **Campaign** | The whole campaign, with checkpoints, saves and cinematics. Xbox maps of every region; PAL maps are made to play as NTSC ones. |
| 🖼️ **Vulkan on Android** | The default renderer. On Qualcomm Adreno GPUs the app can download and use **Turnip**, Mesa's open-source driver, in place of the phone's own. OpenGL ES 3 is still there as a fallback. |
| 🟥 **deko3d on the Switch** | A renderer written for the console's GPU, docked at 1080p or handheld at 720p, switching on its own when you dock. |
| 🟦 **The PS5** | A homebrew title for a jailbroken console: up to native 4K at 120 frames a second, aiming with the DualSense's gyroscope, voice chat through the pad's microphone. |
| 👥 **Split screen** | Up to four players on one device, with one controller each. |
| 🌐 **Multiplayer** | System link on the local network and internet play through invite links and the server browser, with voice chat. |
| 🗺️ **Custom maps** | Xbox maps and Halo PC / Custom Edition maps, downloaded from the host when you join without them. |
| ✋ **Touch** | On Android: touch controls in game with swipe aiming, optional gyro aiming, a layout editor; taps work in every menu. |
| 🔠 **High-res HUD and text** | The HUD, the menus' text and their titles redrawn at high resolution. |
| 🔄 **Self-updating** | Both builds look for a newer release when they start and offer to install it. |

### Known issues

- Bink movies don't play: the game skips them.
- **Android:** kernels with 16 KB memory pages (a developer option on
  Android 15, and some newer phones) can't run the game: the Xbox memory
  uses 4 KB pages. The app also needs Android 9 or later and a 64-bit
  ARM CPU.
- **Android, Vulkan:** the first time a scene shows, a few parts can be
  missing for a moment while the phone compiles their shaders.

## What's improved

### Performance

**Vulkan against OpenGL ES on Android**, in 48-bot matches on Blood Gulch and
Damnation:

| Phone | Measured | Vulkan | OpenGL ES |
| --- | --- | --- | --- |
| Galaxy S21 FE (Mali-G78, its own driver) | Average frame rate | 84-104 fps | 69-86 fps |
| Galaxy Z Fold (Adreno 840) | Slowest 1% of frames | 62.7 fps | 40.5 fps |

These come from separate benchmark runs during development, not one
side-by-side session, so treat them as a guide. The Z Fold's average on Vulkan
was 119.4 fps, at the limit of its 120 Hz screen.

**Multiplayer on the Switch:**

| Match | Players | Frame rate |
| --- | --- | --- |
| Blood Gulch | 64 | 60 fps |
| Damnation | 64 | about 60 fps |
| Chill Out | 64 | about 59 fps |
| LAN game hosted on the console | 48 | steady 60 fps |

Vulkan also records and presents its frames on a thread of its own, and now
runs on phones whose own driver lacks dynamic rendering, such as Samsung's Mali
phones and older Adreno phones.

### Theater

Record your network games and watch them again from **Theater**, below
Multiplayer in the main menu.

- **Views:** behind any player, through their eyes, a free camera, or a
  cinematic camera that follows the player by itself.
- **Playback:** a seek bar, scrubbing with the d-pad, and speeds from paused
  up to 8x.
- **Long matches:** a 22-minute match's replay plays on every platform.
- **Moving replays:** Android imports and exports them. On the Switch and the
  PS5, a page on your local network, opened by a QR code in the Theater, lets
  you download and upload them.

### Bots

- **Game modes:** Capture the Flag, King of the Hill, Infection and SWAT, as
  well as Slayer.
- **Vehicles:** they drive and ride Warthogs, Ghosts and Scorpions, and man
  turrets.
- **Tactics:** they go for health and power-ups, hold vantage points, use
  teleporters and ladders, and can spread over a map's side lanes.
- **Six skill levels,** from Recruit to Legendary, each fighting as a player of
  that level does.
- **Anywhere you play:** up to 63 in a local game, in system link and private
  internet games too, and added or dropped in the middle of a match.

Much of this comes from BirchWoodGod's
[halo-ce-vita](https://github.com/BirchWoodGod/halo-ce-vita).

### Ragdolls

Bodies can fall as ragdolls instead of playing the original death animations:
choose in Settings > Gameplay, or set `game.ragdoll` in `config.toml`. A host
keeps the original body physics, so it still plays with builds that have no
ragdolls.

## Download

Each release is a build of `main` we choose to publish, numbered by its tag.
These links always point to the latest one:

| Platform | Play | Debug |
| --- | --- | --- |
| Android | [slipstreamce-android-release.zip](https://github.com/thelinkin3000/halo-ce-universal/releases/latest/download/slipstreamce-android-release.zip) | [slipstreamce-android-debug.zip](https://github.com/thelinkin3000/halo-ce-universal/releases/latest/download/slipstreamce-android-debug.zip) |
| Nintendo Switch | [halo-switch-release.zip](https://github.com/thelinkin3000/halo-ce-universal/releases/latest/download/halo-switch-release.zip) | [halo-switch-debug.zip](https://github.com/thelinkin3000/halo-ce-universal/releases/latest/download/halo-switch-debug.zip) |
| PlayStation 5 | [halo-ps5-release.zip](https://github.com/thelinkin3000/halo-ce-universal/releases/latest/download/halo-ps5-release.zip) | [halo-ps5-debug.zip](https://github.com/thelinkin3000/halo-ce-universal/releases/latest/download/halo-ps5-debug.zip) |

Use the release build to play. The debug build stops at the first failed
assertion and writes it to the log, which makes it the one to use when
you're chasing a problem. The [releases page](https://github.com/thelinkin3000/halo-ce-universal/releases)
keeps the last five builds, if the latest one has a problem.

## Install

### Android

1. Unzip the download and install the APK.
2. Copy your Xbox disc image to the phone.
3. Start the app. On the first start it asks for the disc image: pick it,
   and it unpacks the maps (about 1.7 GB) into its own data folder.

The app needs Android 9 or later on 64-bit ARM. More, including where the
data folder is and how to keep your saves across a reinstall, is in
[port/android/README.md](port/android/README.md).

### Nintendo Switch

The Switch build is a homebrew program, for a console that runs custom
firmware (Atmosphère). Run it with **title takeover** (hold R while you
start a game, or use a forwarder), not from the album: the game needs the
memory that only a full title gets.

1. Unzip the download into `sdmc:/switch/halo/` on the SD card, so that
   you have `sdmc:/switch/halo/halo.nro` and `sdmc:/switch/halo/halo_guest.elf`.
2. Put your disc image next to them as `sdmc:/switch/halo/halo.iso`.
3. Start `halo.nro`. On the first start it unpacks the maps (about 1.7 GB;
   it takes a few minutes, and the screen shows how far it has got).

An already unpacked `maps/` folder at `sdmc:/switch/halo/maps/` works as
well. Saves go in `sdmc:/switch/halo/save/` and settings in
`sdmc:/switch/halo/config.toml`. See
[port/switch/README.md](port/switch/README.md).

### PlayStation 5

The PS5 build is a homebrew title, for a console on system software 13.60
or lower, jailbroken (the Relapse exploit, or another kernel exploit, then
kstuff), with ShadowMountPlus 1.7beta5 or later and an FTP server such as
ftpsrv.

1. Unzip the download on your computer, and copy its `PPSA99500` folder
   into `/data/homebrew` on the console over FTP.
2. Make the folder `/data/halo` and copy your disc image into it.
3. Start SlipstreamCE from the home screen. On the first start it
   unpacks the maps (under a minute); notifications show how far it has
   got.

Saves and settings go in `/data/halo`. See
[port/ps5/README.md](port/ps5/README.md).

### Updating

The game updates itself: when it starts, it looks for a newer release and
asks whether you want it. `update.auto = false` in `config.toml` turns the
check off. On the PS5 it downloads the new release while you play and
tells you to close the game and start it again. To update by hand, install
the new APK over the old one, or copy the new files over the old ones on
the SD card or the console. Your saves and settings stay.

## Graphics: Vulkan, Turnip and deko3d

**Android** draws with Vulkan by default. To change it, open **SETTINGS**
from the main menu, choose a profile, and open **VIDEO SETUP**:

- **GRAPHICS BACKEND:** `VULKAN` or `OPENGL` (OpenGL ES 3).
- **VULKAN DRIVER** (with `VULKAN`): `STOCK`, the phone's own driver, or any
  freedreno style driver for vulkan, importable from the game itself.

Select **OK**, then close the game and start it again: the backend and the
driver change only at start-up. The line below the version number in the
main menu shows which ones are running. For driver archives of your own and
what to send when something looks wrong, see "Graphics: OpenGL ES and
Vulkan" in [port/android/README.md](port/android/README.md).

**The Switch** draws with deko3d, the console's native graphics interface,
through a renderer written for it ([port/switch/DEKO3D.md](port/switch/DEKO3D.md)).
There is nothing to set.

**The PS5** draws with the same Vulkan renderer as Android, on RADV, Mesa's
driver for AMD's GPUs, built for the console by
[PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan). Its VIDEO SETUP has
MAXIMUM RESOLUTION (2160P, 1440P or 1080P). The first visit to each place
can stutter while its shaders are made; the second is smooth.

## Multiplayer

- **System link** on a local network, with up to 128 players. Android phones,
  Switches and PS5s on the same network see each other's games.
- **Internet play:** the host's invite link (Android puts it on the
  clipboard, and opens `halo://join/...` links), or the server browser.
  No server of this project is needed.
- **Voice chat**, with the phone's microphone, a headset in the Switch's
  jack, or the DualSense's microphone.
- **Split screen** for up to four players on one device.
- **Custom maps** are sent by the host to players who don't have them.

### Custom Edition maps

Put Halo PC / Custom Edition maps in `custom_maps`, beside `maps` (on the
Switch, `sdmc:/switch/halo/custom_maps/`), with Custom Edition's
`bitmaps.map`, `sounds.map` and `loc.map`. The level lists then have CUSTOM
SINGLEPLAYER and CUSTOM MULTIPLAYER. A subfolder of `custom_maps` is listed
under its own name and opens to its maps, so a set of maps (Cursed Halo,
say) can keep its own folder; the campaign maps in it show under CUSTOM
SINGLEPLAYER and the multiplayer maps under CUSTOM MULTIPLAYER. One level
of folders is read, and `bitmaps.map`, `sounds.map` and `loc.map` stay in
`custom_maps` itself. Joining a game on a map you don't have downloads it,
with its progress at the top of the screen.

On Android, IMPORT under the level lists (New Game's and the Map screen's)
copies maps in from the system's file picker: `.map` files, or `.zip` files
of them. A `.zip` with more than one map becomes a folder named like it;
`bitmaps.map`, `sounds.map` and `loc.map` always go to `custom_maps` itself.

<img width="640" alt="CUSTOM SINGLEPLAYER, listing folders of maps" src="docs/images/custom-singleplayer.jpg" />

<img width="640" alt="A folder of CUSTOM MULTIPLAYER opened to its maps" src="docs/images/custom-multiplayer.jpg" />

<img width="280" alt="custom_maps with bitmaps.map, sounds.map, loc.map and a folder for each set of maps" src="docs/images/custom-map-folders.png" />

<img width="640" alt="The lobby downloading a Custom Edition map" src="docs/images/map-download.jpg" />

The netcode is new: each machine moves its own player at once, and the host
makes the decisions for the game. See [port/linux/NETCODE.md](port/linux/NETCODE.md).
Keep the game in the front during a network game on Android: in the
background, Android stops it and the others drop it after 15 seconds.

## Controls

Any controller that Android knows works, and on the Switch the Joy-Con,
the Pro Controller and the Lite's own buttons, with rumble. The first
controller is player 1 and the others are players 2 to 4. The buttons are:

| Xbox | DualSense | Switch | In game |
| --- | --- | --- | --- |
| left / right stick | left / right stick | left / right stick | move / look |
| right trigger | R2 | ZR | fire |
| left trigger | L2 | ZL | throw a grenade |
| A | Cross | B (A in menus) | jump, accept |
| B | Circle | A (B in menus) | melee, back |
| X | Square | Y | action, reload |
| Y | Triangle | X | change weapon |
| white / black | L1 / R1 | L / R | flashlight / change grenade |
| stick clicks | L3 / R3 | left / right stick click | crouch / zoom |
| start / back | Options / touchpad click | + / − | pause / scoreboard |

On Android without a controller, touch controls show in game: a move stick,
swipe to look (with the controller's aim assist), and buttons that you can
move, resize, hide or copy from **Options** at the top of the screen.
In menus, tap an item to pick it and drag to
scroll. All of it is in "Controls" in
[port/android/README.md](port/android/README.md).

The gyroscope can aim as well, the phone's on Android and the controller's
on the Switch (each player's own): open a profile, **GAMEPADS**, then
**GYRO AIM**, to turn it on (ON, or ONLY ON ZOOM for fine aiming through a
scope) and set its sensitivity and inversion each way. See
[port/switch/README.md](port/switch/README.md#gyro-aiming).

<img width="640" alt="Controller setup, with Gyro Aim as its own row" src="docs/images/gyro-aim-row.jpg" />

<img width="640" alt="The Gyro Aim screen: aim, sensitivity and invert" src="docs/images/gyro-aim.jpg" />

The settings, which are the same on every platform, are described in
[docs/settings.md](docs/settings.md).

## The SlipstreamCE overlay

A panel over the left of the screen with cheats, bots and switches, in the
menus and in a game, on the Switch, the PS5 and Android.

### Opening it

| | Open and close |
| --- | --- |
| Switch | hold **L** and **R**, press **d-pad down** |
| PS5 | hold **L1** and **R1**, press **d-pad down** |
| Android, with a controller | hold **L1** and **R1** (LB and RB), press **d-pad down** |
| Android, touch only | tap the screen with **three fingers** together (or four: the touch controls' Options, General) |

The same buttons close it. While it is open the game gets none of your
input, but it is not paused.

| Control | Does |
| --- | --- |
| d-pad or left stick | move up and down the list (hold to repeat) |
| A (Cross) | choose: run a command, flip a toggle, open a folder |
| left / right | flip a toggle; otherwise right opens a folder, left goes back |
| B (Circle) | back out of a folder; at the top, close |
| Back / Start (Create / Options) | close |
| touch | tap a row to choose it, tap the folder's name to go back, tap outside the panel to close, drag to scroll |

A label too long for the panel scrolls while it is chosen, and some entries
show a note above the panel's foot (for example, "Applies at the next
restart"). What a command did shows at the foot for a few seconds.

Its folders: **Cheats**, **Bots** (add and drop bots, their difficulty,
vehicles and routes), **Game Speed**, **HUD**, **Camera** and **System**
(the frame rate, the renderer's switches and, on the Switch and the PS5,
Xbox button pictures). `game.overlay_menu = false` in `config.toml` stops the
buttons from opening it.

### Cheats and multiplayer

The overlay gives no way to cheat in another player's game. It runs the
same commands as the game's console, and in a game someone else hosts
(system link or internet) the game refuses every command that changes the
game: cheats, game speed, spawning, bots, the camera, wireframe, fog and
the other drawing switches. Cheats, a game speed and drawing switches
turned on before you joined are put back as soon as you join, and kept so
while you play. What still works changes only what you see and hear: the
HUD's parts, the frame rate, sound and your own settings. The host decides the game in any case (damage,
deaths, pickups and spawns), so nothing a player's machine does changes
anyone else's game.

The cheats work where the game is yours: the campaign, split screen, and
games you host.

### Adding your own entries

Your entries are `.toml` files in the `overlay` folder of the game's data
folder, the one with `maps/` and `config.toml` in it. The game makes the
folder the first time the overlay opens.

| Device | Folder |
| --- | --- |
| Switch | `sdmc:/switch/halo/overlay/` |
| PS5 | `/data/halo/overlay/` |
| Android | `/sdcard/Android/data/ar.com.slipstreamce/files/overlay/` (copy files with `adb push`) |

Each folder in it is a folder of the overlay, and each folder in one of
those a folder in that, by its name. A folder with the name of one of the
overlay's own (`Cheats`, `Bots`, `System`, in any case) adds its entries to
it. Each `.toml` file is one entry:

```
overlay/
  Cheats/
    infinite_grenades.toml   added to the overlay's Cheats
  Fun/                       a new folder, after the overlay's own
    big_heads.toml
    Weather/                 a folder in Fun
      rain.toml
```

There are three kinds of entry:

```toml
# a command, run when it is chosen
label   = "Spawn All Weapons"
type    = "command"
command = "cheat_all_weapons"
```

```toml
# a toggle, ON or OFF
label = "VSync"
type  = "toggle"
on    = "config display.vsync true"
off   = "config display.vsync false"
state = "config:display.vsync"
note  = "Takes effect at once"
```

```toml
# a choice: one of several in a folder, marked ON while it is the one
label   = "Half Speed"
type    = "choice"
command = "game_speed 0.5"
state   = "speed=0.5"
```

| Key | Means |
| --- | --- |
| `label` | the entry's name in the list |
| `type` | `command`, `toggle` or `choice` |
| `command` | a command's or a choice's: a console command, or `config <setting> <value>` to set and save a setting of `config.toml` |
| `on`, `off` | a toggle's commands, as `command` |
| `state` | where a toggle's ON or a choice's being chosen is read (below); a toggle without one shows what it was last set to |
| `note` | a line shown while the entry is chosen (optional) |

| `state` | ON while |
| --- | --- |
| `global:<name>` | the console global is true (`global:cheat_deathless_player`) |
| `global:<name>=<number>` | the console global has that value (`global:rasterizer_stats=5`) |
| `config:<setting>` | the setting is true, or a number not 0 (`config:display.vsync`) |
| `config:<setting>=<word>` | the setting has that value (`config:bots.routes=lanes`) |
| `speed=<number>` | the game runs at that speed (`speed=2`) |
| `remembered` | the overlay last turned it on |

A folder lists the overlay's own entries first, then its folders, then its
files' entries, each in order of name. The files are read again each time
the overlay opens after one has changed, so an entry copied over FTP shows
the next time you open it. A file that is not a whole entry shows in red as
**LABEL: MALFORMED ENTRY** (**MALFORMED ENTRY: NO LABEL** without a label),
and choosing it says what is wrong; `debug.txt` says so too. Entries from
files follow the same multiplayer rules as the overlay's own.

The overlay's first entries are those of hardran3's
[OpenCE-Ultrahand](https://github.com/hardran3/OpenCE-Ultrahand) and
HaloDrones packages for the Switch's Ultrahand Overlay.

## Building

See [docs/building.md](docs/building.md).

## Credits

- The decompilation: [punpckhdq/halo](https://github.com/punpckhdq/halo),
  continued in [bnunu/halo-1](https://github.com/bnunu/halo-1). It is of the
  Xbox build 2342 (`cachebeta.exe`, SHA-256
  `4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).
- [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), the upstream
  project, which these ports follow and share the game
  code, netcode and menus with.
- [Mesa](https://mesa3d.org/)'s Turnip driver, in the builds of
  [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers).
- [deko3d](https://github.com/devkitPro/deko3d), [libnx](https://github.com/switchbrew/libnx)
  and devkitPro, for the Switch.
- [SDL](https://www.libsdl.org/), on both platforms.
- [Kenney](https://kenney.nl/)'s [Input Prompts](https://kenney.nl/assets/input-prompts)
  (CC0), the controller buttons' pictures on the Switch and the PS5, and
  a replay's button prompts everywhere.
- AL2009man's [Gamepad Asset Pack](https://github.com/AL2009man/Gamepad-Asset-Pack)
  (MIT), the controllers of the button-layout overlay (Gamepad Setup's
  BUTTON LAYOUT).
- puginatux, for the menu assets and the logo.

Halo is a trademark of Microsoft. This project is not affiliated with or
endorsed by Microsoft, Bungie or 343 Industries.

## License

This fork includes OpenCE's code, which OpenCE moved to the GNU Affero
General Public License, version 3 or (at your option) any later version
(`AGPL-3.0-or-later`), in its commit `4b09168e7`. The full text is in
[LICENSE](LICENSE). The commits before that change came into this fork,
and the releases built from them (up to Build 106), were released under
[CC0 1.0](LICENSES/CC0-1.0.txt) and stay that way. Third-party code under
`port/third_party`, and the fonts and art under `port/assets`, keep their
own licenses.

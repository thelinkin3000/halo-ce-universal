# Android

`ninja android` builds the game for 64-bit ARM Android (arm64-v8a).
`ninja android_apk` makes an app from it:
`port/android/app/build/outputs/apk/debug/app-debug.apk`.

The game shows its graphics with Vulkan (the default) or OpenGL ES 3
(refer to "Graphics: OpenGL ES and Vulkan"). It plays sound through SDL3
(AAudio). It accepts input from game controllers, for example a PlayStation
5 DualSense on Bluetooth, and from the touchscreen (refer to "Controls").
The app needs Android 9 (API 28) or later. It operates on 64-bit-only
devices, for example the Pixel 9 Pro XL.

The Android build uses the platform layer of the Linux build
(`port/linux/src`). Refer to [port/linux/README.md](../linux/README.md).
Physical mice use the same control bindings. During play, F12 toggles mouse
capture; captured mode hides Android's pointer and uses relative movement for
continuous aiming, while releasing capture restores normal pointer behavior.

## Requirements

You do not need the Xbox SDK. You need the tools of the Linux build (Python,
ninja) and these items:

- A clang with the `arm64_32` target, for example the clang of the system.
  The option `--android-guest-cc` of `configure.py` selects a different
  compiler.
- The Android NDK. `configure.py` looks for it in `ANDROID_NDK_HOME`, then
  in `$ANDROID_HOME/ndk`, `~/Android/Sdk/ndk` and `/opt/android-sdk/ndk`.
  The option `--android-ndk` selects a different NDK.
- CMake, and a JDK 17 or later for Gradle.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5 (checked against its SHA-256) and SDL 3.4.16 to
  `build/android/third_party`. Gradle downloads the Android Gradle Plugin.

On Windows, build in WSL2 (Ubuntu 24.04). The Android build does not run
on Windows itself. Enter
`wsl -d Ubuntu -- bash tools/wsl_setup_android.sh` one time: it installs
the tools and the Android SDK and NDK in WSL (apt asks for your
password). Then enter
`wsl -d Ubuntu -- bash tools/wsl_build_android.sh` for each build. The
script copies the repository into WSL (`~/halo-build/src`) with LF line
endings (a Windows checkout has CRLF line endings, which the build's text
tools do not accept), builds there, and puts the APK in
`dist/android/app-debug.apk`. Only the changed files are copied, so the
next build is incremental. Install the APK with the adb of Windows.

A build that you make yourself has a different signature from the builds
of GitHub Actions. Android installs it only after you remove the app, and
removing the app deletes its data folder (`maps/`, `save/` and
`config.toml`). Thus a change between the builds of GitHub Actions and your
own builds loses the saved games, unless you copy them first (below).
Your own builds all have the same signature (the debug key of WSL,
`~/.android`), so they install over each other and keep the data
(`adb install -r`).

To keep the saved games before you remove the app, enter
`adb pull /sdcard/Android/data/com.halo.decomp/files/save/u` (the profiles).
After the new install, before the first start of the app, enter
`adb push u /sdcard/Android/data/com.halo.decomp/files/save/u`. The app reads
and overwrites what adb pushes. adb cannot add files to `save/u` or `save/z` once the app made them, or
change files that the app wrote, so push before the first start. adb cannot read
some files that the app writes (the `blam.lst` files of `save/z`), so `adb pull`
of the whole `save` folder stops there.

## Build and install the app

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja android_apk`.
4. Connect the device with adb.
5. Enter `adb install -r port/android/app/build/outputs/apk/debug/app-debug.apk`.

`ninja android` builds only the game image and the native libraries.

## Game data

The game needs the `maps/` folder from an Xbox disc image (`.xiso` or
`.iso`) of any version of the game. The app extracts `maps/` from the disc
image. The app keeps the data in `/sdcard/Android/data/com.halo.decomp/files`.

To install the data with the app:

1. Copy the disc image to the phone.
2. Start the app.
3. Push the button. The file picker of the system opens.
4. Select the disc image.
5. Wait while the app extracts the data (approximately 1.8 GB). Then the
   game starts.
6. You can delete the disc image.

To install the data from a computer:

1. Start the app one time. The app makes its folders.
2. Enter `adb push <folder>/. /sdcard/Android/data/com.halo.decomp/files/`.

| Item | Location in `/sdcard/Android/data/com.halo.decomp/files` |
| --- | --- |
| Saved games (`z:\` and `u:\`) | `save` |
| Mods (`docs/mods.md`) | `mods`: `adb push <mod folder> /sdcard/Android/data/com.halo.decomp/files/mods/`; the main menu's Mods orders them and turns them off |
| Log | `debug.txt`, the log of the game, and `halo.log`, the log of the app (`log_files = false` and `halo_log = false` in `[debug]` turn them off) |
| Settings | `config.toml` |

To make a copy of the saved games, enter
`adb pull /sdcard/Android/data/com.halo.decomp/files/save/u` (the profiles).
An `adb pull` of the whole `save` folder stops at the `blam.lst` files of
`save/z`, which adb cannot read.

## Controls

The game reads controllers through the gamepad functions of SDL3. All the
controllers that Android knows operate. The first controller is player 1.
The other controllers are players 2 to 4 (split screen). The buttons agree
with the positions on the Xbox controller:

| DualSense | Xbox | Function in the game |
| --- | --- | --- |
| left stick, right stick | left stick, right stick | move, look |
| R2 | right trigger | fire |
| L2 | left trigger | throw a grenade |
| Cross | A | jump, accept |
| Circle | B | melee, back |
| Square | X | action, reload |
| Triangle | Y | change the weapon |
| L1 | white | flashlight |
| R1 | black | change the grenade |
| L3, R3 | left and right stick clicks | crouch, zoom |
| D-pad | D-pad | |
| Options | start | pause menu |
| Create | back | |

The controller gets the rumble. The back gesture of Android is the B
button. A Bluetooth or USB keyboard operates as on Linux.

The touchscreen operates the menus: tap an item to select it (on a
setting with values, tap its left or right half), tap a button of the key
at the bottom of a screen (for example "B = Back") to push it, and drag to
scroll a list (down or right steps back, up or left steps forward). A drag
stops at the first and the last item, and it does not change a setting's
value. Touches that start in the edge-gesture zones of Android do not tap
or scroll at the sides, and do not scroll at the top and bottom, because the
first swipe from an edge in full screen only shows the system bars.

A tap during a cinematic that can be skipped skips it, as A does. On the
game's on-screen keyboard (a player profile's name), tap a key to press it,
"B =BACK" to cancel and "A =ENTER" to accept the name.

While a name is typed, on the game's on-screen keyboard or in a menu's text
field (Server Setup's server name and password, a game's password when
joining it), Android's keyboard comes up as well when the touch controls
would show (see "Touch controls"). Its letters, numbers, symbols (those of
ASCII only) and backspace type into the name, and its Enter accepts it, as a
keyboard's Enter does. A name is typed as a visible password to Android's
keyboard, so that it offers no suggestions and does not learn the names
typed. The back gesture closes Android's keyboard first; it comes back with
the next name begun, and the game's own keyboard takes taps meanwhile. A
Bluetooth or USB keyboard types into the names in the same way, with or
without Android's keyboard (`port/linux/src/sdl_platform.c`, "the system's
on-screen keyboard").

After a multiplayer game, the carnage report shows buttons in place of its
prompt at the bottom: the host's CONTINUE goes on to the next game (PICK
GAME), as A does, and LEAVE leaves the game, as B does (the host's LEAVE
ends it for everyone). A client has LEAVE only. The buttons follow the touch
controls (`input.touch_controls`): while a controller is connected, or with
`off`, the report shows the prompt as on the Xbox, and A and B act as before
(`port/linux/game/touch_postgame.c`).

### Touch controls

In a game, the app shows touch controls over the picture. They are a
controller for player 1. With the default buttons of the profile:

| Control | Function in the game |
| --- | --- |
| stick (lower left) | move |
| swipe on the screen away from the buttons | look |
| Fire | right trigger |
| Grenade | left trigger |
| A / Jump, B / Melee, X / Reload, Y / Weapon | A, B, X, Y |
| Crouch, Zoom | left and right stick clicks |
| Light, Gren. type | white, black |
| Pause, Back | start, back |
| Up, Down, Left, Right | D-pad (in a network game, Up opens the text chat's keyboard) |

The names on the buttons follow the profile's "Button layout" (Settings >
Gamepads): with "Swap triggers", the right trigger's button says "Grenade".
The stick moves the player with every "Stick layout", southpaw too.

A finger that holds a button can also swipe to look, so you can fire and
aim with one thumb ("Held buttons don't aim" in General turns this off).
The controls are a finger wide (48 dp) or larger, but
on a small screen of high density they stay apart rather than reach that
size. A short tap reaches the game even when it is shorter than one frame.
Swipes that start in the edge-gesture zones of Android do not turn the
view or place the floating stick, but a control drawn in one still presses
(Android keeps the zones while the system bars are hidden: with 3-button
navigation, all of where the bar was). The swipe aims as the controller's stick does, not as a mouse: the
aim slows over a target and follows a moving one, as with a controller,
and it follows the profile's "invert look" (the gyroscope turns the view
as the phone turns, never inverted). The setting `input.touch_aim_assist`
turns the aim assist off. "Look sensitivity" sets how far a swipe turns;
the mouse settings do not apply.

The touch controls show only in a game. In the menus and during
cinematics they hide, and the touchscreen operates the menus as described
above. They also hide when a controller is connected, for example the
built-in controller of a handheld. A device without a touchscreen (a TV)
never shows them. The setting `input.touch_controls` changes this (refer
to "Settings").

The buttons at the top of the screen:

- "Hide" removes the controls (and stops the gyroscope aiming and the
  vibration) until you push "Touch"; the app remembers it.

Aiming with the phone's gyroscope is in the game's settings, as on the
Switch: open a profile, **GAMEPADS**, then **GYRO AIM**. ON aims with it all
the time, ONLY ON ZOOM only while you are zoomed in (a scope), for fine
aiming; OFF (the default) is the swipe and the stick only. The same screen
sets its sensitivity and invert each way (`1` is the phone's own turn).
These are `input.gyro_aim` and the four after it, which each profile keeps in
the `settings.toml` in its folder (`config.toml`'s are those of a profile that
has not set them). The gyroscope aims while the touch controls show.
- "Options" opens these items:
  - "General": the phone's vibration (on by default; it follows the
    vibration setting of the game's profile), a floating move stick (off by default: the stick goes
    where your thumb lands in the lower left of the screen), the opacity of
    the controls, how many fingers tap together to open the cheat menu
    (three, the default, or four), "Held buttons don't aim" (off by
    default: a finger that swipes from a button only holds it),
    "Hide or add buttons" (hide a button, add a copy of a
    button) and "Edit buttons size".
  - "Edit buttons layout": drag the controls, and the "Options" button
    itself, to new positions, then push "Save" in the middle of the
    screen. "Export" and "Import", beside it, write and read a layout
    file with the file picker of the system.
  - "Look sensitivity".

"Hide" and "Options" act when the finger lifts on them: a swipe that
starts on them turns the view instead.

The app keeps the layout in its own preferences, not in `config.toml`.
Removing the app's data or the app removes the layout.

## Settings

The settings are in `config.toml` in the data folder of the app. To change
them:

1. Enter `adb pull /sdcard/Android/data/com.halo.decomp/files/config.toml`.
2. Change the file.
3. Enter `adb push config.toml /sdcard/Android/data/com.halo.decomp/files/`.

At the first start, the game writes the file with the default values. To
get the default values again, delete the file.

The settings are the settings of Linux, without the window, the mouse and
the paths. Refer to [port/linux/README.md](../linux/README.md#settings).
Some video settings also apply on Android, with these notes; the first three
have rows of their own in Video Setup:

- `display.resolution_limit` (`MAXIMUM RESOLUTION` in the menus) is the
  most lines the game draws at, no more than the screen has: `"native"`, or
  `"2160"`, `"1440"`, `"1080"`, `"720"` or `"480"`. More are sharper, but more
  work for the GPU.
- `display.max_fps` (`FRAME RATE LIMIT`) limits the frame rate while
  `display.vsync` is off: `0` (the default) is twice the display's refresh
  rate, `-1` no limit, or the most frames each second.
- `display.show_fps` (`SHOW FRAMES`) draws the frame rate at the bottom right
  of a game, which is the quickest way to see what a device manages; the
  same setting on the other ports (`port/linux/README.md`).

These settings are only for Android:

| Setting | Function |
| --- | --- |
| `input.touch_aim_assist` | `true` (the default): the touch controls' swipe aiming gets the aim assist of a controller (the aim slows over a target and follows a moving one). `false`: none, as with a mouse; the bullets' own autoaim stays. |
| `input.touch_controls` | The touch controls in a game. `"auto"` (the default): shown on a touchscreen while no controller is connected. `"on"`: also shown with a controller. `"off"`: never shown. A device without a touchscreen never shows them. The carnage report's CONTINUE and LEAVE buttons follow the same rule (otherwise it shows its A and B prompt). The menus take taps in any case. |
| `display.screen_width` | The number of columns of the 480-line picture. `0` (the default): the shape of the display (1068 on a 20:9 phone). `640`: the 4:3 shape of the Xbox. |
| `display.stats_log` | `false` (the default): `true` writes a line a second to the log with the frame rate, the mode, the map, the renderer and the size of the 3D view, for a bug report or to compare two builds. |
| `debug.sample_seconds` | Refer to "Find problems". |
| `debug.memory_watch` | `true` (the default): the app notices the game's writes to textures and vertices by page protection. `false`: it compares page contents once a frame instead, which is slower. Under OpenGL ES, geometry is streamed in either mode. Refer to "Limits". |
| `debug.profile_hz` | Refer to "Find problems". |
| `debug.profile_run` | `true`: each start records a profile of the run into its own folder, `Download/halo_profiles/DDMMYYYY_HHMMSS` (the day and time it started), to send as it is. Refer to "Find problems". `false` (the default): none. Takes effect the next time the game starts. |
| `display.renderer` | How the game draws: `"vulkan"` (the default; OpenGL ES if Vulkan cannot start, which sets it to `"gl"`) or `"gl"` (OpenGL ES over the phone's driver). Takes effect the next time the game starts. The line below the version number in the main menu shows the renderer that runs. VIDEO SETUP's GRAPHICS BACKEND sets it. Refer to "Graphics: OpenGL ES and Vulkan". |
| `display.vk_driver` | The Vulkan driver of the Vulkan renderer (and of the Vulkan probe). Empty (the default): the phone's own. Otherwise the file name of a driver archive in `freedreno_drivers` in the data folder (or in the data folder itself): the Turnip build the app downloads on an Adreno (refer to "Turn on Turnip"), or one that you add. VIDEO SETUP's VULKAN DRIVER sets `""` (STOCK) or the name of an archive in `freedreno_drivers`. `"auto"`, which was the downloaded Turnip build, is the phone's own driver now: the game writes `""` over it. |
| `debug.vk_probe` | Refer to "Find problems". |
| `debug.vk_validation` | Refer to "Find problems". |
| `debug.vk_present_marker` | Under the Vulkan renderer, draws a red square at the top left and a green one at the top right of the picture, so that a screenshot shows which way up it is. Default `false`. |
| `debug.vk_self_test` | Under the Vulkan renderer, at start-up tries the renderer's clears and the copying of a draw's data (rewritten buffers, data larger than the command stream) on small targets of its own; the log says `clears self-test: ok` and `data self-test: id N ok`, or `FAILED`. Default `false`. |

## Graphics: OpenGL ES and Vulkan

The game can draw in two ways:

| Renderer | `display.renderer` | Driver (`display.vk_driver`) |
| --- | --- | --- |
| Vulkan (the default) | `"vulkan"` | `""` (the default): the phone's own Vulkan driver. Or the file name of a driver archive: the Turnip build the app downloads on an Adreno GPU (refer to "Turn on Turnip"), or one that you add. |
| OpenGL ES | `"gl"` | The phone's own. |

If Vulkan cannot start on a phone, the game uses OpenGL ES.

### Change the renderer in the game

1. In the main menu, open **SETTINGS**, choose a profile, and open
   **VIDEO SETUP**.
2. At the top of the screen:
   - **GRAPHICS BACKEND:** `VULKAN` or `OPENGL` (OpenGL ES).
   - **VULKAN DRIVER:** `STOCK` (the phone's own driver), then every driver
     archive in `freedreno_drivers` by its file name: the Turnip build the
     app downloads (refer to "Turn on Turnip") and any that you add. This
     row, and **IMPORT DRIVER**, show only when the backend is `VULKAN`.
3. Select **OK**. The menu writes `display.renderer` and
   `display.vk_driver` in `config.toml`.
4. Close the game and start it again. The renderer and the driver change
   only when the game starts, as the note below the rows says.

**DEFAULTS** sets `VULKAN` and `STOCK`. An archive named in `config.toml`
that is not in `freedreno_drivers` (one in the data folder itself) is not
listed: with one set, the menu shows `STOCK`, and the archive stays set
unless you choose another driver and select **OK**.

On some phones the phone's own driver draws the game with errors: shapes
that stretch across the screen, textures that are missing or wrong. The
Vulkan renderer lets the game use another driver. On phones with a
Qualcomm Adreno GPU, that is Turnip, the open-source Vulkan driver for
Adreno from the Mesa project. Turnip is off until you choose it.

To see which renderer and driver run, look at the main menu: the line
below the game's version number (`01.01.14.2342`, at the lower right) says
`OpenGL ES`, `Vulkan`, or `Vulkan` and the name of the driver. Refer to
"Make sure that it operates".

### Find your GPU

Look up your phone's GPU (in the phone's specifications, or with an app
such as CPU-Z or AIDA64). The log also names it (`driver: the GPU is ...`,
refer to "Make sure that it operates").

| GPU | Turnip build | Notes |
| --- | --- | --- |
| Adreno 6xx or 7xx (for example Adreno 650, 730, 750) | `Turnip_v26.0.0_R8.zip` | The build that we tested (Adreno 750). |
| Adreno 8xx (for example Adreno 830) | `Turnip_v26.0.0_R8_A8xx.zip` | The same release's build for the A8xx series. We did not test it. |
| Mali, Immortalis, PowerVR, Xclipse, other | None | Turnip does not operate on these GPUs. Use Vulkan on the phone's driver, or OpenGL ES. |

### Get to the settings file

The settings file (`config.toml`) and the driver archives are in the data
folder: `/sdcard/Android/data/com.halo.decomp/files`. Start the game one
time first; the game then writes `config.toml`.

Since Android 11, most file manager apps on the phone cannot open
`Android/data`. Use a computer:

- With a USB cable: set the phone to "File transfer", and open
  `Internal storage/Android/data/com.halo.decomp/files` on the computer.
  Copy `config.toml` to the computer, change it with a text editor, and
  copy it back.
- With adb: `adb pull /sdcard/Android/data/com.halo.decomp/files/config.toml`,
  change the file, then
  `adb push config.toml /sdcard/Android/data/com.halo.decomp/files/`.

Close the game before you change `config.toml`: the game writes the file
when it starts and when you change a setting in its menus. Change only the
value after `=`, and keep the quotes.

### Turn on Turnip (Adreno only)

1. Start the app one time. On an Adreno 6xx, 7xx or 8xx, the app shows
   "Downloading the Vulkan driver" before the game starts: it downloads
   the Turnip build for the GPU's series into `freedreno_drivers` in the
   data folder (approximately 2.5 to 3.5 MB, from
   [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases),
   release `v26.0.0-rc08`; the app keeps the file only if it is exactly
   the build that we chose). It does this only one time. If the download
   fails or you push "Skip", the app tries again at the next start.
   The download does not turn Turnip on.
2. Set **VULKAN DRIVER** to the downloaded build, listed by its file name
   without `.zip` (for example `Turnip_v26.0.0_R8`; refer to "Change the
   renderer in the game"), select **OK**, close the game and start it
   again. Then go to step 5. Or, to change `config.toml` instead, close
   the game.
3. Open `config.toml` (refer to "Get to the settings file"). Find the
   `[display]` section. It has these lines, with comments above them:

   ```toml
   [display]
   ...
   vk_driver = ""
   ...
   renderer = "vulkan"
   ```

4. Change `vk_driver = ""` to the build's file name, for example:

   ```toml
   vk_driver = "Turnip_v26.0.0_R8.zip"
   ```

   (`Turnip_v26.0.0_R8_A8xx.zip` on an Adreno 8xx: the name is the file's
   in `freedreno_drivers`.)

   Make sure that `renderer = "vulkan"`. Save the file (and copy it back
   to the phone, if you changed it on a computer).
5. Start the game. The first start with Turnip takes a moment more: the
   app unpacks the driver into its private storage.
6. Look at the main menu. Below the version number, it says `Vulkan` and,
   on the next line, the name of the driver, for example
   `Mesa Turnip driver v26.0.0 - R8`. If it says only `Vulkan`, Turnip is
   not on: refer to "Make sure that it operates".

The app writes the name of the build it chose for this phone in
`vk_driver_auto.txt` in the data folder. `vk_driver = "auto"`, which chose
that build before, now means the phone's own driver: the game writes
`vk_driver = ""` over it the next time it starts.

To turn Turnip off, set **VULKAN DRIVER** to `STOCK` (or
`vk_driver = ""` again).

### Try another Turnip build

1. Download a Turnip archive for your GPU. These sites release them (they
   are made by other people, not by this project):

   | Source | Notes |
   | --- | --- |
   | [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases) | The source of the builds that the app downloads. |
   | [StevenMXZ/Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases) | Automatic builds of current Mesa. Variants for A6xx/A7xx and for A8xx. |
   | [whitebelyash/freedreno_turnip-CI](https://github.com/whitebelyash/freedreno_turnip-CI/releases) | Automatic builds of current Mesa. |
   | [The412Banner/Banners-Turnip](https://github.com/The412Banner/Banners-Turnip/releases) | Automatic builds of current Mesa. |

   Take the `.zip` file for your GPU's series (A6xx/A7xx, or A8xx). Do not
   unzip it. The archive must be an "AdrenoTools" archive: a zip with a
   `meta.json` file and the driver library (`.so`) at its top level. Some
   of these sites also have Qualcomm's own drivers in this format; they
   load in the same way.
2. In **VIDEO SETUP**, with **GRAPHICS BACKEND** on `VULKAN`, select
   **IMPORT DRIVER** and choose the `.zip` file (several at once if you
   like). The app copies each one into the `freedreno_drivers` folder of
   the data folder, once it has checked that it is a driver archive (a
   `meta.json` naming a library that is in the zip, 256 MB at most); a
   name the folder has already is left as it is. Leave Video Setup and open
   it again to see it listed. Or copy the `.zip` file into that folder
   yourself (`/sdcard/Android/data/com.halo.decomp/files/freedreno_drivers/`;
   the app makes the folder when it looks for its own download).
3. In **VIDEO SETUP**, set **VULKAN DRIVER** to the archive: every `.zip`
   in `freedreno_drivers` is listed after `STOCK`, by its file
   name without `.zip` (a long name shows its start and end). Select
   **OK**. Or set `vk_driver` to the exact file name of the archive, for
   example `vk_driver = "Turnip_v26.0.0_R7.zip"`, and
   `renderer = "vulkan"`.
4. Start the game again, and look at the main menu.

You can keep more than one archive in the folder. An archive in the data
folder itself, next to `config.toml`, still loads when `vk_driver` names
it, but the menu does not list it.

### Choose another renderer or driver

In **VIDEO SETUP**, or in `config.toml`, in the `[display]` section:

| You want | In VIDEO SETUP | In `config.toml` |
| --- | --- | --- |
| Vulkan on the phone's own driver (the default) | `VULKAN`, `STOCK` | `renderer = "vulkan"` and `vk_driver = ""` |
| Vulkan on Turnip (Adreno) | `VULKAN`, the downloaded build's name (for example `Turnip_v26.0.0_R8`) | `renderer = "vulkan"` and `vk_driver = "Turnip_v26.0.0_R8.zip"` |
| Vulkan on a driver archive that you add | `VULKAN`, the archive's name | `renderer = "vulkan"` and `vk_driver = "<file name>.zip"` |
| OpenGL ES | `OPENGL` | `renderer = "gl"` |

Either way, the change applies the next time the game starts. If Vulkan
does not start (the phone's Vulkan cannot run the renderer, or the driver
chosen cannot be loaded), the game runs on OpenGL ES and sets `renderer =
"gl"`, so that VIDEO SETUP shows `OPENGL`; choose `VULKAN` there to try
again at the next start.

When the app is updated to the version that makes Vulkan the default, it
changes `renderer = "gl"` in an existing `config.toml` to `"vulkan"` one
time (the old default was `"gl"`). After that, a `"gl"` that you set stays.

### Make sure that it operates

The main menu shows what the game uses, in orange, below the version
number (`01.01.14.2342`) at the lower right:

| The main menu shows | Meaning |
| --- | --- |
| `Vulkan` and a driver name (for example `Mesa Turnip driver v26.0.0 - R8`) | The Vulkan renderer on that driver. |
| `Vulkan` | The Vulkan renderer on the phone's own driver. With an archive's name in `vk_driver`: the name is wrong, the file is not a driver archive, or the archive's driver did not load (the log says why). |
| `OpenGL ES` | The OpenGL ES renderer. With `renderer = "vulkan"`: Vulkan could not start, and the game used OpenGL ES. The phone's Vulkan cannot run the renderer, or the archive's driver was accepted but Android loaded the phone's driver in its place. |

The reasons are in the log of the app: connect the phone to a computer and
enter `adb logcat -s halo`. Look for the lines that start with
`renderer:`, `vk driver:` and `driver:` (the download).

### Go back to the defaults

Select **DEFAULTS** and then **OK** in **VIDEO SETUP** (this resets every
row of that screen), or set `renderer = "vulkan"` and `vk_driver = ""`, or
delete `config.toml` to get all the defaults again. Then start the game
again.

### What to expect from Vulkan

- The first time that a scene shows, a few parts of it can be missing for
  a moment, while the phone prepares their shaders. The app keeps them, so
  the next time is faster.
- The Vulkan renderer is new. Compare it with OpenGL ES on the same
  scenes: the menus, the first campaign map, split screen, water, the
  sniper rifle's zoom, lens flares, decals and fog.

### What to send in a report

- The phone model, the GPU and the Android version.
- For each renderer and driver that you tried: the line from the main menu,
  what looked wrong, and screenshots of the same scene.
- `debug.txt` from the data folder.
- `halo.log` from the data folder: the lines of `adb logcat -s halo`,
  without a computer. If **Write halo.log** in the overlay's System folder
  (`halo_log` in the `[debug]` section of `config.toml`) is off, turn it
  on, then start the game again and play until the problem.
- If you can use adb: the output of `adb logcat -s halo` during the run.
- If you want to help more: set `vk_probe = "all"` in the `[debug]` section
  of `config.toml` and start the app. The app tests the Vulkan driver
  instead of starting the game, writes `vk_probe.txt` in the data folder
  and closes. Send that file, then set `vk_probe = ""` again. The last
  step shows a colour that changes and waits for a controller: push A if
  the picture is right, Y if it is upside down, B to end.

## Internet play

Internet play operates as on Linux, but without Discord. When the game
hosts a system link game, it puts the invite link on the clipboard and
shows a notice.

To join a game, do one of these steps:

- Open the link. The app is the handler of `halo://join/...` links. If the
  game does not operate, the app starts it. The app writes the link to
  `files/join_link.txt`, and the game reads it.
- Copy the link and go to the game.

On the local network:

- The game uses the address of the Wi-Fi (or of the hotspot of the
  phone), not the address of the mobile data.
- The app holds a Wi-Fi multicast lock while the game operates. Some
  phones otherwise drop the broadcasts that find system link games.

Keep the game in the front during a network game. When the app goes to the
background, Android stops the game. After 15 seconds the other machines
drop it, and when it hosts, its players leave.

## Updates

The app from GitHub Actions can update itself, as on Linux (refer to
"Updates" in [port/linux/README.md](../linux/README.md#updates)). When you
select "Yes":

1. The app downloads the new version.
2. The package installer of Android opens. At the first update, Android asks
   you to let Halo install apps. Allow it.
3. Select "Update". Android replaces the app.
4. Select "Open" to start the new version.

To install over the previous version, each build must have the same
signature. GitHub Actions signs each build with the key in the
`ANDROID_KEYSTORE_BASE64` and `ANDROID_KEYSTORE_PASSWORD` secrets of the
repository. If you installed a build that has a different signature, remove
that build before you install a new build. Removing the app deletes its data
folder: first make a copy of `maps/` and `save/u`.

## Widescreen

The game shows 480 lines in the shape of the display, not the 640x480 of
the Xbox:

- The 3D view is wider. The camera keeps the vertical field of view.
- The HUD stays at the edges of the screen.
- The menus, the loading bar and the screens after a game have 640
  columns, at the center of the screen.
- Black bars and fades cover all of the screen, and so do the menus' dims
  and backgrounds (the pause menu's dim, dialogs, the menus' gradient).

All the ports draw this way.

## How the port operates

### ILP32 code

The data of the game contains 32-bit pointers. The cache files have the
layout of the Xbox memory. The saved games are copies of the memory.
Direct3D resources contain 32-bit physical addresses. Current Android
devices cannot execute 32-bit ARM code. 64-bit pointers change the layout of
the structures that the game reads from its files.

Thus the game is ILP32 AArch64 code: 64-bit ARM instructions with 32-bit
`int`, `long` and pointers, in a 64-bit app.

### The guest image

The guest is the game, the platform layer and a small runtime:

1. clang compiles the guest for `arm64_32-apple-watchos`, the only ILP32
   AArch64 target of clang. The options `-U__APPLE__` and
   `-fno-define-target-os-macros` hide the Darwin environment.
2. `tools/android_asm_convert.py` changes the Mach-O assembly to ELF
   assembly. It renames symbols, but keeps string literals (`.ascii`,
   `.asciz`) byte for byte: they hold tables as well as text
   (`tools/test_android_asm_convert.py`).
3. The AArch64 assembler makes the objects.
4. `ld.lld` links the objects with `guest/guest.ld` to a static image,
   `build/android/halo_guest.elf`, at a fixed address above the Xbox memory
   (`include/halo_android_abi.h`).

The APK contains the image as an asset.

`ARM64_32` in `tools/guest_arch.py` holds what in these steps is particular
to the processor: the target and its options, the defines, the conversion
of the assembly, the linker's emulation, the image's address, the
builtins, the instructions of the import stubs and the calling convention
of the host for the OpenGL stubs. The Android build and the desktop guest
builds (`tools/linux_guest_build.py`) take it; the 64-bit x86 Linux build
takes `X32` instead ("64-bit x86" in [port/linux/README.md](../linux/README.md#64-bit-x86)).
`tools/guest_image_check.py` checks each linked image: no thread-local
storage, no use of the thread pointer, nothing outside its range.

The C library of the guest is a part of musl for a new `arm64_32`
architecture (`guest/libc/arch/arm64_32`; `x32` for the x86-64 guest). It
has ILP32 types and a 32-bit `time_t`, as in the MSVC runtime of the game.
Its system calls go to the host (`guest/libc/arch/generic/syscall_arch.h`). `guest/runtime/guest_thread.c` makes the threads
and supplies the thread pointer and TLS.

The system calls have an interface of their own, the same for every guest
architecture (`include/halo_guest_syscall.h`, and for musl
`guest/libc/arch/generic`): AArch64's numbers, kernel `stat` and open
flags, with `_llseek` in place of `lseek` so that a 64-bit file offset
fits the guest's 32-bit result. The host translates each call to its own
kernel's (`host/host_syscall.c`), so the same guest runs on an AArch64 or
an x86-64 host. `tools/test_host_syscall.py` tests the translation on the
build machine.

### The host library

`libmain.so` is an arm64 NDK library. The activity of SDL3 starts it
(`app/.../HaloActivity.java`). The host library:

- Reserves the address space of the guest below 4 GB: the Xbox memory at
  `0x80000000`, the image, and pools for the memory of the guest
  (`host/host_memory.c`). Refer to "The fixed addresses".
- Loads the image and fills its import table (`host/host_loader.c`). The
  image must be a static executable of the host's guest (a 64-bit AArch64
  ELF file of `arm64_32` code), whose segments, header and import table lie
  in the file and in the image's reserved range; the loader checks each
  before it uses it.
- Starts the `main` of the game and each guest thread on a stack in guest
  memory, because ILP32 code keeps stack addresses in 32-bit registers
  (`host/host_thread.c`).
- Gives the audio callback of SDL to a thread with a guest stack
  (`host/host_sdl.c`).
- Does the calls of the guest: system calls (`host/host_syscall.c`), SDL
  (`host/host_sdl.c`), OpenGL ES (`host/host_gl.c`), and the file, socket,
  UPnP and DTLS functions of `port/linux/src/posix_*.c` (DTLS with Mbed TLS,
  for internet play with browsers: `posix_dtls.c`).

The guest calls the host through stubs (`tools/android_imports.py`, with the
instructions of `import_stub` in `tools/guest_arch.py`). The two ABIs use
the same registers for 32-bit integers, floats and pointers.
`host_imports.list` lists every guest's imports, `android_imports.list` the
app's own (its folders, gesture edges and touch controls).
`tools/check_host_imports.py` checks the lists against their declarations,
and that every argument has a register on each host (on x86-64, six for
integers and pointers: a later one may be an integer, never a pointer).
`tools/android_gl_stubs.py` makes the OpenGL ES stubs from
`port/linux/src/gl.h`. `tools/android_posix_stubs.py` makes the stubs of the
`posix_*` functions, which copy the `errno` of the host.

### The fixed addresses

The guest needs the Xbox memory at `0x80000000` to `0x88000000` and the
image above it, to `0x8c000000`. The cache files contain pointers to these
addresses. The Java runtime of Android (ART) also reserves its spaces below
4 GB. On some devices, for example handhelds with a large Java heap (the
AYN Thor, the Retroid Pocket), ART's large object space covers
`0x80000000`. ART fills that space from its bottom, so the part at
`0x80000000` is usually empty.

Thus:

1. The game operates in a process of its own (`:game`), with a fresh Java
   heap. The launcher, the import of a disc image and an earlier game do
   not leave objects there.
2. At the start of that process, `HaloApplication` loads `libmain.so`. Its
   `JNI_OnLoad` reserves the fixed addresses before the Java side
   allocates large objects.
3. If ART's large object space is in the way, the host takes back only the
   part that covers the fixed addresses, and only if no page of it is in
   use (`/proc/self/pagemap`, or `mincore` and the swap of that part if
   the device refuses `pagemap`). The host never takes the other spaces of
   ART.

If the addresses are not available, the game shows a message, and writes
the mappings below 4 GB to `memory_map.txt` in the data folder and to the
log.

To test the reclaim on any device, set a system property before you start
the game. The app then puts a stand-in for ART's large object space over
the fixed addresses:

- `adb shell setprop debug.halo.art_overlap idle`: the stand-in is empty
  at `0x80000000`. The log shows `reclaimed idle ART range`, and the game
  starts.
- `adb shell setprop debug.halo.art_overlap busy`: a page at `0x80100000`
  is in use. The game shows the message and writes `memory_map.txt`.
- Add `-nopagemap` to either value (`idle-nopagemap`) to test as on a
  device that refuses `/proc/self/pagemap`. Add `-swapped`
  (`idle-nopagemap-swapped`) to also move the objects of the stand-in to
  swap; with `busy`, the page in the fixed addresses too.
- `adb shell setprop debug.halo.art_overlap ""`: normal operation.

### OpenGL ES

The renderer (`port/linux/src/d3d8_gl.c`) uses OpenGL ES 3.0, and some
functions of OpenGL ES 3.2 if they are available:

- The vertex shaders flip y and change the depth range from 0..1. The front
  face winding is inverted.
- BGRA textures go to the GPU as RGBA with a swizzle. If the driver has no
  S3TC (Mali GPUs), the CPU decodes the DXT textures.
- The pixel shaders apply the LOD bias of the sampler.
- The upload changes the byte order of `D3DCOLOR` vertex attributes.
- Dynamic vertex and index data goes into a ring of three buffers, one for
  each frame. On Mali, other methods used too much memory.
- On OpenGL ES 3.2, indexed draws use a base vertex. Before 3.2, the CPU
  changes the indices.
- On OpenGL ES 3.1 and later, the visibility tests (lens flares) count
  samples with an atomic counter, as the NV2A did. OpenGL ES 3.0 tells only
  if a sample is visible. The GPU copies the counters at the end of each
  frame, and the CPU reads the copy two frames later, when the frame's fence
  has passed: a result is the latest count the GPU has finished, as with
  the query buffer of desktop OpenGL. A read of the counters themselves
  waits for the GPU, which halved the frame rate on Turnip (Zink).

### Calling conventions

Some files of the game declare a function differently from its definition,
or call a function without a prototype. On 32-bit x86, this has no effect.
The guest ABI passes floating-point arguments in their own registers and
variadic arguments on the stack. Thus such a call gives incorrect values.

`tools/android_abi_check.py` compares each declaration with its definition
in the LLVM IR. The problems are repaired:

- `hs.c` declared the red component of the script fades as `long`.
- Some files call `error`, `console_printf` or `terminal_printf` without a
  prototype. `include/halo_android_variadic_prototypes.h` gives the
  prototypes.

`guest/runtime/guest_misc.c` supplies the Darwin library functions that the
target calls (`__sincos_stret`, `__exp10f`). The guest compiles without
floating-point contraction, as on x86.

### Game source changes

The port's code tells these things apart, each its own macro, which the
Android build defines all of (`tools/android_build.py`):

- `HALO_GUEST`: the game is an ILP32 guest in a 64-bit process (its
  memory, its host imports, the SDL it reaches). The instruction set is the
  compiler's own macro, `__aarch64__`;
- `HALO_GUEST_MACHO`: the guest is compiled for clang's Darwin target
  (`arm64_32`), whose Mach-O objects are converted afterwards;
- `HALO_GLES`: the OpenGL ES renderer;
- `HALO_ANDROID`: the app (its display, input, files and lifecycle).

`HALO_ARM64_GUEST`, the name of `HALO_GUEST` before, is still defined:
`HALO_GUEST` on AArch64.

The x86 inline assembly is replaced by C (refer to
[port/linux/README.md](../linux/README.md#game-source-changes)).
These changes, which the Linux arm64 build shares, are:

- Seven `#pragma bss_seg(".bss")` lines are removed (`#ifndef
  HALO_GUEST_MACHO`). The Darwin target does not accept them.
- A stack walker follows the 64-bit frame records (`__aarch64__`, and
  `__x86_64__`'s, which are alike). Thus the log of an assertion
  (`debug.txt`) shows the call sites.

The musl of the guest uses its C math, not the AArch64 assembly. The only
assembly of the port is necessary:

- The import stubs. A 32-bit guest cannot keep or go to a 64-bit host
  address.
- The symbol aliases in `guest/libc/src_include/features.h`. The Darwin
  target does not accept alias attributes.

## Geometry uploads

Android selects its rendering fixes once at startup from the chip identity:

| Chip | Geometry fixes | Custom Edition texture fixes |
| --- | --- | --- |
| Qualcomm Snapdragon | On | On |
| MediaTek | Off | On |
| Other identified vendors, including Google Tensor and Samsung Exynos | Off | Off |
| Unknown or conflicting identity | On | On |

The host reads `ro.soc.manufacturer` and `ro.soc.model`, falling back to
`ro.hardware` and `ro.board.platform` on older devices. Phone brand is not
used: Samsung phones, for example, can have different chip vendors. Mali
and PowerVR GPU names alone do not identify a chip vendor. Unrecognized
identifiers retain both fixes. The policy is selected before the game
creates resources, stays fixed for the process, and is logged at startup.
No console command, configuration change or device permission is needed.

With geometry fixes enabled, Android bypasses the persistent geometry
mirror and its cached index extents and uses the upload paths below.
Affected players reported that bypassing the cache resolved stretched or
missing geometry, with some also reporting better performance. The exact
cache failure mechanism remains unconfirmed.

With geometry fixes disabled, Android uses the normal persistent mirror
and fixed stream buffers, without registering static-cache entries or
allocating per-draw pooled buffers. Desktop and web retain their existing
geometry cache and upload paths. These geometry
changes preserve shaders, texture caching and CPU write tracking.
Xbox and Custom Edition maps keep their existing loaders and resource
paths. `tools/test_android_mirror_uploads.py` checks the bypass with large
and reused ranges, frame-counter rollover and desktop/web cache controls;
it does not substitute for device gameplay validation. The chip and policy
matrix is covered by `tools/test_android_render_policy.py`.

Streamed vertices and indices go into pooled buffers, a buffer for each
draw that no queued draw reads, written with `glBufferSubData` and reused
only after the ring's frame fence (`web_chunk_get`, as the web build does):
unsynchronized writes into the frame's one stream buffer drew some
geometry as long spikes on Snapdragon. Geometry the game writes once (the
models and BSPs of Xbox, Custom Edition and mod maps) gets GL buffers of
its own, filled at its first draw and released when it unloads; a range
whose storage GL cannot allocate is streamed instead.
`tools/test_android_static_geometry.py` checks both with fake GL: no upload
writes a buffer a queued draw reads, and the static ranges' reuse, unload
and reload, address reuse and allocation failure.

## Find problems

- Enter `adb logcat -s halo` to see the log of the port and the errors of
  the game. `files/debug.txt` is the log of the game, and
  `files/halo.log` has logcat's lines too, begun again each start
  (`debug.halo_log`, the overlay's **Write halo.log**, from the next start).
- If the guest code stops, the log shows the registers and the frame chain
  (`host/host_arch.c`), from a stack of the signal handler's own, so a
  stack overflow is reported too.
  To find the functions, enter
  `llvm-symbolizer --obj=build/android/halo_guest.elf <address>`.
- Set `sample_seconds = <seconds>` in `[debug]` of `config.toml`. The log
  then shows the program counter and the frame chain of each guest thread
  at this interval. This finds hangs on devices without root access.
- Set `gl_debug = true` in `[debug]` of `config.toml`. The log then shows
  the OpenGL ES errors.
- The log says which write tracking the app uses: "page protection"
  (devices) or "page hashes" (the emulator, or `debug.memory_watch = false`).
- Set `profile_hz = 1000` in `[debug]` of `config.toml` to profile. Every
  game thread is interrupted 1000 times a second, and every ten seconds the
  app appends the samples (the program counter, the link register and the
  return addresses of the frame chain) to `profile.bin` in the data folder,
  with the CPU time of each thread. Pull the file, and enter
  `tools/android_profile_report.py profile.bin --logcat <log>`. It prints,
  for each thread, the share of the samples in the game, the OpenGL ES
  renderer, the boundary between the guest and the host, the OpenGL ES
  driver and everything else, and the milliseconds of CPU for each frame.
  `--from` and `--to` select seconds of the run. Set `gpu_stats = true` too,
  for the frames per second.
- Set `profile_run = true` in `[debug]` of `config.toml` to profile a
  player's run without adb. Each start then makes a folder,
  `Download/halo_profiles/DDMMYYYY_HHMMSS`, that a file manager or a USB
  cable reaches, and the game writes into it:
  - `profile.bin`: the samples of `profile_hz` (500 a second if it is `0`),
    and the length of each frame shown;
  - `gpu.txt`: under Vulkan, every 300 frames, the GPU's time for each
    rendering, by what it draws into (its size, colour and depth) and by
    the part of the frame it began in (the sky, the objects, the HUD...),
    the time between renderings (copies and barriers), the renderings a
    frame and the time between presents. A phone's GPU gives every
    timestamp inside a rendering the same time, so the parts within one
    are not told apart;
  - `halo.log`: the log of the run, as logcat has it;
  - `run.txt` and `config.toml`: the build, the phone, its GPU and driver,
    and the settings.

  Android 9 and 10 ask once for access to the storage; without it, the
  folder is `files/halo_profiles/` in the data folder. The player sends
  the folder, and `tools/android_profile_report.py <folder>` prints the
  same report as for `profile.bin`, with the slowest frames and where each
  thread was during each. It needs the game image of the same build:
  `--apk` takes it from that build's APK (a release's, from its zip), or
  `--elf` names it (`halo_guest_vk.elf`, or `halo_guest.elf` for OpenGL ES). The
  profile costs some speed: compare runs with it on.
- Set `vk_probe = "all"` in `[debug]` of `config.toml` to run the Vulkan
  probe (port/android/VULKAN.md, phase 0) instead of the game. The value
  can be a list of `caps`, `memory`, `compile`, `pipelines`, `draw` and `present`.
  The probe writes `vk_probe.txt` in the data folder and ends the app. The
  `present` step waits for a person: it shows a colour that changes, and it
  accepts the buttons A (the picture is right), Y (it is upside down), X
  (the way to clear) and B (end). A step that stops the app is left out of
  the next run (`vk_probe_skip.txt`; delete the file to try it again).
  `vk_validation = true` turns on the Vulkan validation layer, which is in
  the app only if `python configure.py --android-vulkan-validation` made it.
- To run the probe on another Vulkan driver (Turnip, for an Adreno GPU), copy
  an adrenotools archive (a zip with a `meta.json` and the driver's library,
  for example `Turnip_v26.0.0_R8.zip` from the K11MCH1/AdrenoToolsDrivers
  releases) into `freedreno_drivers` in the data folder and set
  `display.vk_driver = "<the file's name>"`. The app unpacks it into its private storage (Android loads a library
  only from there) and opens it with libadrenotools; the report's `driver.*`
  lines say which driver is running (`driver.check` says if the archive's
  library was really loaded). An archive that does not work is logged
  (tag `halo`, and `hook_impl` for the loader's reason) and the phone's driver
  is used. The app is packaged with its libraries extracted for this.
- To install a debug build beside the app from GitHub Actions (another
  signing key stops it from replacing that app), set
  `HALO_APPLICATION_ID_SUFFIX=.vk` when you run `ninja android_apk`. The
  package is then `com.halo.decomp.vk`, with a data folder of its own, which
  needs a copy of `maps/`.

## Limits

- Bink video is not available. The game skips the movies.
- The device must let the app reserve the fixed guest addresses, from
  `0x80000000` to `0x8c000000`. If ART uses them, the app shows a message
  (refer to "The fixed addresses").
- Touch operates the menus and skips cinematics. In the game, the touch
  controls appear when no controller is connected; their size follows the
  height of the screen (larger on a tablet than on a phone), never smaller
  than a finger.
- Kernels with 16 KB pages (a developer option of Android 15) do not
  operate. The Xbox memory uses 4 KB pages.
- The x86 Android emulator runs the app through its ARM translation. The
  translation cannot deliver the page faults that the renderer uses to
  notice changed textures. On the emulator the app compares
  page contents once a frame instead ("write tracking: page hashes" in the
  log). This is slower, and texture changes may appear one frame later.
  `debug.memory_watch = false` does the same on a device. The translation
  also has no scalar conversion from floating
  point to an integer in a SIMD register (`fcvtzs s0, s1`, an illegal
  instruction there), so `tools/android_asm_convert.py` writes each as the
  vector form, which leaves the register and the exception flags the same.

## Custom Edition texture storage

When the chip policy enables texture fixes, cached Custom Edition textures
on Android use stable individual allocations,
created on demand with a 128 MiB pixel limit and 8,192-entry limit, outside the
fixed Xbox memory window. This avoids exhausting the old arena with textures
needed in the same frame and avoids re-uploading surviving textures after
compaction. Current-frame entries cannot be evicted. Normal GPU caching,
texture conversion and HUD replacements remain. With texture fixes disabled,
Custom Edition uses the established Xbox texture arena and normal GPU cache.
The blanket GL shadow-state refresh before every draw is disabled on all
Android devices; resource deletion and context changes still invalidate state
where necessary. Desktop, web and other map storage paths retain their
established behavior. Fullscreen behavior is unchanged.

Cleanup releases bindings and GPU copies before pixel/header reuse. Map unload,
cache flush and mod reload release the stored pixels. Diagnostics are limited to
an unload summary and the first allocation failure per cache lifetime in the
normal platform log. No test log files, per-frame reports, texture fingerprints
or uniform readbacks run in the permanent build. The cache can still exhaust
its limit or system memory; normal fallback handling remains available.

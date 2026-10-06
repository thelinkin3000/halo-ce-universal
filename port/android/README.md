# Android

`ninja android` builds the game for 64-bit ARM Android (arm64-v8a).
`ninja android_apk` makes an app from it:
`port/android/app/build/outputs/apk/debug/app-debug.apk`.

The game shows its graphics with OpenGL ES 3. It plays sound through SDL3
(AAudio). It accepts input from game controllers, for example a PlayStation
5 DualSense on Bluetooth. The app needs Android 9 (API 28) or later. It
operates on 64-bit-only devices, for example the Pixel 9 Pro XL.

The Android build uses the platform layer of the Linux build
(`port/linux/src`). Refer to [port/linux/README.md](../linux/README.md).

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
  1.2.5 and SDL 3.4.16 to `build/android/third_party`. Gradle downloads the
  Android Gradle Plugin.

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
| Log | `debug.txt` |
| Settings | `config.toml` |

To make a copy of the saved games, enter
`adb pull /sdcard/Android/data/com.halo.decomp/files/save`.

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
button. A Bluetooth or USB keyboard operates as on Linux. The screen does
not accept touch input.

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
These settings are only for Android:

| Setting | Function |
| --- | --- |
| `display.screen_width` | The number of columns of the 480-line picture. `0` (the default): the shape of the display (1068 on a 20:9 phone). `640`: the 4:3 shape of the Xbox. |
| `debug.sample_seconds` | Refer to "Find problems". |
| `debug.profile_hz` | Refer to "Find problems". |
| `display.renderer` | How the game draws: `"vulkan"` (the default; OpenGL ES if Vulkan cannot start) or `"gl"` (OpenGL ES over the phone's driver). Takes effect the next time the game starts. The line below the version number in the main menu shows the renderer that runs. VIDEO SETUP's GRAPHICS BACKEND sets it. Refer to "Graphics: OpenGL ES and Vulkan". |
| `display.vk_driver` | The Vulkan driver of the Vulkan renderer (and of the Vulkan probe). Empty (the default): the phone's own. `"auto"`: Turnip on an Adreno, which the app downloads (refer to "Turn on Turnip"). Otherwise the file name of a driver archive in the data folder. VIDEO SETUP's VULKAN DRIVER sets `""` (STOCK) or `"auto"` (TURNIP). |
| `debug.vk_probe` | Refer to "Find problems". |
| `debug.vk_validation` | Refer to "Find problems". |
| `debug.vk_present_marker` | Under the Vulkan renderer, draws a red square at the top left and a green one at the top right of the picture, so that a screenshot shows which way up it is. Default `false`. |
| `debug.vk_self_test` | Under the Vulkan renderer, at start-up tries the renderer's clears and the copying of a draw's data (rewritten buffers, data larger than the command stream) on small targets of its own; the log says `clears self-test: ok` and `data self-test: id N ok`, or `FAILED`. Default `false`. |

## Graphics: OpenGL ES and Vulkan

The game can draw in two ways:

| Renderer | `display.renderer` | Driver (`display.vk_driver`) |
| --- | --- | --- |
| Vulkan (the default) | `"vulkan"` | `""` (the default): the phone's own Vulkan driver. `"auto"`: Turnip on an Adreno GPU (refer to "Turn on Turnip"). Or the file name of a driver archive that you add. |
| OpenGL ES | `"gl"` | The phone's own. |

If Vulkan cannot start on a phone, the game uses OpenGL ES.

### Change the renderer in the game

1. In the main menu, open **SETTINGS**, choose a profile, and open
   **VIDEO SETUP**.
2. At the top of the screen:
   - **GRAPHICS BACKEND:** `VULKAN` or `OPENGL` (OpenGL ES).
   - **VULKAN DRIVER:** `STOCK` (the phone's own driver) or `TURNIP`
     (refer to "Turn on Turnip"). This row shows only when the backend is
     `VULKAN`.
3. Select **OK**. The menu writes `display.renderer` and
   `display.vk_driver` in `config.toml`.
4. Close the game and start it again. The renderer and the driver change
   only when the game starts, as the note below the rows says.

**DEFAULTS** sets `VULKAN` and `STOCK`. A driver archive of your own (refer
to "Try another Turnip build") is set in `config.toml` only: with one set,
the menu shows `STOCK`, and the archive stays set unless you choose
another driver and select **OK**.

On some phones the phone's own driver draws the game with errors: shapes
that stretch across the screen, textures that are missing or wrong. The
Vulkan renderer lets the game use another driver. On phones with a
Qualcomm Adreno GPU, that is Turnip, the open-source Vulkan driver for
Adreno from the Mesa project. Turnip is off until you turn it on.

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
   the Turnip build for the GPU's series into the data folder
   (approximately 2.5 to 3.5 MB, from
   [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases),
   release `v26.0.0-rc08`; the app keeps the file only if it is exactly
   the build that we chose). It does this only one time. If the download
   fails or you push "Skip", the app tries again at the next start.
   The download does not turn Turnip on.
2. Set **VULKAN DRIVER** to `TURNIP` (refer to "Change the renderer in
   the game"), select **OK**, close the game and start it again. Then go
   to step 5. Or, to change `config.toml` instead, close the game.
3. Open `config.toml` (refer to "Get to the settings file"). Find the
   `[display]` section. It has these lines, with comments above them:

   ```toml
   [display]
   ...
   vk_driver = ""
   ...
   renderer = "vulkan"
   ```

4. Change `vk_driver = ""` to:

   ```toml
   vk_driver = "auto"
   ```

   Make sure that `renderer = "vulkan"`. Save the file (and copy it back
   to the phone, if you changed it on a computer).
5. Start the game. The first start with Turnip takes a moment more: the
   app unpacks the driver into its private storage.
6. Look at the main menu. Below the version number, it says `Vulkan` and,
   on the next line, the name of the driver, for example
   `Mesa Turnip driver v26.0.0 - R8`. If it says only `Vulkan`, Turnip is
   not on: refer to "Make sure that it operates".

`"auto"` uses the build that the app downloaded for this phone (the app
writes its name in `vk_driver_auto.txt` in the data folder). Instead of
`"auto"`, you can write the file name of the archive, for example
`vk_driver = "Turnip_v26.0.0_R8.zip"`.

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
2. Copy the `.zip` file into the data folder, next to `config.toml`.
3. Set `vk_driver` to the exact file name of the archive, for example
   `vk_driver = "Turnip_v26.0.0_R7.zip"`, and `renderer = "vulkan"`.
4. Start the game, and look at the main menu.

You can keep more than one archive in the folder.

### Choose another renderer or driver

In **VIDEO SETUP**, or in `config.toml`, in the `[display]` section:

| You want | In VIDEO SETUP | In `config.toml` |
| --- | --- | --- |
| Vulkan on the phone's own driver (the default) | `VULKAN`, `STOCK` | `renderer = "vulkan"` and `vk_driver = ""` |
| Vulkan on Turnip (Adreno) | `VULKAN`, `TURNIP` | `renderer = "vulkan"` and `vk_driver = "auto"` |
| Vulkan on a driver archive that you add | (not in the menu) | `renderer = "vulkan"` and `vk_driver = "<file name>.zip"` |
| OpenGL ES | `OPENGL` | `renderer = "gl"` |

Either way, the change applies the next time the game starts.

When the app is updated to the version that makes Vulkan the default, it
changes `renderer = "gl"` in an existing `config.toml` to `"vulkan"` one
time (the old default was `"gl"`). After that, a `"gl"` that you set stays.

### Make sure that it operates

The main menu shows what the game uses, in orange, below the version
number (`01.01.14.2342`) at the lower right:

| The main menu shows | Meaning |
| --- | --- |
| `Vulkan` and a driver name (for example `Mesa Turnip driver v26.0.0 - R8`) | The Vulkan renderer on that driver. |
| `Vulkan` | The Vulkan renderer on the phone's own driver. With `vk_driver = "auto"`: the download did not finish yet, or there is no Turnip build for this GPU. With an archive's name: the name is wrong, or the file is not a driver archive. |
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
folder: first make a copy of `maps/` and `save/`.

## Widescreen

The game shows 480 lines in the shape of the display, not the 640x480 of
the Xbox:

- The 3D view is wider. The camera keeps the vertical field of view.
- The HUD stays at the edges of the screen.
- The menus, the loading bar and the screens after a game have 640
  columns, at the center of the screen.
- Black bars and fades cover all of the screen, and so do the menus' dims
  and backgrounds (the pause menu's dim, dialogs, the menus' gradient).

The changes are in `#ifdef HALO_ANDROID` in `rasterizer_xbox.c`, `render.c`,
`ui_widget.c`, `cinematics.c`, `main.c` and
`rasterizer_xbox_screen_effect.c`.

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
   assembly.
3. The AArch64 assembler makes the objects.
4. `ld.lld` links the objects with `guest/guest.ld` to a static image,
   `build/android/halo_guest.elf`, at a fixed address above the Xbox memory
   (`include/halo_android_abi.h`).

The APK contains the image as an asset.

The C library of the guest is a part of musl for a new `arm64_32`
architecture (`guest/libc/arch/arm64_32`). It has ILP32 types and a 32-bit
`time_t`, as in the MSVC runtime of the game. Its system calls go to the
host (`syscall_arch.h`). `guest/runtime/guest_thread.c` makes the threads
and supplies the thread pointer and TLS.

### The host library

`libmain.so` is an arm64 NDK library. The activity of SDL3 starts it
(`app/.../HaloActivity.java`). The host library:

- Reserves the address space of the guest below 4 GB: the Xbox memory at
  `0x80000000`, the image, and pools for the memory of the guest
  (`host/host_memory.c`).
- Loads the image and fills its import table (`host/host_loader.c`).
- Starts the `main` of the game and each guest thread on a stack in guest
  memory, because ILP32 code keeps stack addresses in 32-bit registers
  (`host/host_thread.c`).
- Gives the audio callback of SDL to a thread with a guest stack
  (`host/host_sdl.c`).
- Does the calls of the guest: system calls (`host/host_syscall.c`), SDL
  (`host/host_sdl.c`), OpenGL ES (`host/host_gl.c`), and the file and socket
  functions of `port/linux/src/posix_*.c`.

The guest calls the host through stubs (`tools/android_imports.py`). The
two ABIs use the same registers for 32-bit integers, floats and pointers.
`tools/android_gl_stubs.py` makes the OpenGL ES stubs from
`port/linux/src/gl.h`. `tools/android_posix_stubs.py` makes the stubs of the
`posix_*` functions, which copy the `errno` of the host.

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
  if a sample is visible.

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

The x86 inline assembly is replaced by C (refer to
[port/linux/README.md](../linux/README.md#game-source-changes)).
These changes are in `#ifdef HALO_ANDROID`:

- Seven `#pragma bss_seg(".bss")` lines are removed. The Darwin target does
  not accept them.
- A stack walker follows the AArch64 frame records. Thus the log of an
  assertion (`debug.txt`) shows the call sites.

The musl of the guest uses its C math, not the AArch64 assembly. The only
assembly of the port is necessary:

- The import stubs. A 32-bit guest cannot keep or go to a 64-bit host
  address.
- The symbol aliases in `guest/libc/src_include/features.h`. The Darwin
  target does not accept alias attributes.

## Find problems

- Enter `adb logcat -s halo` to see the log of the port and the errors of
  the game. `files/debug.txt` is the log of the game.
- If the guest code stops, the log shows the registers and the frame chain.
  To find the functions, enter
  `llvm-symbolizer --obj=build/android/halo_guest.elf <address>`.
- Set `sample_seconds = <seconds>` in `[debug]` of `config.toml`. The log
  then shows the program counter and the frame chain of each guest thread
  at this interval. This finds hangs on devices without root access.
- Set `gl_debug = true` in `[debug]` of `config.toml`. The log then shows
  the OpenGL ES errors.
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
  releases) into the data folder and set `display.vk_driver = "<the file's
  name>"`. The app unpacks it into its private storage (Android loads a library
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
  `0x80000000` to approximately `0x89000000`. If the addresses are not
  available, the app shows a message.
- The game does not accept touch input. Use a controller or a keyboard.
- Kernels with 16 KB pages (a developer option of Android 15) do not
  operate. The Xbox memory uses 4 KB pages.

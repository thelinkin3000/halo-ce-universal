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

Alternatively, **Descargar paquete de datos** on the first screen or in touch
settings downloads a player's HTTPS URL directly on the phone. It accepts an
Xbox ISO/XISO or a ZIP containing one `maps/` folder (Xbox map format 5, at most
2 GiB of extracted data). It validates and stages the maps before installation;
existing maps, saves and configuration are not replaced. No game data or preset
commercial download URL is included.

Public MediaFire file pages are also accepted. The importer resolves their
download button over HTTPS, without executing scripts or following ads. Pages
that require a browser check or do not expose a public file offer **Abrir
MediaFire** instead. Finish the download in your browser, then return and choose
the downloaded ISO with **Elegir imagen de disco**. An HTML response is never
installed as game data; existing Xbox-map validation applies. This does not add
support for PC game data or compressed 7z/RAR imports.

The separately signed `.controls` edition also has **Actualizar APK** in both
places. Its channel is `dorlanpabon/halo-ce-universal`, release tags `controles-N`
with a `halo-controls.apk` asset and version code `10600 + N`. Draft and
prerelease releases are ignored. Before opening Android's installer it checks
the downloaded package ID, newer version and current signing certificates. The
standard edition retains its existing updater.

To install the data from a computer:

1. Start the app one time. The app makes its folders.
2. Enter `adb push <folder>/. /sdcard/Android/data/com.halo.decomp/files/`.

| Item | Location in `/sdcard/Android/data/com.halo.decomp/files` |
| --- | --- |
| Saved games (`z:\` and `u:\`) | `save` |
| Mods (`docs/mods.md`) | `mods`: `adb push <mod folder> /sdcard/Android/data/com.halo.decomp/files/mods/`; the main menu's Mods orders them and turns them off |
| Log | `debug.txt` |
| Settings | `config.toml` |

To make a copy of the saved games, enter
`adb pull /sdcard/Android/data/com.halo.decomp/files/save/u` (the profiles).
An `adb pull` of the whole `save` folder stops at the `blam.lst` files of
`save/z`, which adb cannot read.

## Controls

The touch buttons stay inside the stable system-bar, gesture and display-cutout
margins on all four edges. Hide and Settings have a separate top row. These
display adjustments do not overwrite the saved HUD positions. Mobile presets
keep their earlier arrangement, with an aligned right column, an aligned bottom
row and slightly smaller Fire targets. Secondary buttons share a 34-unit radius;
Fire uses 48 and left Fire 44 on the 960 x 540 layout grid. Unchanged earlier
presets are upgraded; custom HUD positions, visibility and manually changed
button sizes are preserved. Game Icons silhouettes
replace the small line drawings; A / SALTO, B / GOLPE, X / USAR and Y / ARMA
match the profile's actual button bindings without moving the controls.
The icons' CC BY 3.0 attribution is in `assets/notices/game-icons.txt` in the APK.
Touch Pause and Back open the Android pause dialog, including in demo maps
without pause-menu tags. Reanudar or Atrás resumes; Ajustes de controles opens
the touch options. Physical gamepad Start and Back keep the game's bindings.

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
on-screen keyboard, tap a key to press it, "B =BACK" to cancel and
"A =ENTER" to accept the name.

### Touch controls

The launcher's "Jugar / Continuar" stays available without game data and opens
the data-download form instead of starting the engine. Installed data keeps
the usual Play / Continue flow; demo download and local ISO import remain available.

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

The action icons follow the profile's "Button layout" (Settings >
Gamepads): with "Swap triggers", the right trigger's button shows a grenade.
The stick moves the player with every "Stick layout", southpaw too.

The touch button mapped to Grenade throws when its finger lifts; holding
and dragging only aims. Release sends an 80 ms controller pulse, while
cancellation or focus loss clears it. This follows remapped buttons and
does not change physical gamepads.

A finger that holds a button can also swipe to look, so you can fire and
aim with one thumb. The controls are a finger wide (48 dp) or larger, but
on a small screen of high density they stay apart rather than reach that
size. A short tap reaches the game even when it is shorter than one frame.
Swipes that start in the edge-gesture zones of Android do not turn the
view. The swipe aims as the controller's stick does, not as a mouse: the
aim slows over a target and follows a moving one, as with a controller,
and it follows the profile's "invert look" (the gyroscope turns the view
as the phone turns, never inverted). The setting `input.touch_aim_assist`
turns the aim assist off. "Sensibilidad al mirar" sets how far a swipe turns;
the mouse settings do not apply. Its independent horizontal/vertical swipe
inversion switches are saved on the device (off by default) and do not affect
the gyroscope. Vertical swipe inversion combines with the active game profile's
Invert Look / Invert Flight settings; the preview shows the touch adjustment.

"Configuración interna de Halo" in Pause or the touch settings opens the
original game menu. Tap Settings to edit the profile; Gamepads contains Invert
Look and Invert Flight. Native menu taps pass through unchanged.
The same launcher action opens the main menu on a cold start so a profile
can be selected or created. It temporarily suppresses the startup map command,
then restores init.txt exactly after the engine's acknowledgement. It never
replaces named saves and refuses to restart an already running game.
Pause/Back also offers "Salir sin guardar": confirming exits the game without
creating or replacing a save. Cancelling leaves the game paused.

The touch controls show only in a game. In the menus and during
cinematics they hide, and the touchscreen operates the menus as described
above. They also hide when a controller is connected, for example the
built-in controller of a handheld. A device without a touchscreen (a TV)
never shows them. The setting `input.touch_controls` changes this (refer
to "Settings").

The buttons at the top of the screen:

- "Ocultar" removes the controls (and stops gyroscope aiming and
  vibration) until you push "Mostrar"; the app remembers it.

Aiming with the phone's gyroscope is in the game's settings, as on the
Switch: open a profile, **GAMEPADS**, then **GYRO AIM**. ON aims with it all
the time, ONLY ON ZOOM only while you are zoomed in (a scope), for fine
aiming; OFF (the default) is the swipe and the stick only. The same screen
sets its sensitivity and invert each way (`1` is the phone's own turn).
These are `input.gyro_aim` and the four after it in `config.toml`. The
gyroscope aims while the touch controls show.
- "Ajustes" stays available during loading and cinematics and opens these items:
  - "Controles predefinidos": two thumbs, three fingers or the original Ports layout.
    Three fingers includes a second Fire button above the movement stick; two
    thumbs keeps it hidden. General > Disparo izquierdo toggles it without
    replacing the HUD; its visibility can also be changed in the HUD editor.
    The mobile presets use a floating stick, 60% opacity and no vibration.
    Hold the movement stick straight forward for 0.6 seconds to lock forward
    movement, then lift the finger; the lock indicator stays visible. Touch the
    stick again to resume manual movement. Pausing, opening settings, hiding the
    HUD, leaving the app or a cancelled touch clears the lock. General > Avance
    automático disables or enables this gesture; the choice is saved on the phone.
  - "Perfiles gráficos": five presets. Supremo ligero draws 480 x 360 pixels
    internally (640 x 480 logical, 4:3), disables shadows and extra effects,
    and caps presentation at 30 FPS. Ligero panorámico draws 801 x 360
    (1068 x 480 logical), also capped at 30. Both use OpenGL ES and interpolation.
    Equilibrado and Alta calidad retain 1068 x 480, try Vulkan with OpenGL ES
    fallback, and restore render scale 1.0 with no explicit frame cap.
    Fluidez experimental uses OpenGL ES at 320 x 240 (640 x 480 logical),
    the cuts of Supremo, interpolation and a 60 FPS cap; actual FPS vary by
    device and scene. Review the settings, then apply and restart; progress
    since the last checkpoint is lost. The first use backs up config.toml
    and init.txt as .before-profiles files.
  - "Pantalla y resolución": choose the full phone width, 16:9 or classic 4:3,
    independently of the internal 480p, 360p or 240p detail. Build 107's
    `display.resolution_limit` also offers "Nativa", the phone's own height;
    existing higher limits are shown and preserved. Explicit 480p, 360p or
    240p selections use a 480-line limit before render scaling, so the pixel
    preview and profiles retain their stated sizes on high-resolution phones.
    Lower detail keeps
    the chosen image size. The preview uses the renderer's pixel rounding;
    review and apply to restart, or cancel without saving. "Restablecer" selects
    full width and the native resolution in the draft on build 107; old
    build 106 configuration without `resolution_limit` retains its original
    480p default and choices. Only resolution settings change
    (lower detail uses OpenGL ES); FPS, effects and HUD settings are preserved.
    The first save backs up config.toml as config.toml.before-resolution.
  - "General": the phone's vibration, a floating move stick (the stick goes
    where your thumb lands in the lower left of the screen), the opacity of
    the controls, "Hide or add buttons" (hide a button, add a copy of a
    button) and "Edit buttons size".
  - "HUD personalizado": three independent HUD slots. Select and drag a
    button, change its size, opacity or visibility, or use the arrows for
    fine positioning. Guardar saves all drafts; Salir discards them. Tap
    the bottom bar to collapse the panel, or drag it to move the panel.
  - "Sensibilidad al mirar": a numerical 0.25–4x slider, 0.025x step buttons,
    Preciso (0.75x), Normal (1x) and Rápido (2x). Swipe the preview on the right
    to compare unzoomed camera response before saving. It uses the same swipe
    normalization and angular factor, without sending input to the running game.
    Guardar persists the value with the current HUD; Cancelar discards the draft
    and Restablecer selects 1x in the draft. Zoom and aim assistance remain the
    game's own. Resolution and the layout file format are unchanged.
    Giro horizontal has Clásico (1x), Ágil (2.25x, the default) and Amplio (4x)
    independently of the vertical response. The preview applies the same gain.
    Motion remains proportional to distance, without acceleration or inertia;
    historical touch samples and the last displacement on release are retained.
    The optional horizontal-sensitivity field travels with each HUD in format 4;
    layouts without it use Ágil, and older readers can ignore the extra field.
  - "Exportar HUD" and "Importar HUD". Files use
    layout version 4 for per-button opacity; versions 1 through 3 still load.

"Ocultar" and "Ajustes" act when the finger lifts on them: a swipe that
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
These settings are only for Android:

| Setting | Function |
| --- | --- |
| `input.touch_aim_assist` | `true` (the default): the touch controls' swipe aiming gets the aim assist of a controller (the aim slows over a target and follows a moving one). `false`: none, as with a mouse; the bullets' own autoaim stays. |
| `input.touch_controls` | The touch controls in a game. `"auto"` (the default): shown on a touchscreen while no controller is connected. `"on"`: also shown with a controller. `"off"`: never shown. A device without a touchscreen never shows them. The menus take taps in any case. |
| `display.screen_width` | The number of columns of the 480-line picture. `0` (the default): the shape of the display (1068 on a 20:9 phone). `640`: the 4:3 shape of the Xbox. |
| `display.render_scale` | Android OpenGL ES: internal render scale from `0.5` to `1.0` (default). `0.75` draws 360 lines, `0.5` draws 240; logical coordinates and field of view stay unchanged. The Vulkan guest keeps its original resolution. |
| `display.max_fps` | Android OpenGL ES: `30` or `60` limits presentation using a monotonic clock. `0` (default) leaves it uncapped; interpolation must be enabled to render above the original 30 FPS. The Vulkan guest keeps its original frame timing. |
| `debug.sample_seconds` | Refer to "Find problems". |
| `debug.memory_watch` | `true` (the default): the app notices the game's writes to textures and vertices by page protection. `false`: it compares page contents once a frame instead, which is slower. Refer to "Limits". |
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
folder: first make a copy of `maps/` and `save/u`.

## Widescreen

The game's logical picture has 480 lines in the shape of the display,
not the 640x480 of the Xbox:

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

## Find problems

- Enter `adb logcat -s halo` to see the log of the port and the errors of
  the game. `files/debug.txt` is the log of the game.
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
  notice changed textures and vertices. On the emulator the app compares
  page contents once a frame instead ("write tracking: page hashes" in the
  log). This is slower, and a write shows one frame later, so moving
  geometry can glitch briefly. `debug.memory_watch = false` does the same
  on a device. The translation also has no scalar conversion from floating
  point to an integer in a SIMD register (`fcvtzs s0, s1`, an illegal
  instruction there), so `tools/android_asm_convert.py` writes each as the
  vector form, which leaves the register and the exception flags the same.

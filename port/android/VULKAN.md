# Vulkan renderer — plan

A second renderer for the Android build that draws through Vulkan instead of
OpenGL ES, so that the game can run on a Vulkan driver of the player's
choosing: on Adreno GPUs, Mesa's open Turnip driver in place of the phone's
own. It follows the plan of the Switch's deko3d renderer
(`port/switch/DEKO3D.md`) step for step, which went from nothing to a
complete new backend in phases each tried on the console before the next.
Read that plan, and its "Progress", before this one: most of what is decided
here was decided there first, and most of what will go wrong here went wrong
there first.

The deko3d renderer was written for speed. This one is written for
correctness: where the deko3d plan made a choice only to save time (reading
the game's memory in place, shared key files), this plan takes the simpler
choice, and says so where it differs. Where deko3d's choice is also the
right one here (a draw whose pipeline is still compiling is left out until
it is ready), it is kept.

Status: phase 0, part A (the probe, on the phone's own driver) is done and
committed; its results are in "Progress". Phase 0, part B (the probe on a
driver loaded by the app, Turnip first) is written out and next. The work is
on the `vulkan-backend` branch, which starts again from `main`. An earlier
attempt, kept on the `vulkan-backend-old` branch, is not the base of this
work and nothing here builds on it; see "Lessons from the earlier attempt".

---

## Why

The Android build renders through OpenGL ES and the phone's own GL driver.
On some GPU and driver combinations the game draws wrongly: vertices
explode across the screen, textures are missing or wrong. These are the
kinds of faults a vendor's driver gets wrong and Mesa's drivers do not, and
on Android a player cannot change the GL driver, only the Vulkan one: an app
can load a Vulkan driver of its own (the mechanism emulators use,
libadrenotools), and Turnip, Mesa's Vulkan driver for Adreno, is built for it.

So the renderer exists to give the game a correct driver. That decides three
things:

- **Speed is not a goal.** Nothing is done for speed alone. The game should
  stay playable, and the phases measure nothing they do not need.
- **Turnip is a first-class target from phase 0**, not a later option: every
  phase is tried on the phone's driver and on Turnip, on the test device.
- **Turnip exists only for Adreno.** On a Mali or another GPU the Vulkan
  renderer runs on the vendor's own Vulkan driver, which may or may not draw
  better than its GL driver. The GL ES renderer stays as the fallback.

Two things this does not assume: that every fault is the driver's (some may
be in `d3d8_gl.c`'s use of GL, and a renderer copied from it can carry them:
a fault seen on both Vulkan drivers is ours), and that Turnip runs inside this
app at all. The earlier attempt saw Turnip crash the process inside
`vkUpdateDescriptorSets`; whether that was its own bug or a conflict with
this app (the guest's memory reserved below 4 GB, the 32-bit guest) is phase
0's first question.

---

## Decisions

Taken from the deko3d plan unless the row says otherwise. Rows marked
**(to confirm)** are proposals for the user.

| Question | Decision |
|---|---|
| What the renderer is for | **Correctness, through the choice of driver.** Speed is not a goal (see "Why"). |
| Shared code with the GL ES renderer | **Copied**, not extracted, as for deko3d: `d3d8_gl.c`, `xbox_textures.c`, `nv2a_vsh.c`, `nv2a_psh.c` stay untouched so the other platforms keep working; Vulkan versions are copies under `port/android/guest/`. Files under `port/linux/src` change only by read-only accessors behind `#ifdef HALO_ANDROID_VK` (as the deko3d work added behind `HALO_SWITCH`), and by rows in `port_config.c`'s settings table. |
| How the renderer is chosen | **Two guest images**, as on the Switch: `halo_guest.elf` (GL ES) and `halo_guest_vk.elf` (Vulkan), both in the APK. The host reads `display.renderer` before it loads the guest. Nothing in the guest chooses at run time. |
| How the Vulkan driver is chosen | **`display.vk_driver`**: empty is the phone's own driver; otherwise the name of a driver archive (an adrenotools zip: a `meta.json` and the driver's library) left in the data folder. The host unpacks it into the app's private files (the only place Android loads a library from), reads the library's name from `meta.json`, and opens it with libadrenotools. A driver that does not load is logged and the phone's own is used; a device where Vulkan does not come up at all gets the GL ES image, logged. Phase 0, part B builds this. |
| The GL ES renderer | **Kept, selectable for good**, and the automatic fallback. **(to confirm)** which is the default once phase 7 is done. |
| A shader or pipeline not compiled yet, met during play | **The draw is skipped** while it compiles on another thread, as for deko3d; it is drawn from the first frame its pipeline is ready. The compiled SPIR-V and the `VkPipelineCache` are kept on the device so that this happens once per device and driver, not once per run. |
| Shader compiler | **glslang on the device** (phase 0 measured 2 to 4 ms for a game-sized shader, 100 ms for the first compile of a process, 3.8 MB of library). |
| The game's memory | **Copied at the hand-over** into an upload ring, never read in place. The test device cannot import host memory, and one path on every driver is simpler to get right. Busy tracking stays (phase 3), because the game's locks need it. |
| Pipeline state | Dynamic rendering is required (Vulkan 1.3, or 1.1 with `VK_KHR_dynamic_rendering`); only Vulkan 1.0's core dynamic states are dynamic (viewport, scissor, depth bias, blend constants, stencil masks and reference), and everything else is in the pipeline key. More pipelines, but the oldest and most tested paths in every driver, and the same code on both drivers. Extended dynamic state can be added later if the number of pipelines becomes a problem. |
| Validation | `VK_LAYER_KHRONOS_validation` in debug builds from phase 2 on, run at the end of every step, on the phone's driver and, if the loader finds the layer with a custom driver loaded (phase 0 B finds out), on Turnip. |
| What players share | **Nothing.** No key files and no collection of keys (deko3d's phase 8): each device compiles what it meets and keeps it. |
| Devices | The test device (Adreno 750) on both drivers is what each phase is accepted on. Other devices are tried in phase 7. |

---

## Where the renderer lives

The renderer runs in the guest (ILP32), and Vulkan is a host library (LP64):
its handles are 64-bit, its structures are full of pointers, and its entry
points cannot be called from 32-bit code. Stubbing it call by call would put
a guest→host call on every state change and a marshaller on every structure.

So the renderer is split at the draw, exactly as deko3d's is:

- **Guest:** the `D3DDevice_*` entry points, the state they keep, the vertex
  declarations, reading the state back at each draw, the shader keys and the
  GLSL generation, the texture cache and decoding.
- **Host:** a Vulkan backend that takes a stream of compact commands (fixed-
  width fields, in guest memory) and records them into command buffers, one
  stub call each time the guest hands the stream over. The host also loads
  the driver (`host_vk_driver.c`, phase 0 B) and compiles the shaders.

The host reads guest memory directly (the guest runs in the low 4 GB of the
host process: `port/android/README.md`). **No command names a host address
in a 32-bit field** - the deko3d work faulted once on exactly that.

### The one rule about guest memory

**The host reads what a command names when it processes the command, at the
hand-over, never later.** The game rewrites vertex buffers, index buffers and
constants within a frame, and frees and reuses memory between draws; a
backend that names guest memory and reads it at the end of the frame draws
every draw with the frame's last bytes. Everything the GPU reads after the
hand-over is copied at the hand-over, into an upload ring or into an image;
and a resource the game locks while the GPU still needs it makes the game
wait - which deko3d does through the Xbox's own `Lock` field.

### Both renderers in one Android build

Two guest images, and the host runs one. `halo_guest.elf` is the game with
the GL ES renderer, built as now; `halo_guest_vk.elf` is the same objects
with `port/android/guest/d3d8_vk.c` in place of `d3d8_gl.c` (and the other
copies in place of theirs), linked by `tools/android_build.py` as the Switch
build links `halo_guest_dk.elf`.

Under Vulkan the host makes no GL context: Android lets one API own a window,
and a window that backs a GL ES context refuses a Vulkan surface
(`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`). SDL keeps its window for input and
lifecycle; the guest's GL context is a stand-in (as `host_sdl2.c` makes on the
Switch), and the Vulkan surface is made on the window's `ANativeWindow`
(`SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`) with `vkCreateAndroidSurfaceKHR`
from the loaded driver, not through SDL's own Vulkan loader, which would load
the phone's driver beside the chosen one.

Config, under `[display]`: `renderer = "gl"` or `"vulkan"`, and `vk_driver`
(above).

---

## Files

New, Android only:

| File | From | What |
|---|---|---|
| `port/android/host/host_vk_driver.c` | — | unpacking a driver archive, opening the driver (libadrenotools, or the system loader), handing out `vkGetInstanceProcAddr` (phase 0 B; used by the probe and by the backend) |
| `port/android/guest/d3d8_vk.c` | `d3d8_gl.c` | entry points and state kept; GL calls replaced by commands sent to the host |
| `port/android/guest/xbox_textures_vk.c` | `xbox_textures.c` | format decoding kept; images and uploads go to the host |
| `port/android/guest/nv2a_vsh_vk.c`, `nv2a_psh_vk.c` | `nv2a_vsh.c`, `nv2a_psh.c` | GLSL for Vulkan (phase 4) |
| `port/android/guest/vk_commands.h` | — | the command stream, guest and host both include it |
| `port/android/host/host_vk.c` | — | the Vulkan backend |
| `port/android/host/host_vk_shaders.c` | — | glslang, the SPIR-V and pipeline caches |

Already in the tree from phase 0 A: `port/android/host/host_vk_probe.c` (the
probe), `port/android/probe/` (its shaders, glslang's build, the reports).

Unchanged and shared: `d3d8_resources.c`, the rest of the platform layer.
`port/android/host_imports.list` gains the host functions the guest calls.

---

## Phase 0 — spike

A probe in the Android host, behind a setting, that answers the questions the
rest of the plan depends on before any of the renderer is written. Its
results go into "Progress" before phase 1 starts. It stays in the tree: it is
how a driver or a tester's device is reported later (phase 7).

Two parts:

- **Part A — the probe on the phone's own driver. Done.** What the device
  has, a frame on the screen and the surface's loss, the cost of copying the
  game's data, glslang on the device, pipelines. Specified below as it was
  worked, with its results and every deviation in "Progress".
- **Part B — the probe on a driver the app loads. Next.** Turnip first.

Part A's step 6 (profiling the GL ES path) belonged to the speed plan and is
not part of this one: its results stay in "Progress" as a record, and the
profiling mode stays in `host_debug.c` (off unless set) as a tool.

### Part B — a driver the app loads (next)

**The question:** does Turnip, loaded by this app through libadrenotools,
run here - with the guest's memory reserved below 4 GB, the app's signal
handlers, SDL's window - and does it draw the game's own shaders with
descriptor sets, where the earlier attempt saw it crash? And does it report
what phase 1 onwards will need?

#### What this part is, and is not

- **It is** host code and the build: the driver module
  (`host_vk_driver.c`), libadrenotools in the build, the app packaged so that
  its native libraries are extracted, the probe moved onto the driver module,
  and a new probe step, `draw`, that draws with the game's converted shaders.
- **It is not** the renderer. Nothing in the guest changes.
- **The driver module is not throwaway**, unlike the probe: phase 2's
  backend opens its driver through it. It is written as the backend's code,
  and audited as such.

#### Files

| File | What |
|---|---|
| `port/android/host/host_vk_driver.c`, `host_vk_driver.h` | the driver module (below) |
| `port/android/host/host_vk_probe.c` | loads Vulkan through the driver module instead of `SDL_Vulkan_LoadLibrary`; makes its surface with `vkCreateAndroidSurfaceKHR`; the new `draw` step; the report names the driver |
| `port/android/host/host_main.c` | reads `display.vk_driver` and hands it to the probe |
| `port/linux/src/port_config.c` | the row for `display.vk_driver` (`_platform_android`) |
| `tools/android_build.py` | fetches and builds libadrenotools and its hooks, stages them into `jniLibs` |
| `port/android/probe/adrenotools.patch` | the change libadrenotools needs to build here (below) |
| `port/android/app/build.gradle` | `jniLibs.useLegacyPackaging true` |
| `port/android/README.md` | the setting and how to install a driver |

#### The driver module

```c
/* opens the driver named by setting ("" = the phone's own) and returns its
vkGetInstanceProcAddr, or NULL after logging why; *description says which
driver was opened, for the log and the probe's report */
PFN_vkGetInstanceProcAddr host_vk_driver_open(const char *setting, char *description, size_t size);
void host_vk_driver_close(void);
```

- **The phone's own driver** (`setting` empty): `dlopen("libvulkan.so")`, the
  system loader, and `vkGetInstanceProcAddr` from it.
- **A driver archive** (`setting` names a file in the data folder,
  `/sdcard/Android/data/<package>/files`):
  1. Unpack it into `<internal storage>/vk_driver/<archive name>/`
     (`SDL_GetAndroidInternalStoragePath()`), only if the archive is newer
     than what is unpacked there (compare size and modification time,
     written beside the unpacked files). Android loads a library only from
     the app's private storage, not from the shared data folder, which is why
     the archive is left in one and unpacked into the other.
  2. The zip is read with a small reader of its own (central directory,
     stored and deflated entries, inflated with zlib, the NDK's `libz`):
     no new library. A file name containing `..` or starting with `/` is
     refused.
  3. Read `meta.json` for `libraryName` (the driver's file, for example
     `vulkan.ad07XX.so`) and, for the log, `name`, `driverVersion`,
     `description`. A minimal reader for these string fields is enough.
  4. `adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
     NULL, <native library dir>, <unpacked dir>/, <libraryName>, NULL, NULL)`.
     Two traps the earlier attempt hit: the custom driver directory must end
     with `/` (the library joins it to the name with nothing between), and
     the hook library must be in the native library directory or every
     driver is refused, the phone's included. The native library directory
     is the directory of `libmain.so` itself (`dladdr` on a function of the
     host), which with legacy packaging is the extracted
     `nativeLibraryDir`.
  5. `vkGetInstanceProcAddr` from the returned handle.
- **Anything that fails** - no such archive, a broken zip, no `meta.json`, a
  refused driver - is logged with the reason, and the phone's own driver is
  opened instead; the description says so ("Turnip ... failed: <reason>; using
  the phone's driver").
- **Only one driver is open in the process.** Nothing else in the host may
  load the phone's Vulkan driver once a custom one is open: that is why the
  surface is not made through `SDL_Vulkan_*` (SDL would `dlopen` the system
  loader).

#### libadrenotools in the build

- Fetched at configure time as glslang is: Eden's fork,
  `https://github.com/eden-emulator/libadrenotools`, at a pinned commit, with
  its submodule `lib/linkernsbypass`. The commit and the submodule's go into
  "Progress". BSD-2-Clause.
- `port/android/probe/adrenotools.patch`, applied after the fetch: whatever
  the NDK needs (the earlier attempt added `log` to the library's link line,
  because the NDK no longer links liblog by default).
- Built with CMake and the NDK's toolchain as glslang is. Staged into
  `jniLibs/arm64-v8a`: `libadrenotools.so` and the hooks it loads by name
  (`libmain_hook.so`, `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`, or
  whatever the pinned commit builds; the build lists them). The host links
  `libadrenotools.so` or `dlopen`s it; with `dlopen`, the GL ES path loads
  nothing new, which is preferred.
- `useLegacyPackaging true`: libadrenotools installs its hooks from the
  native library directory, which exists only when Android extracts the
  libraries from the APK. The installed app grows by the libraries' size.

#### The probe on the driver module

- `create_instance` takes `vkGetInstanceProcAddr` from
  `host_vk_driver_open(display.vk_driver)` instead of SDL.
- The surface: `vkCreateAndroidSurfaceKHR` on the window's `ANativeWindow`
  (`SDL_GetPointerProperty(SDL_GetWindowProperties(window),
  SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL)`); after backgrounding, the
  pointer is read again (it is a new window) and the surface made again. The
  window is still created with `SDL_WINDOW_VULKAN` so that SDL makes no EGL
  surface on it; if that flag makes SDL load the system loader (check: the
  report lists the loaded libraries, from `dl_iterate_phdr`, after setup),
  create it without the flag and check that no EGL surface exists instead.
- The report's first lines name the driver: the setting, what the driver
  module opened (archive, `meta.json`'s name and version, or "the phone's
  driver"), and then, from `caps`, `driverID`, `driverName`, `driverInfo`
  (Turnip reports `VK_DRIVER_ID_MESA_TURNIP` and the Mesa version).
- Whether the validation layer is found and enabled with the custom driver
  loaded is reported, as in part A. If it is not, say so: Turnip is then run
  without it, and its own checks (`TU_DEBUG`, set with
  `adrenotools_set_freedreno_env` before the open) are the fallback.

#### The new step: `draw`

The draw the earlier attempt crashed on, and the one part A never made: the
game's converted shaders (`port/android/probe/`, the typical and the large
pair) drawn for real.

- A descriptor set layout as the shaders declare it (the vertex shader's
  uniform block at binding 0, the pixel shader's at 1, four combined image
  samplers at 2 to 5), a descriptor pool, a set allocated and written with
  `vkUpdateDescriptorSets`: two uniform buffers (the constants filled with
  values that put the vertices on the screen: an identity transform in
  `c[]` where the shader reads its matrix, and `viewport_scale` /
  `viewport_offset` for a 256×256 target) and four 64×64 textures (`R8G8B8A8`
  and, where the device samples it, `BC1`), each with a sampler.
- A vertex buffer in the layout part A wrote for each shader (`make_pairs`),
  holding a triangle that covers the target.
- Draw into a 256×256 colour target with depth, read it back, and report:
  whether it finished, how many pixels are not the clear colour, and a hash
  of the picture. The two drivers' hashes are compared by hand in
  "Progress": they need not be equal, but a black or empty picture on one
  and not the other is a finding.
- Each sub-step is guarded (`guard_begin`), so that a crash in
  `vkUpdateDescriptorSets`, in pipeline creation or in the draw names
  itself on the next run.
- The same with the descriptor set written twice (the second write after
  the first draw was submitted and waited for), and with push descriptors
  where the device has them, because the earlier crash was in the update.

#### Fixes to part A's code, made in this part

From the audit of part A, the ones that matter for correctness:

- `guard_end()` deletes the running marker, so a crash later in the same
  step is not recorded: restore the step's own name instead.
- In `present`, the retries while the app is away count as surface losses:
  count a loss once, and retry without logging each attempt.
- In `memory_import_cases`, the index-import failure prints the wrong result
  code, and "same pages twice" is not guarded though the comment says so.
  (The import path runs only on a device with `VK_EXT_external_memory_host`;
  phase 3 does not use it, but the probe should not misreport.)

#### Testing on the device

1. `ninja android_apk` (with `--android-vulkan-validation`), install the
   `.vk` build with `adb install -r`.
2. `display.vk_driver = ""`, `debug.vk_probe = "all"`: the phone's driver,
   with the new `draw` step; validation on, then off. As part A's runs, plus
   `draw`.
3. Copy a Turnip archive for the Adreno 7xx (an adrenotools zip; the user
   chooses the build, and its name and Mesa version go into "Progress") into
   the data folder; `display.vk_driver = "<archive>.zip"`; the same runs.
   With validation if the layer loads.
4. `display.vk_driver = "missing.zip"` and a broken zip: the probe logs the
   reason and runs on the phone's driver.
5. `debug.vk_probe = ""`: the GL ES game runs as before (with legacy
   packaging now).

A crash is a result, not a blocker: the guard leaves the sub-step out next
time, and the crash (the last report line, logcat's backtrace) goes into
"Progress".

#### Acceptance

- Reports in `port/android/probe/reports/` for the phone's driver and for
  Turnip (`..._qualcomm_*`, `..._turnip_*`), each a run of `all` with and
  without validation where the layer loads.
- **Turnip runs in this app**: setup, `caps`, `present` (with backgrounding),
  `pipelines` and `draw` finish; or, if one does not, the crash is found,
  understood and written into "Progress", and the plan is changed to answer
  it before phase 1.
- **The `draw` step's picture** is not empty on either driver.
- A table in "Progress" comparing the two drivers: versions, the features
  the Decisions rely on (dynamic rendering; the core dynamic states; the
  formats), presenting and re-creation, pipeline creation, `draw`.
- The driver module audited against the source and its failure paths tried
  (step 4), then committed.
- The GL ES game unchanged on the device.

### Part A — the probe on the phone's own driver (done)

The specification as part A was worked. What was done differently is listed
in "Progress" under "Phase 0, part A: deviations from the spec".

#### What this phase is, and is not

- **It is** host code only: a C file in the Android host, the build changes
  to give it Vulkan, glslang and the validation layer, a profiling mode for
  the existing sampler, and a script that reads the profiles.
- **It is not** a renderer. Nothing in the guest changes. No command stream,
  no guest image, no `display.renderer`. The game is not started while the
  probe runs: the probe owns the window, and when it is done the app exits.
- **Nothing it builds is reused by phase 2 as it stands.** Phase 2 may copy
  pieces (the swapchain's re-creation, the function table) once they are
  proven here, but the probe is written to measure, not to be the backend.
- **The GL ES path does not change**, except the profiling mode in
  `host_debug.c` (step 6), which is off unless set.

#### Files

| File | What |
|---|---|
| `port/android/host/host_vk_probe.c` | the probe: steps 1 to 5. The host's build globs `host/*.c`, so it is built in without a list to edit. |
| `port/android/host/host_main.c` | reads `debug.vk_probe` (as it reads `debug.sample_seconds`, with tomlc17) and, when it is set, runs the probe instead of the guest's `main`, after the image is loaded and SDL's video is up (the existing comment there says why that order). |
| `port/android/host/host.h` | `host_vk_probe_run(const char *steps, const char *data_root)`. |
| `port/android/host/host_debug.c` | the profiling mode (step 6). |
| `port/android/probe/*.vert`, `*.frag` | the shaders steps 4 and 5 compile, staged into the APK's assets as `vk_probe/…` (step 4 says where they come from). |
| `tools/android_build.py` | glslang, the validation layer, the probe's assets (below). |
| `tools/android_profile_report.py` | reads a profile from step 6 and prints where the time went. |
| `port/linux/src/port_config.c` | three rows in the settings table, `_platform_android` only (below). This is the one change under `port/linux/src` in this phase, and it is the precedent `debug.sample_seconds` set: the guest rewrites `config.toml` on every run, so a setting the host reads is lost unless the table has it. |
| `port/android/README.md` | the new settings, in its "Settings" and "Find problems". |

#### Settings

Under `[debug]`, read by the host from `config.toml`, each also a row in
`port_config.c`'s table so that it survives the guest's rewrite:

| Setting | Default | What |
|---|---|---|
| `debug.vk_probe` | `""` | `""`: the game runs as usual. `"all"`: every step. Otherwise a comma list of steps, `caps,memory,compile,pipelines,present`, run in that order whatever order they are written in. A step that crashes the probe is left out of the list on the next run; that is the skip mechanism, as `deko3d_probe_skip.txt` was on the Switch. |
| `debug.vk_validation` | `false` | Enable `VK_LAYER_KHRONOS_validation` when the APK carries it. The probe logs whether the layer was found and enabled, and every message it sends. |
| `debug.profile_hz` | `0` | Step 6's profiling mode: sample every guest thread this many times a second into memory, and write the profile to the data folder. `0` off. |

#### The probe's shape

- **Vulkan through SDL.** The probe makes its own window,
  `SDL_CreateWindow(..., SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN)`, loads
  Vulkan with `SDL_Vulkan_LoadLibrary(NULL)` and takes
  `vkGetInstanceProcAddr` from `SDL_Vulkan_GetVkGetInstanceProcAddr()`. Every
  other entry point is loaded through it into a table, instance-level then
  device-level, and a missing one is logged by name and its step is reported
  as failed: no call through a null pointer. The host does **not** link
  `-lvulkan`, so the GL ES path loads nothing new. The NDK's Vulkan headers
  (1.3.275 in r27c) are the headers; `VK_NO_PROTOTYPES` is defined.
- **The surface** comes from `SDL_Vulkan_CreateSurface`. No GL context and no
  EGL surface exist on that window at any time (Android refuses a Vulkan
  surface on a window that backs one: `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`).
- **The instance** asks for 1.3 if the loader offers it, else what it
  offers; enables `VK_KHR_surface`, `VK_KHR_android_surface`,
  `VK_KHR_get_physical_device_properties2` and, with validation,
  `VK_EXT_debug_utils`. **The device** enables every extension step 1 finds
  and the later steps use, and nothing else; each enabled extension is
  logged. One graphics queue that can present.
- **The report.** Every result is written to `vk_probe.txt` in the data
  folder (`/sdcard/Android/data/com.halo.decomp/files`, pulled with `adb
  pull`) and to logcat with the prefix `vk probe:`. The file is opened at
  the start and flushed after every line, so a probe that dies leaves the
  line it died after. Lines are `section.key: value`, one fact each, so two
  devices' reports can be diffed and a script can turn them into the
  "Progress" table. The first lines are the build (git hash, as the game's
  log prints it), the device (`ro.product.model`, `ro.hardware`, the Android
  API) and the steps asked for.
- **Timing** is `clock_gettime(CLOCK_MONOTONIC)` for wall time and
  `CLOCK_THREAD_CPUTIME_ID` for one thread's CPU time; GPU time, where a step
  asks for it, is timestamp queries, when `timestampValidBits` is not zero on
  the queue's family. Every timing is several runs: report the first run on
  its own, and the minimum and median of the rest.
- **When it ends**, every Vulkan object is destroyed in order (with the
  validation layer on, that is checked too), the report says `probe.done:
  yes`, and the app exits through `host_exit(0)`.

#### The build

- **glslang** is fetched at configure time, as SDL3 is (`fetch_third_party`
  in `tools/android_build.py`): a shallow clone of KhronosGroup/glslang at a
  pinned release tag (the newest when this is done; the tag goes into
  "Progress"). It is built with CMake and the NDK's toolchain, as SDL3 is,
  into `libglslang_probe.so` (or whatever the build names it), a shared
  library with the C++ runtime linked inside it (`ANDROID_STL=c++_static`),
  `ENABLE_OPT=OFF` (no SPIRV-Tools in phase 0; phase 5 measures the
  optimiser), `ENABLE_HLSL=OFF`, no binaries, no tests. It is staged into
  `jniLibs/arm64-v8a` and loaded by the probe with `dlopen`, through its C
  interface (`glslang/Include/glslang_c_interface.h`), so `libmain.so` stays
  C and the GL ES path never loads it. Its log files go to `build/android/`
  as SDL's do.
- **The validation layer** is the Android release of
  KhronosGroup/Vulkan-ValidationLayers at a pinned version, fetched at
  configure time, and its `arm64-v8a/libVkLayer_khronos_validation.so`
  staged into `jniLibs` **only when `configure.py` is given
  `--android-vulkan-validation`**, so the APKs the CI builds do not grow by
  its size. Android's loader finds a layer in the app's own native library
  folder in a debuggable app.
- **The probe's shaders** are copied into `assets/vk_probe/` as the guest
  image and `brokers.txt` are, and the probe reads them with `SDL_LoadFile`.
- `ninja android_apk` with no new flag still builds and the GL ES game still
  runs as before: checked on the device before the phase is done.

#### Step 1: what the device has (`caps`)

Logged in full, on every device, so that the rest of the plan can be decided
from the reports and not from memory:

- **Versions and the driver:** the loader's instance version; the device's
  `apiVersion`, `driverVersion`, `vendorID`, `deviceID`, `deviceName`;
  `VkPhysicalDeviceDriverProperties` (`driverID`, `driverName`,
  `driverInfo`, `conformanceVersion`); `ro.hardware.vulkan` and
  `ro.board.platform` from `__system_property_get`.
- **Every device extension** and its version, one line each.
- **The features that decide the design**, each `yes`/`no`, from
  `vkGetPhysicalDeviceFeatures2` with the structures chained:
  - dynamic rendering (1.3 core or `VK_KHR_dynamic_rendering`);
  - extended dynamic state 1 and 2 (and 2's `LogicOp` and
    `PatchControlPoints`), and every member of
    `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT` by name;
  - `VK_EXT_vertex_input_dynamic_state`;
  - `VK_EXT_external_memory_host` and
    `minImportedHostPointerAlignment`;
  - `VK_KHR_push_descriptor` and `maxPushDescriptors`;
  - `VK_EXT_custom_border_color` (the Xbox's border colour address mode),
    `VK_EXT_4444_formats` (A4R4G4B4), `VK_KHR_maintenance4/5`,
    `VK_EXT_pipeline_creation_cache_control`,
    `VK_EXT_graphics_pipeline_library`, `VK_KHR_pipeline_library`;
  - core features: `textureCompressionBC`, `textureCompressionASTC_LDR`,
    `textureCompressionETC2`, `samplerAnisotropy`, `occlusionQueryPrecise`,
    `depthBiasClamp`, `depthClamp`, `fillModeNonSolid`, `independentBlend`,
    `logicOp`, `shaderClipDistance`, `wideLines`, `pipelineStatisticsQuery`.
- **Limits:** `maxVertexInputAttributes` and `maxVertexInputBindings` (the
  Xbox has 16 of each), `maxVertexInputAttributeOffset`,
  `maxUniformBufferRange` (the vertex shader's 192 constants alone are
  3072 bytes), `maxPushConstantsSize`, `maxBoundDescriptorSets`,
  `maxPerStageDescriptorSamplers`, `maxSamplerAnisotropy`,
  `maxSamplerLodBias`, `maxImageDimension2D/3D/Cube`,
  `maxColorAttachments`, `minUniformBufferOffsetAlignment`,
  `nonCoherentAtomSize`, `optimalBufferCopyOffsetAlignment`,
  `timestampPeriod`, and the queue family's `timestampValidBits`.
- **Memory:** every heap (size, flags) and every type (heap, flags), and
  which types a host-visible, host-coherent, host-cached buffer can use.
- **Formats:** for each Vulkan format a texel kind in `xbox_textures.c`
  could map to, the optimal-tiling `SAMPLED_IMAGE`,
  `SAMPLED_IMAGE_FILTER_LINEAR`, `COLOR_ATTACHMENT`,
  `COLOR_ATTACHMENT_BLEND`, `DEPTH_STENCIL_ATTACHMENT`, `TRANSFER_DST`
  bits. At least: `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM`, `A8B8G8R8_UNORM_PACK32`,
  `R5G6B5_UNORM_PACK16`, `B5G6R5_UNORM_PACK16`, `A1R5G5B5_UNORM_PACK16`,
  `R5G5B5A1_UNORM_PACK16`, `B5G5R5A1_UNORM_PACK16`, `R4G4B4A4_UNORM_PACK16`,
  `B4G4R4A4_UNORM_PACK16`, `A4R4G4B4_UNORM_PACK16` (4444 formats),
  `R8_UNORM`, `R8G8_UNORM`, `R16_UNORM`, `R16G16_UNORM`, `R8G8_SNORM`,
  `R16G16_SNORM`, `BC1_RGBA_UNORM_BLOCK`, `BC2_UNORM_BLOCK`,
  `BC3_UNORM_BLOCK`, `D16_UNORM`, `D24_UNORM_S8_UINT`,
  `D32_SFLOAT_S8_UINT`, `X8_D24_UNORM_PACK32`, `D32_SFLOAT`. (Mali drivers
  usually lack BC and `D24_UNORM_S8_UINT`; that is what this is for.)
- **The surface:** `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` (image
  counts, extents, `supportedTransforms`, `currentTransform`,
  `supportedCompositeAlpha`, usage flags), every surface format and colour
  space, every present mode.

#### Step 2: a frame on the screen (`present`)

Run last, because it waits for the tester. On the window's surface, with SDL
still delivering input and no GL context:

- A swapchain (FIFO, `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM`, whichever the
  surface lists, image count `minImageCount + 1`), two frames in flight
  behind fences, each frame one command buffer that clears the image to a
  colour that cycles over a few seconds and transitions it for present.
  Cleared through `vkCmdClearColorImage` (no render pass); a second mode,
  behind a button, clears through a render pass or dynamic rendering
  instead, because the earlier attempt's driver needed one before it would
  present.
- **The surface's loss.** Backgrounding destroys the `ANativeWindow`. On
  `SDL_EVENT_WILL_ENTER_BACKGROUND` / `DID_ENTER_BACKGROUND` the probe stops
  presenting, waits for the device to be idle and destroys the swapchain and
  the surface; on `SDL_EVENT_DID_ENTER_FOREGROUND` it makes both again.
  `VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR` and
  `VK_ERROR_SURFACE_LOST_KHR` from acquire or present also re-create them.
  Every re-creation is logged with its reason and how long it took.
- **Rotation.** The activities are `sensorLandscape`, so turning the device
  over flips it 180 degrees. Log `currentTransform` before and after, and
  whether the driver reported `SUBOPTIMAL`; create the swapchain with
  `preTransform = currentTransform` (and say in the report whether the
  picture is upside down: the tester answers by pressing a button, below).
- **Pacing.** The interval between presents, measured at the CPU after
  `vkQueuePresentKHR` returns: minimum, median, maximum and a count of
  intervals more than 1.5 times the median, over each ten seconds. It should
  be the display's refresh (the test device's panel can run above 60 Hz;
  log `SDL_GetCurrentDisplayMode`'s refresh rate beside it).
- **SDL beside Vulkan.** The probe opens the first gamepad and logs every
  button press with its name; it plays a one-second tone through an SDL
  audio stream when the step starts. The report says whether input arrived
  and whether the stream opened.
- **The tester's script**, printed in the report and in logcat when the step
  starts: watch the colour cycle; press Home and return, ten times; turn the
  device over and back; press A if the picture looks right, Y if it is
  upside down or squashed, X to switch clear modes, B to end the step. With
  no input for 120 seconds the step ends on its own and says so.

The report's lines: whether presenting worked, the re-creations (count, by
reason, each one's time), the pacing figures, the transforms seen, the
buttons the tester pressed.

#### Step 3: the game's memory, seen by the GPU (`memory`)

The question phase 3 needs answered: can the GPU read the guest's memory
where it is, or must every draw's data be copied?

The memory under test is the kind the guest uses: an anonymous, private
mapping below 4 GB, from `host_low_map` (the window itself is the same kind
of mapping, `host_memory.c`, but is not committed before the game runs).
Each case is run, and reported, on its own; one that fails does not stop
the others.

1. **Import.** For ranges of 64 KB, 4 MB, 16 MB and 64 MB, aligned to
   `minImportedHostPointerAlignment`:
   `vkGetMemoryHostPointerPropertiesEXT` (the memory types it allows, and
   their flags), `vkAllocateMemory` with `VkImportMemoryHostPointerInfoEXT`
   (`HOST_ALLOCATION_BIT_EXT`), and a `VkBuffer` made with
   `VkExternalMemoryBufferCreateInfo` bound to it. Log every result code.
   Also: a range that is not aligned (it should be refused, and the
   refusal should be an error code, not a crash), and the same pages
   imported twice (allowed or not).
2. **Reading by copy.** Write a pattern through the host pointer,
   `vkCmdCopyBuffer` from the imported buffer to a host-visible readback
   buffer, wait, compare. Then rewrite the pattern and copy again: the
   second copy must see the second pattern. If the imported memory type is
   not `HOST_COHERENT`, say so and say what the second copy saw.
3. **Reading by vertex fetch.** The path that matters: a draw whose vertex
   buffer is the imported buffer, into a small colour image (a 16×16
   `R8G8B8A8_UNORM`), a vertex shader that passes each vertex's colour
   through, read back and compared. Rewrite the vertices between two
   submissions and check both pictures, as phase 3's acceptance does. Same
   for an index buffer in imported memory.
4. **Cost of the alternative.** A host-visible, host-coherent upload ring
   (and, if a type exists, host-cached with explicit flushes). Copy into it
   per frame the amount of vertex and index data the GL build streams in a
   frame (step 6 measures it with `debug.gpu_stats`: the "mirrored" and
   "streamed" kilobytes per frame; until then 4 MB) and draw from it: the
   CPU time of the copies per frame, over 300 frames, and the GPU time of
   the same draw from imported memory against from the ring.
5. **A texture from imported memory.** `vkCmdCopyBufferToImage` from the
   imported buffer into a `BC1` (or, without BC, an `R8G8B8A8`) image, read
   back. This is what reading compressed textures in place would rest on.

The report says, per range size: imported or not (with the result code),
the memory type and its flags, copy correct, vertex fetch correct, index
fetch correct, rewrite seen; and the ring's figures.

#### Step 4: the shader compiler (`compile`)

**Which shaders.** The Android GL build does not record
`shader_programs.bin`: that file is the Switch's (`#ifdef HALO_SWITCH` in
`d3d8_gl.c`). Take the shaders from the GL build's own dump instead:

1. Run the current GL ES build on the device with
   `debug.gpu_dump_shaders = "<data root>/glsl"`, through the main menu, a
   campaign map's start and the first fight (the dump writes
   `vs<id>_<variant>.glsl` and `ps_<hash>.glsl`).
2. Pull the folder. Choose four: the largest vertex shader, the largest
   pixel shader (most texture stages and combiner stages), and a typical one
   of each (the median size).
3. Convert each to Vulkan GLSL by hand, changing only what Vulkan needs:
   `#version 450`, `layout(location = n)` on every input and output, the
   loose uniforms (`c[192]` and the rest) into one `std140` uniform block
   at set 0 binding 0, each sampler a `layout(set = 0, binding = n)`. Keep
   the bodies as they are. Add a tiny shader pair (a passthrough) for the
   fixed cost.
4. Commit the converted shaders under `port/android/probe/` with a comment
   at the top of each naming the dump file it came from and the build that
   made it.

This conversion is not phase 4: it is four shaders by hand, to time the
compiler on shaders of the right size. Phase 4 writes the generators.

**What is measured.** glslang through its C interface: `glslang_initialize_process`
once, then for each shader, ten times: `glslang_shader_create`,
`preprocess`, `parse`, a program, `link`, `glslang_program_SPIRV_generate`,
destroy. The first compile of the first shader on its own (it pays the
front end's start), then per shader the minimum and median of the rest,
wall time and thread CPU time, on one thread. The size of each SPIR-V.
Each SPIR-V is written to the data folder (`vk_probe_spirv/`), so it can be
checked off the device with `spirv-val` and `spirv-dis`. And the size
`libglslang` adds to the APK.

#### Step 5: pipelines (`pipelines`)

With step 4's SPIR-V (the step compiles them itself if `compile` was not
run):

- **One pipeline per vertex/pixel pair**, the state the game would use: a
  vertex input layout matching the vertex shader's inputs, triangle list,
  a colour attachment in the swapchain's format, `D24_UNORM_S8_UINT` or
  `D32_SFLOAT_S8_UINT` (whichever step 1 found), alpha blending on, depth
  test on. Made against dynamic rendering when the device has it, otherwise
  a render pass. **Every state the device can make dynamic is dynamic**
  (from step 1); the list used is logged.
- **Cold.** The driver keeps its own cache between runs on most Android
  devices, so a pipeline made the run before is not cold. To be sure a
  creation is cold, give each run's shaders a different specialization
  constant value (from the run's time), with an empty `VkPipelineCache`.
  Time `vkCreateShaderModule` and `vkCreateGraphicsPipelines` per pipeline.
- **From a saved cache.** The same shaders and constant, with a
  `VkPipelineCache` made from `vk_probe_pipeline_cache.bin` in the data
  folder, saved by the previous run (`vkGetPipelineCacheData`). The probe
  saves the cache at the end of every run, and logs the cache header's
  vendor, device and UUID, and whether the driver accepted the file. Run the
  probe twice in a row to get this figure: the second run reports it.
  (For this case the specialization constant is the previous run's, which
  the probe saves beside the cache.)
- **Without dynamic state.** The same pipelines with nothing dynamic beyond
  viewport and scissor, timed the same way, to tell whether dynamic state
  costs or saves creation time on this driver.
- **On a second thread while the first draws.** Thread A records and
  submits a frame every 16 ms to an offscreen image (a clear and a hundred
  draws with an already-made pipeline), and logs each frame's CPU time and
  each submission-to-fence time. Thread B makes twenty new pipelines (twenty
  specialization constants). Report thread A's frame times before, during
  and after: does creating pipelines stall the queue or the driver's other
  thread?

#### Step 6: the GL path's costs (`debug.profile_hz`, the game running) — dropped from this plan

Not a probe step: the game runs, as now, under GL ES, with the profiling mode
on. It tells whether this renderer removes what costs the GL path its frame
time.

**The profiling mode** (`host_debug.c`). The existing sampler takes whole
seconds (`atoi`), logs a line per sample to logcat and cannot sample faster
than once a second, so it cannot profile. With `debug.profile_hz` set:

- a thread sends `SIGURG` to every guest thread `profile_hz` times a second
  (`clock_nanosleep`, absolute deadlines);
- the handler writes the thread's id, the program counter, the link
  register and up to eight frame-chain return addresses into a preallocated
  ring (an atomic index; nothing in the handler allocates, locks or logs);
- once a second the thread also reads each guest thread's
  `/proc/self/task/<tid>/schedstat` (nanoseconds on the CPU) and records it;
- every ten seconds the thread appends the ring and the CPU times to
  `profile.bin` in the data folder, with `/proc/self/maps` at the start of
  the file, so addresses outside the guest image can be named by library;
- `debug.sample_seconds` keeps working as before.

**The report script** (`tools/android_profile_report.py profile.bin`):
symbolizes the guest's addresses with `llvm-symbolizer
--obj=build/android/halo_guest.elf`, names the others by the mapping they
fall in, and prints, per thread, the share of samples in:

- the game (guest code outside the renderer);
- the GL renderer (`d3d8_gl.c`, `xbox_textures.c`, `nv2a_*.c`);
- the boundary: the guest's import stubs and the host's GL forwarding
  (`host_gl.c`, the generated stubs in `libmain.so`);
- the GL driver (the vendor's `libGLES*` and what it calls: libc, the
  kernel's `[vdso]`, other vendor libraries);
- everything else (audio, network, idle in the kernel).

And per thread its CPU time per second; with the frames per second from
`debug.gpu_stats` (which also logs the draws and the streamed kilobytes per
frame), that is milliseconds of CPU per frame.

**The runs** on each device, each 60 seconds after things settle, with
`profile_hz = 1000` and `gpu_stats = true`:

1. the main menu, idle;
2. the first campaign map's opening, standing still
   (`debug.test_input = "look:1"` turns and looks without moving, for a
   repeatable scene);
3. the same map in the first fight, played.

Then once with `profile_hz = 0` and the same scenes, to see what profiling
itself costs (frames per second against the profiled run).

#### Testing on the device

On each device:

1. `ninja android_apk` (with `--android-vulkan-validation` given to
   `configure.py` for the validation runs), install with `adb install -r`.
2. With `vk_probe = "all"` and `vk_validation = true`: run, do the step-2
   script, pull `vk_probe.txt`. The validation layer's messages are in it;
   each is either explained in the report or fixed in the probe.
3. The same again with `vk_validation = false` (the timings that count are
   these; the layer slows everything), and once more right after, for the
   saved-cache figures.
4. `vk_probe = ""`: the game runs under GL ES as before (a menu and a map).
5. Step 6's runs, and `tools/android_profile_report.py` on each profile.

A step that crashes is left out of `vk_probe`, the rest are run, and the
crash (the last report line, the logcat backtrace) goes into "Progress" as a
result, not a blocker.

#### Acceptance

- `vk_probe.txt` from the test device and from a device with a GPU of another
  family (an Adreno and a Mali), both runs (cold and warm) and the validation
  run, in `port/android/probe/reports/` with the device in the file's name.
- **A table in "Progress"**, one column per device: the versions and driver;
  the yes/no features; the formats that matter (BC, the 16-bit colours, the
  depth formats); presenting, re-creation and pacing; import by range size
  and what read correctly; the ring's cost; glslang's times (first, and per
  shader min/median); pipeline times (cold, warm, no dynamic state, the other
  thread's stall); step 6's shares and milliseconds per frame per scene.
- **Proposals, written into "Decisions" marked (to confirm)** for the user:
  - the minimum Vulkan version and the required extensions, and what a
    device without them gets (GL ES);
  - phase 3: memory in place or an upload ring (or in place where the
    device allows, the ring elsewhere);
  - phase 5: which states are dynamic, so what is in the pipeline key; and
    whether the second thread's stall is small enough for compile-on-skip;
  - the shader compiler: glslang on the device, or SPIR-V shipped in the APK
    for known keys, from step 4's times and size;
  - whether step 6 supports the case for this renderer, in one paragraph.
- The GL ES game unchanged on the device with `vk_probe = ""`, and with a
  build made without `--android-vulkan-validation`.
- The probe's code audited against the Vulkan specification's valid usage
  for what it calls (the validation run is part of that, not all of it),
  then committed.

If no device of a second family is to hand, the test device's column is
filled, the other is marked missing, and the phase is not called done until
the user decides to go on without it.

## Phase 1 — the Vulkan image

`halo_guest_vk.elf`: the guest's objects with `d3d8_vk.c` (a copy of
`d3d8_gl.c` whose drawing is stubbed - every entry point keeps its state,
nothing draws) in place of `d3d8_gl.c`. The APK carries both images; the host
reads `display.renderer` and loads one, falling back to GL ES (logged) if the
Vulkan image is missing or Vulkan does not come up with what phase 0
requires. No GL context is made under Vulkan.

**Acceptance:** with `renderer = "vulkan"` the game runs to the menus and
plays (sound, input) with a black screen; with `"gl"` it is as before; a
device without Vulkan falls back with a line in the log.

## Phase 2 — the backend's skeleton

The command stream (`vk_commands.h`): targets, clear, present, each a fixed-
width record; one `host_vk_submit` per hand-over. The host: the driver
opened through `host_vk_driver.c` (`display.vk_driver`, logged with what was
opened), instance, device, one graphics queue, the swapchain (FIFO), frames
in flight behind fences, the game's render targets by address (made the
first time they are drawn into), clears, and present as a letterboxed blit
of the back buffer into the swapchain image. Every submission is numbered
and fenced; the guest can ask which number the GPU has finished
(`host_vk_retired`). The surface's loss and return (backgrounding),
`VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` (rotation: phase 0 saw it
on every turn) make the swapchain again; phase 0 saw no SDL lifecycle events
reach the loop, so the loss is found from the result codes.

**Acceptance**, on the phone's driver and on Turnip: the menus' clears show
(colour changes are visible), frames are paced by the display, backgrounding
and returning works ten times in a row, and a validation layer run is clean.

## Phase 3 — reading the game's memory

Everything a draw reads from the game's memory is copied at the hand-over
(the rule in "Where the renderer lives"):

- **Vertex, index and constant data** into an upload ring: a host-visible,
  host-coherent buffer per frame in flight, written as each command is
  processed, the draw's offsets pointing into it. A frame that needs more
  than the ring holds grows it for the next frame and, for this one, ends
  the command buffer early and starts another (never drops the draw). Phase
  0 found 4.2 to 6.8 MB a frame in the first map.
- **Textures** are copied into images when the guest uploads them (phase 6),
  through the same ring as a staging area.

**Per-resource busy tracking**, as deko3d's: a draw marks each resource it
reads with the submission it is in (the Xbox's `D3DResource.Lock`, as
`d3d8_dk.c`'s `resource_used`); `IsBusy`, `BlockUntilNotBusy` and the locks
wait for the GPU to pass it (`halo_resource_busy`/`halo_resource_wait`). With
everything copied at the hand-over the GPU never reads the game's memory, so
a lock waits only where the Xbox would have (the game relies on that timing
for its own buffers). A `Lock` higher than the submission being written was
not written by this device - map files carry headers with stale bytes there -
and counts as not busy (deko3d's first level load waited forever on one). A
wait longer than two seconds says so in the log.

**Acceptance:** a test draw reads vertices from the game's memory correctly
after the game rewrote them mid-frame (two draws from one rewritten buffer
show two different pictures); a lock of a buffer the GPU still reads waits,
and returns; on both drivers.

## Phase 4 — GLSL for Vulkan

Copies of the generators that write GLSL 450 for glslang: the vertex
shader's `std140` uniform block at set 0 binding 0 and the pixel shader's at
binding 1 (phase 0 found two blocks cannot share a binding), **every
member's offset written explicitly** and checked against the C structure the
host fills (a build-time or start-up check that compares every offset);
varyings with `location`s; each texture stage's sampler at its own binding.
The keys are the GL generators' keys.

**Proving it: the game's real shaders.** Off the device, as deko3d's phase 4
did with UAM: take every program a GL build has recorded
(`shader_programs.bin` from a session through the campaign's maps; only the
Switch's GL build records it today, behind `HALO_SWITCH`, so phase 4 starts
by recording it on Android or on Linux), generate both the GL and the Vulkan
GLSL from the same keys, and:

- compile every Vulkan one with `glslangValidator`, and validate the SPIR-V
  (`spirv-val`);
- reflect each one (`spirv-cross --reflect`) and check its block offsets and
  bindings against the host's structure;
- compare each pair: the same texture stages sampled, the same combiner
  inputs, the same outputs. **A shader that compiles cleanly and draws black
  is the failure this exists to catch**; it compiles, so only a comparison
  finds it.

**Acceptance:** every recorded program generates, compiles and validates; the
reflection and the comparison find nothing; the numbers in "Progress".

## Phase 5 — shader and pipeline cache

Vulkan has two compile steps where deko3d had one: GLSL to SPIR-V (glslang,
on the device), and SPIR-V plus state to a pipeline (the driver). A
pipeline's key is the two shaders' keys and every state that is not one of
the core dynamic states (Decisions).

As in deko3d's phase 5, a draw whose pipeline is not made yet is skipped
while one compile thread makes it, and drawn from the first frame it is
ready; the cache makes that happen once per device and driver. Unlike
deko3d's, there are no key files and no startup pass over known keys: a
device compiles what it meets.

### Step 1: glslang in the host
Built for Android as phase 0 built it (`libglslang_probe.so`, renamed for
the backend), loaded with `dlopen` by the backend, front end initialised
once.

### Step 2: the shader service in the host
- SPIR-V cached on the device by shader key, in
  `<internal storage>/shader_cache/<build>/`; a file that does not load
  (truncated, a different build) is deleted and the shader compiled again.
- The `VkPipelineCache` **per driver**: its file is named by the driver's
  `pipelineCacheUUID`, `driverID` and `driverVersion`, so Turnip's and the
  phone's never meet; loaded at start, saved when the game is paused or
  backgrounded and on exit; a file the driver rejects is deleted.
- One compile thread, fed from a queue; a key a draw asks for goes to the
  front. The thread compiles the SPIR-V if it is not cached and makes the
  pipeline; the recording thread finds it in an in-memory table by key
  (finished entries published under a lock, never half-made). A key whose
  compile fails is logged once with glslang's or the driver's message and
  marked failed, so its draws are counted, not retried every frame.

**Testing on the device**, on both drivers: a cold start (the log counts the
shaders and pipelines compiled), a second start (it compiles nothing), a map
loaded twice; a log line per 60 frames with draws, pipelines made and
draws skipped for a pipeline not ready.

**Acceptance:** the second start compiles nothing, on both drivers; switching
`vk_driver` and back keeps both caches; on a warm cache no draw is skipped
for a pipeline not ready; a failed compile names its key.

## Phase 6 — draws, textures and render targets

The largest phase: the equivalent of most of `d3d8_gl.c` on Vulkan. Read
`d3d8_gl.c` whole first, and `d3d8_dk.c` beside it: the deko3d version is
the same work against a command-buffer API, and its "Progress" entries are a
list of the traps.

### The order, each step tried on the device before the next, on both drivers

1. **First triangle, and the picture's orientation.** Immediate-mode draws,
   no texture, vertex colour. Vulkan's clip space has Y down and depth 0 to
   1: settle it here, once, in the viewport (a negative height) or the
   generated shaders, and nowhere else.
2. **The menus.** Textures (2D, the menus' formats), samplers, blending, the
   alpha test, `D3DPT_QUADLIST`. The PC menus and the Xbox ones both.
3. **A map.** Indexed draws, vertex declarations and every attribute type,
   depth and stencil, z bias, fog, cube and 3D textures, the remaining
   texture formats.
4. **Render targets.** Render-to-texture (the sniper's zoom), the mip
   composite (the water's ripples), split screen (the scissor follows the
   viewport; clears are clipped to it).
5. **Visibility tests** (lens flares): occlusion queries whose results the GPU
   copies to a host-visible buffer; the game reads a slot's latest count and
   never waits for the GPU.
6. **The high-res HUD and text, and the menus' art**, as images of their own
   (deko3d's step 6).

### Pitfalls already known (from deko3d's phase 6 and the earlier attempt)

- The device's state at a draw is the guest's to describe and the host's to
  apply; a target change resets viewport and scissor on the host.
- A pipeline's state must match the rendering attachments it is used with, or
  some drivers drop the draws silently (the earlier attempt's driver did).
- Images change layout; track each image's layout and transition from it.
- Descriptors bound by a recorded command must not be rewritten before the GPU
  is done; take a fresh set or use push descriptors.
- Barriers are recorded outside rendering.
- A texture written in place waits for the draws that read it; a render target
  sampled after it is drawn needs a barrier between.
- Constants a vertex format does not feed are reset when the format changes.

### Diagnostics, from the first step

- A log line every 60 frames: draws, immediate-mode draws, pipelines made,
  and draws skipped, by reason (pipeline not ready, pipeline failed, no
  target, a format the driver lacks).
- Every wait on the GPU logs once it has taken two seconds, with what it waits
  for; the queue's loss (`VK_ERROR_DEVICE_LOST`) is logged with the
  submission number and the driver.
- The validation layer in the debug APK, run at each step's end.
- A dump of generated GLSL behind a setting, into the app's own files.

**Acceptance:** the game looks under Vulkan as it does under GL ES, the menus
and a map, on both drivers, with the diagnostics clean - except where GL ES
draws wrongly, which is phase 7's subject.

## Phase 7 — the drivers compared

The point of the renderer: does a chosen driver draw the game correctly
where the phone's GL driver does not?

The devices that draw the game wrongly are not to hand: testers have them.
So this phase has two halves.

- **On the test device, three ways, the same scenes:** GL ES; Vulkan on the
  phone's driver; Vulkan on Turnip. Every map's scenes, especially split
  screen, water, lens flares, decals (z bias), fog, the HUD's meters and the
  PC menus. A table in "Progress", one row per scene. A difference between
  the two Vulkan drivers, or between Vulkan and GL ES where GL ES is right,
  is found here first.
- **A fault seen on both Vulkan drivers is the renderer's**, not a driver's,
  and is fixed in the renderer; a fault seen on one is reported with the
  driver's name and version.
- **Testers.** A build and short instructions (phase 8's README text, early)
  go to testers with the devices that break: run the probe once
  (`debug.vk_probe = "all"`) and send `vk_probe.txt`; then play the scenes
  where GL ES draws wrongly under GL ES, under Vulkan on their phone's
  driver and, on an Adreno, under Turnip; send a screenshot of each and
  `debug.txt`. Each report goes into "Progress" with the device, GPU, driver
  and what each renderer showed.
- **What the testers' reports decide:** a fault fixed by Turnip is the
  renderer doing its job; a fault fixed by Vulkan on the phone's driver too
  says the GL ES driver was at fault; a fault in every renderer is a bug in
  the game's port to find in `d3d8_gl.c` and its copy.

## Phase 8 — for players

- `port/android/README.md`: choosing the renderer, installing a driver
  archive (where to put it, the setting, what the log says when it is or is
  not used), which GPUs Turnip is for.
- The release build carries the Vulkan image, glslang and libadrenotools;
  CI builds it.
- **(to confirm)** The default renderer and driver, from phase 7's results on the test device and from testers.

---

## How this plan is worked

What made the deko3d plan go well, kept here on purpose:

- **Each phase is written out in detail before it is handed over**, with its
  files, its steps, how to test it on the device and what counts as done.
- **Each step is tried on the device before the next starts**, on the
  phone's driver and on Turnip. A step that is "done in the tree" and not
  seen on the device is not done.
- **Every result goes into "Progress"** with what shows it: what was seen,
  what the log said, which driver. "Progress" is newest first.
- **Work is audited against the source, not against its own description,**
  before it is committed.
- **Diagnostics come before the feature**: the draw counts, the two-second
  wait reports and the validation layer are in from phase 2, because a silent
  stop or a silently dropped draw costs hours without them.
- **The GL ES renderer stays untouched**, so the comparison in phase 7 is
  against something that does not move, and a tester always has a fallback.

## Lessons from the earlier attempt

A first Vulkan backend (branch `vulkan-backend-old`) reached geometry and depth
but never a correct frame. It is not the base of this plan; what it learned
is kept:

- It drew the line at the D3D8 level with fixed-width records, which was right
  and is kept.
- Its host read the guest memory a record named at the end of the frame,
  which the rule in "Where the renderer lives" forbids.
- It hooked into `d3d8_gl.c` and edited the shared generators in place,
  which the copying decision forbids.
- Several of its worst bugs compiled and ran without a word: every pixel
  shader generated as black (the key was never filled in), and a uniform
  block whose `std140` layout disagreed with the C structure from byte 3108
  on. Phase 4's offline proof is there to catch both kinds.
- The driver on its device dropped draws silently on a render pass/pipeline
  mismatch; treat a driver's silence as no evidence, and validate.
- It loaded drivers with libadrenotools (commits `cb2fbe06`, `693ffe90`,
  `4e38429b` on that branch, a reference for phase 0 B, not code to copy):
  the app must be packaged with its native libraries extracted; the custom
  driver directory needs its trailing `/`; a missing hook library makes every
  driver fail, the phone's included; the driver's library name comes from
  the archive's `meta.json`; the settings file must write text settings
  quoted, or an empty one makes the whole file invalid TOML (`main`'s
  `port_config.c` already does).
- Turnip faulted inside `vkUpdateDescriptorSets` under that backend. Phase 0
  B's `draw` step is there to find out why before anything is built on it.
- Device operations: never `adb uninstall` (it deletes the game's data and
  saves); install with `adb install -r` and an absolute path, and check
  `lastUpdateTime`; the settings file is rewritten on every run, so a setting
  must be in `port_config.c`'s table; do not reboot the device without asking.

---

## Risks

| Risk | Settled in |
|---|---|
| Turnip does not run inside this app (the guest's memory layout, the signal handlers, SDL), or crashes on descriptor sets | Phase 0 B |
| The validation layer does not load with a custom driver, so Turnip runs unvalidated | Phase 0 B (reported); `TU_DEBUG` as the fallback |
| The faults are in the renderer's logic copied from `d3d8_gl.c`, not in the driver | Phase 7 (a fault on both Vulkan drivers is ours) |
| A device without dynamic rendering | Phase 1's fallback to GL ES |
| Too many pipelines with static state, so many draws are skipped the first time a scene is seen | Phase 5's cache; extended dynamic state can be added |
| The devices that break are only testers' | Phase 7's tester round: the probe's report and screenshots per renderer and driver |
| Drivers differ: a device that drops draws, misreports, or lacks a format | Phase 0 on each driver, the validation layer, phase 7 |
| The surface is lost when the app is backgrounded or rotated | Phase 0 A (seen and handled), phase 2 |
| Players cannot install a driver | Phase 0 B (an archive in the data folder and one setting), phase 8 |

---

## Progress

*Phase 0, part A was worked under the earlier, speed-first version of this plan. Its measurements stand. Its proposals were settled in "Decisions" as rewritten (skipping a draw until its pipeline is ready: kept; everything dynamic: replaced by static state; memory read in place: replaced by copying; step 6: dropped), and its "(to confirm)" rows were removed from there.*

Phase 0 was worked on the test device (Lenovo TB321FU, Adreno 750, Android 16,
API 36, the phone's own driver). The reports are in `port/android/probe/reports/`.
Newest first.

### Phase 0, part A — summary

**Step 6 is partial, and no second GPU family was tried (not needed yet).** The probe's steps
1 to 5 are done and tried on the device; step 6 has the menu and the
standing-still map scene profiled, and not the fight scene, nor the
profiled/unprofiled comparison on the map (the user stopped profiling).
Everything marked (to confirm) in "Decisions" is a proposal for the user.
Still to do for the phase: the user's decisions, and, if the user wants them, the
fight scene; a Mali is not needed yet (the user's word) and none has been tried.

**Deviations from the spec:** see the next section, which lists them all.

### Phase 0, part A: deviations from the spec

Everything phase 0 did differently from what the "Phase 0 — spike" section above
says, or beyond it, in one place. Those marked *(agreed)* the user accepted when
asked; the rest are for the user to accept or to have undone.

**Device and build**

1. *(agreed)* **The debug build is installed beside the user's.** The device had build
   50 from GitHub Actions (package `com.halo.decomp`, signed with the release key),
   which a debug-signed build cannot replace (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`;
   `adb uninstall` is forbidden), and the validation layer is only found by a
   debuggable app. So the probe was worked as **`com.halo.decomp.vk`**, with
   `HALO_APPLICATION_ID_SUFFIX=.vk` in the environment of `ninja android_apk` (one line
   of `port/android/app/build.gradle`; without the variable the id is unchanged), the
   manifest's provider authority changed to `${applicationId}.update` and
   `UpdateProvider.AUTHORITY` to `BuildConfig.APPLICATION_ID + ".update"` (the same text
   in a normal build; two apps cannot share an authority). The data folder is
   `/sdcard/Android/data/com.halo.decomp.vk/files`, with the game's `maps/` copied in by
   adb. This touches the app's Java and Gradle files, which the spec did not list.
2. **`ninja android_apk` needed one more input.** Dropping `--android-vulkan-validation`
   left the layer in an APK ninja thought up to date, so `tools/android_build.py` writes
   `build/android/vulkan_validation.stamp` (rewritten only when the flag changes) and the
   APK depends on it. Not in the spec's list of build changes.
3. **Stale files removed.** The residue of the old branch in `build/android` (adrenotools,
   hook libraries, an older `libglslang.so`, its build folders) was deleted so it would not
   end up in the APK, as was `port/third_party/glslang/` as instructed. `build/` is ignored
   by git.
4. **The probe knows its build by a compile-time define** (`-DHALO_PROBE_BUILD`, the
   commit with `+changes` if the tree is dirty, taken at configure time), because the game's
   log prints no git hash for it to copy.
5. **Documentation beyond the spec's list:** `port/android/README.md` gained the three
   settings, the probe, the profiler and the `.vk` install under "Settings" and "Find
   problems" (the spec asked for the settings and "Find problems" only).
6. **The GL ES path** is unchanged apart from the three `port_config.c` rows and `host_debug.c`'s
   profiling mode (off unless set), as the spec allows.

**The probe's steps**

7. **`caps` always runs**, whatever `debug.vk_probe` lists, because the device, the
   extensions and the features are queried by the setup that every step needs; it is
   skipped only if it killed the probe once. The spec says only listed steps run.
8. **`present` does not use SDL's lifecycle events** (`SDL_EVENT_WILL_ENTER_BACKGROUND` and
   the like): none ever arrived in the probe's loop (they are still handled if they come).
   The surface's loss is found from `VK_ERROR_SURFACE_LOST_KHR`; the loop retries
   `SDL_Vulkan_CreateSurface` until the app is back. The spec's step 2 assumes the events.
9. **`present`'s orientation marker.** The spec has the tester judge whether the picture is
   upside down, which a flat colour cannot show. The probe draws a white 160-pixel square
   with a black corner at the image's top-left, by a buffer copy in the whole-image-clear
   mode and by `vkCmdClearAttachments` in the other two. Three clearing modes are cycled by
   X (the spec says two: a clear and a render pass): `vkCmdClearColorImage`, dynamic
   rendering, a render pass.
10. **`present` was partly driven by adb**, not only by the tester: the user did the script
    once (four Home round trips, the turn-over, A, X three times, B) and I did ten Home round
    trips, the rotation (by `settings put system user_rotation`) and the clearing modes with
    adb key events (letters arrive as keyboard keys; adb cannot press gamepad buttons).
    Also, the tester's answers about the marker's position and the tone were not given.
11. **`memory`'s import cases could not run** (`VK_EXT_external_memory_host` is absent on
    the test device). They are written but **not exercised on any device**, nor are the
    negative tests behind `guard_begin("memory.negative")`; the report says "not possible".
12. **`memory` has an extra case** the spec does not have: an allocation exported as an
    opaque file descriptor (`VK_KHR_external_memory_fd`, enabled on the device for it) and
    mapped with `mmap` below 4 GB, with the CPU's speed there. Its figures are in step 3.
13. **`memory`'s GPU timing** is of a whole pass (a clear and one draw, timestamps outside the
    pass: on a tiler a timestamp inside a pass does not bracket the work) with a one-triangle
    baseline pass taken away, not of the draw alone; and a device-local variant (with and
    without the 4 MB copy) is measured as well. The ring's size is the spec's 4 MB: step 6's
    renderer log gave 4.2 to 6.8 MB mirrored and 0.2 to 0.7 MB streamed a frame, and the probe
    was not rerun at that size.
14. **`compile`: the shaders were converted by a script, not by hand** (it keeps the bodies
    and changes only what the spec lists), and checked by eye; the script is not in the tree.
    Layout differences from the spec's text: the vertex block is at set 0 binding 0, the
    **pixel block at binding 1** (two blocks cannot share a binding) and the samplers at
    bindings 2 to 5. Added to every shader: the specialization constant `PROBE_SALT` that
    step 5 gives a value of its own per run. The shaders are named `vs_large`, `vs_typical`,
    `ps_large`, `ps_typical`, plus `pass`; each says which dump file it is from. The dump
    covered the main menu, a new campaign's opening and its cinematics (150 files), not "the
    first fight".
15. **glslang targets Vulkan 1.0 / SPIR-V 1.0** (the spec does not say). The SPIR-V was not
    checked with `spirv-val` or `spirv-dis` off the device (not installed here); the
    validation layer and the driver accepted it.
16. **`compile`'s first-compile figure** is only the very first compile of a process in a run
    of `compile` alone; in a run of `all` step 3 loads glslang first (it compiles the
    pass-through shaders). `_compile_only.txt` is that run. The library's size in memory
    could not be read from `/proc/self/maps` (it prints 0): the APK's entry is the figure.
17. **`pipelines`: the second thread's frames draw with the pass-through pipeline** (an
    already-made one, as the spec says), not with a game pipeline; the pipelines' vertex
    layouts for the game's shaders were written by hand from the converted shaders' inputs
    (16 `vec4`; and a `vec4`, three packed words and twelve `vec4`). No descriptor sets were
    allocated or bound (no draw uses the game's pipelines).
18. **`pipelines`' warm figure uses the first run of each pair** of the previous probe run
    (a salt for each pair, saved with the cache), and the saved cache holds all of that run's
    cold pipelines; the case "the driver's own cache, ours empty" is added to it.
19. **Reports in `port/android/probe/reports/`** are `lenovo_tb321fu_adreno750_*`: `all_cold`,
    `all_warm`, `all_validation`, `compile_only`, and the two profile reports. The spec asks
    for a cold and a warm run and a validation run of the same `all`; the three are, but the
    tester's part of `present` in them was answered by adb key events, and the one with the
    user at the device is not kept as a file (its lines are in "Progress").
20. **Settings read twice:** the probe reads `debug.vk_validation` itself from `config.toml`,
    because `host_vk_probe_run(steps, data_root)` has no room for it in the spec's signature.

**Step 6, the profiling mode**

21. **Partial.** The menu and the first map standing still (in the cryo-tube, `look:1`, a
    new game's cinematics played first, about ten minutes) were profiled; **the fight scene
    and the unprofiled run of the map scene were not done** (the user stopped profiling).
    The cost of profiling itself is judged only from the menu: 164.9 fps profiled and 165
    without, which is not the spec's comparison.
22. **The handler takes no locks, and so differs from the first design:** it reads the frame
    chain with `process_vm_readv` (the first version's `host_low_owns` took the memory
    lock and hung the game at 1000 Hz). It also flags a sample taken after a system call
    (blocked), and the file carries each thread's name and the loaded objects' base
    addresses (`#dl` lines after the maps: a library mapped from the APK shows as the APK in
    `/proc/self/maps`). The profiling signal is queued with `rt_tgsigqueueinfo` and a tag so
    that `debug.sample_seconds` (`tgkill`) keeps working beside it.
23. **The report script's attribution is a heuristic**, not the spec's mapping by address
    alone: guest code by the object file of the linker's map (the guest image has no line
    info), the host library and vendor libraries by the loaded object, libc and the like by
    the first caller on the frame chain that is not one (system libraries have no frame
    pointers, so many chains are garbage and those samples count as the driver's). It reads
    only samples between `--from` and `--to` seconds (blocks of ten seconds) and takes frames
    per second from the renderer's log lines.

### Step 6: the GL ES path's costs (partial)

GL ES build `2d2a322e+changes`, `profile_hz = 1000`, `gpu_stats = true`; 0 samples
dropped; frames per second from the renderer's log (the run with profiling on).
The shares are of a thread's samples; a sample taken in libc is the driver's unless
a caller on the frame chain says otherwise, which is a heuristic (system libraries
carry no frame pointers, so the chain past them is often garbage); guest code is
named by the object file the linker's map puts the address in.

| Scene | fps | Main thread CPU | game | renderer | boundary | driver | other (idle in a system call and the rest) |
|---|---|---|---|---|---|---|---|
| Main menu, idle (seconds 20 to 80) | 164.9 | 540 ms/s, **3.27 ms/frame** | 8.5% | 3.6% | 0.6% | 67.7% | 19.5% |
| First map, standing still in the cryo-tube, `look:1` (seconds 580 to 650; 184 draws, 178 immediate) | 110.8 | 715 ms/s, **6.45 ms/frame** | 18.5% | 5.0% | 1.9% | 51.4% | 23.2% (22.5% of it blocked) |

The other threads: six guest threads cost 40 to 110 ms/s each (0.4 to 0.7 ms a
frame), four of them (all in libc, called from the host's boundary) polling or
sleeping and two in other code (probably SDL's audio). Not measured: the fight scene; the cost of
profiling itself (the menu ran at 164.9 fps profiled and at the display's 165 Hz
without, from the log's frames: no visible cost there; the map scene was not run
unprofiled). The renderer's own log: the first map's cinematic and opening drew
180 to 525 draws a frame with **4.2 to 6.8 MB "mirrored" and 0.2 to 0.7 MB
"streamed" a frame**: the ring of step 3 used 4 MB, so it is the right size.

**Does step 6 support the case for the renderer? (to confirm)** Moderately,
and not yet enough to say more. About half of the game thread's samples in the
map (51%) and two thirds in the menu are the vendor driver and what it calls, and
the renderer's and the boundary's own code is under 7% of it: those are the
costs a Vulkan renderer removes or cuts (driver validation, compile at link time).
But part of the driver's share is the wait for the display (22% of the map's
samples are blocked in a system call), the attribution of libc samples is a
heuristic, the fight scene is missing, and the main thread used 72% of a core at
110 fps in the map, not all of a core: the game is not CPU-bound in the scenes
measured. The fight scene (the heaviest) is the one that could change this.

### Step 5: pipelines

`pipelines`, `Adreno 750`, `lenovo_tb321fu_adreno750_all_cold.txt` and `_all_warm.txt`
(no validation layer; every figure is the first run, then min and median of five
more; the device's speed varies by up to 1.5x between runs, so compare within a run).

- **Formats and state:** colour `R8G8B8A8_UNORM`, depth `D24_UNORM_S8_UINT`, dynamic
  rendering (no render pass), alpha blending on, depth test on. Dynamic states, 22:
  `VIEWPORT_WITH_COUNT`, `SCISSOR_WITH_COUNT`, `LINE_WIDTH`, `DEPTH_BIAS`,
  `BLEND_CONSTANTS`, the three stencil states, cull mode, front face, primitive
  topology, depth test, write and compare op, depth bounds test enable, stencil test
  enable and op, rasterizer discard, depth bias enable, primitive restart enable,
  `LOGIC_OP`, `VERTEX_INPUT_EXT`; the stride state is left out when the vertex input is
  dynamic.
- **Cold** (a salt of its own in the shaders' specialization constant, a cache that
  starts empty), `vkCreateGraphicsPipelines`: pass-through pair first 2.8, then min 1.1
  ms, median 1.2 ms; the typical pair first 8.6 ms, min 5.2 ms, median 5.2 ms; the large
  pair first 19.6 ms, min 12.0 ms, median 12.0 ms. `vkCreateShaderModule`: 0.05 to
  0.8 ms.
- **From the saved cache:** a 94,351-byte file (header version 1, vendor 0x5143,
  the device's UUID) is kept by the driver; the same pipelines cost **0.095, 0.090
  and 0.121 ms**. With our cache empty and only the driver's own: 1.8, 7.9 and 18.5
  ms, so the driver's cache does not help; ours is the only one that does.
- **Without dynamic state** (only viewport and scissor): 1.2, 7.2 and 16.9 ms, against
  1.2, 5.2 and 12.0 with it: dynamic state saves creation time on this driver.
- **A second thread while the first draws** (a clear and 100 draws every 16 ms,
  `submit to fence` time): before 1.8 ms median (1 frame of 188 over twice the median),
  **during (20 pipelines, 190 ms in all, 9.5 ms each): 1.5 ms median, 0 of 12 over**,
  after 1.8 ms (0 of 187). Creating pipelines on another thread does not stall the
  queue.
- Validation: clean (`_all_validation.txt`: 2 messages, both information about the
  layer's own cache file under `/tmp`, which an app cannot write).

### Step 2: a frame on the screen

`present`, with the user at the device and by adb.

- **Presenting works**: FIFO, 6 images, `R8G8B8A8`/`B8G8R8A8` (the surface lists both),
  usage colour attachment and transfer destination. The panel runs at **165 Hz**: the
  interval between presents is 6.04 to 6.08 ms median (165.0 to 165.6 Hz), over 10 s
  windows 0 or 1 of 1024 intervals over 1.5x the median, max 7 to 15 ms, apart from the
  moments the app was away.
- **The three ways of clearing** (a whole-image `vkCmdClearColorImage` plus a copy of
  the marker, dynamic rendering, a render pass) all ran, cycled by X on the user's
  gamepad, with a clean validation layer.
- **Backgrounding:** 4 Home round trips by the user and 10 by adb: **each lost the
  surface** (`VK_ERROR_SURFACE_LOST_KHR`) and was remade with a new surface and swapchain
  in 3.0 to 9.0 ms (after one or two retries while the app was still away), every time.
  10 of 10, validation clean.
- **Rotation:** the surface's `currentTransform` is **`ROTATE_90` from the start** on
  this tablet (extent 2560x1600), and turning the device (180 degrees, and the
  settings' `user_rotation`) made the driver report `VK_SUBOPTIMAL_KHR` each time: the
  swapchain was remade with the new `currentTransform` (identity, rotate_90, rotate_270
  seen) in 3.8 to 8.4 ms, never an error. The user pressed A (the picture looks right)
  and never Y. The user did not say where the marker sat or whether the tone was heard.
- **SDL beside Vulkan:** the gamepad (a GameSir Nova 2 Lite) opened and its buttons arrived
  (A, X, X, X, B), keyboard keys arrived too (adb's `KEYCODE_A` and so on); the audio
  stream opened and queued a tone.

### Step 4: the shader compiler

glslang 16.6.0 (`e1b562a8`), `ENABLE_OPT` and `ENABLE_HLSL` off, built into
`libglslang_probe.so` (3,767,008 bytes in the APK, stored; the C++ runtime inside it),
dlopened by the probe; Vulkan 1.0 / SPIR-V 1.0 target; one thread. Shaders: the dump of
the GL ES game (150 files: 23 vertex, 127 pixel) from the main menu, a new campaign's
opening and cinematics; converted by a script (`port/android/probe/*`, each says which
dump file) as the plan describes, and the specialization constant added. `compile` run on
its own, no validation (`_compile_only.txt`):

| Shader | GLSL bytes | SPIR-V bytes | first | min | median |
|---|---|---|---|---|---|
| pass.vert | 677 | 1,208 | 100.7 ms (the very first compile) | 0.90 ms | 0.93 ms |
| pass.frag | 328 | 632 | 0.91 ms | 0.82 ms | 0.87 ms |
| vs_typical.vert (vs004_1, median) | 6,366 | 17,660 | 3.3 ms | 2.1 ms | 2.2 ms |
| ps_typical.frag (ps_e4a3b3ae, median) | 4,100 | 9,356 | 1.7 ms | 1.5 ms | 1.5 ms |
| vs_large.vert (vs051_0, largest) | 11,374 | 35,380 | 4.3 ms | 3.8 ms | 3.9 ms |
| ps_large.frag (ps_297f687f, largest) | 8,568 | 24,184 | 3.1 ms | 2.9 ms | 2.9 ms |

`dlopen` and `glslang_initialize_process`: 4.5 ms. Thread CPU time is within 3% of
the wall time. All six compiled the first time; the SPIR-V was accepted by the driver
under the validation layer (steps 3 and 5), which validates SPIR-V; **it was not run
through `spirv-val` or `spirv-dis` off the device (neither is installed here)**. (In a
run of `all`, step 3 loads glslang first, so its "first compile" is not the very first.)

### Step 3: the game's memory, seen by the GPU

`memory`, no validation for the timings.

- **Import: not possible on this device**: `VK_EXT_external_memory_host` is not offered
  (the instance is 1.4.0, the device 1.3.128, 132 device extensions). `minImportedHostPointerAlignment`
  has nothing to report. The import, copy, vertex-fetch, index-fetch and texture cases
  did not run.
- **The ring (4 MB a frame):** the only host-visible type a vertex buffer can use is
  type 6 (`device_local host_visible host_coherent host_cached`), so there is no
  host-cached non-coherent variant to flush. The copy into it, 300 frames: **0.089 ms
  median (min 0.084, p99 0.15–0.28, max 1.1 ms) of wall and of thread CPU**. GPU time of
  a pass drawing 116,508 small triangles from the ring against from device-local memory
  (timestamps; the baseline pass that draws one triangle is 10 and 27 us in the two runs):
  host-coherent 350 us (run 1) and 654 us (run 2), device-local 355 and 727 us: **the same,
  within the run-to-run variation of the GPU's clocks, which was 1.9x**. A 4 MB copy into
  device-local memory costs about 210 us of GPU time on top.
- **Beyond the spec: an allocation exported as an opaque file descriptor**
  (`VK_KHR_external_memory_fd`), mapped with `mmap` (`MAP_SHARED | MAP_FIXED`) over a
  range of guest-kind memory below 4 GB: it mapped, was the same pages as the driver's own
  mapping, **the GPU read the CPU's writes and then its rewrites** (copy to a readback
  buffer), and the CPU's speed there is close to ordinary memory (8 MB: write 17,800 to
  23,800 MB/s against 23,800 to 28,100; read 27,700 to 38,300 against 26,900 to 40,900).
  The memory type it uses is type 4 (`device_local host_visible host_coherent`), as a
  dedicated allocation; no host-cached exportable type exists for the buffer
  (`memoryTypeBits` 0x13). This is the way to read the guest's memory in place on a device
  without host-pointer import. Not tried: mapping at the Xbox window's own address.
- Validation: clean. (An earlier run found my bug in the cleanup of buffers never made;
  fixed.)

### Step 1: what the device has

`caps`, in full in `_all_*.txt`; the column of the table below.

| | Adreno 750 (Lenovo TB321FU, `qcom`/`pineapple`, `ro.hardware.vulkan` adreno) | Mali (second device) |
|---|---|---|
| Versions and driver | instance 1.4.0; device **1.3.128**; driver 512.762.40, "Qualcomm Technologies Inc. Adreno Vulkan Driver", build 8924aaec70, conformance 1.3.6.0; vendor 0x5143, device 0x43051401 | **missing** |
| Dynamic rendering | yes (core 1.3, the extension too) | |
| Extended dynamic state 1, 2 | yes, yes (logic op yes, patch control points yes) | |
| Extended dynamic state 3 | **no** (the extension is not listed) | |
| Vertex input dynamic state | yes | |
| Host pointer import | **no** (`VK_EXT_external_memory_host` absent); `VK_KHR_external_memory_fd` yes, `VK_ANDROID_external_memory_android_hardware_buffer` yes | |
| Push descriptors | yes, `maxPushDescriptors` 32 | |
| Custom border colour, 4444 formats | yes (with and without format), yes (A4R4G4B4 and A4B4G4R4) | |
| Maintenance4 / 5 | yes / no | |
| Pipeline creation cache control | yes | |
| Graphics pipeline library | **no** (nor `VK_KHR_pipeline_library`) | |
| Core features | BC yes, ASTC yes, ETC2 yes, anisotropy yes, precise occlusion yes, depth bias clamp yes, depth clamp yes, non-solid fill yes, independent blend yes, logic op yes, clip distance yes, wide lines yes, pipeline statistics yes | |
| Limits | 32 vertex attributes and bindings (offset 4096, stride 2048); uniform range 65,536 B; push constants 256 B; 7 descriptor sets; 16 anisotropy, 16 LOD bias; 2D 16,384, 3D 2,048; 8 colour attachments; uniform alignment 256; non-coherent atom 1; copy offset alignment 64; timestamps 48 valid bits, 52.08 ns | |
| Memory | heap 0: 11,273 MB device-local; heap 1: 4,095 MB (protected); a vertex buffer can use types 0 and 6 only (6 is host-visible, coherent and cached) | |
| Formats that matter | `BC1/2/3`: sampled, linear, transfer both ways (not renderable); every 16-bit colour (`R5G6B5`, `B5G6R5`, `A1R5G5B5`, `R5G5B5A1`, `B5G5R5A1`, `R4G4B4A4`, `B4G4R4A4`, `A4R4G4B4`) and the 8 and 16-bit ones (`R8`, `R8G8`, `R16`, `R16G16`, `R8G8_SNORM`, `R16G16_SNORM`) sampled, linear, renderable, blendable; depth: `D16`, `D24_S8`, `X8_D24`, `D32`, `D32_S8` all depth-stencil attachments and sampled (`D32_S8` without linear) | |
| Surface | 5 to 64 images, extent 2560x1600, transform `ROTATE_90` (all 9 supported), composite alpha inherit only, usage 0x9f, 5 formats (37, 43, 4, 97, 64) all colour space 0, present modes mailbox and FIFO, and the two shared-present modes (no immediate) | |
| Presenting, pacing | 165 Hz, steady (see step 2) | |

The validation layer is loaded from the app's own library folder in the debuggable
`.vk` build: `VK_LAYER_KHRONOS_validation` 1.4.363 (`vulkan-sdk-1.4.363.0`'s Android
release), 27.7 MB in the APK; `--android-vulkan-validation` puts it there, and nothing
puts it there otherwise (configure removes a staged copy).

### The build and the files

glslang is fetched at configure time (`tools/android_build.py`, pinned to 16.6.0 and
built by `port/android/probe/glslang/CMakeLists.txt`); `configure.py
--android-vulkan-validation` adds the validation layer. The probe is
`port/android/host/host_vk_probe.c` (about 4,900 lines, one file), run by
`host_main.c` when `debug.vk_probe` is set; `host_debug.c` has the profiler;
`tools/android_profile_report.py` reads it. The three settings are rows of
`port/linux/src/port_config.c` (`_platform_android`); that is the only change under
`port/linux/src`. The GL ES game with `vk_probe = ""` ran the menu, a new game's
cinematics and the first map throughout steps 3 to 6 on the same build. Audited against
the specification's valid usage (below) and the layer, then committed.

**The audit** (valid usage, by hand, beyond the layer's silence): the layer was clean in
every run of `all`, and the code was also read for what the layer cannot see: the
pipelines' dynamic state is complete before every draw (each listed state is set; depth
bounds is not made dynamic, as its feature is off); no dynamic state is listed twice
(counted viewport replaces the plain one; vertex stride is left out with the dynamic
vertex input); an attachment's layouts are changed from `UNDEFINED` before each clearing
pass and the render pass's final layout matches the barrier after it; the swapchain is
destroyed before its surface and recreated with `oldSwapchain`; semaphores are per
frame for acquire and per image for present; timestamps are written outside render passes;
every host read of GPU writes has a transfer-to-host barrier; every `vkMapMemory` is
matched; the exported-memory descriptor is closed after the mapping. Found and fixed in
the audit: buffers freed without having been made (the layer's error); the features
chain never queried (the first caps run); a handler that took a lock.


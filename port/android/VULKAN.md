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

Status: phase 0 is done on the test device (Adreno 750) and committed, parts A
and B: the probe runs on the phone's own driver and on Turnip, loaded by the
app, and Turnip draws the game's shaders with descriptor sets, identically to
the phone's driver; the results are in "Progress". Not tried: a second GPU
family (a Mali). **Phase 1 is done on the test device and committed** (the
Vulkan image, the host's choice and its decision, the stand-ins: the game
runs under `display.renderer = "vulkan"` on the phone's driver and on
Turnip, with a black screen as the phase says). **Phase 2 is done on the test device and committed** (the command stream, the device, the swapchain, the targets, clears and present: the game's clears are on the screen through Vulkan on both drivers). **Phase 3 is done on the test device and committed** (data records, the 4 MB stream, the upload rings and the data table; the data self-test is `ok` on both drivers). **Phase 4 is done on the test device and committed** (the GLSL generators for glslang, the pixel shader key, the dump, the PC check and the probe's `shaders` step: all 308 shaders of the corpus compile, validate and compare clean on both drivers and on the PC). **Phase 5 is done on the test device and committed** (glslang in the backend, the shader and pipeline services with their compile thread and the per-driver pipeline cache, tried cold and warm on both drivers). **Phase 6 is written (steps 1 to 4 tried on both drivers, step 5 on the phone's driver for the opening, step 6 not run) and waits for the user's testing: see the summary under Progress.** The
work is on the `vulkan-backend` branch, which starts again from `main`. An
earlier attempt, kept on the `vulkan-backend-old` branch, is not the base of
this work and nothing here builds on it; see "Lessons from the earlier
attempt".

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
| The GL ES renderer | **Kept, selectable for good**, and the automatic fallback. ~~Decided (the user, after phase 1): it stays the default~~. **Changed (the user, after phase 6): Vulkan is the default for every phone** (`display.renderer = "vulkan"`; an existing config.toml's `"gl"`, the old default, is moved to `"vulkan"` once by the launcher, `vulkan_default.txt` marking it), on the phone's own driver (`display.vk_driver = ""`). Turnip is the player's to turn on: on an Adreno the launcher downloads the K11MCH1 `v26.0.0-rc08` build for the GPU's series (pinned by size and SHA-256) and names it in `vk_driver_auto.txt`, and `display.vk_driver = "auto"` uses it. GL ES is the player's choice (`"gl"`) and the fallback when Vulkan cannot start. |
| A shader or pipeline not compiled yet, met during play | **The draw is skipped** while it compiles on another thread, as for deko3d; it is drawn from the first frame its pipeline is ready. The compiled SPIR-V and the `VkPipelineCache` are kept on the device so that this happens once per device and driver, not once per run. |
| Shader compiler | **glslang on the device** (phase 0 measured 2 to 4 ms for a game-sized shader, 100 ms for the first compile of a process, 3.8 MB of library). |
| The game's memory | **Copied by the guest at each draw** into the command stream, and by the host into an upload ring at the hand-over; never read in place. The test device cannot import host memory, and one path on every driver is simpler to get right. The copy is made at the draw, not at the hand-over, because the game rewrites buffers between draws of a frame (phase 3). Since the GPU then never reads the game's memory, **no busy tracking**: `IsBusy` stays false and locks never wait, as under `d3d8_gl.c`, which also copied at the draw (decided in phase 3's write-up; deko3d needed it because its GPU read the window in place). |
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

**What a draw reads is copied when the draw is made, by the guest, into the
command stream; the host copies it out of the stream when the stream is
handed over, and never reads the game's memory for a draw.** The game
rewrites vertex buffers, index buffers and constants within a frame, and
frees and reuses memory between draws, and a frame's commands are handed over
once, at its end: a host that read the game's memory at the hand-over would
draw every draw with the frame's last bytes (the earlier attempt did). The
guest's copy is the bytes as they were at the draw, which is what
`d3d8_gl.c` uploads to GL at the draw too. Since the GPU reads only those
copies, a resource the game locks or reuses is never in use by the GPU, and
nothing waits for it.

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
- **Part B — the probe on a driver the app loads. Done.** Turnip first.

Part A's step 6 (profiling the GL ES path) belonged to the speed plan and is
not part of this one: its results stay in "Progress" as a record, and the
profiling mode stays in `host_debug.c` (off unless set) as a tool.

### Part B — a driver the app loads (done)

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

A second game image whose Direct3D device draws nothing yet, chosen by
`display.renderer`, and the host's decision of whether Vulkan comes up at all.
The deko3d plan's phase 1 is the model (`port/switch/DEKO3D.md`, and its commit
`25c02a0a`, "Add a deko3d renderer to the Switch build, chosen in
config.toml": read its diff first; most of this phase is that commit on
Android).

### What this phase is, and is not

- **It is** the Vulkan image (`halo_guest_vk.elf`), the host choosing between
  the two images, the host bringing Vulkan up as far as an instance and a
  physical device and deciding from that, and the stand-ins that let the
  game's platform layer run with no GL context.
- **It is not** drawing or presenting: the screen stays black under Vulkan.
  No logical device, no swapchain, no command stream (phase 2).
- **The GL ES image does not change**, nor do the files it is built from,
  except the settings table.

### Files

| File | What |
|---|---|
| `port/android/guest/d3d8_vk.c` | the Vulkan image's Direct3D device: a copy of `d3d8_gl.c` with every OpenGL call taken out (below) |
| `port/android/host/host_vk.c`, `host_vk.h` | the backend's first part: Vulkan brought up to a physical device, and the decision (below); phase 2 grows it |
| `port/android/host/host_main.c` | reads `display.renderer`, reserves the image's range, runs the decision, loads one image |
| `port/android/host/host_loader.c`, `host_memory.c` | the image's range reserved before it is loaded (below) |
| `port/android/host/host_sdl.c`, `host_gl.c` | the stand-in GL context, the real window without `SDL_WINDOW_OPENGL`, frame pacing (below) |
| `port/android/host/host.h` | `host_renderer_vulkan`, as the Switch's `host_renderer_deko3d` |
| `tools/android_build.py` | links `halo_guest_vk.elf`, stages it in the APK beside `halo_guest.elf` |
| `port/linux/src/port_config.c` | `display.renderer` for Android; `debug.vk_validation`'s text says it applies to the renderer too |
| `port/android/README.md` | `display.renderer`, marked in development |

### The Vulkan image

`halo_guest_vk.elf` is linked from the same objects as `halo_guest.elf`, with
`d3d8_gl.o` replaced by `port/android/guest/d3d8_vk.o` (as `switch_build.py`
makes `halo_guest_dk.elf`: the object list without the GL renderer's, plus the
copy). Everything else is shared in this phase, `xbox_textures.c`,
`nv2a_vsh.c`, `nv2a_psh.c`, `hud_hires.c`, `text_hires.c` and `menu_files.c`
included: their own GL calls (listed below) reach no context and do nothing
(Android's GL ES library answers a call made without a current context with a
no-op), which is acceptable until phase 6 replaces them. The APK carries both
images as assets.

**`d3d8_vk.c` is a copy of `d3d8_gl.c`**, then cut down. It keeps, as they
are: the screen's width and scale and everything `halo_screen_*` exports, the
XDK's device and state, the vertical blank and its thread and callbacks, the
reserved viewport constants, render and texture stage state, transforms,
vertex shaders and declarations, streams and immediate mode, the surfaces and
render targets as the game sees them, and every function another object
calls. It drops every `gl*` call and every `host_gl_*` import; what drew,
cleared or uploaded becomes a function that keeps its state and returns.
Answers the game reads back are the GL build's where they do not depend on
the GPU, and otherwise: `IsBusy` false, `BlockUntilNotBusy` and locks return at
once, visibility tests report 0 samples. `d3d8_dk.c` at `25c02a0a` is the
reference for what such a device keeps; it was written from the same
`d3d8_gl.c` for the same reason. **The link is the check**: every symbol
`d3d8_gl.o` defines that another object uses must be defined by
`d3d8_vk.o`, and none may be a stub that changes what the game sees.

A comment at the top of `d3d8_vk.c` says what it is, which `d3d8_gl.c` it was
copied from (the commit), and that changes to `d3d8_gl.c` are not followed
automatically.

### The image's range, reserved first

`host_load_image` reserves the image's range (`host_memory_initialize`) and
places the memory window as it loads, and the existing comment in
`host_main.c` says why that comes before the display: a driver's own
mappings can land on the image's fixed address. Bringing Vulkan up maps
memory the same way, and must happen before the host knows which image to
load. So the reservation is split from the load:

1. Read both images' program headers from the APK (`SDL_LoadFile` on each,
   as now for one), and reserve, at `HALO_GUEST_IMAGE_BASE`, the larger of the
   two spans, then place the window, as `host_memory_initialize` does now.
2. Bring Vulkan up and decide (below), if `display.renderer` asks for it.
3. Map the chosen image into the reservation; the other image's data is
   freed.

`host_load_image` gains the form that loads into a reservation made before;
the reservation and the window placement keep their order and their log
lines. A device where the reservation fails fails as it does now.

### Bringing Vulkan up, and the decision

`host_vk.c`, `host_vk_startup(const char *vk_driver)`, run by `host_main.c`
after the reservation and SDL's video, when `display.renderer = "vulkan"`:

1. `host_vk_driver_open(display.vk_driver)`.
2. An instance: Vulkan 1.3 if the loader offers it, otherwise 1.1; extensions
   `VK_KHR_surface` and `VK_KHR_android_surface` (required),
   `VK_KHR_get_physical_device_properties2` where the instance is 1.0-level;
   `VK_LAYER_KHRONOS_validation` and `VK_EXT_debug_utils` when
   `debug.vk_validation` is true and the layer is there, its messages logged
   (tag `halo`, prefix `vk:`), with a count of errors kept for phase 2's
   diagnostics.
3. `host_vk_driver_verify()`. A failure ends here (below).
4. A physical device with: a graphics queue family; `VK_KHR_swapchain`;
   dynamic rendering (Vulkan 1.3's feature, or `VK_KHR_dynamic_rendering`
   with what it depends on before 1.2). The first that has all of these;
   none is a failure.
5. Kept, for phase 2: the instance, its function table, the physical
   device, the queue family, the API version to use. Nothing is made twice.

The entry points are loaded into a table as the probe does (an X-macro list,
`VK_NO_PROTOTYPES`, a missing one logged by name and treated as a failure),
written afresh in `host_vk.c` for the backend: phase 2 adds the device's.

**The log line**, one, either way: `renderer: Vulkan on <driver description>,
<deviceName>, Vulkan <api>, <driverName> <driverInfo>` or `renderer: GL ES
(Vulkan was asked for: <reason>)`.

**A failure** (no driver, no instance, the check failed, no device with what
is required, a missing entry point) destroys what was made, logs the reason
and loads the GL ES image. After a custom driver failed this way the process
keeps it loaded (`host_vk_driver.h`); the GL ES renderer does not use Vulkan,
so that is harmless.

**`display.renderer`**: `"gl"` (the default) or `"vulkan"`; anything else is
`"gl"` with a warning. A row in `port_config.c` for Android (`#ifndef
HALO_SWITCH`, the Switch has its own with `"deko3d"`). `debug.vk_probe` still
runs the probe first, as now, whatever the renderer is.

### The stand-ins

Under Vulkan the game's platform layer (`sdl_platform.c`, shared) still
creates an SDL window with `SDL_WINDOW_OPENGL`, a GL context, a swap interval,
and swaps at each present. In `host_sdl.c`, under `host_renderer_vulkan`:

- **The window is real**, made without `SDL_WINDOW_OPENGL` (the flag is
  taken out), so SDL makes no EGL surface on it: phase 2 makes its Vulkan
  surface on it, and the probe has shown such a window takes one. Its size,
  input and lifecycle are SDL's as now.
- **The GL context is a stand-in**: a handle to nothing that
  `make_current`, `set_attribute` and `set_swap_interval` accept (the interval
  is kept for pacing).
- **The swap paces the frames**, as the stand-in swap does on the Switch:
  with a swap interval above 0, frames are held to the display's refresh rate
  (`SDL_GetCurrentDisplayMode`), so the game runs at the rate it runs at
  under GL ES, until phase 2's swapchain paces them instead
  (`host_vk_presenting`, set by phase 2). Every 10 seconds the log says how
  many frames were swapped.
- `host_gl.c`'s `host_gl_get_string` answers `"Vulkan"` for the renderer and
  version strings and nothing for extensions, so the platform layer's
  `OpenGL %s on %s` line prints sense instead of what a GL library answers
  with no context.

The GL calls the shared files still make under Vulkan are listed by the
agent in "Progress" (file, function, how many calls a frame in the menus,
from a count kept in `host_gl.c` behind the stand-in), so that phase 6 knows
what to replace.

### Testing on the device

On the test device, the `.vk` build:

1. `renderer = "gl"`: the game as before: the menus, a new game, the first
   map.
2. `renderer = "vulkan"`, `vk_driver = ""`: the log says Vulkan on the
   phone's driver; the screen is black; the menus run (the sound plays, keys
   move through them: adb's `KEYCODE_DPAD_DOWN` and `KEYCODE_ENTER` drive the
   main menu), a new game starts and the map's sound and the game's log go
   on; the frame count every 10 s is the display's rate. Backgrounding and
   coming back ten times does not stop the game.
3. The same with Turnip (`vk_driver = "Turnip_v26.0.0_R8.zip"`).
4. With validation on, for 2 and 3: no messages beyond the layer's cache
   file.
5. The failures: `vk_driver = "missing.zip"` (Vulkan on the phone's driver,
   the reason logged); an archive whose library is an ELF file but no driver
   (for example one of the hooks, renamed, with a `meta.json`): the GL ES
   image, the reason logged, the game drawn by GL ES; `renderer = "vlukan"`:
   GL ES with a warning.
6. A build where the APK has no `halo_guest_vk.elf` is not made for the test;
   the code path (GL ES, logged) is read in the audit instead.

### Acceptance

- Each of the cases above as described, on the test device, with the log
  lines in "Progress".
- The GL calls left in the Vulkan image listed in "Progress".
- `ninja android_apk` with and without `--android-vulkan-validation` builds
  both images; of the GL ES image's objects only `port_config.o` changes (the
  settings rows).
- The code audited against the source and the Vulkan specification, then
  committed.

## Phase 2 — the backend's skeleton

The first picture: the game's clears, on the screen, through Vulkan. The
deko3d plan's phase 2 is the model (`port/switch/DEKO3D.md`, and
`port/switch/guest/dk_commands.h`, `d3d8_dk.c`'s command stream,
`targets_bind`, `Clear` and `Present`, and `host_dk.c`, as of commit
`25c02a0a`: read them first). What differs is Vulkan's, and is spelled out
below.

### What this phase is, and is not

- **It is** the command stream from the guest to the host, the logical
  device, the swapchain on the game's window, frames in flight, the game's
  render targets as images, clears, presenting the back buffer, numbered
  submissions, the surface's loss and return, and the diagnostics every later
  phase relies on.
- **It is not** drawing: no vertex data, no shaders of the game, no textures
  (phases 3 to 6). The menus show as their clears: plain colours, black for
  most of them.
- **The GL ES image does not change.**

### Files

| File | What |
|---|---|
| `port/android/guest/vk_commands.h` | the command stream's records, included by both halves (below) |
| `port/android/guest/d3d8_vk.c` | the stream (as `d3d8_dk.c`'s), `targets_bind`, `Clear` and `Present` writing commands |
| `port/android/host/host_vk.c` | the device, the swapchain, the frames, the targets, the commands (it may be split: `host_vk_targets.c`, `host_vk_present.c`, as it grows) |
| `port/android/host/host_vk.h` | the device's state and its function table |
| `port/android/host/host_sdl.c` | hands the backend the game's window (the last window made, under Vulkan) |
| `port/android/host_imports.list` | `host_vk_submit`, `host_vk_retired` |
| `port/linux/src/port_config.c` | `debug.vk_present_marker` (below), `_platform_android` |

### The command stream (`vk_commands.h`)

As `dk_commands.h`: every field a 32-bit integer or float (the guest's
pointers are 32 bits, the host's 64), an address a guest address, each
record a header (`type`, `size` in bytes including the header, a multiple of
4) and its fields. Phase 2's records:

- `VK_COMMAND_TARGETS`: the colour and the depth-stencil surface later
  commands draw into, either `NONE`. A surface is `data` (the `D3DSurface`'s
  `Data`, as `d3d8_gl.c` knows targets), `width`, `height` (the game's units),
  `pixel_width`, `pixel_height` (what it is drawn at: the screen's targets at
  the screen's scale, `render_target_get` in `d3d8_gl.c`; the guest works
  this out, so the host never needs the screen's scale), and `kind`
  (`COLOR`, `DEPTH`).
- `VK_COMMAND_CLEAR`: which of red, green, blue, alpha, depth, stencil; the
  colour (four floats), depth, stencil; and rectangles in the targets'
  **pixels**, already clipped and scaled by the guest exactly as `d3d8_gl.c`'s
  `Clear` does (`target_pixel`: `floor(coordinate * scale + 0.5)`; a clear
  without rectangles is the viewport's; rectangles are clipped to the
  viewport and moved by `UI_OFFSET`, the viewport is not). Row 0 is the top
  of the picture, as it is in Vulkan; `d3d8_gl.c`'s GL coordinates were the
  other way up and flipped at present, which Vulkan does not need.
- `VK_COMMAND_PRESENT`: the back buffer's surface.

The guest writes into a 1 MB buffer in its own memory over a frame and calls
`host_vk_submit(commands, size)` at `Present`, and earlier if the buffer
fills (as `d3d8_dk.c`). The host reads each record **during that call** and
records what it says into the current frame's command buffer; nothing of the
guest's memory is read later (the rule in "Where the renderer lives"). A
record the host does not know, or a size that runs past the end, is logged
once with its offset and type, and the rest of that hand-over is dropped.

### The device

Made at the first `host_vk_submit` (the window exists by then), from what
`host_vk_startup` kept:

- **The surface** on the game's window: `vkCreateAndroidSurfaceKHR` on the
  `ANativeWindow` (`SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`), read again each
  time the surface is made.
- **The queue family**: the one `host_vk_startup` chose, checked with
  `vkGetPhysicalDeviceSurfaceSupportKHR`; if it cannot present, the first
  graphics family that can. None: logged, and the backend draws nothing for
  the rest of the run (the game keeps running on the stand-in swap). The
  player's way back is `display.renderer = "gl"`; the process cannot switch
  images.
- **The logical device**: one queue; `VK_KHR_swapchain`; dynamic rendering
  (the 1.3 feature, or the extension and its dependencies, as
  `host_vk_startup` found them). Nothing else yet.
- **The device-level function table**, as the instance's: an X-macro list,
  each entry checked, a missing one a failure.
- **Formats**: colour targets `B8G8R8A8_UNORM` (the Xbox's A8R8G8B8 byte
  order), checked for colour attachment, sampling with linear filtering,
  blit source and transfer; depth-stencil targets `D24_UNORM_S8_UINT`, else
  `D32_SFLOAT_S8_UINT`, checked for depth-stencil attachment. Neither
  available: the backend draws nothing, logged.

### The game's render targets

Images found by the surface they stand for: the key is `data`, `width`,
`height`, `kind` and the pixel size, as `render_target_get` in `d3d8_gl.c`
keys its textures (a table by `data`, a few entries per bucket). Made the
first time a `TARGETS` command names them, at the pixel size, with usage
colour or depth-stencil attachment plus transfer source and destination (and
sampled, for phase 6), and **cleared once when made** (black, depth 1,
stencil 0), so that a target read before anything was drawn into it reads the
same on every driver. Each image's layout is tracked; every use transitions
from the layout it is in. Images are kept for the run (as `d3d8_gl.c` keeps
its textures); the log's statistics line says how many exist.

### Rendering and clears

The frame's command buffer has at most one rendering open
(`vkCmdBeginRendering`) at a time: opened on the current targets when a
command needs it, with load `LOAD` and store `STORE` (the game's targets keep
what earlier frames drew), and ended when the targets change, before a
transfer or a barrier, and before present. Viewport and scissor are set after
every opening (Vulkan keeps no state across command buffers or renderings,
unlike deko3d, whose state carried over).

**A clear** is `vkCmdClearAttachments` on the rectangles, when it names all
four colour channels or none (depth and stencil are separate aspects there,
and depth or stencil without a depth target is skipped, as `d3d8_gl.c`
skips them). **A clear of some channels only** - the fog's alpha-only clear
is one - cannot be: `vkCmdClearAttachments` ignores colour write masks. It is
drawn: a rectangle over each clear rectangle (scissor) with a small built-in
pipeline whose colour write mask is the channels asked for (one pipeline per
mask, made the first time; the colour in push constants; no vertex input;
the vertex shader makes the rectangle from `gl_VertexIndex`). Its two shaders
are GLSL compiled at device creation with glslang, loaded as the probe loads
it (`libglslang_probe.so`, `dlopen`): phase 5 makes glslang the backend's own,
and this is its first use. A depth or stencil clear in the same command goes
through `vkCmdClearAttachments` as before.

### Presenting

At a `PRESENT` command:

1. The rendering is ended. The back buffer's image goes to transfer source.
2. A swapchain image is acquired (FIFO; `minImageCount + 1` images; the
   format `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` with the sRGB-nonlinear colour
   space, whichever the surface lists first; usage colour attachment and
   transfer destination). It is cleared black, and the back buffer is blitted
   into it letterboxed to the back buffer's shape (`vkCmdBlitImage`, linear
   filter; the rectangle as `d3d8_gl.c`'s `Present` works it out, without its
   flip), then transitioned for present.
3. The command buffer is ended and submitted with the frame's fence, waiting
   on the acquire semaphore and signalling a render-done semaphore of that
   swapchain image (one per image, not per frame); then
   `vkQueuePresentKHR`.
4. The next frame in flight (two) waits for its fence before its command
   buffer is reset and recorded.

**The transform.** The test device's surface reports `ROTATE_90` as its
current transform (phase 0). The swapchain is made with
`preTransform = IDENTITY` (Android's compositor turns the picture), not the
current transform (which would make the backend rotate the picture itself,
which the probe never proved it did right). Android then answers some
presents with `VK_SUBOPTIMAL_KHR` for the mismatch: that is not a reason to
make the swapchain again; a different `currentExtent` is (checked when
`SUBOPTIMAL` comes, and every 60 frames).

**`debug.vk_present_marker`** (false by default): draws, after the blit, a
red square at the top-left corner of the letterboxed picture and a green one
at its top-right (`vkCmdClearColorImage` on regions of the swapchain image,
outside rendering), so that a screenshot shows whether the picture is the
right way round and where its corners land. Phase 2's test uses it, because
the menus' clears are plain colours that show no orientation.

**Pacing.** `host_vk_presenting` is set once a present has succeeded; the
stand-in swap then stops holding frames (FIFO's acquire and present do). It
is cleared while there is no swapchain, so that a game with nothing to
present does not spin.

### Numbered submissions

Each submit is numbered from 1. `host_vk_retired()` returns the highest
number whose fence has signalled (checked with `vkGetFenceStatus`, not
waited on), for the diagnostics and for phase 6's visibility tests (phase 3 found the locks need no waiting). In this phase a frame is one submission.

### The surface's loss and return

Phase 0 found that SDL's lifecycle events do not reach the game's loop, and
that the surface is lost when the app goes to the background:
`VK_ERROR_SURFACE_LOST_KHR` or `VK_ERROR_OUT_OF_DATE_KHR` from acquire or
present, and a window whose `ANativeWindow` changed or is gone. Then: wait for
the device to be idle, destroy the swapchain (and the surface, if lost), and
make them again at the next present if the window is back; until then each
present records nothing to the screen (the frame's work is still submitted,
so its fence and its number move on) and `host_vk_presenting` is clear. Each
loss and return is logged once, with how long it took (the probe's
`surface_lost` is the model), not each attempt.

### Diagnostics, from this phase on

- Every 60 frames, a log line: frames, hand-overs, commands by type, clears
  (drawn and by `vkCmdClearAttachments`), target changes, images alive,
  submissions made and retired, swapchain re-creations, and validation errors
  so far (`host_vk_validation_errors`).
- A wait on a fence that takes more than two seconds logs what it waited for
  and the submission number, then keeps waiting.
- `VK_ERROR_DEVICE_LOST` from any call logs the call, the submission number
  and the driver's description, then stops the backend (nothing more is
  recorded or submitted; the game keeps running on the stand-in swap), so a
  lost device is a log line and not a crash.
- The validation layer (`debug.vk_validation`) on for every test of the phase.

### Testing on the device

On the test device, the `.vk` build, `display.renderer = "vulkan"`, each case
on the phone's driver and on Turnip:

1. The menus: the screen shows their clears (black, and any colour a menu
   clears to); with `debug.vk_present_marker = true` a screenshot shows the
   red square top-left and the green top-right of the letterboxed picture,
   with black bars at the sides where the back buffer is narrower than the
   screen.
2. A new game and the first map: the clears of a map (the sky's colour, the
   fog's alpha-only clear among them: the statistics line counts drawn
   clears) show; the game runs on.
3. The pacing: the statistics line's frames per second at the display's
   rate, paced by the swapchain (`host_vk_presenting`), the stand-in swap no
   longer holding them.
4. Backgrounding and returning, ten times (HOME, then `am start`): the
   picture comes back each time; the log says each loss and return once.
5. Turning the device over (`settings put system user_rotation 1` and `3`,
   with `accelerometer_rotation 0`; put back afterwards): the picture stays
   the right way round (the marker).
6. Validation on for all of the above: no message beyond the layer's own
   cache file.

Move `config.toml` with `adb pull` and `adb push` only (phase 1's audit:
`adb shell cat` corrupts it).

### Acceptance

- Each case above on both drivers, with the log lines and the screenshots'
  findings in "Progress".
- The GL ES image unchanged on the device.
- No `VK_ERROR_DEVICE_LOST`, no validation error, no two-second wait in any
  run.
- The code audited against the source and the Vulkan specification's valid
  usage for every call it makes (barriers, layouts, semaphores per swapchain
  image, the swapchain's re-creation), then committed.

## Phase 3 — reading the game's memory

How a draw's data gets from the game to the GPU, before any draw exists:
phase 6 makes the draws, and they use what this phase builds. The deko3d
plan's phase 3 read the game's memory in place and needed busy tracking for
it; this one copies (Decisions), and what it builds is the copy and the proof
that it is made at the right moment.

### What this phase is, and is not

- **It is** a way for the guest to put bytes into the command stream at a
  draw (data records), the host's upload rings those bytes land in, a way
  for later commands to name them, and a self-test that a buffer rewritten
  between two draws is drawn with both of its contents.
- **It is not** the game's draws (phase 6), its vertex formats, or textures;
  nor busy tracking: with every draw's data copied at the draw, the GPU never
  reads the game's memory, and `IsBusy`, `BlockUntilNotBusy` and the locks
  keep answering as they do now (not busy, return at once), as under
  `d3d8_gl.c`. This is a change from the plan as first written (and from
  deko3d's phase 3), recorded in "Decisions".

### Why the copy is the guest's, at the draw

The guest's command stream is handed over at `Present` (and when it fills).
Between two draws of a frame the game rewrites buffers - dynamic vertex
buffers reused within a frame, the grass rebuilt in a locked buffer, immediate
mode's vertices, constants - and it writes some of them without locking them
at all (`d3d8_gl.c`'s mirror watches its pages for that, `memory_watch.c`). A
copy made at the hand-over would give every draw of a frame the last bytes
written. So the guest copies the bytes a draw reads at the moment of the draw,
as `d3d8_gl.c`'s `stream_upload` and `mirror_range` upload them to GL at the
draw. Phase 0's measure of what that is: 4.2 to 6.8 MB a frame "mirrored" and
0.2 to 0.7 MB "streamed" in the first map, under `d3d8_gl.c` - the bytes its
draws referenced. Copying them twice a frame (into the stream, then into the
ring) is a few milliseconds; speed is not a goal, and the mirror's
write-protection and generations (`memory_watch.c`) are not needed to be
right, only to be fast. They can come later if the game is too slow.

### Files

| File | What |
|---|---|
| `port/android/guest/vk_commands.h` | `VK_COMMAND_DATA` (below), and the data reference later commands carry |
| `port/android/guest/d3d8_vk.c` | `vk_data_put`, the guest's side (below); the stream grows to 4 MB |
| `port/android/host/host_vk_render.c` (or a new `host_vk_data.c`) | the upload rings, the data table of the current frame, the self-test |
| `port/linux/src/port_config.c` | `debug.vk_self_test` (below), `_platform_android` |
| `port/android/README.md` | the setting |

### Data records

`VK_COMMAND_DATA`: a header, then `id`, `part_offset`, `total_size`,
`part_size`, and `part_size` bytes of payload (the record padded to a
multiple of 4).

- **`id`** is the frame's running number of the data the guest put, from 1,
  reset at each `PRESENT`. Later commands of the same frame name data by its
  id and an offset in it; nothing names data of another frame.
- **Parts.** Data larger than what is left in the stream is split: parts of
  one id come in order (`part_offset` 0, then the next, until `total_size`),
  possibly in different hand-overs, and the host places all of an id's parts
  contiguously in its ring. A command naming an id comes after all its parts.
- **The guest's function**, `uint32_t vk_data_put(const void *bytes,
  unsigned long size)`: copies the bytes into the stream (flushing it when
  full, splitting as above) and returns the id. Called at the draw, for every
  range the draw reads (phase 6); the bytes are the game's as they are at
  that moment.
- **The stream** grows to 4 MB (a busy frame fills it two or three times;
  each fill is a hand-over, which is cheap).

### The upload rings (host)

- One per frame in flight: a list of host-visible, host-coherent buffers
  (16 MB each to start), usage vertex, index, uniform buffer and transfer
  source (phase 6 stages textures through it), mapped once.
- At a `DATA` record, the host copies the part to the place its id was
  given: the first part of an id takes `total_size` bytes from the frame's
  ring, at an offset aligned to 256 or the device's
  `minUniformBufferOffsetAlignment`, whichever is larger (a uniform block can
  then be any data; an index buffer's offset is aligned too). A frame that
  needs more than its buffers hold gets another (never moves what is placed:
  commands already recorded name it); a single id larger than a buffer gets a
  buffer of its own size. The ring's buffers are reused when the frame's fence
  has passed (phase 2's frames), the extra ones kept for the next time.
- The frame's **data table**: id to buffer and offset, kept until `PRESENT`,
  for the commands that name data (`host_vk_data_find(id, offset, size)`:
  the buffer and the offset, or none if the id or the range is unknown, which
  is logged once and the command skipped).
- Bad records (a part out of order, past its total, an id reused) are
  logged once, and the rest of the hand-over is dropped, as phase 2's
  unknown commands are.
- The statistics line (phase 2) gains: data records, bytes of data a frame,
  ring buffers alive.

### The self-test (`debug.vk_self_test`)

`debug.vk_self_test` (false by default) runs the backend's self-tests once,
at the device's creation: phase 2's clears self-test (which phase 2 ran with
`debug.vk_present_marker`: the marker then only draws the marker) and this
phase's:

1. **In the guest**, at the device's creation (`Direct3D_CreateDevice`, after
   the window is up, when the setting is on): a buffer of three vertices in
   the game's own memory (a red triangle covering the target), put with
   `vk_data_put`; then the same buffer rewritten in place (green); put
   again; then a third time, larger than what is left in the stream (blue,
   padded with unused bytes past 1 MB, so it is split into parts across a
   hand-over); then a `VK_COMMAND_TEST_DRAW` naming each id in turn, and a
   hand-over.
2. **In the host**, `VK_COMMAND_TEST_DRAW` (an id): draws the three vertices
   of that data into a small target of its own (16x16) with a built-in
   pipeline (position and colour, as the probe's pass-through shaders; phase
   2's glslang loader), reads it back, and logs `data self-test: id N
   ok/FAILED (expected colour, seen colour)`. The three ids must show red,
   green and blue: the first two prove the copy was made at the put and not
   at the hand-over; the third proves parts land contiguously across a
   hand-over.
3. The test's commands and target are not the game's and draw nothing on the
   screen.

`VK_COMMAND_TEST_DRAW` exists only for this; phase 6's draws replace it as
the way data is drawn.

### Testing on the device

On both drivers:

1. `debug.vk_self_test = true`: the clears self-test and the data
   self-test each say `ok` for every case.
2. The game as in phase 2 (menus, a new game's map): unchanged on the
   screen; the statistics line shows the data counts (zero until phase 6,
   except in the self-test's frame).
3. Validation on: no new message.
4. `renderer = "gl"`: unchanged.

### Acceptance

- The self-tests `ok` on both drivers, the log lines in "Progress".
- `IsBusy`, `BlockUntilNotBusy` and the locks unchanged (read in the audit:
  nothing in the Vulkan device waits on the GPU for a resource).
- The code audited against the source and the Vulkan specification, then
  committed.

## Phase 4 — GLSL for Vulkan

The Vulkan renderer's two GLSL generators, and the proof that what they
produce for the game's real shaders compiles, validates, lays its blocks out
as the C structures say, and says the same as the GL ES generators. The
deko3d plan's phase 4 is the model (`port/switch/DEKO3D.md`, "Phase 4 — GLSL
for UAM", and `port/switch/guest/nv2a_vsh_dk.c`, `nv2a_psh_dk.c`,
`dk_shaders.h`: read them first; most of this phase is that phase with
glslang for UAM and Vulkan's binding model for deko3d's).

### What this phase is, and is not

- **It is** the generators (copies), the header that fixes the blocks'
  layout and the bindings, the making of a draw's pixel shader key in the
  Vulkan device (without drawing), a dump of every shader the game meets,
  a probe step that compiles them on the device, and a check on the PC.
- **It is not** drawing (phase 6), caching (phase 5) or compiling in the
  backend (phase 5). Nothing on the screen changes.
- **The GL ES image does not change**: `port/linux/src/nv2a_vsh.c` and
  `nv2a_psh.c` are not edited (they are in both images; the copies' new
  names do not collide).

### Files

| File | What |
|---|---|
| `port/android/guest/nv2a_vsh_vk.c` | a copy of `nv2a_vsh.c`, the function renamed `nv2a_vk_vertex_shader_to_glsl` (same arguments) |
| `port/android/guest/nv2a_psh_vk.c` | a copy of `nv2a_psh.c`, the function renamed `nv2a_vk_pixel_shader_to_glsl` (same argument: a `struct nv2a_pixel_shader_key`) |
| `port/android/guest/vk_shaders.h` | the bindings, the interface locations, every block member's offset as a `#define`, the C structures of the blocks with each offset and size checked at compile time, the generators' prototypes. Fixed-width types and floats only, so the host can include it |
| `port/android/guest/d3d8_vk.c` | the pixel shader key made at each draw, the vertex shaders kept by id, the dump |
| `tools/android_build.py` | the two copies in the Vulkan image's objects only |
| `port/android/host/host_vk_probe.c` | a step `shaders` (below) |
| `tools/vk_shader_check.py` | the check on the PC (below) |

The Android guest is compiled with `HALO_ANDROID`, so the originals take
their GL ES branches; the copies drop every `#ifdef HALO_ANDROID` branch and
keep what Vulkan needs, as below.

### What changes from the GL ES generators

**Version.** `#version 450` first; no `precision` statements; nothing from
`xgpu_capabilities.shading_language`. glslang compiles with the Vulkan
client, SPIR-V 1.0 (as phase 0 and phase 2's built-in shaders do).

**No loose uniforms; one set, unique bindings.** Vulkan, unlike deko3d,
numbers bindings per set, not per stage, so the two stages share one
numbering. Set 0:

| Binding | Stage | What | Contents |
|---|---|---|---|
| 0 | vertex | uniform block `vertex_constants` | `vec4 c[192];` - 3072 bytes |
| 1 | vertex | uniform block `vertex_parameters` | `vec4 viewport_scale; vec4 viewport_offset; vec4 point_and_screen;` (x the point size, y the screen offset) - 48 bytes |
| 2 | fragment | uniform block `pixel_parameters` | `vec4 ps_c0[8]; vec4 ps_c1[8]; vec4 ps_final_c0; vec4 ps_final_c1; vec4 fog_color; vec4 fog_parameters; vec4 alpha_reference;` (x) `vec4 bump_matrix[4]; vec4 bump_luminance[4]; vec4 texture_scale[4];` - 528 bytes |
| 3 to 6 | fragment | `tex0` to `tex3` | `sampler2D`, `sampler3D` or `samplerCube`, the type from the key as now |

Every block is `layout(std140, set = 0, binding = N) uniform name { ... };`,
**every member a `vec4` or an array of them** (a lone `float` and an array of
floats are aligned differently by std140, and mixing them is how the earlier
attempt's block drifted from its C structure at byte 3108), and **every
member has `layout(offset = N)`**, the N from `vk_shaders.h`'s `#define`s:
the C structures' `offsetof` is checked against the same `#define`s at
compile time, and glslang rejects an explicit offset that std140 forbids, so
the shader, the header and the structure cannot disagree without a build
failing. The GL ES generators' `uniform float point_size`, `screen_offset`
and `alpha_reference` become components (`point_and_screen.x`,
`point_and_screen.y`, `alpha_reference.x`) and every use changes to match.
`texture_lod_bias` goes: under Vulkan the bias is the sampler's
(`mipLodBias`, phase 6), so `SAMPLE_BIAS` is the desktop one, empty.

**Locations between the stages**, as deko3d's (each stage is compiled and
cached apart, phase 5, and a vertex output meets a pixel input only by
location):

| Location | Vertex output / pixel input |
|---|---|
| 0 | `xD0` |
| 1 | `xD1` |
| 2 | `xB0` |
| 3 | `xB1` |
| 4 to 7 | `xT0` to `xT3` |
| 8 | `xFog` (float) |

Every vertex shader writes all nine and every pixel shader declares all nine,
read or not, so any vertex shader goes with any pixel shader. The pixel
output is `layout(location = 0) out vec4 fragment_color;`. Vertex inputs keep
their `layout(location = N)`, 0 to 15, all sixteen declared: phase 6 must
then give every pipeline all sixteen attributes (Vulkan requires an
attribute for every input a shader declares), feeding the ones the game's
streams do not supply from a stride-0 binding holding their constant value,
as deko3d does. A packed (`D3DVSDT_NORMPACKED3`) attribute stays `in uint`
and is unpacked in the shader as now.

**Clip space.** The ES branch emulates `glClipControl(GL_UPPER_LEFT,
GL_ZERO_TO_ONE)` at the end of the vertex shader (`gl_Position.y =
-gl_Position.y; gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;`).
Vulkan's depth is already 0 to 1, so the `z` line goes. Its y axis is the
other way round from those conventions (deko3d, whose device used them, came
out the right way up without a flip, DEKO3D.md phase 6 step 2), so the
picture needs one flip; it is made **in the viewport, with a negative height
(phase 6), not in the shaders**, and the `y` line goes too. The shaders are
then the desktop branch's. Kept: the ES branch's `clip_captured` path (the
precision of `oPos` near the camera plane, which is not about ES: deko3d
kept it for the same reason), the half-pixel offset `+ 0.5`,
`screen_offset`, and `invariant gl_Position;`. Phase 6 step 1 is where the
picture's orientation and the front face's winding are seen and, if wrong,
fixed in the viewport and the front face, not here.

**Occlusion.** No `count_samples` branch and no atomic counter: under Vulkan
the visibility tests are occlusion queries (phase 6). The Vulkan device
makes keys with `count_samples` 0, and the generator ignores it.

**Everything else is kept as it is**, line for line: the instruction
decoding, the operations and their helper functions, the texture stage
modes, the combiner stages, the final combiner, fog, the alpha test (in the
shader, against `alpha_reference.x`), alpha kill, colour sign,
`coverage_alpha`, and the `debug.gpu_debug_*` settings. This phase is about
the GLSL dialect, not the translation.

### The key, made at each draw

The corpus is the game's own, met in play, and the Vulkan image makes the
keys itself (the Android GL ES image records none: `shader_programs.bin` is
the Switch's, behind `HALO_SWITCH` in `d3d8_gl.c`, which is not edited).

- **`pixel_key_make`** in `d3d8_vk.c`: what `prepare_draw` in `d3d8_gl.c`
  does to make `struct nv2a_pixel_shader_key`, without GL: the combiner
  state with the constants zeroed, the texture modes, alpha kill, colour
  sign, the alpha test, fog, and from `bind_textures` only its decisions:
  a stage with no texture (or modes 0, 0x04, 0x05) has no sampler, mode
  0x11 a 2D one; otherwise the sampler type from the texture's description
  (`xgpu_texture_describe`: a cube map is a cube, a volume texture 3D,
  anything else 2D), and `coverage_alpha` as `bind_textures` and the line
  after it decide it. Where a decision needs what only `d3d8_gl.c`'s texture
  cache knows (the high-res substitutes' coverage, `hires_coverage`), find
  where that is set and reproduce it without GL, or record the gap in
  "Progress". `count_samples` is 0.
- **At every draw** (`DrawVertices`, `DrawIndexedVertices`, their `UP`
  forms, and `End` for immediate mode: the entry points that stay stubs
  until phase 6), the device makes the key, as `prepare_draw` would at that
  point, and notes it and the vertex shader with its packed mask (or 0 for
  immediate mode) for the dump. Nothing is drawn.
- **The vertex shaders by id**, as `d3d8_gl.c`'s Switch code keeps them
  (`vertex_shaders_by_id`), so the dump can name them.

### The dump

With `debug.gpu_dump_shaders` naming a folder (an existing setting; under
GL ES the GL renderer writes its GLSL there), the Vulkan device writes,
the first time it meets each:

- `vs_<id>_<packed mask, hex>.vert`: the Vulkan GLSL, and
  `vs_<id>_<mask>.gl.vert`: the GL ES generator's GLSL for the same program
  and mask (`nv2a_vertex_shader_to_glsl`: the original is in the Vulkan
  image too);
- `ps_<key hash, hex>.frag` and `ps_<hash>.gl.frag` likewise
  (`hash_words` of `d3d8_gl.c` is the hash to copy), and `ps_<hash>.key`:
  the key's bytes;
- `manifest.txt`: one line a file, what it was made from.

Each vertex shader is also written with mask 0 at the first `Present`, so
that every program has at least its immediate-mode form. The log says how
many of each were written.

**The corpus to collect**: the main menu and its screens, a new game's
first campaign map from its opening through the first fight, and, if the
time allows, a second map. The screen is black under the Vulkan image (the
game runs: phase 1), so the corpus is collected by keys and the log, as in
phases 1 to 3.

### Proving it

**1. On the device, every shader compiled and validated.** A probe step,
`shaders`: for every `.vert` and `.frag` (not `.gl.*`) in the dump folder
(`debug.gpu_dump_shaders`), compile with glslang (the probe's loader),
`vkCreateShaderModule`, and log each failure with glslang's message or the
result code; then the count, the total time, the average and the slowest
ten per stage. Run with the validation layer, which validates each module's
SPIR-V (the layer runs `spirv-val` on every `vkCreateShaderModule`): a
validation message on a module is a failure. On the phone's driver and on
Turnip. (`debug.vk_probe = "shaders"`; the probe's other steps are as they
were.)

**2. On the PC, `tools/vk_shader_check.py <folder>`**, runnable from a clean
checkout once `configure.py` has fetched glslang:

- builds `glslangValidator` for the PC from the fetched glslang
  (`build/android/third_party/glslang`, CMake, into `build/host-glslang`)
  the first time;
- compiles every Vulkan shader (`-V --target-env vulkan1.0`) and reports
  failures with glslang's message and the file;
- reflects each (`glslangValidator -q`): every block's binding, size and
  members' offsets must be what `vk_shaders.h` says (the script reads the
  `#define`s), and every sampler's binding 3 + its stage;
- runs `spirv-val` on each module where it is available (on `PATH`, or
  built from the SPIRV-Tools glslang pins, if the script can fetch it), and
  says when it is not (the device's step 1 then stands for it);
- **compares each Vulkan shader with its GL ES twin**: applies to the GL
  ES text the changes this phase makes (the version line, the precision
  lines, the loose uniforms to blocks, the renamed components, the removed
  lines, the locations, `texture_lod_bias` and `count_samples` gone),
  normalises whitespace, and diffs. Any other difference is a failure, with
  the diff. **A shader that compiles cleanly and draws black is the failure
  this exists to catch**: it compiles, so only a comparison finds it.
- counts and prints: files, failures by kind, distinct pixel keys, and how
  many of them differ in each key field (a field that never varies across
  hundreds of keys - the texture modes, say - is a key that was not filled
  in, which is the earlier attempt's black-shader bug; say so).

### Testing on the device

1. The `.vk` build with `display.renderer = "vulkan"` and
   `debug.gpu_dump_shaders` set to a folder in the data folder: the corpus
   (above), pulled with `adb pull`.
2. `debug.vk_probe = "shaders"` with validation on, on both drivers.
3. `tools/vk_shader_check.py` on the pulled folder.
4. `renderer = "gl"`: unchanged.

### Acceptance

1. `ninja android_apk` builds with no new warnings; `git diff` shows
   `port/linux/src` untouched (apart from settings rows, if any).
2. The generated GLSL has no uniform outside a block, every block member a
   vec4 or an array of them with an explicit offset, every block and sampler
   in set 0 at the bindings above, both stages declaring all nine interface
   variables at the locations above, and no y flip, depth remap, precision
   statement or atomic counter.
3. `vk_shaders.h`'s structures match the blocks (offsets and sizes checked
   at compile time; 3072, 48 and 528 bytes).
4. Every dumped shader compiles and validates on the device, on both
   drivers, and on the PC; the reflection and the comparison find nothing;
   the key fields vary as a real corpus's would. The numbers - files,
   distinct keys, times per stage, the slowest shaders - in "Progress".
5. The check script is in `tools/` and runs from a clean checkout.

## Phase 5 — shader and pipeline cache

The shaders a draw needs, compiled in the background on the device and kept
there, and the pipelines made from them, kept in a `VkPipelineCache` per
driver. The deko3d plan's phase 5 is the model (`port/switch/DEKO3D.md`,
"Phase 5 — shader cache", steps 2 to 4, and `port/switch/guest/dk_shaders.c`,
`port/switch/host/host_dk_shaders.c`: read them first), less what this plan
dropped (key files, the startup pass, priorities by map: Decisions) and plus
what deko3d has no need of: pipelines.

### What this phase is, and is not

- **It is** glslang in the backend, the shader service (identity, the disk
  cache, the compile thread, handles), the pipeline service (identity, the
  compile thread, the per-driver `VkPipelineCache` saved on the device), and
  the guest's side that asks for them at each draw.
- **It is not** drawing: phase 6 draws, and makes the pipeline's state from
  the game's. Here the pipelines asked for carry a placeholder state, so that
  the service, its thread and its cache are made and tried before draws
  depend on them; phase 6 replaces the placeholder and nothing else.
- **No key files, no startup pass, no shared keys** (Decisions): a device
  compiles what its draws meet, and keeps it.

### Files

| File | What |
|---|---|
| `port/android/probe/glslang/CMakeLists.txt`, `tools/android_build.py` | the library renamed `libhalo_glslang.so` (it is the backend's now, and the probe's) |
| `port/android/host/host_vk_shaders.c` | glslang's loader (moved from `host_vk_render.c`, which keeps using it for its built-in shaders), the shader service, the pipeline service, the compile thread, the caches |
| `port/android/host/host_vk.h` | the services' state and functions |
| `port/android/host_imports.list` | `host_vk_shader_find`, `host_vk_shader_compile` (below) |
| `port/android/guest/vk_shaders.h` | the state a pipeline is made with (`struct vk_pipeline_state`, below), the shader identities' hashing |
| `port/android/guest/d3d8_vk.c` | the shader handles at each draw, and the pipeline asked for |
| `port/android/guest/vk_commands.h` | `VK_COMMAND_PIPELINE` (below) |
| `port/android/host/host_vk_probe.c` | `dlopen`s the renamed library |

### Identity

As deko3d's: a shader is known by a 64-bit FNV-1a hash the guest makes, and
the host never sees keys, only hashes and GLSL.

- **A vertex shader:** `VK_SHADER_GENERATOR_VERSION`, the hash of its
  program (FNV-1a 64 over the instruction count and words, made once in
  `D3DDevice_CreateVertexShader` and kept in the object), and the packed
  mask (0 for immediate mode).
- **A pixel shader:** `VK_SHADER_GENERATOR_VERSION` and the
  `nv2a_pixel_shader_key`, `count_samples` 0.
- **A pipeline:** the two shaders' hashes and the hash of its
  `struct vk_pipeline_state` (the host makes this one; the guest sends the
  state).

### The shader service (host)

- **Imports** (the import stubs pass a 64-bit integer in one register on
  both sides, as deko3d's do; log one on both sides the first time to be
  sure):
  - `uint32_t host_vk_shader_find(uint32_t stage, uint64_t hash)`: a handle
    (1 or more) if the shader's module is made; else, if its SPIR-V is in
    the disk cache, the module is made now from the file (fractions of a
    millisecond: phase 4 measured `vkCreateShaderModule` at 0.4 to 1.8 ms)
    and its handle returned; else 0, with "queued", "compiling", "failed" or
    "unknown" told apart (an out parameter).
  - `void host_vk_shader_compile(uint32_t stage, uint64_t hash, uint32_t
    glsl, uint32_t glsl_size)`: queues it. The GLSL is copied into host
    memory **during the call** (`glsl` is a guest address: the rule about
    guest memory holds). Already queued or made: nothing.
- **The disk cache:** `<internal storage>/vk_cache/spirv/<glslang tag>-<
  VK_SHADER_GENERATOR_VERSION>/`, one `<v|f><hash, 16 hex>.spv` a shader:
  a header (magic, the hash, the SPIR-V's size, a CRC of it) and the
  SPIR-V. Written as `.tmp` and renamed when complete. At start the folder is
  listed once into a set (the files are not opened), and the other folders
  under `vk_cache/spirv/` are removed (other glslang versions' or
  generators'; nothing else is touched). A file whose header or CRC does not
  check is deleted and the shader compiled again. SPIR-V does not depend on
  the driver, so both drivers share these files.
- **A failed compile** (glslang's error, or `vkCreateShaderModule`'s) is
  logged once with the hash, the stage and glslang's message, and the shader
  is marked failed for the run (its draws are counted, not retried every
  frame). The GLSL is written next to the log (`vk_cache/failed/`) so that
  it can be compiled on the PC with `tools/vk_shader_check.py`.
- **Handles** index a table of `VkShaderModule`s, kept for the run.

### The pipeline service (host)

- **The state** (`struct vk_pipeline_state`, fixed-width fields only, in
  `vk_shaders.h`): everything in a pipeline that is not one of the core
  dynamic states (Decisions): the topology; polygon mode; cull mode and
  front face; depth test, write and compare op; depth bias enable; stencil
  test enable, and each face's fail, pass, depth-fail ops and compare op;
  colour blend enable, the four factors and two ops, the colour write mask;
  the colour and depth-stencil attachments' formats (none or which); and the
  vertex input: sixteen attributes (format, binding, offset; every one
  present, Phase 4's rule) and their bindings (stride, rate). Unused fields
  are zero, so that equal states hash equal. Phase 6 fills it from the game's
  render state; this phase sends a placeholder (below).
- **`VK_COMMAND_PIPELINE`** (vertex handle, pixel handle, a state): the
  host looks the pipeline up by the three hashes; if made, it is ready (phase
  6's draw binds it); if not, it is queued for the compile thread and the
  draw that asked would be skipped (counted: "pipeline not ready"). The
  pipeline layout is one for every pipeline: set 0 as Phase 4's table
  (two vertex uniform blocks, one pixel uniform block, four combined image
  samplers), made at the device's creation.
- **The `VkPipelineCache`, per driver:** one cache object, used for every
  pipeline, loaded at the device's creation from
  `<internal storage>/vk_cache/pipelines/<pipelineCacheUUID, hex>-<driverID>-<
  driverVersion, hex>.bin` (so the phone's driver's and Turnip's never meet,
  and a driver update starts afresh), and saved (as `.tmp`, renamed) when the
  app goes to the background (phase 2's surface loss), every five minutes if
  pipelines were made since, and at the game's exit (`host_exit`). A file
  the driver does not accept (`vkCreatePipelineCache` fails with it, or its
  header names another device) is deleted and a new cache started.

### The compile thread

One host thread (phase 0 found one enough: a game-sized shader compiles in
2 to 4 ms, a pipeline in 1 to 12 ms on the phone's driver and less on
Turnip), a plain `pthread` with a stack of 2 MB (it never runs guest code),
at a lower priority than the game thread (`setpriority`, as Android allows an
app). It takes the oldest entry of one queue (shaders before pipelines that
need them, a pipeline whose shaders are not made waiting behind them),
compiles, writes the cache file, makes the module or the pipeline, and logs
a line every so often (counts, the queue's length), not one an item.
Nothing else calls glslang while it runs (one lock around it: phase 2's
built-in shaders are compiled through the same loader).

### The guest's side

- At each draw (phase 4's `draw_note`, now made at every draw, not only with
  the dump: phase 6 will need it at every draw anyway), the device makes the
  vertex shader's and the pixel shader's hashes, keeps a table of hash to
  handle, and for a hash without a handle asks `host_vk_shader_find`; an
  unknown one has its GLSL generated (phase 4's generators) and sent with
  `host_vk_shader_compile`. Then it writes `VK_COMMAND_PIPELINE` with the two
  handles (0 if not ready yet: the host then counts the draw as skipped for
  the shader, without asking for a pipeline) and the placeholder state.
- **The placeholder state:** triangle list, fill, no culling, depth test
  and write off, no stencil, no blend, all four channels written, the colour
  target `B8G8R8A8_UNORM` and the depth target as phase 2 chose it, and the
  vertex input of sixteen `R32G32B32A32_SFLOAT` attributes from one binding
  of stride 0. Phase 6 replaces it with the draw's real state; until then it
  makes one pipeline per pair of shaders met, which is what this phase
  needs to try the service on the game's real shaders.
- The dump of phase 4 is unchanged (it stays behind `debug.gpu_dump_shaders`).

### Diagnostics

The statistics line (phase 2) gains: shaders made from the cache and
compiled this run, failed, queued; pipelines made and queued; draws that
would be skipped for a shader and for a pipeline not ready; the pipeline
cache's size. The compile thread's own line says what it did since the last
one, with average times.

### Testing on the device

On both drivers, the `.vk` build, `display.renderer = "vulkan"`:

1. **A cold start** (`vk_cache/` removed by adb): the menus and a new game's
   opening; the log counts the shaders compiled and the pipelines made, the
   draws that would be skipped falling to 0 within seconds of each new
   screen; the game's frames unaffected (the compile thread is not the game
   thread: the frame count every 10 s as before).
2. **A second start**: no shader compiled (all from the cache), pipelines
   made from the `VkPipelineCache` (their average time far below the first
   run's; the compile thread says so).
3. **Both drivers in turn**: each has its own pipeline cache file; the
   SPIR-V files are shared; switching back and forth keeps both.
4. **A broken cache**: a truncated `.spv` and a truncated pipeline cache
   file (`truncate` by adb); each is said, deleted and made again.
5. **Validation on** for 1 and 2: no message beyond the known two.
6. `renderer = "gl"`: unchanged.

### Acceptance

- The second start compiles nothing, on both drivers; switching drivers
  keeps both caches; a broken file is replaced, not fatal.
- No draw stays "would be skipped" for a shader or a pipeline once it has
  had a few seconds; no failed compile in the corpus of phase 4's screens.
- The game's frame rate unchanged while the thread compiles.
- The code audited against the source and the Vulkan specification (the
  thread's use of the device and the cache, the files' writes), then
  committed; the numbers in "Progress".

## Phase 6 — draws, textures and render targets

The game drawn through Vulkan: at the end, the menus and the maps look as
they do under GL ES, on the phone's driver and on Turnip. It is the largest
phase - the equivalent of most of `port/linux/src/d3d8_gl.c` - and is worked
in six steps, each tried on the device before the next.

Read first, in this order: "Decisions", "Where the renderer lives", the
specs and "Progress" entries of phases 1 to 5 (what exists, and the notes
they left for this phase); then `d3d8_gl.c` whole (about 4,100 lines: it is
the specification of what the Xbox's Direct3D does, as this port has
debugged it against the game), `xbox_textures.c`, `hud_hires.c`,
`text_hires.c`, `menu_files.c`; then the deko3d plan's phase 6
(`port/switch/DEKO3D.md`, "Phase 6", and its "Progress" entries: **the
list of traps that renderer met, most of which this one will meet**) with
`port/switch/guest/d3d8_dk.c`, `xbox_textures_dk.c`, `dk_commands.h` and
`port/switch/host/host_dk.c` beside it: the same work against a
command-buffer API.

### What exists, what this phase adds

Exists: the Vulkan image's device (`d3d8_vk.c`) keeps every piece of
Direct3D state the game sets, as `d3d8_gl.c` does; the command stream
(`vk_commands.h`) with targets, clears, present, data records; the host's
device, frames, render targets by address, swapchain, clears (phase 2);
`vk_data_put`, the upload rings and `host_vk_data_find` (phase 3); the
generators and `vk_shaders.h`'s blocks, bindings and locations (phase 4);
the shader and pipeline services with handles, the per-driver pipeline
cache, and the guest's `draw_note` asking for each draw's shaders and a
pipeline with a placeholder state (phase 5).

Adds: draws (indexed, not, immediate mode, every primitive type), the
pipeline state made from the game's (replacing phase 5's placeholder), the
dynamic state, the uniforms, vertex input, textures and samplers, the
high-res HUD and text and the menus' art, render-to-texture and the mip
composite, and visibility tests.

### The draw record

The guest decides; the host records. Unlike deko3d's renderer, which sends
only what changed and lets the host keep the rest (and met, in its
"Progress", state the host had not kept as told), **each draw is one
self-contained `VK_COMMAND_DRAW` record**: the host keeps no draw state
between draws apart from what it has bound, and binds what a record says.
A record is a few hundred bytes; a busy frame's two thousand draws are under
a megabyte, which the 4 MB stream holds. Making it smaller is phase 7's,
if ever.

A `VK_COMMAND_DRAW` holds (fixed-width fields, `vk_commands.h`'s rules):

- **Shaders**: the vertex and pixel shader handles (phase 5's
  `shader_handle`). Either 0: the draw is skipped, counted ("shader not
  ready").
- **Pipeline state**: `struct vk_pipeline_state` (phase 5), made from the
  game's render state as `apply_raster_state` in `d3d8_gl.c` decides it
  (below), and the vertex input as `setup_streams` and `attribute_format`
  decide it. The host's pipeline service looks it up; not ready: skipped,
  counted ("pipeline not ready").
- **Dynamic state**: the viewport, the scissor, the depth bias (constant,
  clamp and slope), the blend constants, the stencil compare and write masks
  and reference - Vulkan 1.0's core dynamic states, set at every draw.
- **Uniforms**: three data references (phase 3), one per block, each a
  `vk_data_put` of the block's C structure (`vk_shaders.h`):
  `vk_vertex_constants` (3,072 bytes), `vk_vertex_parameters` (48),
  `vk_pixel_parameters` (528). Put again only when it changed since it was
  last put this frame (data ids are good for the whole frame: a later draw
  may name an earlier draw's id): the vertex constants by
  `d3d8_gl.c`'s constant serials, the other two by comparing with what was
  put. The constants with no draw between two changes are put once.
- **Vertex streams**: up to 16 bindings, each a data reference and a stride,
  and one more binding of stride 0 holding the current values of the
  attributes the declaration does not feed (below).
- **Textures**: four stages, each an image reference (below) and a sampler
  state.
- **The draw**: the primitive (Vulkan's), the vertex or index count, the
  index data reference (16-bit indices), the vertex offset, the instance
  count 1.
- **The visibility test** the draw is in, if any (step 6).

The host, for each record: looks up the pipeline (skip if not ready), opens
the rendering on the current targets if it is not open (phase 2), binds the
pipeline if it changed, sets the dynamic state, finds each data reference
(`host_vk_data_find`: a reference it cannot find skips the draw, said once),
writes a descriptor set (below), binds the vertex buffers (and the index
buffer) at the ring's buffers and offsets, and draws.

**Descriptor sets**: one per draw, allocated from the frame's descriptor
pools (several pools a frame, a new one when one is full; all reset when the
frame's fence has passed), written with the three uniform buffers (the
ring's buffer, offset and the block's size) and the four combined image
samplers. One path for every driver, rather than push descriptors (both
Adreno drivers have them; a Mali might not). Every binding is written: a
stage without a texture gets a dummy (an opaque black 1x1 image of the type
the pixel shader declares for that stage - 2D, 3D or cube - so the view's
type always matches the shader's sampler).

### The pipeline state, from the game's

Made in the guest, as `apply_raster_state` and its helpers in `d3d8_gl.c`
make the GL state, into `struct vk_pipeline_state`:

- **The Xbox's enumerants are OpenGL's** (`D3DBLEND_*`, `D3DCMP_*`, the
  stencil operations, `D3DBLENDOP_*`: `d3d8_gl.c` passes many to GL as they
  are): map them to Vulkan's once, in a table, in the guest, and send
  Vulkan's values.
- **Cull mode and front face**: start from `d3d8_gl.c`'s **desktop** branch
  (`D3DFRONT_CCW` is GL's `GL_CCW`), not its Android branch, whose inversion
  answers its y flip in the shaders, which the Vulkan shaders do not make
  (phase 4: the flip is the viewport's). Whether Vulkan's negative-height
  viewport inverts it again is seen in step 4 (a map culls everything): if
  geometry is seen from inside, invert the front face in one place, not the
  shaders.
- **Depth**: test, write, compare; the depth range comes from the viewport
  (dynamic). **Stencil**: test, each face's ops and compare (D3D8 has one
  face: both get the same), masks and reference dynamic.
- **Z bias**: `D3DDevice_SetRenderState_ZBias`'s comment in `d3d8_gl.c`
  says what the bias is (a constant and a slope term, for decals); depth
  bias enable in the pipeline, the values dynamic (`vkCmdSetDepthBias`;
  Vulkan's constant factor is in units of the format's minimum resolvable
  difference, as GL's `glPolygonOffset` units are: check decals in step 4).
- **Blend**: enable, the four factors, the two ops, the colour write mask
  (`D3DRS_COLORWRITEENABLE`); the blend constant dynamic.
- **Fill mode** (point, line, fill) and line width 1; the alpha test is in
  the pixel shader (phase 4), not here.
- **The attachments**: whether a colour target and a depth target are
  bound (phase 2's formats), so the pipeline matches the rendering it is
  used in.
- **Primitive** (below) and **vertex input** (below).

### Primitives

As `primitive_mode` and `quad_indices` in `d3d8_gl.c`: point, line list and
strip, triangle list, strip and fan map to Vulkan's; `D3DPT_QUADSTRIP` is a
triangle strip and `D3DPT_POLYGON` a triangle fan, as there; `D3DPT_QUADLIST`
becomes indexed triangles (two a quad, the guest makes the indices, as
`quad_indices` does), and `D3DPT_LINELOOP` a line strip with the first
vertex again at its end (Vulkan has no loop). Vulkan has no quads, so
deko3d's quad primitive does not apply.

### Vertex input

As `setup_streams` and `attribute_format` in `d3d8_gl.c` decide:

- **Each stream** a draw reads is put (`vk_data_put`) at the draw: for a
  plain draw, its vertices from the first to the last drawn; for an indexed
  one, from the lowest index to the highest (`index_extent`'s scan), the
  draw's vertex offset then minus that lowest index (as `d3d8_dk.c` does
  for deko3d's `vertexOffset`, and `d3d8_gl.c` with its base vertex), and
  `SetIndices`' base vertex index added as `d3d8_gl.c` adds it. Immediate
  mode's vertices (`End`) are put as they are (the next `Begin` reuses their
  buffer; the put copies them first).
- **Index data**: the draw's 16-bit indices put as they are (or the quad
  list's made ones). Phase 3's note: the offset of index data inside a piece
  must be a multiple of 2 - put the indices as a piece of their own.
- **Formats**: `D3DVSDT_*` to Vulkan, as `attribute_format` maps them to GL
  (the desktop branch: `D3DCOLOR` is `B8G8R8A8_UNORM`, which reads the
  Xbox's byte order, so no swizzle; `FLOAT2H` three floats; `SHORT*`
  unnormalised, `NORMSHORT*` and `PBYTE*` normalised; `NORMPACKED3` is
  `R32_UINT`, unpacked in the shader - phase 5's validation found that a
  packed attribute's format must be an integer one). **Not every such format
  is one a driver must read as a vertex attribute** (Vulkan requires few: the
  unnormalised `SSCALED` shorts and the three-component 8- and 16-bit ones
  are optional): the host checks each format the table can give for
  `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT` at the device's creation and tells
  the guest (an import, once) which are missing; the guest then puts those
  attributes expanded to `R32G32B32A32_SFLOAT` (a stream of its own, made at
  the draw from the game's bytes), and logs once which formats were
  expanded.
- **The attributes the declaration does not feed** read the current values
  `D3DDevice_SetVertexData*` set (`device.attributes`): one binding of
  stride 0 holding all sixteen, put at each draw whose values changed, every
  attribute the declaration leaves out read from it (phase 4: every
  shader declares all sixteen inputs). deko3d's "Progress" has the trap:
  its copy of these was made once a frame and reused across formats, so a
  `SetVertexData` between draws - how the HUD and menus set a colour - drew
  with the frame's first values.
- **Every one of the sixteen attributes is in the pipeline**, from a stream
  or from the stride-0 binding.

### Uniforms

The three blocks are made in the guest as `prepare_draw` makes the GL
uniforms (`struct draw_uniforms` in `d3d8_gl.c`, its uploads, and
`viewport_update_constants`), into `vk_shaders.h`'s structures: the vertex
constants (`c[192]`, with the reserved viewport constants), the viewport
scale and offset, the point size, the screen offset (the menus' centring),
the combiner constants, fog, the alpha reference, the bump matrices and
luminance, the linear textures' coordinate scale. No LOD bias here (phase
4: it is the sampler's).

### Textures

- **`port/android/guest/xbox_textures_vk.c`**, a copy of `xbox_textures.c`
  (the original stays in both images; the copy replaces it in the Vulkan
  image's objects, as `d3d8_vk.c` replaces `d3d8_gl.c`): its decoders and
  its cache kept; its GL calls replaced by commands. **BC1 to BC3 are sent
  as they are** where the device samples them (both Adreno drivers do;
  `xgpu_capabilities.s3tc`'s role), decoded to BGRA8 where not, as the GL ES
  path decodes them for Mali; every other format is decoded to BGRA8
  (`B8G8R8A8_UNORM`), as deko3d's copy does. Swizzled, palettized (the
  stage's palette), linear (with the coordinate scale), cube (six faces) and
  3D textures, every mip level.
- **Upload**: `VK_COMMAND_TEXTURE` (an image id the guest gives, type,
  format, size, levels) makes the image; `VK_COMMAND_TEXTURE_DATA` (id, level,
  face or slice, a data reference) fills a level; `VK_COMMAND_TEXTURE_FREE`
  releases it when the cache evicts it (the host destroys it once the frames
  that may use it have passed). The texels are put with `vk_data_put` when
  the cache uploads them, so they are the game's as they were then; the host
  copies them from the ring into the image (`vkCmdCopyBufferToImage`) outside
  any rendering (ending it), with a barrier from the image's last use
  (draws of this frame may still sample its old texels, recorded before) and
  one to the shader-read layout after.
- **Staleness: start the memory watch.** The cache tells a texture was
  rewritten by `memory_watch.c`'s write generations (page protection and
  announced writes). `d3d8_gl.c` starts the watch (`memory_watch_initialize`
  in `gl_initialize`); **`d3d8_vk.c` never does**, so without it a texture
  the game rewrites would be drawn stale for the run. Start it where
  `d3d8_gl.c` does.
- **Samplers**: from the stage's state as `configure_sampler` in
  `d3d8_gl.c` decides it: filters, mip filter, address modes, the border
  colour (Vulkan's three fixed ones, or `VK_EXT_custom_border_color`, which
  both Adreno drivers have, for another), the LOD bias (`mipLodBias`; phase
  4 took it out of the shaders), the maximum level, anisotropy (the device's
  `samplerAnisotropy` and its limit). Sent as a fixed-width state in the
  draw; the host keeps one `VkSampler` per distinct state (a table by hash:
  the device allows a few thousand).
- **A texture whose data is a render target's** samples the target's image,
  not the game's memory (where the GPU never writes): the guest notes every
  colour surface it binds as a target (address, size, last bound), and a
  stage whose texture's data is one names that target instead of an image
  id (with one level and the linear-texture scale from the target's size),
  as `bind_textures` in `d3d8_gl.c` and deko3d's `rendered_note` do. The
  host takes the target last bound at that address, transitions it to the
  shader-read layout (a barrier from its rendering), and back when it is
  drawn into again.

### The high-res HUD and text, the menus' art

`hud_hires.c`, `text_hires.c` and `menu_files.c` make GL textures and hand
GL names to `xbox_textures.c` (`texture_entry_result`); under the Vulkan
image those GL calls reach phase 1's stubs and do nothing. `xbox_textures_vk.c`
makes the three replacements itself as images of its own (deko3d's step 6
did this, and its "Progress" says how): the text's atlas (after the first
upload only the rows written since), a menu's art (by file name), a high-res
HUD bitmap (found by `hud_hires_override_find`), decoded as there and sent
as textures. It needs the accessors deko3d added behind `#ifdef HALO_SWITCH`
(`text_hires_atlas_rows`, `hud_hires_png_pixels`, `menu_art_name`,
`menu_art_png`): widen their guards to `#if defined(HALO_SWITCH) ||
defined(HALO_ANDROID)` - read-only accessors the GL ES image links but does
not call, the one kind of change to `port/linux/src` this plan allows; say
so in "Progress". The GL stub count (phase 1, `debug.gpu_stats`) must stay
at zero.

### Render targets, the mip composite, the screen

- Phase 2 makes the targets (keyed as `render_target_get` keys them, at the
  screen's scale for the screen's targets) with sampled usage. Sampling one
  is above.
- **The mip composite** (`mip_composite_get` in `d3d8_gl.c`: the water
  renders a texture one mip level at a time, each level a target): a
  mipmapped image the levels are copied into (`vkCmdBlitImage`), the levels
  below the last drawn made by halving, after a barrier from the levels'
  rendering; copied again only when one of its levels was drawn into since
  (deko3d's `composite_sampled`).
- **The viewport** in the target's pixels (the screen's scale), with a
  **negative height** (y from the bottom of the viewport's rectangle,
  height negated: Vulkan 1.1's rule) - the one place the picture is turned
  (phase 4) - and its depth range; **the scissor follows the viewport**
  (the NV2A's does by default, and split screen depends on it).
- **Direct3D's half pixel** is the vertex shaders' (`+ 0.5`, phase 4): not
  again in the viewport.

### Visibility tests

Occlusion queries (`occlusionQueryPrecise`, which both drivers have): a
query pool of 4,096 slots (the game's test indices, as `d3d8_gl.c`'s
`VISIBILITY_TEST_SLOTS`). `BeginVisibilityTest` marks the draws until
`EndVisibilityTest(index)` as the test's: the host begins the slot's query
before the first and ends it after the last. A query may not span a
rendering's end: if the rendering must end inside a test (a target change),
the test is counted over several queries (two slots of a second pool, added
up) - or, simpler and enough if the game never does it, the log says so
once and the count is of the part inside. Results: copied
(`vkCmdCopyQueryPoolResults`, outside rendering, at the frame's end) into a
host-visible buffer per frame; `GetVisibilityTestResult` asks the host
(an import) for the slot's latest finished count and never waits (as
`d3d8_gl.c`'s query buffer path: the latest from this test, or from an
earlier one while the GPU is behind); reset each slot before it is used
again (`vkCmdResetQueryPool`, outside rendering). **The count is in the
target's pixels**: divide by the target's scale (`visibility_unscaled`:
the game divides by its own test's area, so a count at the screen's scale
would make lens flares too bright).

### Pitfalls already known

From deko3d's phase 6, the earlier attempt and this plan's phases:

- **A pipeline must match the rendering it is used in** (attachment
  formats; the earlier attempt's driver dropped such draws silently): the
  state's attachments come from the targets bound, and validation runs at
  every step.
- **Layouts**: every image's layout is tracked; render targets go between
  attachment and shader-read; textures between transfer and shader-read.
  **Barriers outside rendering**; ending the rendering for a copy, a
  barrier, a query reset or a copy of results.
- **Descriptors bound by a recorded command are not rewritten**: a fresh set
  per draw, pools reset only when the frame's fence has passed.
- **Data references are this frame's**: a draw naming an earlier frame's id
  is a guest bug, said once by `host_vk_data_find`.
- **The unfed attributes' values**: put when they change, not once a frame
  (deko3d's trap above).
- **A texture written after draws that sample it**: the copy waits for them
  (a barrier), or the earlier draws sample the new texels.
- **A target sampled after it is drawn into** needs a barrier between; a
  target drawn into again after it is sampled, another.
- **Viewport and scissor are set at every draw** (Vulkan keeps no state
  across renderings: phase 2).
- **The memory watch** (above): started, or textures go stale.
- **A screen-sized count**: visibility counts divided by the scale.
- **The Xbox's enumerants are OpenGL's**: compare `d3d8_gl.c` before writing
  a mapping table.
- **`d3d8_gl.c` is the reference, at this branch's `main`**: it moved with
  upstream since `d3d8_vk.c` was cut (phase 1); where the Vulkan device
  copies a decision from it, copy it from the file as it is now, and say in
  "Progress" if a decision `d3d8_vk.c` already holds is older than the
  file's.

### Diagnostics, from the first step

- `debug.gpu_stats`: every 60 frames, draws made, immediate-mode draws, and
  draws skipped by reason (shader not ready, pipeline not ready, data
  missing, no target, a texture missing, too big), as `d3d8_gl.c`'s line
  does; phase 2's backend line and phase 5's services line as they are; the
  GL stub count (zero).
- `debug.texture_log`: every texture upload (format, size, levels, BC or
  decoded), as `d3d8_gl.c`'s.
- The validation layer at every step's end, on both drivers.
- **Screenshots by `adb exec-out screencap -p`**: the same scene under
  `renderer = "gl"` and `"vulkan"`, looked at side by side (the agent reads
  PNGs). Where the two differ, `debug.gpu_dump_shaders` and the draw log
  (`debug.gpu_trace_frame`, if the Vulkan device keeps it) say what was
  drawn.

### The order, each step tried on the device before the next

Each step: built, run on both drivers with validation on, compared with GL ES
by screenshot, its results in "Progress", committed.

1. **First draws.** The draw record, the pipeline state, the dynamic state,
   uniforms, vertex input and primitives, for draws without textures (a
   draw that needs a texture is skipped, counted, until step 2). The main
   menu's untextured draws (its fades and solid layers). **Done when**:
   draws are made with no validation error and nothing skipped but for
   textures; the statistics' counts match GL ES's for the same screen.
2. **Textures.** `xbox_textures_vk.c`, uploads, samplers, the dummies, the
   memory watch; the menus' textured draws. **Done when**: the Xbox-style
   parts of the menus (`display.menus = "xbox"`) look as under GL ES, by
   screenshot, and the picture is the right way up (the viewport's flip
   settled).
3. **The high-res HUD and text, and the menus' art** (deko3d pulled this
   ahead of its render targets, because the PC menus cannot be read
   without them; `display.menus = "pc"` is the default). **Done when**: the
   PC menus look as under GL ES; the GL stub count stays at zero.
4. **A map.** The first campaign map, from a new game: the opening
   cinematic (**does it reach play as under GL ES?** phase 2's open question:
   time both), then the cryo-tube and the first corridors; indexed draws,
   every vertex format met, culling and front face, depth, stencil, z bias
   (decals), fog, cube and 3D textures, skinned characters. And **phase
   4's corpus again, from play** (`debug.gpu_dump_shaders` and
   `tools/vk_shader_check.py`: fog, colour sign and alpha kill on stages 1
   to 3 were never met). **Done when**: the scenes look as under GL ES
   (render-target effects aside), nothing is skipped but for a shader or
   pipeline on first sight, and the shader check is clean on the new corpus.
5. **Render targets.** Render-to-texture (the sniper's zoom, screen
   effects), the mip composite (water), split screen (needs a second
   controller: the user's, see "Testing"). **Done when**: those scenes
   look as under GL ES, those the agent can reach by keys, and the rest
   are listed for the user.
6. **Visibility tests** (lens flares: the first map has lights through
   which they show). **Done when**: flares show and fade as under GL ES.

### Testing on the device

- The `.vk` build, both drivers, validation on for the step's runs and off
  for a last run of each step (to see the game's pace).
- **Reaching scenes by keys**: the menus by `KEYCODE_DPAD_*`, `ENTER`,
  `BACK`; a new game (ENTER, DOWN, ENTER, ENTER, ENTER); now that the
  screen shows the game, screenshots say where it is. The first map's play
  needs moving and looking: keyboard keys (the game's PC bindings, as on
  Linux: `port/linux/README.md`) by `adb shell input keyevent`, held keys by
  `input keycombination` or repeated events, or `debug.test_input =
  "look:<seed>"` (turns and looks, standing) and `"bot:<seed>"` (a scripted
  pattern) from `port_config.c`'s table.
- **What needs the user**: split screen (a second controller), the sniper's
  zoom and water if they cannot be reached by keys, and a judgement of the
  picture over a longer play: list them in "Progress" with what to look for.
- Move `config.toml` only with `adb pull` and `adb push` (phase 1's audit).

### Acceptance

1. `ninja android_apk` builds with no new warnings; `port/linux/src`
   changed only by the widened accessor guards, argued in "Progress"; the
   GL ES image unchanged on the device.
2. Each of the six steps done as it says, on both drivers, with screenshots
   compared with GL ES's, and validation clean.
3. Nothing the game draws is missing for longer than its shader's and
   pipeline's first compile, on a warm cache.
4. The opening cinematic's question answered (it reaches play, or why not).
5. The shader check clean on a corpus from play.
6. "Progress" has each step's result, every deviation, and what is left for
   the user, with what to look for.

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
- The default renderer and driver: Vulkan, on the phone's own driver, with Turnip one setting away on an Adreno (changed after phase 6, see "Decisions"); phase 7's results may reopen it.

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

### Phase 6 — audit (steps 1 to 6 as committed, `d4687ab2`)

Read against the spec, `d3d8_gl.c` at this branch and the Vulkan rules: the guest's mapping tables (compare, stencil, blend
factors and operations, topologies, polygon modes, address modes: the Xbox's OpenGL enumerants to Vulkan's numbers) are right
entry by entry; the raster state, the uniforms and the visibility result follow `apply_raster_state`, `prepare_draw` and the
desktop query path line for line; indexed draws put the vertices from the lowest index with the vertex offset its negative; the
unfed attributes are put when they change; the memory watch is started; the texture decoders are `xbox_textures.c`'s (the one
change there since the copy, `197c1994`, is a lookup speed-up, not a fix). The host's layouts, barriers, descriptor pools,
graveyard and query resets are where the rules want them. Fixed, built (`ninja android_apk`, no new warnings), **not yet run on
the device** (the user's testing was under way):

1. **A visibility test left open at the frame's end wrote its count into the game's slot 1** (`host_vk_visibility_frame_end`
   ended it as test 1): it is now closed and dropped; its `END`, in the next frame, finds no test, as before.
2. **Two copies into one texture with no draw between them had no barrier between them** (the text atlas's rows, a texture
   sent again in the same frame): copies are not ordered without one, so the older texels could land last. A transfer-to-transfer
   barrier now separates them.
3. **A compressed 3D texture went to the device as a BC image**, which Vulkan does not require a driver to make (2D and cube
   ones it does): 3D ones are decoded to BGRA.
4. **A render target sampled as a texture took its sampler type from the texture's description** (a cube or 3D one would have
   drawn the dummy), where `bind_textures` makes it 2D whatever the description: the key now says 2D for a texture whose data is
   a target, as there.
5. **Wireframe and point fill without `fillModeNonSolid`** would make an invalid pipeline on a device that lacks it (both
   Adreno drivers have it): filled there.
6. The mip composite's halving blits use the filter the colour format allows (`B.blit_filter`), not linear unconditionally.

Left as they are, for phase 7 if they show: the border colour as the nearest of Vulkan's three fixed ones (deviation 3; an
exact one is `VK_EXT_custom_border_color`); a mip composite copied again only when one of its levels' targets is bound again,
so draws into a level after it was sampled with no new `TARGETS` between are not seen until then (`mip_composite_get` copies at
every bind); more than 1,024 visibility tests in one frame would reuse a group before its results are copied; one
`VkSampler` per distinct state, with no limit against `maxSamplerAllocationCount`; composites, samplers and dummies are never
destroyed (the device lives as long as the game).

### Phase 6 — summary

Worked unattended on the test device (Adreno 750, validation layer on) as the `.vk` build; the user then took over the testing of what is
marked below. Steps 1 to 4 were tried on the device on both drivers and each is its own entry below; **step 5 was tried on the phone's driver
only, for the map's opening**; **step 6 was written and builds but has not been run at all**.

| Step | Written | Tried on the device |
|---|---|---|
| 1 first draws | yes | both drivers, validation clean |
| 2 textures | yes | both drivers, validation clean, Xbox menus match GL ES |
| 3 high-res HUD, text, menu art | yes | both drivers, PC menus match GL ES, GL stub count 0 |
| 4 a map | (no new code) | both drivers: the opening cinematic reaches play in the same 3 min 45 s as GL ES; 4.7 million draws, nothing skipped but first-sight pipelines, validation clean |
| 5 render targets | yes | phone's driver, the opening: shadows back, validation clean; **Turnip, the flash, the water, the sniper zoom and split screen not tried** |
| 6 visibility tests | yes | **not run** |

**What the user should test, and what to look for**

1. Build with `--android-vulkan-validation`, `debug.vk_validation = true`, `debug.gpu_stats = true`; look in logcat (tag `halo`) for `[error]`,
   `validation errors N` (the statistics line) and `vk: a draw is skipped`. The data folder's `debug.txt` is the game's.
2. **Step 6, lens flares** (the first map has lights through which they show): a flare should appear when its light is in view,
   fade as something moves in front of it and not show when it is hidden, as under GL ES. If flares are too bright or never go
   away the area division is the suspect (`vk_command_visibility_end.area`, `host_vk_visibility_retired`); if there is no flare at
   all, the count is not reaching the game (`host_vk_visibility`, the guest's `visibility_pending`). The result arrives two frames
   late, as the frame's fence passes. A validation error about a query ("not reset", "inside a render pass") means a test's
   rendering ended while its query was open: `host_vk_rendering_end` closes it, and a test uses up to four queries.
3. **Step 5**: the wake-up flash in the first map (GL ES: white and blurred as the cryo tube opens); the water (a map with
   water, the ripples' mip composite: a wrong picture or a validation error from `composite_sampled`'s barriers); the sniper's zoom;
   split screen (a second controller: each window's geometry and clears inside its half). On Turnip too.
4. **The shader corpus**: walk the first corridors and the first outdoor area with `debug.gpu_dump_shaders` naming a folder, pull it, and
   run `tools/vk_shader_check.py` on it (fog, colour-signed textures and alpha kill on stages 1 to 3 have never been compiled by
   anything); watch decals (z bias), distant fog, reflections and grass.
5. `display.renderer = "gl"` once at the end: the GL ES image was run after steps 3 and 4 and was as before; nothing in its objects has changed
   since the accessor guards (the host library, which both images share, has).

**Deviations from the spec in this phase**

1. The format check of the vertex attributes is made lazily, the first time the guest meets each optional format
   (`host_vk_format_supported`, an import), not at the device's creation. No format needed expanding on the phone's driver or Turnip.
2. The device is created with `samplerAnisotropy`, `fillModeNonSolid`, `occlusionQueryPrecise` and `textureCompressionBC` where it has them.
3. The sampler's border colour is the nearest of Vulkan's three fixed ones (transparent black, opaque black, opaque white), not
   `VK_EXT_custom_border_color`: the game sets a border colour rarely, and none was seen.
4. The texture images are suballocated from 64 MB blocks (a map has more textures than the 4,096 allocations Adreno allows).
5. A mip composite copies the levels the game drew with `vkCmdCopyImage` (equal format and size) and blits only the halving.
6. The visibility tests are driven by two commands (begin and end) rather than a field of each draw: the host keeps the test open
   across draws, and ends and begins a query where a rendering ends; the draw record's `reserved` field is what the plan called
   `visibility`.
7. `d3d8_vk.c` and `xbox_textures_vk.c` are copies, not extractions, as decided; `xbox_textures_vk.c` replaces `xbox_textures.c` in the
   Vulkan image's objects (`tools/android_build.py`).
8. The only change to `port/linux/src` is the widened guards of the accessors (step 3, argued there).
9. The cinematic's question (phase 2's open one) is answered in step 4: it reaches play, in the same time as GL ES.

### Phase 6 — progress (under way; each step's result, newest first)

**Step 6, visibility tests: written and built, not run.** `host_vk_visibility.c`: a pool of 4,096 occlusion queries in groups of four. The
guest sends `VK_COMMAND_VISIBILITY_BEGIN` at `BeginVisibilityTest` and `VK_COMMAND_VISIBILITY_END` (the game's slot and the target's scale
area) at `EndVisibilityTest`. The host resets the test's group outside a rendering at its beginning, begins a query at the first draw
inside a rendering, ends it at the test's end or where the rendering ends (`host_vk_rendering_end` closes it; the next draw opens the
group's next query, up to four, added up); at the frame's end, outside rendering, `vkCmdCopyQueryPoolResults` (waiting) copies the tests'
counts into a host-visible buffer of the frame's slot, and when that slot's fence has next passed the counts, divided by the scale
area (`visibility_unscaled`), become the game's slot's latest, which `D3DDevice_GetVisibilityTestResult` asks for through the import
`host_vk_visibility` and never waits for. Not seen on the device: see "Phase 6 - summary" for what to look for.

**Step 5, render targets: written and built, only partly tried on the device (the rest is for the user).**
*Render-to-texture.* The guest notes every colour surface it binds as a target (`rendered_note`, by address, with a clock) and a stage
whose texture's data is one names that target instead of an image (`VK_TEXTURE_TARGET`, the address, `xgpu_render_target_find`'s choice:
the one bound last), with the linear-texture scale from the target's size. The host (`host_vk_target_at`, `host_vk_draw.c`) takes the
colour target bound last at that address (`host_vk_target.bound`, a clock the targets command stamps), ends the rendering, and
transitions it to the shader-read layout with a full barrier (rendering_begin puts it back when it is drawn into again); a target
that is the one being drawn into gets a dummy and is counted as a texture missing.
*The mip composite.* A texture the game renders a level at a time (not linear, not a cube, more than one level, its top level the
size of the target drawn there: `bind_textures`'s test) sends `VK_COMMAND_COMPOSITE` once (the levels' addresses by
`xgpu_texture_level_offset`) and is sampled through a mipmapped image the host keeps for it (`composite_sampled`): each level the game drew
is copied from its target (`vkCmdCopyImage`, same format and size), the levels below the last drawn are made by halving
(`vkCmdBlitImage`, linear), each level's layout tracked, outside rendering; copied again only when a level's target has been bound since.
**Deviation:** copies rather than blits for the levels the game drew, since the formats and sizes are equal.

What was seen (phone's driver, validation on, the first map's opening, 7,680 frames): validation errors 0, nothing skipped, **the soft
blob shadows under the marines are back** (they were missing before this step, which is what a shadow drawn through a render target
looks like). **Not seen, for the user:** (1) **Turnip** with this step; (2) **the wake-up flash** when the cryo tube opens (GL ES goes
white and blurred, Vulkan showed black before this step; the sheet of screenshots taken around it was not finished); (3) **the water**
(the mip composite: a map with water, for example the Silent Cartographer, and ripples that look like GL ES's); (4) **the sniper
rifle's zoom** and the other screen effects; (5) **split screen** (a second controller: look for each window's geometry and clears
staying inside its half). The composite's barriers are the part most likely to draw wrongly or to draw a validation error: run with
`debug.vk_validation = true` and look for `[error]` lines.

**Step 4, a map: done on the device, both drivers, with what is left listed.** The first campaign map from a new game, no code
changed for it: the opening cinematic and the cryo bay draw through Vulkan as under GL ES, upright, with no face drawn inside out (the
desktop front-face rule of `raster_state_make` needed no flip) and depth right (the marines, the Warthogs, the Pelican, the consoles and
the HUD's "Use [right stick] to look around" prompt over the bay).

**The open question, answered: yes, the opening cinematic reaches play under Vulkan, in the same time as under GL ES.** From the key that
starts the new game to `object_create - 'cryotube_1' already exists` in `debug.txt` (the map's script reaching the cryo tube): Vulkan on the
phone's driver 20:31:50 to 20:35:35, **3 min 45 s**; GL ES 20:37:48 to 20:41:33, **3 min 45 s**; screenshots every 20 s put the
bay's "look around" prompt at the same shot (the 12th) in both, and both `debug.txt`s have the same lines in the same order (`too many lens
flares submitted to frame`, two `fell outside world and was erased`: the game's own, as phase 1 found under GL ES). Phase 2's 14 minutes
in a black picture were the black picture, not the game waiting. With `debug.test_input = "look:5"` the player looks around and the
prompt changes to "Use [X] to exit the cryo-tube" under Vulkan too.

| 7,680 frames of the map (128 statistics windows), validation on | Phone's driver | Turnip |
|---|---|---|
| draws made | 4,690,731 (about 610 a frame; 38,000 to 39,000 in a 60-frame window) | 4,696,417 |
| skipped for a shader / data / target / other / a texture missing | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 |
| skipped for a pipeline not ready | 201 (first sight of a state; the pipeline cache was warm) | 241 |
| images, texture memory | 117 images, 192 MB | the same |
| validation errors, `[error]` lines, `vk: a draw is skipped` | 0, none, none | 0, none, none (the Turnip pipeline cache file was written by this run) |
| vertex formats the device could not read | none: no expansion was needed | none |

Shot for shot against GL ES (`g4_*` against `m2_*`, 20 s apart) the cinematic's scenes agree: the Pillar of Autumn's engines, the bridge and
its consoles, the officers in red, the Pelican, the marines and the Warthogs, the crewman, the hands on the cryo controls. Differences
seen, which are step 5's: **the soft blob shadows under the marines on the floor** (GL ES has them, Vulkan does not) and **the wake-up
flash** (the picture goes white and blurs in GL ES at the moment the cryo tube opens, where Vulkan shows black with the "Reveille"
caption): both are made through render targets, which are step 5.

**The shader corpus from play** (`debug.gpu_dump_shaders` while playing, 847 files pulled to `build/shaders_play2`, `tools/vk_shader_check.py`):
90 vertex shaders and **222 pixel shaders, 0 failed**, spirv-val on every module, the reflection and the GL ES comparison clean, 35
of the key's 57 combiner words varying. **Not met, and not met by the menus either: fog, colour-signed textures and alpha kill on stages 1
to 3** (`alpha_kill[1..3]`, `color_sign[0..3]`, `fog_enable`, `fog_table_mode` never vary in 222 keys). The reason is not the key: nothing
the unattended input can do gets the game out of the cryo tube. `adb shell input` cannot press a gamepad button, a keyboard `X` does not
exit the tube, and `debug.test_input = "bot:5"` also plays the menus (it ended in Load Game); so the corridors, where the fog and the
bump-mapped materials are, were not reached. The phase-4 comparison of the generators with the GL ES ones is line for line there too,
but nothing has compiled those paths.

Left for step 4, for the user: walk the first corridors and the first outdoor area with the dump on (the same `debug.gpu_dump_shaders`
folder, `tools/vk_shader_check.py` on it), and look for: decals (bullet holes, scorch marks: z bias, `vkCmdSetDepthBias`'s units against
`glPolygonOffset`'s, not seen at all), fog in the distance, the cube-map reflections on shiny surfaces and an Elite's or a Marine's skin
(skinned characters were seen: the marines are right), grass and other dynamic vertex data. A pipeline skipped for a state's first
sight shows as a part of a scene missing for a frame or two the first time it is seen.

**Step 3, the high-res HUD and text and the menus' art: done on the device, both drivers.** `xbox_textures_vk.c` makes the three
replacements as images of its own from the same id allocator (`VK_HIRES 1`): the text's atlas (2048 square, white with the coverage as
alpha; after the first upload only the rows written since, as `VK_COMMAND_TEXTURE_DATA`'s new `top` and `rows`), a menu's art (by
file name, kept) and a high-res HUD bitmap (found by `hud_hires_override_find`, kept); PNGs by `hud_hires_png_pixels`, their mip levels
by halving, red and blue swapped to BGRA; their samplers filter linearly from the levels (`hires`). **The one change to
`port/linux/src`**: the four read-only accessors deko3d added (`hud_hires_png_pixels`, `text_hires_atlas_rows`, `menu_art_name`,
`menu_art_png`) and `menu_art_serial` with the serial it returns, which sit behind `#ifdef HALO_SWITCH` in `hud_hires.c/.h`,
`text_hires.c/.h` and `menu_files.c/.h`, are now behind `#if defined(HALO_SWITCH) || defined(HALO_ANDROID)`; the GL ES image links
them and does not call them (and the serial is bumped where art is registered, which costs nothing), so it is unchanged in what it does.

PC menus (`display.menus = "pc"`, the default): the main menu (title, the five items, the version number in orange, the planet, the
stars and the ring) and the Campaign screen (CONTINUE / NEW GAME / LOAD GAME / BACK) draw in the Vulkan image as in GL ES, on the
phone's driver and on Turnip (the ring is at another point of its loop, which two screenshots of the same renderer show too: a run
under GL ES at 25 s has the ring away and one at 32 s has it in front, and Vulkan's at 24 s and 30 s the same way round). 31 to 41
images, 0 draws with a texture missing, nothing skipped for a shader, pipeline, data or target after the first window; validation 0
errors; **the GL calls the shared files still make: 0 of the 102 imports in 1,650 frames**, on both drivers.

**Step 2, textures: done on the device, both drivers.** `xbox_textures_vk.c` (a copy of the deko3d copy, which is a copy of
`xbox_textures.c`; it replaces the original in the Vulkan image's objects, `tools/android_build.py`) keeps the decoders and the
cache; a texture is `VK_COMMAND_TEXTURE` (the image's description, numbers from 1 handed back when the cache drops one) and one
`VK_COMMAND_TEXTURE_DATA` for each level of each face, the texels put with `vk_data_put` when the cache uploads (so they are the
game's as they were then). BC1 to BC3 go as they are where the device samples them (`host_vk_bc_supported`; both drivers do, the
log says `BC1 to BC3 textures are sampled as they are`), else decoded to BGRA by the Mali path's decoder copied in. The host
(`host_vk_texture.c`) makes the images from suballocated 64 MB blocks (a map has more textures than the 4,096 allocations
Adreno allows), copies from the ring outside any rendering after a barrier from the image's last use, and makes the written images
readable before the next draw; a dropped image is destroyed when the frame slot's fence has next passed. The stages' samplers are
`configure_sampler`'s state (`vk_sampler_state`, one `VkSampler` for each distinct state on the host). The stage's dummy is the
image of the type the shader declares. `memory_watch_initialize` was started in step 1.

Xbox menus (`display.menus = "xbox"`), 60-frame windows: 6 draws + 92 to 94 immediate a frame under both (GL ES: 7 + 94 to 118 over the
animation); Vulkan makes all of them (6,060 in 60 frames), skipped 0 for a shader, a pipeline, data or a target, **0 with a texture
missing**; 22 images, 64 MB of texture memory, 2.7 MB copied in the first window and 0 after. Validation: 0 errors on both drivers
(nothing but the layer's cache-file note and the expected pre-transform note). Screenshots: the Halo logo, "CAMPAIGN / MULTIPLAYER /
SETTINGS", the planet, the stars and the ring are as under GL ES and **the right way up**, identically on the phone's driver and on
Turnip; the ring is at another point of its animation (the two runs are not frame-locked) and the orange version number at the
lower right, which is the high-res text (the atlas), is missing until step 3, as the plan says.

**Step 1, first draws: done on the device, both drivers.** `VK_COMMAND_DRAW` (`vk_commands.h`) is one self-contained record
(shader handles, `vk_pipeline_state`, viewport, scissor, depth bias, blend constants, stencil masks, the three uniform blocks and the
vertex and index data as data references, four texture slots); the guest makes it in `draw_make` (`d3d8_vk.c`: `prepare_draw`'s
shaders and key, `apply_raster_state`'s state through one table from the Xbox's enumerants (which are OpenGL's) to Vulkan's,
`setup_streams`' vertex input, `quad_indices`, a line loop as a strip with its first vertex again) and the host records it
(`host_vk_draw.c`: the pipeline from `host_vk_pipeline_find`, a descriptor set a draw from the frame's pools, the dummies of the
three sampler types, a sampler table, the viewport with a negative height, the scissor following it, the draw). The draw note of
phase 5 (the placeholder `VK_COMMAND_PIPELINE`) is gone from the guest. `memory_watch_initialize` is started where `d3d8_gl.c` starts it.

Main menu (`display.menus = "pc"`), 60-frame windows, validation on:

| | GL ES | Vulkan, phone's driver | Vulkan, Turnip |
|---|---|---|---|
| draws a frame | 7 draws + 118 immediate (125) | 1 immediate made, 124 skipped as textured (125) | the same |
| skipped for a shader / pipeline / data / target / other | 0 | 0 / 0 (after the first window) / 0 / 0 / 0 | the same |
| validation errors | | 0 | 0 |

The screen is the fade only (every other draw is textured until step 2): the picture is black, the GL ES one is the whole menu.
The GL stub count stays 0. Not decided by this step: the front face (nothing culls in the menus), the picture's orientation.

Choices of this step: the unfed attributes are one stride-0 binding put whenever `SetVertexData` changed them (compared with what
this frame last put); formats the device cannot read as attributes are asked for (`host_vk_format_supported`, an import, once for
each optional format, lazily instead of at the device's creation) and expanded to four floats by the guest; the device is created
with `samplerAnisotropy`, `fillModeNonSolid`, `occlusionQueryPrecise` and `textureCompressionBC` where it has them.

### Phase 3 — audit

Nothing to fix. Read against the spec and the code: the guest's
`vk_data_put` never overruns the stream (a part is at most what is left, the
record's padding zeroed) and puts the bytes as they are at the call; the
self-test's third piece does straddle a part boundary; ids reset with the
frame on both sides. The host copies only the record's payload (no guest
address of data is read), places each id once and never moves it, checks
parts in order, resets a frame's ring only after that frame's fence (each
slot is marked at its own `PRESENT` and reset at its next use), and drops the
rest of a hand-over at a bad record. `IsBusy`, `BlockUntilNotBusy` and the
locks are untouched. Re-run at the tip (`13e5fc09`), validation on, on both
drivers: the clears self-test and the three data cases `ok`, 0 validation
errors, nothing but the layer's cache-file note and the expected
pre-transform note.

For phase 6: data offsets are aligned to 256 (or the uniform alignment), but
an offset a command adds inside a piece is the command's to align: an index
buffer's offset must be a multiple of its index size, and a draw's vertex and
index ranges should start at offsets the formats are aligned for.

### Phase 4 — audit

The record is true to the code, and the proof holds. Read: the generator
copies differ from `nv2a_vsh.c` and `nv2a_psh.c` only by what the spec lists
(version, blocks with explicit offsets and bindings, locations, no ES
branches, `point_and_screen`, `alpha_reference.x`, no LOD bias or counter);
`vk_shaders.h` pins every offset once, checked against the structures at
compile time, and carries a generator version for phase 5's cache;
`pixel_key_make` is `prepare_draw`'s key without GL; every draw entry point
the game has (`DrawVertices`, `DrawIndexedVertices`, immediate mode's `End`)
notes its draw. Re-run on the PC from the committed script, on the corpus
in `build/shaders`: 308 shaders, 0 failed, `spirv-val` on every module, the
same key-field counts; the GL ES run's keys (`build/shaders_gl`) share 213
hashes with the Vulkan run's, the 5 each side has alone being the GL
renderer's `count_samples` keys and their zero twins.

Fixed: a stray `frag.spv` (a hand-run `glslangValidator` writes one into the
current folder) was committed at the repository's root; removed, and `/*.spv`
is ignored there.

Open, outside this branch: deviation 4's finding is a bug in the Switch's
deko3d renderer. `port/switch/guest/nv2a_vsh_dk.c` was copied before
`nv2a_vsh.c` gained the camera-plane guard (commit `3d2c04d6`, "Fix
vertices exploding to the screen centre near the camera plane"), and copies
do not follow their originals, so the Switch's deko3d image still draws
vertices out to the screen's centre where its OpenGL image no longer does.
For the user, on the Switch's branch.

For phase 6: the corpus has no fog, colour-signed textures or alpha kill on
stages 1 to 3 (none met in the menus and the opening). The generators are
line-for-line copies there too, but nothing has compiled those paths yet:
run the dump and `tools/vk_shader_check.py` again on a corpus from play once
the game draws.

### Phase 5 — audit

The record is true to the code: the guest's hashes are the spec's (the key
zeroed first, so its padding hashes the same), the GLSL is copied during
`host_vk_shader_compile`, the locks are taken in one order (the backend's,
then the services'; the compile thread takes only the services'), and the
`VkPipelineCache` is used from the compile thread and read for saving from
others, which Vulkan allows (a pipeline cache is synchronized by the
implementation unless made with `EXTERNALLY_SYNCHRONIZED`). Fixed:

- **Two saves of the pipeline cache could run at once.** The game thread
  saves (the surface's loss, five minutes, the exit) and so does the
  activity's thread (deviation 5's watch, which fires twice when the app
  goes away: "will" and "did"), with nothing between them: both wrote the
  same `.tmp` file, and the one renamed last could be a mixture (the CRC
  would then have thrown the whole cache away at the next start). One save
  at a time now (a lock around the whole of it), and the count of pipelines
  made since the last save is read and cleared under the services' lock,
  less what was made while the file was written. On the device (phone's
  driver, validation on): a cold pipeline cache, HOME after 40 s: one
  `pipeline cache saved (the app went to the background): 90793 bytes` (the
  second event found nothing new), and the next start `loaded from the
  device`, no error.
- **A compile request the host dropped was silent**: when the host could
  not queue a shader (out of memory, or its table full), the guest asked
  three times and stopped, and that shader's draws would have been skipped
  for the run with nothing in the log. The host now says so (the first
  eight times).

For phase 6: the pipeline cache grows with every state the game uses and
has no size limit (the record says so too); the placeholder pairs stay in
it, harmlessly.

### Phase 5 — summary

Worked unattended on the test device (Adreno 750, validation layer on in every run) as the `.vk` build. **Phase 5 works on both
drivers**: the guest asks for each draw's shaders by hash, the host compiles what it lacks on a thread of its own and keeps the SPIR-V
on the device, a pipeline (with the placeholder state) is made for each pair, and a `VkPipelineCache` per driver is kept and saved.

| | Phone's driver (Qualcomm 512.762.40) | Turnip v26.0.0 R8 |
|---|---|---|
| Cold start (`vk_cache/` removed), menus and a new game's opening | 143 shaders compiled (glslang 2.5 to 3.4 ms, `vkCreateShaderModule` 0.7 to 1.4 ms each), 108 to 109 pipelines made (first batch 9 pipelines at 11.0 ms each, later ones 0.3 to 8 ms), 0 failed; the first 60-frame window had 1495 draws that would be skipped for a shader and 1357 for a pipeline, the next window 0; a new screen's window had 22 and 13, the next 0 | the same numbers (first batch 16 shaders at 2.8 ms glslang, 9 pipelines at 9.0 ms), 143 compiled, 109 pipelines, 0 failed |
| Second start | `shader cache ...: 143 shaders on the device`, `pipeline cache ...: loaded from the device`; 143 from the cache, 0 compiled, 0 failed; pipelines **0.42 ms** each on average in the first batch (cold: 11.0 ms) and 0.37 ms later | 143 from the cache, 1 compiled (a menu screen the first run had not met), pipelines **0.17 ms** (cold: 9.0 ms) |
| The files | `pipelines/ae4a9208435100000000011405430000-8-802fa028.bin`, 568 KB after the menus and the opening; 144 `.spv` files in `spirv/16.6.0-1/` | `pipelines/980bbf894907979323fe10eda42c11f3-18-06463063.bin`, 895 KB; the same `.spv` files |
| Both in turn (Turnip's cache present, then the phone's started, then Turnip again, no removal in between) | each loads its own file (`loaded from the device`; the phone's, started `new` when only Turnip's existed), both files are kept, the SPIR-V files are shared | |
| Broken files (Turnip) | three `.spv` truncated to 100 bytes: `the cached shader ... is damaged (100 bytes); deleted, to be compiled again` x3, then 3 compiled; the pipeline cache truncated to 400000 bytes: `is damaged (400000 bytes); deleted, a new one is started`, then a new file saved (895 KB) | |
| A cache file that passes the file's own checks but is junk to the driver (phone, 4 KB of random bytes) | the driver accepts it and ignores it (the spec allows it; the log says `loaded`, the next save replaces it) | |
| The save | `pipeline cache saved (the app went to the background): N bytes` the moment HOME is pressed (see deviation 5); also `(the surface was lost)` on return and `(five minutes)` after five minutes of a run (568 KB) | the same |
| Frames, validation | 1650 frames per 10 s in every run (as phase 3, the compile thread does not show); validation errors 0, no `[error]` line, the layer's two cache-file notes only | the same |
| Shader service self-test (`debug.vk_self_test`) | `a vertex shader that compiles ok (handle 1, ...)`, `a pixel shader that compiles ok`, `a pixel shader that does not ok (handle 0, status 3 ...)`: glslang's message logged once (`'this' : Reserved word`) and the GLSL in `vk_cache/failed/f5e1f7e57deadbef1.frag`; the clears and data self-tests as before | |

The 64-bit hash crosses the import boundary intact: the guest logs `hash aa684cffee1030c6` and the host `hash aa684cffee1030c6` for the
first find. `renderer = "gl"` runs unchanged (`renderer: GL ES`, no `vk:` line). The phase 4 probe step `shaders` still passes with the
renamed library (90 vertex, 218 pixel shaders, `validation.summary: 2 messages, 0 errors`).

**Found on the way**: the first placeholder gave every attribute `R32G32B32A32_SFLOAT`; the layer rejected the pipelines of the
programs with a packed attribute (`in uint` in the shader: VUID-VkGraphicsPipelineCreateInfo-Input-08733). A packed attribute's format
is now `R32_UINT` (the placeholder takes the mask); phase 6 must do the same from the declaration.

**What is left for the user**: a second GPU family is not tried; a shader that fails to compile was only met by the self-test
(none in the game's corpus); the cache is saved at the background, at the surface's loss, every five minutes and at the game's own exit
(`host_exit`), but a process killed from the task switcher while it is in front is lost since the last of those (the first
four-or-five minutes' pipelines); the pipeline cache grows with the placeholder pairs and phase 6's states make many more, which a
size limit has not been put on.

### Phase 5: deviations from the spec

1. **`host_vk_shader_find`'s out parameter** is a guest address of a `uint32_t` (as deko3d's `state_out`), written during the call, with
   the statuses of `vk_shaders.h` (unknown, queued, compiling, failed) and one more, `NOT_READY` (the backend's device is not made yet).
2. **The imports make the device if it is not made yet** (`host_vk_backend_ensure`): the game's first draws come before its first
   hand-over, which is where the device was made, and a shader asked for earlier would have been counted as unknown.
3. **Pipelines are looked up by handles** as far as the guest is concerned (the command carries the handles), and by the two shaders'
   hashes and the state's FNV-1a hash in the host's table, which is the identity the spec asks for.
4. **One queue each for shaders and pipelines**, shaders first; a pipeline is only asked for with made handles, so none ever waits behind a
   shader.
5. **The pipeline cache is also saved from SDL's `WILL_ENTER_BACKGROUND` event** (an event watch, on the activity's thread): Android stops
   the game thread when the app goes away, so the surface's loss is found only when it returns, and a process ended meanwhile would
   have saved nothing. The surface's loss, five minutes and `host_exit` save as the spec says. Nothing is written when no pipeline
   was made since the last save.
6. **The pipeline cache file is wrapped** in a header of its own (magic, size, CRC) that is checked before the driver sees the data; the
   driver's own check (`vkCreatePipelineCache` failing on it) is kept as the spec says, but the drivers here ignore bad data instead
   of failing, which the spec allows.
7. **The SPIR-V files** have the header the spec describes (magic, version, hash, size, CRC); a file that fails is deleted when it is
   asked for (not at the start: the start only lists names).
8. **The statistics**: the services' line is printed with one statistic line in ten (about every four seconds), the compile thread's every
   five seconds when it worked.
9. **The placeholder takes the packed mask** (found above), which the spec's placeholder (sixteen four-float attributes) does not say.
10. **The self-test** of `debug.vk_self_test` gained the shader service's three cases.
11. **The compile thread** is niced with `setpriority(PRIO_PROCESS, tid, 10)`.
12. **An old `libglslang_probe.so`** in the build's staging folder is removed by `tools/android_build.py` so that it does not ride along in
    the APK.

### Phase 4 — summary

Worked unattended on the test device (Adreno 750) and the PC. **Phase 4 works**: every shader the game met in the corpus compiles with
glslang, validates, lays its blocks out as `vk_shaders.h` says, and says exactly what the GL ES generators say, on the PC and on both
drivers.

**The corpus.** Collected with `display.renderer = "vulkan"` and `debug.gpu_dump_shaders` naming a folder (pulled to `build/shaders`, not
committed): the main menu and its screens (each item entered and left), then a new game's opening cinematic on the first map, about 15
minutes in all. The screen is black, so the first fight and a second map were not reached (nothing can be played until phase 6): the
corpus is the menus' and the opening's shaders. 835 files: 90 vertex shaders (67 programs with their immediate-mode form, mask 0, and 23
with a packed-attribute mask, `e` or `8e`) and 218 pixel shaders, each with its `.gl` twin (and a `.key`). The 218 keys give 76 distinct
GLSL texts (keys differ in combiner words that the text does not use). How the key's fields vary over the 218: 35 of its 57 combiner
words vary; `texture_modes` 13 values; `sampler_type[0..3]` 3, 4, 4, 3 (none, 2D, cube); `alpha_kill[0]` 2 (stages 1 to 3 never);
`alpha_test_function` 2; `coverage_alpha` 2; `color_sign`, `fog_enable` and `fog_table_mode` never vary; `count_samples` is 0 by design.
The fields that never vary are the corpus's, not an unfilled key: **the same run under the GL ES renderer** (`debug.gpu_dump_shaders`
under `renderer = "gl"`, `build/shaders_gl`) made 218 pixel shaders too, and **213 of the 218 key hashes are identical** (`hash_words`
of the key's bytes); the other 5 are GL's own keys with `count_samples` 1 (a visibility test with atomic counters, which the Vulkan
device never sets), whose GLSL is the same text apart from the counter. The GL ES run's 76 distinct pixel texts (normalised for the
counter) are the Vulkan run's 76, and its 31 distinct vertex texts are among the Vulkan run's 90 (GL compiled only what it drew; the
Vulkan dump writes every program at mask 0 as well).

| | PC | Phone's driver (Qualcomm 512.762.40) | Turnip v26.0.0 R8 |
|---|---|---|---|
| Compile (`glslangValidator -V --target-env vulkan1.0`; glslang in the probe) | 308 of 308 | 90 vertex, 218 pixel, 0 failed | the same |
| Reflection against `vk_shaders.h` (binding, size, offsets, samplers) | 0 differences | | |
| spirv-val (built from the SPIRV-Tools glslang pins) / the layer's validation of each module | 0 failures | `shaders.vertex.count: 90 compiled and made without a complaint, 0 failed`, `shaders.fragment.count: 218 ... 0 failed`, `validation.summary: 2 messages, 0 errors` (the layer's cache-file notes) | the same, 2 messages, 0 errors |
| Comparison with the GL ES twins | 308 of 308 identical after the listed changes | | |
| Times (glslang / `vkCreateShaderModule`) | | vertex 3.9 / 1.8 ms average, 350 / 163 ms in all; pixel 1.6 / 0.41 ms average, 348 / 89 ms in all; 1.0 s for all 308 | vertex 3.6 / 1.7 ms, pixel 1.6 / 0.37 ms; 0.9 s |
| Slowest | | glslang: `vs_48_e` 100 ms (the first compile of the process: it pays the front end's start), then `vs_11_0` 5.5, `vs_32_e` 4.7, `vs_10_e` 4.6 ms; pixel `ps_297f687f` 4.7, `ps_59a624fb` 3.9, `ps_1034bada` 3.7 ms | `vs_48_e` 78 ms (the same), `ps_297f687f` 3.2 ms |

The comparison was tried on a changed shader (a one-line difference in a pixel shader): it fails with the diff. The game itself, with
the dump off, runs as in phase 3 on both drivers (4 images, 240 clears per 60 frames, validation errors 0, no wait over two seconds),
and `renderer = "gl"` runs (the corpus was also collected under it): `port/linux/src` is not changed.

**What is left for the user**: the corpus has no map's fight, fog, colour-signed textures or alpha-killed stages 1 to 3 (they never
occur in what was played unattended): a corpus from play (the dump while playing under GL ES, which writes `ps_<hash>.glsl`, or under
the Vulkan image once it draws) would add them; `tools/vk_shader_check.py` runs on any folder of the Vulkan image's dump. A second GPU
family is not tried.

### Phase 4: deviations from the spec

1. **The key is made only when `debug.gpu_dump_shaders` names a folder** (the dump is its only use until phase 6), not at every draw
   unconditionally. Phase 6 makes it unconditional.
2. **The draw's note does not bind the targets** (`prepare_draw` skips a draw with no target; the Vulkan device's `targets_bind` writes a
   command, which phase 6 owns), so a draw without a target is dumped too.
3. **`coverage_alpha`** is decided as the texture cache does (`hud_hires_override_find` on the bitmap's address and sizes, then
   `hud_hires_override_coverage`), without the high-res texture's own decoding (that needs GL, and would fail only there). The key
   matched the GL renderer's for 213 of 213 comparable keys. A render-target texture is not told from a bitmap here (`d3d8_gl.c` tells
   it by its target table); it takes the bitmap's path, and has coverage only if its pixels are a HUD bitmap's.
4. **The vertex generator keeps the camera-plane guard** (`if (!(abs(position.w) > 0.0)) position = vec4(0.0, 0.0, 0.0, -1.0);`) that the
   original has and `nv2a_vsh_dk.c` lost: the spec says everything else is kept line for line, and the comparison proves it.
5. **The copies' instruction words are `const uint32_t *`** (as the deko3d copies), because `vk_shaders.h` is shared with the host, which has
   no `DWORD`; the generated text is the same.
6. **Every vertex shader is also written at mask 0 at the 60th Present, not the first**: the game makes its vertex shaders during its
   start-up, so the first Present would find some missing. The log says `shader dump: N vertex shaders and M pixel shaders written`
   every 600 frames when more came.
7. **The GL ES twins are made in the Vulkan image** by the original generators, with `xgpu_capabilities.shading_language` set to
   `"310 es"` for the call (the Vulkan image has no context to read it from).
8. **A pixel file name collision** (two keys with one 32-bit hash) would get `_<n>` appended (none met: 0 in 218).
9. **The comparison** builds the Vulkan text it expects from the GL twin and from `vk_shaders.h`'s numbers (the whole header of each stage,
   exactly), and compares the rest line by line. The reflection reads glslang's `-q` output (members the shader does not use are not
   listed there, so only the used ones are checked; the blocks' sizes always are). `spirv-val` is fetched and built into
   `build/host-spirv-tools` from the commits glslang's `known_good.json` pins, once.
10. **The probe step reads the folder from `config.toml` itself** (`debug.gpu_dump_shaders`), like the probe's other settings; "all" runs
    it, and it says `skipped` when no folder is named. A warning or error from the validation layer while a module is made is a failure
    of that shader; the first compile of the process (80 to 100 ms) is the front end's start, not a slow shader.

### Phase 3 — summary

Worked unattended on the test device (Adreno 750) as the `.vk` build, validation on. **Phase 3 works on both drivers.**

| | Phone's driver | Turnip v26.0.0 R8 |
|---|---|---|
| `debug.vk_self_test`, clears | `vk: clears self-test: ok (...)` | the same |
| `debug.vk_self_test`, data | `vk: data self-test: id 1 ok (wanted R255 G0 B0, seen R255 G0 B0)`, `id 2 ok (R0 G255 B0)`, `id 3 ok (R0 G0 B255)` | the same three lines |
| Statistics line | first one after the test: `data: 4 records, 68 KB a frame, 1 ring buffers` (three puts, one split into two parts); then `data: 0 records, 0 KB a frame, 1 ring buffers` | the same |
| Stress (temporary hook in the guest's `Present`, not committed: three 6 MB puts a frame, each split into parts across hand-overs, and a test draw of each, every frame) | 6144 tests `ok`, 0 `FAILED`; `18432 KB a frame, 4 ring buffers` (a second 16 MB buffer made in each frame's ring, kept); validation errors 0 | 6158 `ok`, 0 `FAILED`, the same line, retired equal to submissions |
| Menus and a new game's map, no self-test | 60 hand-overs, 240 clears and 4 images per 60 frames, `data: 0 records`; validation errors 0; no wait over two seconds | the same |
| `renderer = "gl"` | `renderer: GL ES`, no `vk:` line, the game runs | |
| Validation | only the layer's cache-file message and the expected pre-transform note from phase 2 | the same |

Nothing in the backend waits on the GPU for a resource: the only waits are the frame's fence (phase 2's, which the frame's command buffer needs as well) and the self-tests' own. `IsBusy`, `BlockUntilNotBusy` and the locks are as they were. The host reads only the bytes the guest put into the stream for a draw (`host_vk_data_command` copies out of the record; no command carries a guest address of data).

**What is left for the user**: a second GPU family is not tried; a single piece of data larger than a ring buffer (16 MB, which gets a buffer of its own) and the bad-record paths (part out of order, id reused) are written but not met on the device; the game itself puts no data until phase 6.

### Phase 3: deviations from the spec

1. **`VK_COMMAND_TEST_DRAW`** carries a `vk_data_ref` (id and offset: it draws the three vertices at that offset), the colour expected, and a `last` flag. With `last` the host lets the frame's data go afterwards (as `PRESENT` does), since the self-test runs at the device's creation, before any `PRESENT`, and the game's first frame would otherwise find ids 1 to 3 taken ("id reused").
2. **The third buffer's vertices straddle the first part's boundary** (they start 40 bytes before the end of the stream's room) rather than being at its start, so that a part placed anywhere but contiguously would show garbage; the buffer is the stream's size plus 4 KB, so it always splits.
3. **The ring is reset lazily**: at the first `DATA` record of a frame after its fence has passed (the record opens the frame's command buffer, which waits for the fence), and `PRESENT` marks the frame's ring for reset (`ring_valid`). The spec says only "reused when the fence has passed".
4. **A ring's pieces are placed first-fit** over all its buffers (never moved), 16 MB buffers, a larger piece getting its own size.
5. **The test's 16x16 target stays alive** as an image (counted in "images").
6. **The test draw submits and waits** on the frame's fence (`frame_submit_and_wait`, shared with the clears self-test, which now also stops if the frame could not be submitted instead of waiting on a fence that is never signalled).
7. **`debug.vk_present_marker` no longer runs the clears self-test**: `debug.vk_self_test` does, and the marker only draws the marker. The host reads the new setting in `host_main.c` like the others.
8. **"KB a frame"** in the statistics is the average over the 60 frames.
9. **The ring and table live in a new `host_vk_data.c`**, with `host_vk_buffer_make` (host-visible, coherent, mapped) shared with the test.

### Phase 2 — audit

The audit found the record true to the code and no fault in what runs: the
guest's clears follow `d3d8_gl.c` to the arithmetic, layouts are tracked and
every barrier is outside a rendering, the clear pipelines match the
rendering they are used in, clear rectangles are kept inside the render area,
and the swapchain's semaphores, letterbox, transform and loss paths are as the
spec says. Fixed after it:

- **An acquired image could be stranded**: when a frame's `vkQueueSubmit`
  failed (other than a lost device) after an image was acquired, the acquire
  semaphore stayed signalled with nothing waiting on it, which makes the
  frame's next acquire invalid. An empty batch now waits on it and signals
  the image's render-done semaphore, and the image is presented; if that
  fails too, the swapchain is let go and the semaphore made anew with the
  device idle (`present_unsubmitted`). Not met on the device: it needs a
  failing submit.
- A stale comment and an empty block in `swapchain_recreate` that described
  an order of events that does not happen.

Left as it is: Android answers most presents with `SUBOPTIMAL` (the
transform the swapchain leaves to the compositor), and each one looks at the
surface's size, which costs a call a frame and is right.

Open, for when the game draws: a new game's opening cinematic was not seen
to end under Vulkan in 14 minutes (phase 2's deviation 12). Whether that is
timing or the game waiting on something the device answers differently
(visibility tests report 0, `IsBusy` is false) is looked at once the screen
shows the game.

### Phase 2 — summary

Worked unattended on the test device (Lenovo TB321FU, Adreno 750, Android 16) as the `.vk` build, with the validation
layer in the APK and `debug.vk_validation = true`. **Phase 2 works on both drivers**: the game's clears reach the
screen through Vulkan, paced by a FIFO swapchain at the display's rate, with the surface's loss and return, rotation and
validation all clean.

| | Phone's driver (Qualcomm 512.762.40) | Turnip v26.0.0 R8 |
|---|---|---|
| Device | `vk: device ready: queue family 0, colour targets B8G8R8A8_UNORM, depth targets D24_UNORM_S8_UINT, blit filter linear, dynamic rendering from Vulkan 1.3` | the same line |
| Swapchain | `vk: swapchain 2560x1600, 6 images, format R8G8B8A8_UNORM, FIFO, preTransform identity`, then `vk: first frame presented` | the same |
| The clears' self-test (below) | `vk: clears self-test: ok (a full clear, alpha only on the left half, red only on the top right quarter; 2 partial clears drawn, 2 by vkCmdClearAttachments)` | the same line |
| Menus, with `debug.vk_present_marker` | screenshot: a red square at the top left and a green one at the top right (pixel (10,10) is (255,0,0), the top right corner (0,255,0)), the rest black: the picture is the right way up and fills the screen (the back buffer is 768x480, 16:10 as the screen) | the same |
| Letterbox (`display.screen_width = 640`) | the picture 2133 pixels wide with 213-pixel black bars: the red square starts at x = 213, the green one ends at x = 2347 | the same |
| Frames | `vk: 1635 frames swapped in 10.0 s, paced by the swapchain (the display mode says 165.0 Hz)`; the statistics line: `60 frames: 60 hand-overs; commands: 60 targets, 60 clears, 60 presents; clears: 0 drawn, 60 by vkCmdClearAttachments; 0 target changes; 4 images; submissions 1741, retired 1739; 0 swapchain re-creations; validation errors 0` | the same, `retired` equal to `submissions` |
| First map's opening cinematic | 4 to 6 images (the screen's 768x480 colour and depth, 64x64 and 128x128 colour targets), 3 to 5 clears a frame, all by `vkCmdClearAttachments`; the map's objects were created at 16:27:21, 2 minutes after the start, and the game went on for 14 minutes (the opening's end was not reached, 12 of the deviations) | the same (`cryotube_1` at 16:22:20) |
| Backgrounding, ten times (HOME, `am start`) | each loss and return logged once: `vk: the surface is lost (vkAcquireNextImageKHR)` or `(the window's ANativeWindow changed)` or `(the window has no ANativeWindow)`, then `vk: the surface is back after 4.1 ms, 1 attempts: swapchain 2560x1600, 6 images, transform identity` (2.5 s when the app was away for the whole 3 s); the picture (the marker) is back after the tenth | the same |
| Rotation (`user_rotation` 1, 3, 1, 3, 0) | no re-creation (Android answers with `SUBOPTIMAL`, which is looked at, not obeyed); a screenshot in each: the red square top left, the green one top right | the same |
| Validation | no message but `WARNING-cache-file-error` (the layer's own cache file) and one performance warning, expected and logged as `[expected, performance]` (5 of the deviations); 0 errors | the same |
| `VK_ERROR_DEVICE_LOST`, a wait over two seconds | not met; tried with temporary hooks on the phone's driver (9 of the deviations) | not tried |

The GL ES image is unchanged: of the GL ES image's objects only `port_config.o` changed (the settings row), and a run with
`renderer = "gl"` shows `renderer: GL ES` and the menu drawn (a screenshot of more than 100,000 colours). Both images build with and without
`--android-vulkan-validation`.

**Found and fixed on the way**: a deadlock in the first run, the game thread waiting on the backend's lock that it held (`retire`
took it again inside a hand-over); found from `debuggerd -b` of the stuck process (the thread was in `host_vk_frame_command`).

**What the clears' self-test is.** The fog's alpha-only clear did not come in the first map's opening (0 clears drawn in
every statistics line), so the clears are tried by the backend itself, once, when the device is made with
`debug.vk_present_marker` on: an 8x8 colour and depth target of its own are cleared in full (0.25, 0.5, 0.75, 1.0, depth 0.5,
stencil 7), then alpha only (0.5) on the left half, then red only (1.0) with a depth clear on the top right quarter, and read
back with `vkCmdCopyImageToBuffer`; every texel is compared (within 1) with what the NV2A would give. It says `ok` on both
drivers, so the drawn path (the built-in pipeline, glslang loaded at the device's creation) writes only the channels named and
`vkCmdClearAttachments` clears the rest. The game's own alpha-only clear is not seen yet.

**What is left for the user**: a second GPU family (a Mali) is not tried; the game's own partial-channel clear (the fog) and a
split-screen clear are not yet met in a run; the picture is tried at 768x480 and 640x480 only.

### Phase 2: deviations from the spec

1. **The host is three files**, `host_vk.c` (the startup, as it was), `host_vk_render.c` (device, frames, targets,
   rendering, clears, commands, glslang's loader and the built-in pipeline) and `host_vk_present.c` (surface, swapchain,
   loss, the blit and the marker), as the spec allows. The device's functions are hidden globals (`host_vk.h`), so that the
   library exports no symbol named as a Vulkan function.
2. **The marker is drawn in a rendering on the swapchain image, not with `vkCmdClearColorImage`**: that command clears
   whole subresources, never regions, so each square is a `vkCmdClearAttachments` of one rectangle, in a rendering on a view
   of the swapchain image (made only with the marker on). The picture is blitted first, so the squares are over it.
3. **The formats' checks are not all demands**: B8G8R8A8_UNORM must be a colour attachment and a blit source (and a transfer
   source, from Vulkan 1.1); linear filtering, sampling and transfer destination add their usage bits, or fall back to
   nearest filtering, with a log line, instead of making the backend refuse. The depth formats need only the
   depth-stencil attachment feature. Both drivers have all of them.
4. **A target is cleared when made by a rendering with load op CLEAR** (not by `vkCmdClearColorImage` and
   `vkCmdClearDepthStencilImage`), so that it does not depend on the transfer destination feature of a depth format.
5. **The layer's `WARNING-Swapchain-PreTransform`** (performance) says the swapchain's `preTransform` (identity, as the
   spec decides) differs from the surface's current transform (rotate 90): the presentation engine does the turn, which is
   what the spec wants. The layer's callback logs that one id at info level as `[expected, performance]`; anything else is
   logged as the layer sent it, and errors are counted.
6. **A hand-over that is not the frame's end submits nothing**: the commands are recorded into the frame's command buffer,
   which stays open until `PRESENT` (the guest's stream flushes when it is full, not at a frame's end), where `host_dk.c`
   submitted. Everything a command names is read during the call all the same.
7. **Every barrier is a full one** (all commands to all commands, memory read and write), and one is made even to the
   layout an image is already in, because a rendering's writes are not visible to the next without one. Correctness first;
   a later phase can narrow them.
8. **`vkAcquireNextImageKHR` has a one-second timeout**, and a timeout is treated as the surface being lost (with the app
   away nothing frees an image); the spec's waits of two seconds are for fences. The window's `ANativeWindow` is also read
   every frame, and a different pointer is a loss without waiting for a result code.
9. **The two-second wait and the device-lost path were exercised with temporary hooks, not committed**: the wait's timeout was
   made 1 microsecond (`vk: waiting for the frame's fence (submission 4) has taken more than two seconds; waiting on`, then
   `... was ready after 0.0 s`) and a `VK_ERROR_DEVICE_LOST` was injected after 600 frames (`vk: VK_ERROR_DEVICE_LOST from
   TEMP vkQueueSubmit at submission 601, on Vulkan on the phone's driver ...; the backend stops (the game goes on without a
   picture)`); the game went on at the stand-in swap's pace (1651 frames in 10 s), with no crash. The real paths have not
   been met on the device.
10. **A clears' self-test** (not in the spec) runs with `debug.vk_present_marker`: see the summary.
11. **"Target changes" in the statistics** counts a pair of colour and depth targets that differs from the one last named
    by a command, not every rebinding (the guest names its targets again after each present).
12. **A new game's cinematic did not end** within 14 minutes in the runs of the map, with the picture black and the keys
    that might skip it (ESC, space, enter) doing nothing: the map was told from the log and the statistics (clears and
    images in the map), not from play.

### Phase 1 — audit, and the rebase onto the relocatable image

The audit of phase 1 found its results sound: `d3d8_vk.c`'s cut checks out
against `d3d8_gl.c` (sixty functions byte-for-byte, the others differing only
by GL; the exported symbols identical; no GL import), `host_vk.c` handles
`VK_INCOMPLETE`, dynamic rendering's dependencies by version, the driver check
before the device, and its failures; the stand-ins do what the spec says.
What changed after it:

- **`main` made the game image relocatable** (commit "Load the Android game
  image elsewhere when its address is taken": a table of the image's 32-bit
  pointers, `halo_guest.relocs`, and the image loaded elsewhere where the Java
  runtime holds `0x40000000`). The branch was rebased onto it, and phase 1's
  split joined with it: `host_memory_initialize` reserves the larger image's
  span there or wherever there is room, and `host_load_image_reserved` loads
  the chosen image into it with that image's own table. The link rule now
  writes one table per image (`halo_guest.relocs`, `halo_guest_vk.relocs`:
  9,128 pointers each, at different offsets) where it wrote one fixed file,
  which the Vulkan image's link would have overwritten.
- **One log line for the renderer**: a reason found before Vulkan is tried
  (no `halo_guest_vk.elf`, or not a guest image) went out as its own
  `renderer:` line, followed by a second, `renderer: GL ES`. It is now the
  reason in the one line.
- Notes for later phases, added to their sections: phase 2 checks that the
  queue family `host_vk_startup` chose can present; phase 6 replaces each GL
  stub's caller (the stubs write no output parameters).
- A finding of the audit that was wrong, for the record: it took
  `halo_ui_pointer_update` in `d3d8_vk.c` (no menu pointer) for a stub that
  changes what the game sees; `d3d8_gl.c`'s Android branch is the same
  function (the menu pointer is the desktop's).

**On the device after the rebase** (build `c2069599`, 45 s each from a cold
start): GL ES as before (`renderer: GL ES`, the image at `40000000`, the GL
renderer's frame lines); Vulkan on the phone's driver and on Turnip, each with
its one `renderer:` line, 1,651 frames in 10 s (165 Hz), `vk gl stubs:
self-test ok`, 0 of the 102 GL imports called, no error. The `.vk` install's
`config.toml` had to be made again from the defaults first: the test scripts of
part B's tip runs and of this audit pulled it with `adb shell cat`, which from
Windows' adb turns each line ending into `\r\n`, and every push and rewrite
added more, until the host's TOML reader gave up and used the defaults (GL ES).
Scripts must move the file with `adb pull` and `adb push`, or `adb exec-out`.

Not tried on the device: the image loaded away from `0x40000000` (nothing on
the test device holds that address; `main`'s commit tested the GL image's
move on a Nokia 8). The Vulkan image's table is made by the same tool, which
fails the build on any pointer it cannot move.

### Phase 1 — summary

Worked unattended on the test device (Lenovo TB321FU, Adreno 750, Android 16) as the `.vk` build, with
`--android-vulkan-validation`. **Phase 1 works on both drivers.**

**What works**

| | Phone's driver (Qualcomm 512.762.40) | Turnip v26.0.0 R8 |
|---|---|---|
| Log line | `renderer: Vulkan on the phone's driver (libvulkan.so), Adreno (TM) 750, Vulkan 1.3.128, Qualcomm Technologies Inc. Adreno Vulkan Driver Driver Build: 8924aaec70, ...` | `renderer: Vulkan on Turnip_v26.0.0_R8.zip, Mesa Turnip driver v26.0.0 - R8, Vulkan 1.4.335 (asked for vulkan.ad07xx.so through libadrenotools), Turnip Adreno (TM) 750, Vulkan 1.4.335, turnip Mesa driver Mesa 26.0.0-devel (git-5ac41be677)` |
| Menus, new game | keys move through the menus; a new game starts, the map loads (`WARNING: object_create - 'cryotube_1' already exists` at 15:09, 3.5 minutes after the start), the log goes on | the same (map loaded 2 minutes after the start) |
| Frames | `vk: 1651 frames swapped in 10.0 s ... (the display mode says 165.0 Hz)`, every 10 s, in the menus and in the map | the same |
| Backgrounding, ten times (HOME, `am start`) | the game goes on (frames dip while away: 803 in a 10 s window, 1226, then 1651 again) | the same, process still alive |
| Validation (`debug.vk_validation = true`) | `validation on` in the log line, no `vk: [` message | the same (the probe's two messages about the layer's cache file did not appear) |
| GL calls from the shared files | `vk gl calls: 0 of the 102 GL imports called` per 10 s | the same |

The screen is black under Vulkan (a screenshot is 19.8 KB, an empty picture; under GL ES the same keys give the
menu), and the audio track of the app exists (`dumpsys audio`), as does the game's `starting main menu music` line.
Under GL ES the same keys give the same debug.txt lines, including two that look like faults and are not ours:
`checksum failed on persistent storage` (the test profile) and, in the map, `too many lens flares submitted to frame` and
`biped marine_armored ... fell outside world and was erased`: the GL ES image logs the same lines at the same point
of the map, so they are the game's, not the renderer's (the lens-flare one is what a visibility test that reports
no samples would be expected to leave alone, and it is not caused by it).

**The failure cases** (all as the spec says)

- `vk_driver = "missing.zip"`: `vk driver: missing.zip failed: .../missing.zip: No such file or directory; using the phone's driver`, then
  `renderer: Vulkan on missing.zip failed: ...; using the phone's driver, Adreno (TM) 750, Vulkan 1.3.128, ...`: Vulkan on the phone's driver.
- `vk_driver = "fake.zip"` (a zip with `libgsl_alloc_hook.so` renamed `fake.so` and a `meta.json`): `renderer: GL ES (Vulkan was asked for:
  fake.zip, Not a driver, 1 (asked for fake.so through libadrenotools) has no physical device)`; the game is drawn by GL ES
  (the `frame N: ... draws` lines of the GL image are in the log; the process keeps the driver loaded, harmlessly).
- `renderer = "vlukan"`: `display.renderer "vlukan" is not "gl" or "vulkan"; using GL ES`, `renderer: GL ES`.
- `debug.vk_probe = "caps"` with `renderer = "vulkan"`: the probe runs (on the GL image, as before).
- No `halo_guest_vk.elf` in the APK: not made for the test (as the spec says); the code path is `host_main.c`, which logs
  `renderer: GL ES (Vulkan was asked for: the APK has no halo_guest_vk.elf: ...)` and the same for a file that is not a guest image.

**The build.** `ninja android_apk` with and without `--android-vulkan-validation` builds both images (checked: the APK
without the flag has both and no layer). Of the GL ES image's objects only `port_config.o` changed.

**The GL calls left in the Vulkan image.** None are reached in the menus or in the first map's opening (the shared
files' GL calls are all reached through `d3d8_gl.c`, which the Vulkan image does not have, so with no draw they
are not made). The count is real: each of the 102 `hostgl_` imports resolves to a stub of its own
(`host_gl.c`), a self-test at start-up calls one and sees it counted (`vk gl stubs: self-test ok`), and with
`debug.gpu_stats = true` the log says every 10 s which were called, how often and from which guest address
(`vk gl call: <name> <n> calls in <frames> frames, from <guest address>`; symbolize the address with
`llvm-symbolizer` on `build/android/halo_guest_vk.elf`). **Not tried:** a call from the guest through a stub (the
self-test calls it from the host, and no guest call happens until phase 6 adds draws), and no scene with a HUD, text drawn by
`text_hires.c` over a map or a menu texture upload was seen making one; phase 6 reads this count again at each
step.

**What is left for the user**: the (to confirm) decisions; a second GPU family; the second device that appeared on
adb partway through (`NB1GAD1791306511`) was not touched; every command was pinned to the Lenovo's serial.

### Phase 1: deviations from the spec

1. **`d3d8_vk.c` was cut from `d3d8_dk.c` (commit `25c02a0a`), not from `d3d8_gl.c` directly.** `d3d8_gl.c` has not changed
   since commit `151bbfd0`, before that file was cut; so `d3d8_dk.c` is a faithful cut of the same source, the spec
   names it the reference, and its deko3d command stream was removed. Checked: the exported symbols of `d3d8_vk.o` and
   `d3d8_gl.o` are the same set (`llvm-nm -g --defined-only`, diff empty; `d3d8_gl_map_loaded`, which is
   Switch-only, is not in either), and the link needs nothing else. What the cut keeps and answers is as the spec lists
   (`IsBusy` false, locks and waits return at once, visibility tests report 0, `Clear` and draws keep nothing and
   return, `Present` swaps the stand-in window and keeps the flip logic). `debug.gpu_stats`' own line is not kept in the
   Vulkan device (phase 2's diagnostics).
2. **`host_vk_startup(vk_driver, validation, line, size)`**: it takes `debug.vk_validation` as an argument (the host
   reads `config.toml` in `host_main.c`, which now also reads `display.renderer` and `debug.gpu_stats`) and returns
   the text of the log line, which `host_main.c` prints, rather than printing it. `host_vk.h` holds the state kept for
   phase 2.
3. **`config.toml` is read before the images are loaded**, since the renderer decides which are. Both images are read
   with `SDL_LoadFile` (17 MB each), the larger span is reserved, and the one not used is freed after the choice. The
   GL ES image therefore runs with a reservation as large as the larger image's (the Vulkan image is 0x13000 bytes
   shorter, so for a GL ES run nothing changes; the log's "guest image 40000000-41909000" is the span used).
   `host_load_image` keeps its old form beside the new `host_load_image_reserved` and `host_image_span`.
4. **The GL stubs return zero and do not call the driver.** The spec says Android's GL library answers a call made
   without a context with a no-op; the stubs make that answer themselves, so that the GL library is not entered
   without a context (it logs a warning per thread) and so that each call can be counted and its caller recorded. The
   host-side `host_gl_*` imports (extensions, buffer write, fences) are no-ops under Vulkan for the same reason.
5. **The pacing uses the display mode's rate each frame** (`SDL_GetCurrentDisplayMode`, as the spec says): on this
   device it says 165 Hz, and 1651 frames in 10 s came out (earlier in one run, 60 Hz for a few seconds right after the
   start, and 596 frames: the mode changes with the panel's refresh rate switching; the log line says the rate it saw).
6. **Menu keys in this build**: New Game is `ENTER`, `DOWN`, `ENTER` (Campaign, then New Game), then `ENTER` for the
   level (the test profile shows a Load Level screen) and `ENTER` for the difficulty; the spec's
   `DOWN, ENTER, ENTER` goes to the multiplayer menu here (`searching for a network game`). The opening cinematics did not
   take ten minutes: the map's objects were created after two to three and a half minutes, and the screen being black, the
   map was told from the log.
7. **Not done**: the spec's "frame count every 10 s is the display's rate" was checked against the display mode's
   rate, not against an independent measure of the panel's; no GL ES run was compared frame for frame.

### Phase 0, part B — audit and fixes

The audit of part B found its results sound and five things to fix, now fixed (commit "Fix
what the audit of phase 0 B found"):

- `host_vk_driver.h` promised that a driver that cannot be used falls back to the phone's;
  that holds only until libadrenotools accepts the library. The header now says what
  cannot be undone after `vkCreateInstance`, and phase 1 above makes the host check the
  driver (`host_vk_driver_verify`, a physical device) before it chooses the game image.
- `host_vk_driver_close()` let the driver be opened again, with libadrenotools' hooks still
  installed; it is now final for the process.
- The probe set the blend enable to true whenever it was dynamic, so on Turnip (extended
  dynamic state 3) the `full_dynamic` draws, meant opaque, were blended. It now follows the
  pipeline. The hashes did not change: the shaders' output alpha is 1 on these inputs.
- The probe's build name was taken at configure time, so every report of part B said
  `fc0f335a+changes`, and the Qualcomm reports predate `driver.check`. It is now written
  at every build (`tools/android_build_stamp.py`).
- The libadrenotools patch was applied only when first fetched; it is applied again when it
  changes, and the build rule touches its outputs so it does not rerun at every build.

**The tip, run on the device** (build `35f29e2b`, `all`, validation on, `present` ended
by a key after 15 s): on the phone's driver and on Turnip, every step finishes, the layer
reports 0 errors, `driver.check` is ok, and every `draw` hash is the same on both drivers.
These two runs replace `..._qualcomm_all_validation.txt` and `..._turnip_all_validation.txt`;
the cold and warm reports are part B's as it ran them.

### Phase 0, part B — summary

Worked unattended on the test device (Lenovo TB321FU, Adreno 750, Android 16) as the
`.vk` build. Reports are in `port/android/probe/reports/` (`..._qualcomm_all_{validation,cold,warm}.txt`,
`..._turnip_all_{validation,cold,warm}.txt`).

**Turnip runs in this app, and the draw that crashed the earlier attempt does not crash.** Turnip
(Mesa 26.0.0, loaded through libadrenotools) did setup, `caps`, `memory`, `compile`, `pipelines`, `draw`
and `present` (ten backgroundings, rotation, three clearing modes), with the validation layer on
(it loads with the custom driver: 0 errors, the only 2 messages are about the layer's own cache file). The
`vkUpdateDescriptorSets` crash of the earlier attempt was not reproduced in any form (written once, written
again after a draw, pushed): it was that attempt's own bug, not a conflict with this app.

**The pictures of `draw` are the same on both drivers, bit for bit**: every variant has the same 64-bit hash
on the phone's driver and on Turnip (the control pair, the typical and large game shaders, each opaque and
blended, static and dynamic state, a bound set and pushed descriptors). Not one pixel differs, so the shaders
as converted say nothing about a driver fault on these inputs (they are small inputs: one triangle).

**What each driver did**

| | Phone's driver (Qualcomm 512.762.40) | Turnip v26.0.0 R8 (Mesa 26.0.0-devel, git 5ac41be677) |
|---|---|---|
| Archive | – | `Turnip_v26.0.0_R8.zip`, sha256 `e634db0f929e2205e95511c769071817d0390180ec72c8e690bc76375e813715`, "Mesa Turnip driver v26.0.0 - R8" by KIMCHI (K11MCH1/AdrenoToolsDrivers release `v26.0.0-rc08`), library `vulkan.ad07xx.so`, reports Vulkan 1.4.335 |
| Versions | instance 1.4.0, device 1.3.128, driver id 8, conformance 1.3.6.0 | device 1.4.335, driver id 18 (`VK_DRIVER_ID_MESA_TURNIP`), conformance 1.4.0.0 |
| Setup, `caps`, `compile`, `pipelines`, `draw`, `present` | all finish | all finish |
| Validation layer | clean (2 messages: the layer's cache file) | loads with the custom driver, clean (the same 2 messages) |
| Dynamic rendering, extended dynamic state 1 and 2, vertex input dynamic state, push descriptors, custom border colour, 4444, pipeline cache control, maintenance4 | yes | yes |
| Extended dynamic state 3 | no | **yes (all of it)** |
| Maintenance5, graphics pipeline library (+ pipeline library) | no, no | **yes, yes** |
| `VK_EXT_external_memory_host` | no | no (the plan's copy-at-hand-over is right for both) |
| Memory | heap 0 11,273 MB + a 4,095 MB protected heap; plain device-local types exist | **one 8,454 MB heap, every type host-visible** (no device-only type a buffer can use; the probe's device-local-buffer variant says "no memory type" and is left out) |
| Formats the plan needs (BC1/2/3, the 16-bit colours, depth formats) | all present | all present; Turnip also reports colour-attachment and blend bits on its depth formats (the driver's claim, unverified) |
| Limits that differ | vertex attribute offset 4096, uniform alignment 256, non-coherent atom 1, copy offset alignment 64 | **vertex attribute offset 4095**, uniform alignment 64, non-coherent atom 64, copy offset/row alignment 128 |
| Presenting | FIFO, 6 images, 165 Hz, 6.04 ms median interval | the same, 6.04 ms |
| Surface lost on backgrounding | 10 of 10 remade, 4 to 8 ms | 10 of 10 remade, 3.5 to 8.6 ms |
| Rotation | `SUBOPTIMAL` each time, remade in 5 to 17 ms | the same, 5 to 11 ms |
| Pipeline creation, cold (pass / typical / large), min | 1.08 / 5.19 / 12.0 ms | **0.31 / 3.24 / 12.3 ms** |
| From our saved cache | 0.097 / 0.091 / 0.120 ms (cache 94 KB) | 0.006 / 0.004 / 0.004 ms (cache 260 KB) |
| Driver's own cache only (ours empty) | 1.8 / 7.9 / 17.9 ms | 0.38 / 3.3 / 12.4 ms |
| Static state against dynamic (typical, large) | 7.2 / 16.9 against 5.2 / 12.0 ms (dynamic cheaper) | 3.25 / 12.36 against 3.24 / 12.35 ms (no difference) |
| Pipelines made on a second thread while a frame loop runs | no stall (0 of 12 frames over twice the median) | no stall (0 of 10) |
| `draw`: pixels changed by the typical / large shader | 63,520 of 65,536 (the triangle's corner is off the target by construction) | the same 63,520 |
| `draw`: rewrite of the set after a draw (the triangle halved, then restored) | picture changed (37,456 pixels), restored picture equals the first | the same |

Differences that matter for the plan: Turnip has extended dynamic state 3 and the pipeline library, which would
let phase 5 make fewer pipelines on Turnip (the plan's decision is the same code on both, so unused for
now); it creates pipelines faster; and its attribute offset limit is 4095, one below the phone's driver's 4096; a vertex
layout with an attribute at offset 4096 would not run on Turnip (not checked against the game's layouts; the Xbox's
stride limit makes it unlikely).

**Deviations from the spec**: the next section. **Left for the user**: the (to confirm) decisions; a second GPU
family (a Mali) is not tried; the tester's part of `present` (the picture's orientation, the tone) was answered
by key events only (the screenshot of the Turnip run shows the cycling colour and the corner marker as on the
phone's driver); a Turnip release is the user's choice (this one was the newest plain release).

### Phase 0, part B: deviations from the spec

1. **libadrenotools**: Eden's fork at `8ba23b42d742545b709064d6e2523cdb86de68f5`, its submodule `lib/linkernsbypass`
   at `aa3975893d83ef1bc84c321ec60c65fbf1287887`; fetched with `git clone` + `checkout` + `submodule update`
   + `git apply port/android/probe/adrenotools.patch` (one line: `log` in the library's link line). Five
   libraries are staged, not three hooks: `libadrenotools.so` and `libhook_impl.so`, `libmain_hook.so`,
   `libfile_redirect_hook.so`, `libgsl_alloc_hook.so` (the first hook needs `libhook_impl.so` beside it). It is
   `dlopen`ed by full path from the native library folder (not linked); `-lz` is linked into `libmain.so` for the zip reader
   (the NDK's zlib, a system library).
2. **The window is made without `SDL_WINDOW_VULKAN`** straight away, as the spec says to do if the flag loads
   the system loader; I did not measure whether it does. `/system/lib64/libvulkan.so` is in the process anyway
   (in both runs, from something other than the probe's own code), but with Turnip the phone's
   `vulkan.adreno.so` is **not** loaded (`driver.loaded_library` lists the process's libraries) and the loader
   copy that libadrenotools makes (`memfd:/system/lib64/libvulkan.so`) serves.
3. **The driver module also verifies** (`host_vk_driver_verify`, not in the spec), and refuses a library that is
   not an ELF file: libadrenotools returns the system loader for any input and its hook falls back to the phone's
   own driver without a word when the library will not load, so without this a junk archive was reported as
   opened while the Adreno driver ran. The probe reports `driver.check` (ok / FAILED). Tried: a text file named
   `.so` (refused by the ELF check, falls back), a truncated ELF (hook falls back: `driver.check: FAILED`, the
   reason is in logcat tag `hook_impl`), an ELF that is not a driver (the loader is left with no driver:
   `probe.setup: failed`; the **module cannot recover from that one**, since the loader is already bound to the
   hook, so phase 1's choice must treat "no Vulkan device after opening a custom driver" as a reason to start
   the GL ES image).
4. **Zip reader**: stored and deflated entries, CRC checked, sizes bounded (256 MB a file or archive, 256 entries),
   encrypted, zip64 and multi-part archives refused, a name containing `..` anywhere or starting with `/` is
   refused (stricter than the spec's wording). The `.unpacked` stamp is the archive's size and modification
   time. Files of an older unpacking of the same archive name are not removed.
5. **`draw`**: the typical pixel shader's first sampler and the large one's last are **cube maps** (`samplerCube`),
   so two of the four textures per pair are not 2D: a cube of six 64x64 layers (RGBA8, and BC1 for the large pair's)
   instead of four 2D textures. BC1 is used where the device samples it (`textureCompressionBC` is enabled on the
   device now). A pass-through pair is drawn first as a control; each pair is drawn opaque and blended, static
   ("plain": only viewport and scissor dynamic, as the Decisions say) and with every dynamic state the device
   has. The constants are set by hand from reading the shaders (identity at `c[28..31]` / `c[0..3]`, the
   skinning matrices at `c[60..62]`, `c[7].w = 1`, `c[58]`, `c[59]`, `viewport_*` for pixel coordinates on a
   256x256 target, every other constant 0.5), the vertices are in pixel coordinates. The pictures are written as
   `vk_probe_draw_*.ppm` in the data folder and were looked at (shaded and textured, as expected).
   The rewrite test writes a second vertex block (halving the triangle) and writes the first back, rather than
   changing a texture. `VK_KHR_push_descriptor` is now enabled on the device when offered.
6. **Sub-step guards added**: `draw.setup`, `draw.update.<pair>`, `draw.<variant>`, `draw.rewrite.<pair>`,
   `draw.push`, `memory.same_pages` (part A's fix) and `driver_close`. No guard fired: nothing crashed.
7. **Part A's fixes**: done as listed. `guard_end` returns the marker to the running step; a surface lost while the
   app is away is logged and counted once, with the attempts and the time it took (the loop retries without a line each).
   The memory step's fixes are in the code but **not exercised**: neither driver offers `VK_EXT_external_memory_host`.
8. **Settings**: `display.vk_driver` is a row in `port_config.c` (not under `HALO_SWITCH`); the host reads it from
   `config.toml`; `host_vk_probe_run` has a third argument for it. After the game's own start the file still
   showed my row without the comment the other rows have, because the game only adds the keys that are missing;
   I did not get the game to rewrite the file (quitting by adb key events did not confirm the dialog).
9. **Not done / different**: `TU_DEBUG` was not needed (the layer loads with Turnip). No second thread of
   frames used game pipelines (as in part A). The reports' names carry `qualcomm` / `turnip` as asked; part A's
   reports keep their names. Each release other than R8 was not tried, since R8 worked on the first try.
10. **The GL ES game** ran with the legacy-packaged build both with and without the validation layer in the
    APK (main menu reached, the quit dialog shown, frames drawn; the user's own release build was not touched).
    The build without `--android-vulkan-validation` has no `libVkLayer_khronos_validation.so`.

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
   log prints no git hash for it to copy. *(Since part B's audit it is written at every build,
   into `build/android/host/probe_build.h`, by `tools/android_build_stamp.py`: at configure
   time it went stale whenever a commit was made without configuring again.)*
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


# Vulkan renderer — plan

A second renderer for the Android build that drives the GPU through Vulkan
instead of OpenGL ES, with its shaders and pipelines cached on the device.
It follows the plan of the Switch's deko3d renderer (`port/switch/DEKO3D.md`)
step for step, which went from nothing to the game playing at full speed in
phases each tried on the console before the next. Read that plan, and its
"Progress", before this one: most of what is decided here was decided there
first, and most of what will go wrong here went wrong there first.

Status: phase 0 is done on the test device (Adreno 750) except step 6's
fight scene and its profiling-cost comparison, which the user stopped, and
the second GPU family (no Mali yet): the phase is not called done until the
user decides on both. Its results and proposals are in "Progress" and in
the rows of "Decisions" marked **(to confirm)**. The work is on the
`vulkan-backend` branch, which starts again from `main`. An earlier attempt, kept on the
`vulkan-backend-old` branch, is not the base of this work and nothing here
builds on it; see "Lessons from the earlier attempt".

---

## Why

The Android build renders through OpenGL ES, called from the guest through a
generated stub for every GL entry point (`port/android/host/host_gl.c`,
`guest_gl.c`). Two costs come from that, the same two the Switch had:

- **CPU cost per draw.** Every GL call crosses the guest→host boundary and then
  goes through a mobile GL driver's validation. `d3d8_gl.c` was written to cut
  thousands of calls a frame, and they are still thousands.
- **Shader hitches.** Mobile GL drivers compile at link time, and their
  program binaries are not reliable across driver updates;
  `shader_programs.bin` hides the cost behind map loads.

Phase 0 measures both on a device before anything else is built: if the GL
path's frame time is not dominated by them, the case for this renderer is
weaker and the plan says so.

Vulkan records draws into command buffers for a few words each, takes SPIR-V
shaders as data, and keeps compiled pipelines in a cache the application
saves.

---

## Decisions

Taken from the deko3d plan unless the row says otherwise. Rows marked
**(to confirm)** are proposals for the user.

| Question | Decision |
|---|---|
| Shared code with the GL ES renderer | **Copied**, not extracted, as for deko3d: `d3d8_gl.c`, `xbox_textures.c`, `nv2a_vsh.c`, `nv2a_psh.c` stay untouched so the other platforms keep working; Vulkan versions are copies under `port/android/guest/`. Files under `port/linux/src` change only by read-only accessors behind `#ifdef HALO_ANDROID_VK` (as the deko3d work added behind `HALO_SWITCH`). |
| How the renderer is chosen | **Two guest images**, as on the Switch: `halo_guest.elf` (GL ES) and `halo_guest_vk.elf` (Vulkan), both in the APK. The host reads `display.renderer` before it loads the guest. Nothing in the guest chooses at run time. |
| The GL ES renderer | **Kept, selectable for good**, and the automatic fallback: on a device without what phase 0 finds Vulkan needs, the host loads the GL ES image and says why in the log. **(to confirm)** which is the default once phase 7 is done. |
| A shader or pipeline nobody has compiled yet, met during play | **The draw is skipped** while it compiles on another thread, as for deko3d. |
| Shader compiler | glslang, linked into the Android host, GLSL to SPIR-V on the device. **(to confirm)** after phase 0 measures it; the alternative is shipping SPIR-V for the known keys in the APK. |
| What players share | **Keys**, not compiled shaders or pipelines, as for deko3d: the pixel and vertex shader keys come from the same `nv2a_*` key structures, so **(to confirm)** one key file format serves both renderers. |
| Compiling the known keys a device has not cached | **In the background, on one thread, while the game runs**, unless phase 5's step 1 measures otherwise on Android (deko3d measured one thread faster than several on the Switch; a phone has more cores). |
| Minimum Vulkan | **Decided by phase 0**: the version and extensions the design needs (below), and nothing more. A device without them gets GL ES. **(to confirm)** Proposed from one device: Vulkan 1.1 with `VK_KHR_swapchain` is the floor. Dynamic rendering, extended dynamic state 1 and 2 and vertex input dynamic state are used where the device has them (the Adreno 750 has all four; none has extended dynamic state 3), and the renderer keeps a render-pass path and static state for a device without, at the price of a larger pipeline key. Settle it with a Mali's report and an older Adreno's. |
| Guest memory (phase 3) | **(to confirm)** Proposed: **an upload ring**. The test device has no `VK_EXT_external_memory_host`, so nothing can be imported; the ring costs 0.09 ms of CPU a frame for 4 MB, and the GPU reads vertices from host-visible memory as fast as from device-local memory. Reading in place is possible by another way, which the probe found (an allocation exported as a file descriptor and mapped with `mmap` at an address below 4 GB: the GPU read the guest's writes and rewrites), and is for later, when the window itself would be made from such a mapping. |
| Dynamic state and the pipeline key (phase 5) | **(to confirm)** Proposed: everything the device can make dynamic is dynamic (22 states on the Adreno 750: viewport and scissor with count, depth bias and its enable, blend constants, stencil masks and reference, cull mode, front face, topology, depth test, write and compare, depth bounds enable, stencil test and op, rasterizer discard, primitive restart, logic op, vertex input). What stays in the key: the two shaders' keys, the colour and depth formats, and the colour blend state (enable, factors, write mask), which only extended dynamic state 3 makes dynamic and this device lacks. Making the state dynamic did not cost creation time (cold: 5.2 and 12.0 ms; with nothing dynamic: 7.2 and 16.9 ms). |
| A pipeline met during play (phase 5) | **(to confirm)** Proposed: compile-on-skip is small enough on this device. Twenty pipelines made on a second thread (9.5 ms each) did not disturb a thread drawing every 16 ms (0 of 12 frames during, over twice the median; submit to fence 1.5 ms before and during). A pipeline from a saved `VkPipelineCache` costs 0.1 ms. The driver's own cache does not do that (7.9 and 18.5 ms with ours empty). |
| Shader compiler (phase 4) | **(to confirm)** Proposed: glslang on the device. 2 to 4 ms for a game-sized shader (1.5 to 3.9 ms with the CPU at its best), 100 ms for the very first compile of a process, 3.8 MB of library in the APK, 17 to 35 KB of SPIR-V a shader. Shipping SPIR-V for known keys is not needed for the cost, and can still be added as a cache. |

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
  stub call each time the guest hands the stream over.

The host reads guest memory directly (the guest runs in the low 4 GB of the
host process: `port/android/README.md`). **No command names a host address
in a 32-bit field** - the deko3d work faulted once on exactly that.

### The one rule about guest memory

**The host reads what a command names when it processes the command, at the
hand-over, never later.** The game rewrites vertex buffers, index buffers and
constants within a frame, and frees and reuses memory between draws; a
backend that names guest memory and reads it at the end of the frame draws
every draw with the frame's last bytes. Data the GPU reads later than the
hand-over is either copied at the hand-over (an upload ring) or read in place
under per-resource busy tracking (phase 3), and a resource the game locks
while the GPU still needs it makes the game wait - which deko3d does through
the Xbox's own `Lock` field.

### Both renderers in one Android build

Two guest images, and the host runs one. `halo_guest.elf` is the game with
the GL ES renderer, built as now; `halo_guest_vk.elf` is the same objects
with `port/android/guest/d3d8_vk.c` in place of `d3d8_gl.c` (and the other
copies in place of theirs), linked by `tools/android_build.py` as
`tools/switch_build.py` links `halo_guest_dk.elf`.

Under Vulkan the host makes no GL context: Android lets one API own a window,
and a window that backs a GL ES context refuses a Vulkan surface
(`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`). SDL keeps its window for input and
lifecycle; the guest's GL context is a stand-in (as `host_sdl2.c` makes on the
Switch), and the Vulkan surface is made on the window's `ANativeWindow`.

Config: `display.renderer = "gl"` or `"vulkan"`, under `[display]`.

---

## Files

New, Android only (copies of the GL renderer's files, adapted):

| File | From | What changes |
|---|---|---|
| `port/android/guest/d3d8_vk.c` | `d3d8_gl.c` | entry points and state kept; GL calls replaced by commands sent to the host |
| `port/android/guest/xbox_textures_vk.c` | `xbox_textures.c` | format decoding kept; images and uploads go to the host |
| `port/android/guest/nv2a_vsh_vk.c`, `nv2a_psh_vk.c` | `nv2a_vsh.c`, `nv2a_psh.c` | GLSL for Vulkan (phase 4) |
| `port/android/guest/vk_commands.h` | — | the command stream, guest and host both include it |
| `port/android/guest/vk_shaders.c` | — | keys, key files, the startup pass (as `dk_shaders.c`) |
| `port/android/host/host_vk.c` | — | the Vulkan backend |
| `port/android/host/host_vk_shaders.c` | — | glslang, the SPIR-V and pipeline caches, the compile thread |

Unchanged and shared: `d3d8_resources.c`, the rest of the platform layer.
`port/android/host_imports.list` gains the host functions the guest calls.

---

## Phase 0 — spike

A probe in the Android host, behind a setting, run on the test device and on
at least one device with another GPU family (Adreno and Mali at least). It
answers the questions the rest of the plan depends on, and its numbers go
into "Progress" before phase 1 starts. It stays in the tree after phase 0:
it is how a tester's device is reported later (phase 7).

### What this phase is, and is not

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

### Files

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

### Settings

Under `[debug]`, read by the host from `config.toml`, each also a row in
`port_config.c`'s table so that it survives the guest's rewrite:

| Setting | Default | What |
|---|---|---|
| `debug.vk_probe` | `""` | `""`: the game runs as usual. `"all"`: every step. Otherwise a comma list of steps, `caps,memory,compile,pipelines,present`, run in that order whatever order they are written in. A step that crashes the probe is left out of the list on the next run; that is the skip mechanism, as `deko3d_probe_skip.txt` was on the Switch. |
| `debug.vk_validation` | `false` | Enable `VK_LAYER_KHRONOS_validation` when the APK carries it. The probe logs whether the layer was found and enabled, and every message it sends. |
| `debug.profile_hz` | `0` | Step 6's profiling mode: sample every guest thread this many times a second into memory, and write the profile to the data folder. `0` off. |

### The probe's shape

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

### The build

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

### Step 1: what the device has (`caps`)

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

### Step 2: a frame on the screen (`present`)

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

### Step 3: the game's memory, seen by the GPU (`memory`)

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

### Step 4: the shader compiler (`compile`)

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

### Step 5: pipelines (`pipelines`)

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

### Step 6: the GL path's costs (`debug.profile_hz`, the game running)

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

### Testing on the device

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

### Acceptance

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
Vulkan image is missing or the device lacks what phase 0 requires. No GL
context is made under Vulkan.

**Acceptance:** with `renderer = "vulkan"` the game runs to the menus and
plays (sound, input) with a black screen; with `"gl"` it is as before; a
device without Vulkan falls back with a line in the log.

## Phase 2 — the backend's skeleton

The command stream (`vk_commands.h`): targets, clear, present, each a fixed-
width record; one `host_vk_submit` per hand-over. The host: instance, device,
one graphics queue, the swapchain (FIFO), frames in flight behind fences, the
game's render targets by address (made the first time they are drawn into),
clears, and present as a letterboxed blit of the back buffer into the
swapchain image. Every submission is numbered and fenced; the guest can ask
which number the GPU has finished (`host_vk_retired`). The surface's loss
and return (backgrounding) and `VK_ERROR_OUT_OF_DATE_KHR` make the swapchain
again.

**Acceptance:** the menus' clears show (colour changes are visible), frames are
paced by the display, backgrounding and returning works ten times in a row,
and a validation layer run (debug APK, `VK_LAYER_KHRONOS_validation`) is clean.

## Phase 3 — reading the game's memory

Decided by phase 0's step 3:

- **In place:** the guest's memory window imported as device memory in chunks
  (deko3d's `window_chunks`), so a draw's vertices, indices and compressed
  textures are read where the game keeps them; the CPU's writes are visible
  through host-coherent memory or flushed per range.
- **Or copied:** a per-frame upload ring, written at the hand-over.

Either way, **per-resource busy tracking**: a draw marks each resource it reads
with the submission it is in (the Xbox's `D3DResource.Lock`, as `d3d8_dk.c`'s
`resource_used`); `IsBusy`, `BlockUntilNotBusy` and the locks wait for the
GPU to pass it (`halo_resource_busy`/`halo_resource_wait`). A `Lock` higher
than the submission being written was not written by this device - map files
carry headers with stale bytes there - and counts as not busy (deko3d's first
level load waited forever on one). A wait longer than two seconds says so in
the log.

**Acceptance:** a test draw reads vertices from the game's memory correctly
after the game rewrote them mid-frame (two draws from one rewritten buffer
show two different pictures); a lock of a buffer the GPU still reads waits,
and returns.

## Phase 4 — GLSL for Vulkan

Copies of the generators that write GLSL 450 for glslang: one `std140`
uniform block at set 0 binding 0 **with every member's offset written
explicitly** and checked against the C structure the host fills (a build-time
or start-up check that compares every offset); varyings with `location`s;
each stage's sampler at its own binding. The keys are the GL generators'
keys.

**Proving it: the game's real shaders.** Off the device, as deko3d's phase 4
did with UAM: take every program a GL build has recorded
(`shader_programs.bin` from a session through the campaign's maps; only the
Switch's GL build records it today, behind `HALO_SWITCH`, so phase 4 starts
by recording it on Android or on Linux), generate
both the GL and the Vulkan GLSL from the same keys, and:

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
on the device), and SPIR-V plus state to a pipeline (the driver, slow). A
pipeline's key is the shaders' keys and whatever state is not dynamic - which
phase 0 decided, and which is kept as small as the device allows.

### Step 1: what glslang and the driver cost, measured on the device
The numbers phase 0 took, at scale: a few hundred of the recorded keys, one
thread against several, with and without a saved `VkPipelineCache`.

### Step 2: glslang in the host
Built for Android, linked into the host, front end initialised once.

### Step 3: the shader service in the host
SPIR-V cached on the device by key (`<data root>/shader_cache/<version>/`),
the `VkPipelineCache` saved at a quiet moment and on exit, one compile thread
fed from a queue with the keys a draw asked for at the front; a draw whose
pipeline is not ready is skipped. Broken cache files are deleted and remade.

### Step 4: keys, key files and the startup pass in the guest
As deko3d's: every key met is recorded to a key file in the save folder; at
start the known keys not in the cache are queued for the background thread,
with no waiting screen.

**Testing on the device:** a cold start, a second start (nothing compiled), a
map loaded twice; the log line per 60 frames with draws, and draws skipped
for a pipeline not ready.

**Acceptance:** the second start compiles nothing; no hitch longer than a
frame on a warm cache; the numbers in "Progress".

## Phase 6 — draws, textures and render targets

The largest phase: the equivalent of most of `d3d8_gl.c` on Vulkan. Read
`d3d8_gl.c` whole first, and `d3d8_dk.c` beside it: the deko3d version is
the same work against a command-buffer API, and its "Progress" entries are a
list of the traps.

### The order, each step tried on the device before the next

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
   (deko3d's step 6: the atlas sends only its changed rows).

### Pitfalls already known (from deko3d's phase 6)

- The device's state at a draw is the guest's to describe and the host's to
  apply; a target change resets viewport and scissor on the host.
- A pipeline's state must match the render pass or rendering attachments it
  is used in, or some drivers drop the draws silently.
- Images change layout; track each image's layout and transition from it.
- Descriptors bound by a recorded command must not be rewritten before the GPU
  is done; take a fresh set or use push descriptors.
- Barriers are recorded outside render passes.
- A texture written in place waits for the draws that read it; a render target
  sampled after it is drawn needs a barrier between.
- Constants a vertex format does not feed are reset when the format changes.

### Diagnostics, from the first step

- A log line every 60 frames: draws, immediate-mode draws, and draws skipped,
  by reason (no pipeline, no target, not ready, too big).
- Every wait on the GPU logs once it has taken two seconds, with what it waits
  for; the queue's loss (`VK_ERROR_DEVICE_LOST`) is logged with the
  submission number.
- The validation layer in the debug APK, run at each step's end.
- A dump of generated GLSL behind a setting, into the app's own files.

**Acceptance:** the game looks under Vulkan as it does under GL ES, the menus
and a map, with the diagnostics clean.

## Phase 7 — parity and performance

- Every map's scenes compared against the GL ES renderer on the same device;
  especially split screen, water, lens flares, decals (z bias), fog, the
  HUD's meters and the PC menus.
- CPU frame time against GL ES; no hitches with a warm cache.
- At least three devices: the test device, another Adreno generation, a Mali.

## Phase 8 — collecting keys

- Builds record keys into the save folder; testers send the files in.
- A PC tool merges them and reports what each submission added (shared with
  deko3d's if the key format is shared).
- Releases ship the merged key file; each device compiles it once in the
  background.

---

## How this plan is worked

What made the deko3d plan go well, kept here on purpose:

- **Each phase is written out in detail before it is handed over**, with its
  files, its steps, how to test it on the device and what counts as done.
- **Each step is tried on the device before the next starts.** A step that is
  "done in the tree" and not seen on the device is not done.
- **Every result goes into "Progress"** with the numbers that show it: what was
  seen, what the log said, what was measured. "Progress" is newest first.
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
- Device operations: never `adb uninstall` (it deletes the game's data and
  saves); install with `adb install -r` and an absolute path, and check
  `lastUpdateTime`; the settings file is rewritten on every run, so a setting
  must be in `port_config.c`'s table; do not reboot the device without asking.

---

## Risks

| Risk | Settled in |
|---|---|
| The device cannot import host memory, so every draw's data is copied | Phase 0 (step 3); phase 3 is designed for both |
| Pipeline creation is slow enough to hitch even when skipped draws hide it | Phase 0 (step 5), phase 5 (step 1) |
| Too little dynamic state on some devices, so the pipeline key grows | Phase 0 (step 1) |
| Drivers differ: a device that drops draws, misreports, or lacks a format | Phase 0 on several devices, the validation layer, phase 7 |
| The surface is lost when the app is backgrounded | Phase 2 |
| Devices with minSdk 28 but no usable Vulkan | Phase 1's automatic fallback to GL ES |
| The GL path's costs are not what this renderer removes | Phase 0 (step 6) |

---

## Progress

Phase 0 was worked on the test device (Lenovo TB321FU, Adreno 750, Android 16,
API 36, the phone's own driver). The reports are in `port/android/probe/reports/`.
Newest first.

### Phase 0 — summary

**Step 6 is partial, and no second GPU family was tried.** The probe's steps
1 to 5 are done and tried on the device; step 6 has the menu and the
standing-still map scene profiled, and not the fight scene, nor the
profiled/unprofiled comparison on the map (the user stopped profiling).
Everything marked (to confirm) in "Decisions" is a proposal for the user.
Still to do for the phase: a Mali (ask the user: none has been tried), the
fight scene if the user wants it, and the user's decisions.

**Deviations from this spec, each for the user to accept:**

- *Installing the build.* The device had build 50 from GitHub Actions installed,
  signed with the release key, which a debug-signed build cannot replace
  (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`), and without a debuggable app the
  validation layer is never found. With the user's agreement the debug build is
  installed **beside it as `com.halo.decomp.vk`**: `HALO_APPLICATION_ID_SUFFIX=.vk`
  in `port/android/app/build.gradle` (the manifest's provider authority became
  `${applicationId}.update`, and `UpdateProvider.AUTHORITY` follows it), with the
  game's `maps/` copied into its folder by adb. Without the variable nothing
  changes. The data folder is `/sdcard/Android/data/com.halo.decomp.vk/files`,
  and `config.toml` there is owned by the app's UID.
- *`present` and SDL's lifecycle events.* The step as specified waits for
  `SDL_EVENT_WILL_ENTER_BACKGROUND` and the like. None of them arrived in the
  probe's event loop (only window shown, pixel size, gamepad added, display
  orientation). The surface's loss is found by `VK_ERROR_SURFACE_LOST_KHR` from
  acquire or present, and `SDL_Vulkan_CreateSurface` fails ("Android native window
  is not available ... usually because of backgrounding") until the app is back,
  so the loop retries. Phase 2 should not wait for SDL's events.
- *`memory`.* The device has no `VK_EXT_external_memory_host`, so the import cases
  could not run (the code for them is written and **not exercised on any device**;
  the same for the negative tests). Added beyond the spec: an export case.
- *The profiler's handler.* The first version called `host_low_owns`, which takes
  the memory lock, and the game hung within a second at 1000 Hz (a thread
  interrupted while it held the lock). The handler now reads the frame chain with
  `process_vm_readv` (a system call that says no for an address that is not mapped),
  and flags a sample taken right after a system call (the instruction before the pc
  is `svc #0`) as blocked. Phase 2's host code must not share locks with a signal
  handler either.

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


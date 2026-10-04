# deko3d renderer — plan

A second renderer for the Switch build that drives the GPU through
[deko3d](https://github.com/devkitPro/deko3d) instead of Mesa, with compiled
shaders cached on the SD card.

Status: phases 0 to 3 done, phase 4 next; see "Progress" at the end.

---

## Why

The Switch renders through `switch-mesa` (Mesa 20.1, nouveau) under SDL2's EGL
surface. Two costs come from that, both measured:

- **Shader hitches.** Linking a program costs Mesa 20 to 56 ms (the nouveau
  compiler runs at link time), and Mesa on the Switch has no shader cache, so
  every run pays it again. `shader_programs.bin` hides this behind map loads
  by relinking each map's programs there (`d3d8_gl.c`, program records).
- **CPU cost per draw.** Every GL call is expensive on this driver and also
  crosses the guest→host stub; the binding work in `d3d8_gl.c` was written to
  cut 3,000-12,000 calls a frame.

deko3d loads compiled shaders (DKSH files) as plain data, binds each stage on
its own with no link step, and records draws into command buffers for a few
words each.

---

## Decisions

| Question | Decision |
|---|---|
| Shared code with the GL renderer | **Copied**, not extracted. The GL renderer stays untouched so the other platforms keep working as they do; the two can be merged later. |
| A shader key nobody has compiled yet, met during play | **The draw is skipped** while the shader compiles on another thread. |
| The Mesa renderer on the Switch | **Kept, selectable for good** in `config.toml`. |
| Shader compiler | UAM's compiler, linked into the Switch host and run on the console (it builds for the Switch). |
| What players share | **Keys**, not compiled shaders: a key is data, compiled shaders are GPU code nobody can check, and keys survive changes to the generators and the compiler. |

---

## Where the renderer lives

The renderer runs in the guest (ILP32), and deko3d is a host library (LP64):
its structs carry 64-bit pointers and GPU addresses that the guest cannot
hold, and stubbing its API call by call would put a guest→host call on every
state change.

So the renderer is split at the draw:

- **Guest:** the `D3DDevice_*` entry points, the state they keep, the vertex
  declarations, reading the state back at each draw, the pixel shader key and
  the GLSL generation.
- **Host:** a deko3d backend that takes one compact description per draw,
  clear and present (fixed-width fields, in guest memory), so one stub call
  each.

The host reads guest memory directly (it is below 4 GB): vertex data,
textures, `D3D__RenderState`.

### Both renderers in one Switch build

Two guest images, and the host runs one. `halo_guest.elf` is the game with
the OpenGL renderer, built as before; `halo_guest_dk.elf` is the same objects
with `port/switch/guest/d3d8_dk.c` in place of `d3d8_gl.c`. The host reads
`display.renderer` from `config.toml` before it loads the guest and picks the
image (`host_main.c`; falling back to OpenGL if the deko3d image is missing).
No source of the OpenGL renderer is touched, and nothing in the guest has to
choose at run time.

Under deko3d the host makes no EGL surface: SDL starts without video, and
the guest's window and GL context are stand-ins (`host_sdl2.c`), so the
guest's platform layer - whose event loop runs only while it has a window -
works as it does over Mesa, and the display is left for deko3d's swapchain.
The guest's remaining GL calls (high-res HUD and text, menu art) have no
context there, and Mesa does nothing with them, until their deko3d versions
replace them.

Config: `display.renderer = "gl"` (the default) or `"deko3d"`, under
`[display]`.

---

## Files

New, Switch only (copies of the GL renderer's files, adapted):

| File | From | What changes |
|---|---|---|
| `port/switch/guest/d3d8_dk.c` | `d3d8_gl.c` | entry points and state kept; GL calls replaced by draw descriptions sent to the host (drawing still stubbed) |
| `port/switch/guest/xbox_textures_dk.c` | `xbox_textures.c` | format decoding kept; upload and cache go to the host |
| `port/switch/guest/nv2a_vsh_dk.c`, `nv2a_psh_dk.c` | `nv2a_vsh.c`, `nv2a_psh.c` | GLSL for UAM (below) |
| `port/switch/guest/hud_hires_dk.c`, `text_hires_dk.c` | `hud_hires.c`, `text_hires.c` | their few GL calls |
| `port/switch/host/host_dk.c` | — | the deko3d backend |
| `port/switch/host/host_dk_shaders.c` | — | UAM compiling, the DKSH cache, the compile thread |

Unchanged and shared: `d3d8_resources.c` (no GL in it), the rest of the
platform layer.

---

## Phase 0 — spike (done)

Settles the unknowns before the bulk of the work.

- Link `deko3d` and UAM's compiler into the Switch host (`configure.py`).
- A device, a queue and a swapchain on `nwindowGetDefault()`. SDL2 must not
  open its EGL window but must keep working for input and audio — check that
  first.
- Compile one generated vertex shader and one pixel shader with UAM on the
  console; time both.
- Wrap the guest's contiguous window in a `DkMemBlock` (`DkMemBlockMaker.storage`,
  0x1000-aligned) and draw from it. If the GPU can read the game's buffers in
  place, the mirror (pages, write protection, uploads) is not needed.

**Done when:** a triangle from the game's own vertex data and generated
shaders is on screen, with compile timings and an answer on the memory
block.

## Phase 1 — the deko3d image (done)

A second guest image with the deko3d renderer's device, `d3d8_dk.c`, chosen
by `display.renderer` (above). The device holds what `d3d8_gl.c` does that is
not OpenGL, copied as it is: the screen's width and scale, the XDK's state,
the vertical blank, the reserved viewport constants, render and texture
stage state, vertex shaders and declarations, streams and immediate mode. No
changes to the GL files.

## Phase 2 — the backend's skeleton (done)

`port/switch/host/host_dk.c`: a device and a queue, a ring of command memory
slices behind fences, render targets and depth buffers as images found by
their data address, and a swapchain on the default window. The guest writes
a stream of commands over a frame (`guest/dk_commands.h`) and hands it over
at Present, one crossing a frame. Clears (clipped to the viewport, per
channel: the fog screen clears alpha only) and presenting (a letterboxed
blit; the swapchain paces the frames) work.

## Phase 3 — reading the game's memory (done)

The GPU reads the game's vertex and index data where the game keeps it, as
the NV2A did, instead of a copy (the mirror) as `d3d8_gl.c` does. What that
involves:

**One memory block per committed chunk of the window.** Where the console
maps low memory only as code memory (Horizon 22.5 and later;
`host_mman_low_code_mode`), the window is made real 16 MB at a time, each
chunk an alias of its own heap made with `svcMapProcessCodeMemory`, and a
chunk once committed stays (`host_memory.c`, `commit`). The probe found that
the GPU maps such an alias, and that memory already mapped for the GPU
cannot then be aliased - nor, it must be assumed, unaliased. So each chunk
gets its deko3d memory block (`DkMemBlockFlags_CpuCached |
DkMemBlockFlags_GpuCached`, storage the chunk's address) right after it is
committed, by the host, and keeps it. A draw's data is then a chunk's GPU
address plus an offset. Data that crosses from one chunk into the next (each
chunk's GPU address is deko3d's choice, so two are not contiguous) goes the
way data outside the window goes.

Where `svcMapMemory` maps low memory (Horizon 21.2), the window is not
chunked: parts of it are mapped and unmapped as the game asks. How it is
mapped there has to be read first (`host_mman.c`, `host_memory.c`); if parts
of it are ever unmapped, those parts cannot be GPU-mapped, and that build
either commits the window in chunks too or copies.

**Data outside the window goes through an upload buffer.** Index buffers
made by `CreateIndexBuffer` are in ordinary guest memory (`calloc`,
`d3d8_resources.c`; a map's own index data is in the window), immediate mode
(`Begin`/`End`) builds its vertices in the device, and quad lists need
indices made for the draw. These are copied into a per-frame slice of a
CPU-mapped buffer (`CpuUncached`), reused once the frame's fence has passed,
like the command memory. The copy is written into the command stream's
draw, or into the slice by the host - whichever keeps the guest-to-host
crossing at one a frame.

**The CPU's cache is cleaned for what the GPU reads.** The window's memory
stays CPU-cached: the game reads its map data from it all the time, and an
uncached window would slow everything. The CPU's writes - map loading, locked
buffers, dynamic vertices, which the game writes without announcing (the
reason the mirror needed `memory_watch`'s faults) - may sit in its cache, so
the host cleans (`armDCacheClean`: written back, not invalidated, the CPU
reading on) each range a draw reads as it records the draw, each range once
a submission (`window_read`). Only what is read is cleaned, written or not;
nothing is watched. If that costs too much, `memory_watch`'s per-page write
generations can skip pages not written since their last cleaning.

**The GPU's caches are invalidated by deko3d.** After every queue flush
deko3d writes back and invalidates the GPU's L2, texture, shader and
descriptor caches (`Queue::postSubmitFlush`, "to ensure the visibility of
CPU updates"), so every submission ends with a flushing fence, and the next
one reads what the CPU has written and cleaned since.

**Waiting for the GPU is real.** `D3DResource_IsBusy` answers "no" and
`BlockUntilNotBusy` and the locks return at once (`d3d8_resources.c`): right
while `d3d8_gl.c` copied every draw's data at the draw, wrong once the GPU
reads the game's memory a frame later. The game relies on them - its texture
cache spins on `IsBusy` before reusing a texture's memory
(`xbox_texture_cache.c`), and the grass rebuilds its vertices in a locked
buffer every frame. So:

- each handing over of the guest's command stream is one submission,
  numbered alike by both halves; the host ends every one with a fence and
  reports the highest the GPU has finished (`host_dk_retired`);
- a draw or clear records in each resource it reads or writes - render
  targets, textures, palettes, vertex buffers, index buffer - the submission
  it is in, in the resource's `Lock` field, as the Xbox's runtime did (the
  game sets it to 0 whenever it makes a resource's header);
- `IsBusy` is "that submission not finished", `BlockUntilNotBusy` waits for
  it, and a lock without `D3DLOCK_NOOVERWRITE` or `D3DLOCK_READONLY` waits as
  the Xbox's does;
- a resource used in the submission still being written makes the check hand
  the stream over first, as the Xbox kicks off its push buffer - otherwise a
  caller spinning on `IsBusy` (the texture cache does) would wait for a
  submission that never comes;
- `D3DDevice_IsBusy` hands the stream over and is "a submission not
  finished"; `KickPushBuffer` hands it over.

Per resource from the start: a coarse "anything submitted unfinished" is
nearly always true while the game runs a frame or two ahead of the GPU, and
the texture cache, which takes a busy texture for a locked one, could then
evict nothing.

## Phase 4 — GLSL for UAM

In `nv2a_vsh_dk.c` / `nv2a_psh_dk.c`:

- GLSL 4.60 with an explicit `layout(location)` on every vertex output and
  pixel input, so any vertex shader goes with any pixel shader, unlinked.
- Uniforms in uniform buffers: the vertex constants (192 vec4, 3 KB) and the
  per-draw values (combiner constants, fog, alpha reference, bump matrices,
  texture scale, screen offset).
- No y flip and no depth remap: `DkDeviceFlags_OriginUpperLeft` and
  `DkDeviceFlags_DepthZeroToOne` give Direct3D's conventions.
- The LOD bias goes back into the sampler.

- The game's real shaders, generated so, timed on the console: what they
  cost says what compiling shared keys at startup costs.

## Phase 5 — shader cache

- **Keys.** A vertex shader: a hash of its instructions (not the order it was
  created in, which only holds for one build), its variant and packed mask. A
  pixel shader: `nv2a_pixel_shader_key` without `count_samples` (Android
  only). Each file carries a format version.
- **Compiled shaders.** DKSH files on the SD card, named by the key's hash;
  the cache is stamped with the generator's and UAM's versions, and a change
  in either throws the compiled shaders away (the keys still compile).
- **At startup:** read every key file in the SD folder, compile the keys
  missing from the cache, with progress shown before the menus. After that a
  shader is a file read.
- **During play:** a missing key goes to a compile thread and the draw is
  skipped until its shader is ready; the key is recorded for sharing.
- **Code memory:** one growing area of a `DkMemBlockFlags_Code` block.

## Phase 6 — draws, textures and render targets

The menus first, then a map.

- **Uniforms:** a ring of uniform buffers behind the frames' fences.
- **Render targets:** render-to-texture, the mip composite (the water's
  ripples), the screen's scale.
- **Textures:** decoded as now, uploaded through a staging buffer and
  `dkCmdBufCopyBufferToImage`, cached. BC1-BC3 and BGRA are native.
- **State:** the Direct3D state into deko3d's rasterizer, color, blend and
  depth-stencil states; samplers and images in descriptor sets.
- **Draws:** native quads, base vertex, immediate mode.
- **Visibility tests:** `dkCmdBufReportCounter(DkCounter_SamplesPassed)` into
  memory — real counts, as the NV2A gave.
- **The high-res HUD and text, and the menus' art**, whose GL calls do
  nothing under deko3d.
- **Debugging:** the existing settings (`debug.screenshot_every`,
  `debug.gpu_trace_frame`, `debug.gpu_stats`, `debug.gpu_dump_shaders`), and
  deko3d's debug build for validation.

## Phase 7 — parity and performance

- Every map's scenes compared against the Mesa renderer on the Switch with
  `debug.screenshot_every`; especially split screen, water, lens flares,
  decals (z bias), fog, the HUD's meters and the PC menus.
- CPU frame time against Mesa; no hitches with a warm cache.

## Phase 8 — collecting keys

- Builds record keys into the SD folder; testers send the files in.
- A PC tool merges them, drops duplicates and reports what each submission
  added.
- Releases ship the merged key file; each console compiles it once at
  startup.

---

## Risks

| Risk | Settled in |
|---|---|
| SDL2's input and audio without its EGL window | phase 0 |
| The GPU reading guest memory: alignment, CPU cache coherence | phase 0 |
| UAM's compile time on the console, at startup and for misses | phase 0 |
| Behaviour of Mesa that the GL renderer relies on without saying so | phase 7 |
| Two copies of the shared code drifting apart until they are merged | ongoing |

---

## Progress

### Phase 3

Built, and run on a console (Horizon 21.2, the svcMapMemory firmware, where
the window used to be mapped and unmapped piecemeal and is now committed in
chunks under deko3d): all eight 16 MB chunks of the window were GPU-mapped
before the first frame (`deko3d: window chunk 0x50000000 is GPU address
0502340000` and seven more), with no refusal from nvmap or the GPU's
address space, and the game booted to the menus. Nothing reads the window
through the GPU yet - there are no draws until phase 6 - so the cleaning
and the busy checks' waits are exercised only by clears so far. The
code-memory firmware (22.5 and later), where the window was already
chunked, is still to be run. What was built:

- Each committed chunk of the window is noted by `host_memory.c` and given
  its memory block by the game thread at the next submission, after the
  probe's nvmap and address-space checks (`chunks_map`). Under deko3d the
  window is committed in chunks on every firmware, the svcMapMemory one
  included, and never unmapped (`window_is_chunked`); pools are as before.
- `window_read` gives a draw a range's GPU address and cleans it from the
  CPU's cache; `upload_copy` puts data outside the window in the frame's
  slice of a `CpuUncached` upload buffer. Both wait for phase 6's draws.
- Submissions are numbered and fenced, and IsBusy, BlockUntilNotBusy and the
  locks answer per resource through `Lock` (`d3d8_resources.c` has weak,
  empty defaults for the OpenGL image).

### Phase 2

On a console, under deko3d: the device comes up as the game starts
(presenting at 1280x720), the game's back buffer (852x480, the screen's
shape) and its depth buffer become images the first time they are drawn
into, frames are presented about a second after launch and then at 60 a
second, paced by the swapchain, with no errors. The game thread spends 0.6
to 0.7 ms a frame (4% of the time) in the menus. Nothing is drawn but the
clears, which in the menus are black.

### Phase 1

- `halo_guest_dk.elf` is built by `ninja switch` and uploaded by
  `tools/switch_deploy.py` with the rest.
- `d3d8_dk.c` holds the non-GL parts of `d3d8_gl.c`, copied as they are: the
  screen's width, the XDK's state, the vertical blank, the reserved viewport
  constants, render and texture stage state, vertex shaders and
  declarations, streams and immediate mode. Drawing, clears and visibility
  tests do nothing yet, and nothing is shown.
- The host picks the image from `display.renderer` and, under deko3d, gives
  the guest a stand-in window and context and holds frames to 60 Hz itself.
- `display.renderer` is in `port_config.c`'s table for the Switch build only.

On a console the deko3d image boots to the menus: the host loads it, SDL
runs without video, the menus play and answer the controller with nothing
drawn, at 60 frames a second. The game thread's work in the menus, a
baseline for the backend: 0.5 ms a frame (3% of the time) drawing nothing,
against 5.8 ms (35%) for the OpenGL image - so nearly all of the OpenGL
image's frame is the translation to GL and Mesa's driver.

### Phase 0

**UAM builds for the console.** `ninja switch-uam` clones UAM at a pinned
commit, applies `uam.patch` and cross-compiles it with meson into
`build/switch/uam/libuam.a` (6 MB, 178 objects). The patch adds a static
library target beside UAM's tool and drops a version check that needs
`distutils`. The build machine needs meson, bison, flex and Python's mako
(`apt install python3-mako`). UAM is Zlib-licensed, with Mesa's MIT for the
files it imports.

**UAM and Mesa clash, and are separated.** UAM is made of Mesa's GLSL
compiler, and so is the Mesa that devkitPro's SDL2 links in - which the port
keeps for the GL renderer. Linked side by side they collide: duplicate
definitions, and C++ vtables in COMDAT groups of the same names, of which
the linker keeps only the first. The fix, used by the probe and to be used
by the host: link UAM together with the one file that calls it into a single
relocatable object (`ld -r`), rename every symbol it defines (`objcopy
--redefine-syms`, which renames the COMDAT groups too), and keep only the
entry point global (`--keep-global-symbol`). The result links beside Mesa
with nothing left unresolved but the C and C++ runtimes.

**The probe** (`probe/deko3d`) answers the rest on the console and writes
the answers to `sdmc:/deko3d_probe.txt`:

1. SDL2's input and audio with no SDL window, deko3d owning the display.
2. UAM's compile time on the console, for a small shader and for shaders
   the size of the game's.
3. Whether the GPU can read memory mapped as the game's window is: heap
   aliased with `svcMapMemory` and with `svcMapProcessCodeMemory`, the memory
   block made on the alias or on the heap behind it, before or after the
   alias. Each case draws from vertices written through the alias, reads the
   pixel back, rewrites the vertices and checks again.

To run it:

```
sudo dkp-pacman -S deko3d
ninja switch-uam
cd port/switch/probe/deko3d && make
```

then copy `deko3d.nro` to the card and launch it the way the game is
launched. A case that crashes the probe can be skipped by naming it on a
line of `sdmc:/deko3d_probe_skip.txt`.

#### Results (Horizon 21.2, Atmosphère)

- **SDL2 without a window:** initialises, opens the controller and plays
  audio while deko3d owns the display, and delivers controller input while
  deko3d presents frames. (Pressing A arrives as controller button 1, B in
  SDL's positional layout, and as joystick button 0.)
- **deko3d keeps GPU state between command lists:** a pass that does not set
  its own viewport and scissor inherits the last ones.
- **deko3d aborts the program when it cannot make a memory block**, in its
  release build as in its debug one (`diagAbortWithResult`, result 0x367).
  The renderer must not ask it for one the console will refuse.
- **Compile times on the console (UAM):** a tiny shader 13-15 ms; a pixel
  shader with 4 textures and 8 combiner stages about 185 ms; a synthetic
  vertex shader with 192 constants and some 40 operations about 1,050 ms.
  Loading a compiled shader back from the card: 2 ms. The game's real
  shaders are still to be timed; compiling shared keys at startup will need
  a progress screen.
- **The GPU reads the window's kind of memory through its alias.** For both
  `svcMapMemory` and `svcMapProcessCodeMemory`, a memory block made on the
  alias address works: drawn from, and CPU writes seen after
  `armDCacheFlush`. The heap behind an alias cannot be mapped into a GPU
  address space (0x275c), and memory already mapped for the GPU cannot then
  be aliased (0xd401). So the renderer maps the window by its alias, after
  the window is committed, and draws from the game's buffers in place: no
  mirror.

- **Presenting:** a deko3d swapchain on the default window presents at the
  display's rate, and the probe shuts down cleanly (every deko3d object
  destroyed, no errors).

Phase 0 is done: every question it was for has its answer, and none of them
changes the plan except for the better (no mirror).

# deko3d renderer — plan

A second renderer for the Switch build that drives the GPU through
[deko3d](https://github.com/devkitPro/deko3d) instead of Mesa, with compiled
shaders cached on the SD card.

Status: phase 0 (the spike) done; see "Progress" at the end.

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
| `port/switch/guest/d3d8_dk.c` | `d3d8_gl.c` | entry points and state kept; GL calls replaced by draw descriptions sent to the host (phase 1: drawing stubbed) |
| `port/switch/guest/xbox_textures_dk.c` | `xbox_textures.c` | format decoding kept; upload and cache go to the host |
| `port/switch/guest/nv2a_vsh_dk.c`, `nv2a_psh_dk.c` | `nv2a_vsh.c`, `nv2a_psh.c` | GLSL for UAM (below) |
| `port/switch/guest/hud_hires_dk.c`, `text_hires_dk.c` | `hud_hires.c`, `text_hires.c` | their few GL calls |
| `port/switch/host/host_dk.c` | — | the deko3d backend |
| `port/switch/host/host_dk_shaders.c` | — | UAM compiling, the DKSH cache, the compile thread |

Unchanged and shared: `d3d8_resources.c` (no GL in it), the rest of the
platform layer.

---

## Phase 0 — spike

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

## Phase 1 — copy the shared pieces

Copy into the deko3d renderer what is not GL: the screen's width and scale,
the menus' pointer, the vertical blank thread, the device state, vertex
declaration parsing, the vertex constants' serials. No changes to the GL
files.

## Phase 2 — GLSL for UAM

In `nv2a_vsh_dk.c` / `nv2a_psh_dk.c`:

- GLSL 4.60 with an explicit `layout(location)` on every vertex output and
  pixel input, so any vertex shader goes with any pixel shader, unlinked.
- Uniforms in uniform buffers: the vertex constants (192 vec4, 3 KB) and the
  per-draw values (combiner constants, fog, alpha reference, bump matrices,
  texture scale, screen offset).
- No y flip and no depth remap: `DkDeviceFlags_OriginUpperLeft` and
  `DkDeviceFlags_DepthZeroToOne` give Direct3D's conventions.
- The LOD bias goes back into the sampler.

## Phase 3 — shader cache

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

## Phase 4 — the backend

- **Frames:** a ring of command buffers with a fence each; a ring of uniform
  buffers.
- **Memory:** draws read vertices and indices straight from the guest window
  if phase 0 showed they can; otherwise a port of the mirror.
- **Render targets:** `DkImage`s by `Data` address as now, with
  render-to-texture, the mip composite (the water's ripples) and the screen's
  scale.
- **Textures:** decoded as now, uploaded through a staging buffer and
  `dkCmdBufCopyBufferToImage`, cached. BC1-BC3 and BGRA are native.
- **State:** the Direct3D state into deko3d's rasterizer, color, blend and
  depth-stencil states; samplers and images in descriptor sets.
- **Draws:** native quads, base vertex, immediate mode.
- **Clears:** clipped to the viewport, per channel (the fog screen clears
  alpha only).
- **Visibility tests:** `dkCmdBufReportCounter(DkCounter_SamplesPassed)` into
  memory — real counts, as the NV2A gave.
- **Present:** a letterboxed blit into the swapchain's image; frame pacing
  and interpolation as now.
- **Debugging:** the existing settings (`debug.screenshot_every`,
  `debug.gpu_trace_frame`, `debug.gpu_stats`, `debug.gpu_dump_shaders`), and
  deko3d's debug build for validation.

## Phase 5 — parity and performance

- Every map's scenes compared against the Mesa renderer on the Switch with
  `debug.screenshot_every`; especially split screen, water, lens flares,
  decals (z bias), fog, the HUD's meters and the PC menus.
- CPU frame time against Mesa; no hitches with a warm cache.

## Phase 6 — collecting keys

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
| Behaviour of Mesa that the GL renderer relies on without saying so | phase 5 |
| Two copies of the shared code drifting apart until they are merged | ongoing |

---

## Progress

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

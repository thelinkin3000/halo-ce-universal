# deko3d renderer — plan

A second renderer for the Switch build that drives the GPU through
[deko3d](https://github.com/devkitPro/deko3d) instead of Mesa, with compiled
shaders cached on the SD card.

Status: phases 0 to 6 done (split screen yet to be tried on the console); phase 7, the game thread measured, under way; see "Progress" at the end.

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
| Compiling the known keys a console has not cached (43 s on one core with UAM's front end kept alive, phase 5 step 1) | **In the background, on one thread, while the game runs** - no waiting screen. Several threads were measured slower than one (phase 5, step 1). Draws whose shader is not ready are skipped, and the keys they need go to the front; the cache keeps every shader compiled, so it happens once a console (and again after a change to the generators or UAM). |

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

### The Switch's only renderer

Until deko3d was the only renderer, the Switch build made two guest images,
and the host ran the one `display.renderer` named: `halo_guest.elf` with the
OpenGL renderer and `halo_guest_dk.elf` with `port/switch/guest/d3d8_dk.c`
in place of `d3d8_gl.c` (the phases below were measured that way, the one
image against the other). The OpenGL image is no longer built:
`halo_guest.elf` is the deko3d one, the host always loads it, and
`display.renderer` is not a Switch setting. An update removes the
`halo_guest_dk.elf` an older release left (`host_update.c`). The host's
OpenGL-over-Mesa paths, which it skips under deko3d, are still there, to be
taken out later.

Under deko3d the host makes no EGL surface: SDL starts without video, and
the guest's window and GL context are stand-ins (`host_sdl2.c`), so the
guest's platform layer - whose event loop runs only while it has a window -
works as it does over Mesa, and the display is left for deko3d's swapchain.
The guest's remaining GL calls (high-res HUD and text, menu art) have no
context there, and Mesa does nothing with them, until their deko3d versions
replace them.

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

## Phase 4 — GLSL for UAM (done)

Written to be picked up by an agent that has not seen the work so far. Read
"Where the renderer lives" and phases 0 to 3 above first; then read
`port/linux/src/nv2a_vsh.c` and `port/linux/src/nv2a_psh.c` whole (380 and
660 lines), and `prepare_draw` and `program_get` in `port/linux/src/d3d8_gl.c`,
which show what the generated shaders are fed. The work is reviewed against
the acceptance list at the end of this phase.

### What this phase is, and is not

It produces the deko3d renderer's two GLSL generators and proves that UAM
compiles what they produce for the game's real shaders - on the PC, and on
the console with the times taken. It does **not** draw anything (phase 6),
cache anything (phase 5) or link UAM into the host (phase 5). Nothing in it
changes what the console shows.

### Files

- `port/switch/guest/nv2a_vsh_dk.c`, copied from `nv2a_vsh.c`; the function
  renamed `nv2a_dk_vertex_shader_to_glsl` (same arguments: the program's
  instructions, their count, the declaration's packed-attribute mask).
- `port/switch/guest/nv2a_psh_dk.c`, copied from `nv2a_psh.c`; the function
  renamed `nv2a_dk_pixel_shader_to_glsl` (same argument: a
  `struct nv2a_pixel_shader_key`).
- `port/switch/guest/dk_shaders.h`: the generators' prototypes, the binding
  numbers and locations below as `#define`s, and C structs that are the
  uniform blocks' std140 layout byte for byte (phase 6 fills them), each
  with its size checked at compile time (`typedef char
  name_size_check[sizeof(struct x) == N ? 1 : -1];`, the way `d3d8_gl.c`
  checks `nv2a_pixel_shader_key`). Fixed-width types and floats only, like
  `dk_commands.h`, so the host can include it too.
- `tools/switch_build.py`: the two new sources added to `dk_objects` (the
  deko3d image only; look for `d3d8_dk.c` there). The originals stay in both
  images; the new names do not collide with them.

The originals are not edited: they are compiled into every platform's
image. The Switch's guest is compiled with `HALO_ANDROID` (and
`HALO_SWITCH`), so in the originals the Switch takes the OpenGL ES branches;
the copies drop every `#ifdef HALO_ANDROID` branch and keep what the
deko3d version needs, as below.

### What changes from the OpenGL generators

**Version and precision.** `#version 460` as the first line (checked with
`uam`: it compiles a 460 vertex shader with a std140 block and an invariant
`gl_Position`, and rejects a loose uniform with "uniform 'loose' in driver
constbuf ... not supported"); no `precision`
statements, no `xgpu_capabilities.shading_language`. UAM defines `DEKO3D`
(100) if anything needs to tell.

**No loose uniforms.** UAM rejects any uniform outside a uniform block (its
README: they are "reported as an error"), and every block and sampler needs
an explicit `binding`. Bindings are per stage. Use these:

| Stage | Binding | Block | Contents |
|---|---|---|---|
| vertex | 0 | `vertex_constants` | `vec4 c[192];` - 3072 bytes |
| vertex | 1 | `vertex_parameters` | `vec4 viewport_scale; vec4 viewport_offset; vec4 point_and_screen;` (x: the point size, y: the screen offset) |
| fragment | 0 | `pixel_parameters` | `vec4 ps_c0[8]; vec4 ps_c1[8]; vec4 ps_final_c0; vec4 ps_final_c1; vec4 fog_color; vec4 fog_parameters; vec4 alpha_reference;` (x) `vec4 bump_matrix[4]; vec4 bump_luminance[4]; vec4 texture_scale[4];` |
| fragment | samplers 0-3 | `tex0`-`tex3` | `layout(binding = N) uniform sampler2D/sampler3D/samplerCube texN;`, the type from the key as now |

Declare every block `layout(std140, binding = N) uniform name { ... };`
with **vec4 members only**: std140 gives a lone `float` 4-byte alignment
but an array of floats a 16-byte stride, and mixing them is how a C struct
and a block drift apart. The OpenGL generators' `uniform float point_size`,
`screen_offset` and `alpha_reference` become components of a vec4, and
every use of them changes to match (`point_and_screen.x`,
`point_and_screen.y`, `alpha_reference.x`). The OpenGL ES-only
`texture_lod_bias` goes: under deko3d the LOD bias is the sampler's
(`DkSampler.lodBias`, phase 6), so `SAMPLE_BIAS` is the desktop one, empty.

**Explicit locations between the stages.** UAM has no linking: each stage
is compiled alone, so a vertex output and a pixel input meet only by
location. Give both sides the same numbers:

| Location | Vertex output / pixel input |
|---|---|
| 0 | `xD0` |
| 1 | `xD1` |
| 2 | `xB0` |
| 3 | `xB1` |
| 4 | `xT0` |
| 5 | `xT1` |
| 6 | `xT2` |
| 7 | `xT3` |
| 8 | `xFog` (float) |

Every vertex shader writes all nine, and every pixel shader declares all
nine, read or not, so that any vertex shader goes with any pixel shader -
that is what makes caching them apart (phase 5) work. The pixel output
stays `layout(location = 0) out vec4 fragment_color;`. Vertex inputs keep
their `layout(location = N)` (0-15), all sixteen declared, as now: phase 6
feeds an attribute the game did not put in a stream from a stride-0 buffer
holding its constant value, so the shaders need not know which are which.
A packed (`D3DVSDT_NORMPACKED3`) attribute stays `in uint` and is unpacked
in the shader as now.

**Clip space: the desktop's conventions, the ES branch's precision.** The
host's deko3d device is made with `DkDeviceFlags_DepthZeroToOne |
DkDeviceFlags_OriginUpperLeft` (`host_dk.c`, `initialize`), which is what
`glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)` gives desktop GL. So, at the
end of the vertex shader:
- no `gl_Position.y = -gl_Position.y` and no `gl_Position.z = 2.0 *
  gl_Position.z - gl_Position.w` (both are the ES branch's emulation of
  glClipControl);
- keep the ES branch's `clip_captured` path (capture `oPos` where the
  program takes `rcc` of `r12.w`, and compute `gl_Position` from the
  captured clip position without dividing by w and multiplying again). It
  is about precision near the camera plane, not about ES: it was found on a
  Mali GPU, but dividing by w and multiplying back loses precision on any.
  Its constants are `c[%d]` for `XGPU_VERTEX_CONSTANT_BIAS - 38` and `- 37`,
  as now;
- keep the half-pixel offset `+ 0.5` (Direct3D 8 puts pixel centres on
  integers; the Maxwell rasterizer, like GL, on half-integers) and
  `screen_offset`;
- keep `invariant gl_Position;` (the game draws multipass with equal
  depth).
Whether the picture comes out the right way up can only be seen once
something draws (phase 6). If it is upside down then, the fix is the device
flag (`DkDeviceFlags_YAxisPointsDown`), not the shaders.

**Occlusion.** No `count_samples` branch and no atomic counter: under
deko3d the visibility tests count with `DkCounter_SamplesPassed` (phase 6).
The key's `count_samples` is always 0 in the deko3d image; the generator
ignores it.

**Everything else is kept as it is**, line for line: the instruction
decoding, the MAC and ILU operations and their helper functions, the
texture stage modes, the combiner stages, the final combiner, fog, alpha
test (in the shader, against `alpha_reference.x`), alpha kill, colour sign,
`coverage_alpha`, and the `debug.gpu_debug_*` settings. These are the
translation of the hardware, debugged against the game over a long time;
the phase is about the GLSL dialect, not the translation. Watch for UAM's
other differences (its README, `build/switch/third_party/uam/README.md`):
integer `/` and `%` by a non-constant become float division (the generators
use bit operations, which are fine); `layout(origin_upper_left)` and
`pixel_center_integer` are not supported.

### Proving it: the game's real shaders

The corpus is the game's own:
- **Vertex shaders:** every program the game makes with
  `D3DDevice_CreateVertexShader` (from its table, at startup), each with
  the packed masks it is drawn with, and the immediate-mode variant (packed
  mask 0).
- **Pixel shaders:** the keys the OpenGL image recorded on the console in
  `shader_programs.bin` (`/switch/halo/save/z/shader_programs.bin`, 320 KB,
  about 1,200 records from real play when this was written). The format is
  `d3d8_gl.c`'s program records (look for `PROGRAM_RECORD_MAGIC`): two DWORDs
  (the magic, "PSC1", and the record's size), then records of `struct
  program_record` - map hash, vertex shader id, variant, packed mask, and
  the `nv2a_pixel_shader_key` itself. The key struct is shared, so a record
  read by the deko3d image is the same size; check the header's size anyway
  and refuse a file whose records differ.

**1. A dump in the deko3d image.** When `debug.gpu_dump_shaders` names a
folder (an existing setting, on every platform; the OpenGL renderer writes
its GLSL there), the deko3d image writes, once, after the game has created
its vertex shaders (the first Present is a good moment):
- `vs_<id>_<packed mask in hex>.vert` for every vertex shader object and
  every packed mask it appears with in the records, plus mask 0;
- `ps_<key hash in hex>.frag` for every distinct key in the records
  (`hash_words` in `d3d8_gl.c` is the hash to copy);
- `manifest.txt`: one line a file, what it was made from.
`d3d8_dk.c` keeps no list of its vertex shader objects yet (`d3d8_gl.c`'s
Switch code has `vertex_shaders_by_id`); add one. Make the folder with
`mkdir` and write with `fopen` as the OpenGL dump does; a path like
`sdmc:/halo_dk_shaders` is what the guest's file calls take on the console
(the host's log shows the guest's paths in that form). Log how many of each
were written.

**2. Compiled on the PC.** devkitPro's `uam` package is installed here
(`/opt/devkitpro/tools/bin/uam`, else `which uam`). Pull the folder over FTP
(port 5000, anonymous; `tools/switch_logs.py` shows how) and compile every
file: `uam -s vert FILE -o /dev/null`, `-s frag` for `.frag`. Every file must
compile. Keep the script that does this (`tools/dk_shader_check.py`), so it
can be run again after any change to the generators; it reports failures
with UAM's message and the file, and counts warnings by kind.

**3. Timed on the console.** Extend the probe (`port/switch/probe/deko3d`):
if `sdmc:/halo_dk_shaders` exists, compile every `.vert` and `.frag` there
with UAM (`probe_compile`, which already writes a DKSH) and log each file's
time, then the count, the total, the average and the slowest ten per stage,
into `sdmc:/deko3d_probe.txt`. Leave the probe's existing tests as they are;
put the new pass after them or behind a file's presence, so the probe still
answers what it was written for. Deploy with `python3 tools/switch_deploy.py
--destination /switch port/switch/probe/deko3d/deko3d.nro`; the user runs
it and says when.

The total is what compiling every known key costs a console that has none
cached (phase 5's startup pass), and the slowest single shaders are what a
key first met in play costs while its draws are skipped. Write both into
"Progress" below. If the total is well over a minute, say so plainly: it
changes phase 5 (a pass that compiles in the background while the game runs,
say), and that is the user's call.

### Acceptance

The review checks each of these:

1. `ninja switch` builds with no new warnings; the OpenGL images are
   unchanged (no edits to `port/linux/src/nv2a_*.c`; `git diff` shows the
   shared files untouched apart from anything this list allows).
2. The generated GLSL has no uniform outside a block, every block and
   sampler has a binding, every block member is a vec4 or an array of them,
   both stages declare all nine interface variables at the locations above,
   and there is no y flip, depth remap, precision statement or atomic
   counter.
3. `dk_shaders.h`'s structs match the blocks (sizes checked at compile time;
   the vertex constants block is 3072 bytes, the vertex parameters 48).
4. Every dumped shader compiles with `uam` on the PC; the check script is in
   `tools/` and runs from a clean checkout.
5. The console times are in "Progress", with the number of shaders they
   cover and the probe's log lines they came from.
6. Nothing outside the files named here changes, except `DEKO3D.md`.

Don't commit; the review does that. Note anything unexpected in "Progress",
even if it was dealt with.

## Phase 5 — shader cache (done)

Written to be picked up by an agent that has not seen the work so far. Read
"Decisions", "Where the renderer lives" and phases 0 to 4 above, and phase
4's results under "Progress", first. Then read `port/switch/guest/d3d8_dk.c`
(the guest's device, with phase 4's shader dump), `port/switch/host/host_dk.c`
(the host's deko3d backend), the probe (`port/switch/probe/deko3d`, whose
Makefile links UAM into a program), `port/switch/uam.patch` and the
`_uam_build` part of `tools/switch_build.py`. The work is reviewed against
the acceptance list at the end of this phase.

### What this phase is, and is not

It gives the deko3d renderer its shaders: compiled on the console by UAM,
kept on the card, loaded into GPU code memory when wanted, and compiled in
the background for every key the console knows of but has not compiled.
It does **not** draw (phase 6): nothing asks for a shader from a draw yet,
so the path a draw will take is built and tested from the startup pass. At
the end, a first launch with an empty cache compiles every known shader in
the background while the menus run at full speed, and a second launch
compiles nothing.

The user's decision (see "Decisions"): **no waiting screen**. The game
starts at once; missing shaders compile in the background, on one thread
(step 1 measured more to be slower), in the order a draw needs them, then
this map's, then the rest. A draw whose shader is not ready is skipped
(phase 6).

### Step 1: what UAM can do, measured in the probe (done)

Done: see "Phase 5, step 1" under "Progress" for what was found and
decided - **one compile thread**, with UAM's front end initialised once
(43.1 s for the corpus, from 68.6 s). The text below is what was asked,
kept for the record.

Two things found while writing this, which decide the rest:

**UAM's compiler is not safe on two threads at once, as built.** Its Mesa
has fake locks (`build/switch/third_party/uam/mesa-imported/c11/threads.h`,
which says "This header is fake" and makes `mtx_lock` and the rest do
nothing), and its front end is one global context: `static struct gl_context
gl_ctx` in `source/glsl_frontend.cpp`. Worse, every `DekoCompiler` calls
`glsl_frontend_init()` when made and `glsl_frontend_exit()` when destroyed
(`source/compiler_iface.cpp`, around line 281), and the exit releases
Mesa's global type tables and built-in functions
(`_mesa_glsl_release_types`, `_mesa_glsl_release_builtin_functions`). Two
compiles at once would tear down each other's state.

**And each compile probably rebuilds Mesa's built-in functions from
scratch** - they are released at every exit - which may be much of the 85
to 236 ms a shader takes on the console. Keeping the front end up for the
life of the program, initialised once, could make every compile faster, on
one thread or several.

So, in the probe, in this order, each against phase 4's corpus (the 744
GLSL files the deko3d image dumps to `sdmc:/halo_dk_shaders`; see phase 4
for how to make the dump: set `gpu_dump_shaders = "sdmc:/halo_dk_shaders"`
under `[debug]` in `/switch/halo/config.toml`, run the game to the menus for
twenty seconds, and put the setting back - it slows every start by 15 s):

1. **Front end once.** Change UAM (in `uam.patch`, which `ninja switch-uam`
   applies to a fresh clone; regenerate the patch with `git diff` in a
   scratch clone at the pinned commit) so that `glsl_frontend_init` runs
   once and `glsl_frontend_exit` never runs while the program lives - for
   instance a `DekoCompiler` constructor flag, or a pair of free functions
   the program calls itself. Time the corpus on one thread as phase 4 did
   and compare. Write both numbers into "Progress".
2. **Threads.** Make UAM's locks real and its context per thread, in the
   patch: `c11/threads.h` as `pthread_mutex_t` (devkitA64's newlib has
   pthreads, through libnx), and the `gl_context` one per thread (a
   `__thread` pointer to one allocated and initialised the first time a
   thread compiles; the struct is large, so not `__thread` itself). Then look
   for other mutable state shared between compiles - `static` variables
   that are written, in `source/` and `mesa-imported/` (glsl, compiler,
   program, tgsi, codegen) - and list what you found and did about each
   under "Progress". Then, in the probe: compile the corpus on one thread,
   keeping its DKSH files; then on two and on three threads at once
   (pthreads, stacks of at least 1 MB each - Mesa recurses), comparing each
   DKSH byte for byte with the one-thread run; three times each. Log the
   times.
3. **Decide.** If every multi-thread run is byte for byte the one-thread
   run, with no crash, the host compiles on two threads (the game thread
   keeps one of the console's three cores, `host_thread.c`); if three were
   clearly faster than two, say so and let the user decide on the third.
   Otherwise, the compile thread is one, and the patch keeps whatever made
   compiles faster on one thread. Record what was decided and why.

Step 1 is worth reporting to the user on its own before going on: it
changes how long a first launch takes.

### Step 2: UAM in the host

The host is linked with devkitA64 and with Mesa (through devkitPro's SDL2),
which holds the same GLSL compiler as UAM: linked side by side, they clash
(phase 0, "UAM and Mesa clash, and are separated"). Do in
`tools/switch_build.py` what the probe's Makefile does:

- `port/switch/host/host_dk_compiler.cpp`: the one file that calls UAM,
  with one C entry point, e.g. `int host_dk_compile_glsl(int fragment,
  const char *glsl, const char *dksh_path)` (and the once-only front end
  setup from step 1), built with UAM's include paths and defines (the
  probe's `UAM_FLAGS`: `-std=c++11 -DNDEBUG -DDESKTOP -D_USE_MATH_DEFINES
  -D_GNU_SOURCE -DHAVE_POSIX_MEMALIGN` and `-I` for
  `build/switch/third_party/uam/{source,mesa-imported}` and
  `build/switch/uam/meson/mesa-imported{,/glsl,/glsl/glcpp}`);
- one object from it and `build/switch/uam/libuam.a` (`ld -r
  --whole-archive`), every symbol it defines renamed (`nm --defined-only`,
  `objcopy --redefine-syms`, prefix `uam_`), all but the entry point made
  local (`--keep-global-symbol`); the host linked with that object;
- the build when UAM cannot be built here (`_uam_build` returns None:
  no meson, bison, flex or mako): the host is linked with a stub
  `host_dk_compile_glsl` that fails and logs once, and `ninja switch` still
  succeeds, with a configure-time note saying the deko3d renderer will have
  no shaders it has not already got. The OpenGL renderer must never depend
  on UAM.

The NRO grows by some 5 MB (UAM is 6 MB of objects).

### Step 3: the shader service in the host

`port/switch/host/host_dk_shaders.c`, called by the guest through new
imports in `port/switch/host_imports.list` (the import stubs pass arguments
through in registers; a 64-bit integer goes in one X register on both
sides, AAPCS64 and arm64_32 alike - log one on both sides the first time to
be sure):

- **Identity.** A shader is known by a 64-bit hash of its key, made by the
  guest (step 4). The host never sees keys, only hashes and GLSL.
- **On the card.** `<data root>/shader_cache/<UAM commit, 12 hex>/`
  (`host_main.c`'s `data_root`, `sdmc:/switch/halo` on a console; add an
  accessor), one `<stage letter><hash, 16 hex>.dksh` a shader (`v`, `f`).
  Written as `.tmp` and renamed when complete, so a compile cut short leaves
  no half file. At start the host lists the folder once into a hash set
  (it does not open the files), and removes the other folders under
  `shader_cache/` only (they are other UAM versions'; nothing else is
  touched). The generators' version is in the guest's hash (step 4), so a
  generator change makes new names, not a new folder; a stale file is just
  never asked for.
- **Imports**, something like:
  - `uint32_t host_dk_shader_find(uint32_t stage, uint64_t hash)`: a handle
    (1 or more) if the shader is in GPU code memory; else, if its file is on
    the card, it is read and loaded now (about 2 ms; on the game thread,
    which is the one deko3d runs on) and its handle returned; else 0 with
    the state - queued or compiling, or unknown - told apart (an out
    parameter or two reserved values).
  - `void host_dk_shader_compile(uint32_t stage, uint64_t hash, uint32_t
    glsl, uint32_t glsl_size, uint32_t priority)`: queue it (the GLSL copied
    into host memory: `glsl` is a guest address, readable directly). Already
    queued: its priority is raised if the new one is higher, and the GLSL is
    ignored. Already compiled: nothing.
  - Priorities (in `guest/dk_shaders.h`): 0 a draw needs it now, 1 the map
    being played, 2 the rest.
- **The compile thread**: one (step 1: two or three were slower than one,
  Mesa's global locks being taken all the time), with a stack of at least
  1 MB (2 MB, as the probe's workers had, is safe) and a lower priority than
  the game thread, and never on the game thread's core. A plain host pthread starts on the process's default core,
  which is the game thread's: `host_thread.c`'s `place_thread` (static now)
  is what moves a thread onto the cores the game thread does not keep, and
  it logs "a thread starts on core N" when it does - check for that line.
  Expose it for these threads, rather than use `host_native_thread_create`,
  whose stacks come out of guest memory (they are for threads that call into
  the guest; these never do). It takes the queue's most urgent
  entry, compiles it to the card, adds its hash to the set, and logs a line
  every so often (count, queue length), not one a shader. Keep a lock around
  `host_dk_compile_glsl` all the same, so that nothing else can ever call it
  at the same time.
- **Code memory**: one `DkMemBlockFlags_Code` block (with `CpuUncached |
  GpuCached`), 16 MB to start - phase 4's 744 shaders took about 1 MB of
  DKSH, code and control together - code placed at 256-byte alignment and
  never freed (a shader once loaded stays for the program's life), and
  `DK_SHADER_CODE_UNUSABLE_SIZE` left free at its end. Loading is the probe's
  `shader_load`. Only the game thread touches deko3d (`host_dk.c`'s rule), so
  compile threads write files and nothing else.
- **A handle** is an index into a table of `DkShader`, which phase 6's draws
  will name in the command stream.

### Step 4: keys, key files and the startup pass in the guest

`port/switch/guest/dk_shaders.c` (in `dk_objects`, as phase 4's generators
are), with what it needs from `d3d8_dk.c`:

- **Keys.** A vertex shader's: the generators' version, the hash of its
  program (FNV-1a 64 over the instruction count and words, computed once in
  `D3DDevice_CreateVertexShader` and kept in the object), and the packed
  mask. A pixel shader's: the generators' version and the
  `nv2a_pixel_shader_key`, with `count_samples` set to 0 (it is Android's
  occlusion counting; it does not change the GLSL). The hash: FNV-1a 64 of
  that blob. `DK_SHADER_GENERATOR_VERSION` in `guest/dk_shaders.h`, starting
  at 1, to be raised by any change to what `nv2a_vsh_dk.c` or
  `nv2a_psh_dk.c` write - say so in a comment at the top of both.
- **Key files** - what players share (see "Decisions" and phase 11). In
  `z:\shader_keys\` (the save folder: `/switch/halo/save/z/shader_keys/` on
  the card), every file there read at start. Format: a header (magic
  `"DKK1"`, a format version, the record size) and fixed-size records:
  stage, the hash of the map it was first met on, and the key's data (for a
  vertex shader the program hash and packed mask, for a pixel shader the
  `nv2a_pixel_shader_key`). Records are deduplicated by their shader hash as
  they are read. A vertex record names its program by hash, so it can only
  be compiled once the game has made that program; one naming a program the
  game never makes is kept and skipped.
- **The console's own file**, `z:\shader_keys\console.dkk`: keys this
  console met first are appended to it (phase 6's draws will append the
  ones they miss), so it is the file a player sends.
- **The OpenGL records, imported once.** `z:\shader_programs.bin` (the
  OpenGL image's program records, phase 4) holds the keys of everything the
  user has played under OpenGL. The first time the deko3d image starts with
  no `console.dkk`, it converts them (vertex shader id to program through
  the id table phase 4's dump keeps - both images make their vertex shaders
  in the same order - and the packed mask; the pixel key as it is) and
  writes them as `console.dkk`. That is also what tests the writer in this
  phase.
- **The startup pass.** Once the game has made its vertex shaders - at the
  60th frame, as phase 4's dump learnt (`device.frame >= 60` in
  `D3DDevice_Present`) - go through the known keys a few at a time each
  frame (eight, say: generating GLSL costs the game thread time), asking
  `host_dk_shader_find` first and, for a key unknown to the host,
  generating its GLSL and calling `host_dk_shader_compile` with priority 1
  if its map is the one loaded and 2 otherwise. Log when it starts and
  ends, with counts.
- **Map priority.** `d3d8_gl_map_loaded(name)` (the game calls it as a map
  finishes loading; `d3d8_dk.c` has it empty) notes the map's hash (as
  `d3d8_gl.c`'s `program_record_map_hash`) and sends that map's keys that are
  not yet compiled again with priority 1, which raises them in the queue.
- **For phase 6** (built now, used then): `dk_shader_for_draw(stage, key)`,
  which hashes the key, looks in a guest-side table of hash to handle
  first, and otherwise asks the host; a key unknown to the host is
  generated, queued with priority 0 and appended to `console.dkk`; the
  answer is the handle or "not ready, skip the draw".

### Testing on the console

Deploy with `python3 tools/switch_deploy.py` (it uploads `halo.nro` and both
images; the probe goes with `--destination /switch
port/switch/probe/deko3d/deko3d.nro`). The user runs everything and says
when; logs are `/switch/halo/halo.log` and `/switch/halo/debug.txt` (read
both), crash reports in `/atmosphere/crash_reports`. `config.toml` has
`renderer = "deko3d"` under `[display]`.

1. Step 1's probe runs, with their numbers in "Progress".
2. **A first launch with an empty cache.** Delete `/switch/halo/shader_cache`
   and `/switch/halo/save/z/shader_keys` first (over FTP; nothing else). The
   game reaches the menus at once; the log shows the OpenGL records
   imported (about 1,197 records, about 744 distinct shaders), the startup
   pass queueing them, the compile threads' progress, and when the queue
   is empty and how long it took; the host's `fps` lines stay at 60 the
   whole time (they are printed every five seconds) and the game thread's
   ms a frame stays near the 0.6 ms phase 2 measured. Note the time.
3. **A second launch.** Nothing is compiled; the startup pass finds every
   shader on the card; how long it takes to get through them is in the
   log.
4. **A generator change.** Raise `DK_SHADER_GENERATOR_VERSION` in a local
   build, launch: everything is queued again under new names. Put it back
   (and don't commit the raise).

### Acceptance

The review checks each of these:

1. `ninja switch` builds with no new warnings; the OpenGL image is
   unchanged in behaviour and links no UAM; on a machine without mako the
   Switch build still succeeds, with the stub (try it: run `configure.py`
   with mako hidden, e.g. `PYTHONPATH` pointing at a folder holding a
   `mako.py` that raises `ImportError`, and look for the note).
2. Step 1's findings and numbers are in "Progress": the one-thread time
   before and after the front end is kept, each multi-thread run compared
   byte for byte, the shared state found, and the decision.
3. The first-launch test: every known shader compiled in the background,
   with the time it took, and the game at 60 frames a second while it
   did, from the log lines quoted in "Progress".
4. The second-launch test: no compiles, and the startup pass's time.
5. The host touches deko3d on the game thread only; compile threads write
   files and the queue, nothing else; a shader file is complete or absent.
6. `console.dkk` exists after the first launch, holds the imported keys,
   and a second launch reads it (and not `shader_programs.bin` again).
7. Shared files under `port/linux/src` are unchanged, except for anything
   behind `#ifdef HALO_SWITCH` that is argued for under "Progress".

Don't commit; the review does that. Record anything unexpected under
"Progress", even if it was dealt with.

## Phase 6 — draws, textures and render targets

Written to be picked up by an agent that has not seen the work so far. It is
the largest phase: the equivalent of most of `port/linux/src/d3d8_gl.c` on
deko3d. Read first, in this order: "Decisions", "Where the renderer lives",
phases 0 to 5 and their "Progress" entries; then `d3d8_gl.c` whole (about
4,100 lines: it is the specification of what the Xbox's Direct3D does, as
this port has debugged it against the game), `port/linux/src/xbox_textures.c`,
`port/switch/guest/d3d8_dk.c`, `guest/dk_commands.h`, `guest/dk_shaders.h`
and `.c`, `host/host_dk.c` and `host/host_dk_shaders.c`. The deko3d API is
`/opt/devkitpro/libnx/include/deko3d.h`; the probe
(`port/switch/probe/deko3d/source/main.cpp`) has a working draw (vertex
attributes, shaders, states, a render target, a readback) to copy from.

### What exists, what this phase adds

Exists: the deko3d image's device (`d3d8_dk.c`) keeps every piece of
Direct3D state the game sets, exactly as `d3d8_gl.c` does; a command stream
to the host, one handing-over a frame or sooner (`dk_commands.h`:
targets, clear, present); the host's device, queue, frames behind fences,
render targets by address, swapchain (`host_dk.c`); the game's memory
window GPU-mapped, `window_read` (a range's GPU address, cleaned from the
CPU's cache) and `upload_copy` (a per-frame upload buffer) in `host_dk.c`,
waiting for draws; per-resource busy tracking through `D3DResource.Lock`
(`resource_used`, `draw_resources_used` in `d3d8_dk.c`, waiting for draws);
GLSL generators (`nv2a_dk_*`) and the shader cache, with
`dk_shader_for_draw` (a key in, a handle or "not ready" out).

Adds: draws (indexed, not, immediate mode, quads), the draw state, vertex
constants and the other uniforms, textures, samplers, render-to-texture and
the mip composite, visibility tests, the high-res HUD and text, and the
menus' art. At the end the game looks under deko3d as it does under OpenGL,
the menus and a map; phases 7 to 10 then take work off the game thread.

### The order, each step tried on the console before the next

1. **First triangle, and the picture's orientation.** The menus' simplest
   draws: immediate mode (`D3DDevice_Begin`/`End`, which `d3d8_dk.c`
   records into `device.immediate_vertices`), no texture, vertex colour.
   This settles the one convention phase 4 left open: whether the device
   flags `DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft` (in
   `host_dk.c`'s `initialize`) give the right way up with the generated
   shaders, which follow desktop OpenGL's `glClipControl(GL_UPPER_LEFT,
   GL_ZERO_TO_ONE)` conventions. If the picture is upside down, change the
   device flag (`DkDeviceFlags_YAxisPointsDown`), not the shaders.
2. **The menus.** Textures (2D, the formats the menus use), samplers,
   blending, the alpha test, `D3DPT_QUADLIST`. The PC menus
   (`display.menus = "pc"`) and the Xbox ones both.
3. **A map.** Indexed draws from the window (`window_read`), vertex
   declarations and every attribute type, the depth and stencil states, z
   bias, fog, cube and 3D textures, the remaining texture formats.
4. **Render targets.** Render-to-texture, the mip composite (the water's
   ripples), the screen's scale, split screen (viewports and the scissor
   following them).
5. **Visibility tests** (lens flares).
6. **The high-res HUD and text, and the menus' art** (`hud_hires.c`,
   `text_hires.c`, `menu_files.c`). Done ahead of steps 4 and 5: see
   "Progress".

### Step 3 in detail: a map

The menus (step 2) are a few hundred draws of one kind; a map is thousands
a frame, of every kind the game has - the BSP's lightmapped geometry, models
with skinning, transparent effects, decals, the sky, the HUD - read from the
map's data in the window. Most of the path exists (step 1 built indexed
draws, the vertex format and every state; step 2 textures). This step is
about what a map adds, and about the limits a busy frame meets.

**Where to test.** Two scenes, and the same two under `renderer = "gl"`
for comparison (by eye until screenshots work - see the last point):
- **Blood Gulch**, in a local multiplayer game (no network needed): open
  terrain, the sky, vehicles, fog in the distance, grass (detail objects).
- **The Silent Cartographer** or **The Pillar of Autumn**, early in the level:
  interiors, many lights and decals, AI characters (skinned models),
  transparent effects (shields, plasma).
For each, the log's draw statistics (`gpu_stats`, every 60 frames) and the
`fps` and `game thread` lines, against the OpenGL image's in the same place.

**What a map adds, and what to check.**

1. **Culling and winding.** The menus barely cull; a map culls everything.
   If geometry is missing or seen from inside (back faces drawn instead of
   front), the winding convention under `DkDeviceFlags_OriginUpperLeft`
   differs from desktop OpenGL's under `glClipControl` - the cull mapping
   copies `d3d8_gl.c`'s desktop path (`front_face_ccw`, `cull` in
   `draw_state_make`, mapped in `host_dk.c`'s `state_apply`). The fix, if
   needed, is to invert the front face there (one line, as `d3d8_gl.c`'s
   Android branch does for its own flip), not the shaders.
2. **Depth, stencil, z bias.** Depth testing and writing (most of the map),
   the depth range from the viewport, stencil (the game uses it for some
   effects), and `SetRenderState_ZBias` as a polygon offset (decals - bullet
   holes, scorch marks - flicker or vanish if it is wrong; `d3d8_gl.c`'s
   comment on `D3DDevice_SetRenderState_ZBias` explains the slope term).
   Check `dkCmdBufSetDepthBias`'s units against glPolygonOffset's if decals
   misbehave.
3. **Every vertex attribute type.** Maps use what the menus do not: packed
   normals (`NORMPACKED3`, unpacked in the shader from a `1x32 Uint`
   attribute), normalized shorts (texture coordinates, `NORMSHORT2`),
   skinning weights and indices. A model with garbled or exploded geometry
   points at an attribute's format or offset (`format_receive`,
   `attribute_format_make`).
4. **Index and vertex data from the window**, through `window_read`. A range
   crossing from one 16 MB chunk of the window into the next is copied into
   the upload buffer instead (`range_read`); index data made by
   `CreateIndexBuffer` is in ordinary guest memory, and copied too. Watch the
   log for "a frame needs more than ... bytes of uploads": the upload slice
   is 4 MB a frame (`UPLOAD_SLICE_SIZE`), and a draw whose data does not fit
   is skipped.
5. **Command memory: a full frame ends the program.** The host records a
   frame's commands into one 8 MB slice (`COMMAND_MEMORY_SIZE`), every
   mid-frame submission continuing in it, and when it is full
   `command_memory_exhausted` calls `host_fatal`. A map frame of thousands
   of draws, many with vertex constants (skinned models change dozens of
   registers a draw; up to 3 KB pushed inline each), may get there. Before
   testing in a busy scene, make it safe: when the slice is near full,
   submit what is recorded and continue in the next slice (waiting for its
   fence), or give the command buffer more memory through `cbAddMem`
   instead of failing - and log the frame's peak use, so the size can be
   chosen from what maps really take.
6. **Dynamic geometry.** Vertex buffers the game rewrites every frame:
   detail objects (grass; their buffer is locked and rebuilt each frame -
   the lock waits for the GPU through `Lock`, phase 3), contrails and
   lightning (which draw from an offset into one buffer, through
   `SetIndices`' base vertex), particles. Stale or flickering effects point
   at the busy tracking (`draw_resources_used`, `halo_resource_wait`) or at
   `window_read`'s cleaning.
7. **The remaining texture formats and kinds.** Cube maps (environment
   reflections: `DK_TEXTURE_CUBE`, six faces), 3D textures, palettized
   textures (`P8`, with the stage's palette), bump maps with signed channels
   (`D3DTSS_COLORSIGN`, handled in the pixel shader), linear (unswizzled)
   textures with their coordinate scale (`texture_scale`), and every format
   `xbox_textures_dk.c` decodes. A texture that is all black or noise points
   at its format; `debug.texture_log` logs every upload.
8. **Fog.** Distance fog is in the pixel shader (`fog_parameters`,
   `fog_color`); Halo's atmospheric fog also uses a "fog screen" that clears
   only the back buffer's alpha (the clear's per-channel mask, already done)
   and blends with destination alpha.
9. **Expected, not this step's:** anything drawn through a render target -
   the water's reflections and ripples, the sniper scope, the HUD's motion
   tracker, some screen effects - is wrong until step 4; lens flares are
   missing until step 5 (the visibility tests answer 0); the high-res HUD and
   text are missing until step 6 (the map's own HUD bitmaps draw).
10. **Performance, measured, not tuned.** The game thread's ms a frame in
    each scene against the OpenGL image's. Two costs to watch, for later phases:
    each indexed draw scans its whole index list for its vertex range
    (`d3d8_gl.c` cached that), and a texture written after draws puts a full
    barrier first.
11. **Screenshots, if the comparisons need them.** They fault the GPU now
    (reading the hardware-compressed back buffer; see step 1's entry under
    "Progress"). If comparing by eye is not enough, rework them first, as a
    test of its own: make the back buffer (and the targets the game reads
    back) without `DkImageFlags_HwCompression`, or blit it into an
    uncompressed image the GPU renders into, and copy from that; check it in
    the menus before relying on it in a map.

**Done when:** both scenes draw as they do under OpenGL, apart from the
render-target, lens-flare and high-res items above; nothing ends the
program in a busy frame; the draw statistics show nothing skipped but for a
shader still compiling; and the game thread's time a frame in both scenes,
against OpenGL's, is under "Progress".

### Where the work goes: guest or host

The rule from "Where the renderer lives": the guest reads Direct3D's state
and decides; the host only records deko3d commands. So the guest builds, at
each draw, a compact description in the command stream, and the host turns
it into deko3d calls with as little logic as possible. What crosses is
fixed-width (`dk_commands.h`'s rules: 32-bit fields, guest addresses).

New commands (add to `dk_commands.h`; keep each small, and send only what
changed since the last draw - most consecutive draws share most state):

- **State**: raster (cull, front face, fill mode, polygon offset as
  `D3DDevice_SetRenderState_ZBias` computes it, point size), depth-stencil
  (test, write, function, stencil function, reference, masks, operations),
  blend (enable, factors, equation, constant colour), colour write mask,
  viewport (in the target's pixels, as `apply_raster_state` computes it)
  and the scissor (which follows the viewport: the NV2A's scissor register
  defaults to it, and split screen depends on that). The Xbox's enumerants
  are OpenGL's (`D3DBLEND_*`, `D3DCMP_*`, stencil ops - `d3d8_gl.c` passes
  them to GL as they are); map them to `DkBlendFactor`, `DkCompareOp`,
  `DkStencilOp` in the guest or the host, once, in a table.
- **Shaders**: the vertex and pixel shader handles from
  `dk_shader_for_draw` (`guest/dk_shaders.c`). The keys are built as
  `prepare_draw` builds `nv2a_pixel_shader_key` and as the vertex program
  and its declaration's packed mask make the vertex key (`struct
  dk_vertex_key`: the program's hash and the mask; immediate mode uses
  mask 0). A handle of 0 means "not ready": skip the draw, as decided.
- **Uniforms**: the vertex constants (`vertex_constants`, 192 vec4) change a
  few registers at a time; `d3d8_dk.c` already tracks which (the constant
  serials and log copied from `d3d8_gl.c`). Send the changed ranges, and let
  the host keep one uniform buffer per frame slot and update it with
  `dkCmdBufPushConstants` (it writes into the buffer inline, ordered with
  the draws). `vertex_parameters` and `pixel_parameters` (`dk_shaders.h`'s
  structs, 48 and 528 bytes) are built in the guest as `prepare_draw` builds
  them and pushed whole when they change. Bind each at its binding
  (`DK_BINDING_*`) with `dkCmdBufBindUniformBuffer`.
- **Vertex input**: per stream, either a window address (the host calls
  `window_read`, which cleans the CPU cache for it and gives the GPU address)
  or data the guest could not leave in place (immediate mode, a range
  crossing two window chunks, CreateIndexBuffer's index data in ordinary
  memory) for `upload_copy`; the stride; and per attribute its register,
  stream, offset and type. Map `D3DVSDT_*` to `DkVtxAttribSize`/`Type` (as
  `attribute_format` in `d3d8_gl.c` maps them to GL): `D3DCOLOR` is
  `DkVtxAttribSize_4x8`, `DkVtxAttribType_Unorm`, with `isBgra` set;
  `NORMPACKED3` is `1x32` `Uint` (the shader unpacks it); `FLOAT2H` reads
  three floats. **An attribute the declaration does not feed** reads the
  current value `D3DDevice_SetVertexData*` set (`device.attributes`): give
  it a stream of stride 0 holding that value, in the upload buffer (phase 4
  decided the shaders declare all sixteen inputs and do not know which are
  fed).
- **The draw**: primitive (`DkPrimitive_Quads` exists, so no quad index
  lists), counts, the index buffer's address, and the base vertex
  (`dkCmdBufDrawIndexed`'s `vertexOffset`, from `SetIndices`'
  `base_vertex_index` - see the comment in `d3d8_gl.c`'s device struct).

After writing a draw's command, call `draw_resources_used` (`d3d8_dk.c`) so
every resource the draw reads carries the submission's number in its `Lock`
- after, not before: writing a command can hand the stream over (see
`resource_used`'s comment).

### Textures

`xbox_textures.c` decodes the Xbox's formats (swizzled, palettized,
compressed) and keeps a cache keyed by the texture's address, invalidated
through `memory_watch` generations. Under deko3d the guest still decodes -
it is CPU work, and the decoders are there - but the upload goes to the
host:

- Copy `xbox_textures.c` to `guest/xbox_textures_dk.c` (the original is
  compiled into every platform; do not edit it), keep its decoding and
  cache, and replace its GL upload with a command that hands the host the
  decoded texels (a guest address and size, read by the host into the
  upload buffer, then `dkCmdBufCopyBufferToImage` into a `DkImage` the host
  makes). BC1-BC3 (`DkImageFormat_RGBA_BC1` and so on) and BGRA
  (`DkImageFormat_BGRA8_Unorm`) are native, so compressed textures need no
  decode at all; the swizzled ones need unswizzling, which the decoder
  does.
- **Staleness**: under deko3d nothing is page-protected (phase 3), and on
  the code-memory firmware the window's protection cannot be changed at
  all. What tells a cached texture it was rewritten is the announced writes
  (`memory_watch_prepare_write`, called by the locks and the file reads).
  Check that the deko3d image keeps those generations working
  (`host_memory_watch_prepare_write` returns early unless the watch is
  active; `d3d8_gl.c`'s `gl_initialize` starts it with
  `memory_watch_initialize`, which `d3d8_dk.c` never calls).
- A texture whose data is a render target's (`xgpu_render_target_find`, by
  address) samples the render target's image directly, as `bind_textures`
  does; linear textures have their coordinates scaled (`texture_scale`).
- **Samplers and descriptors**: deko3d binds textures through image and
  sampler descriptor sets in GPU memory (`dkCmdBufBindImageDescriptorSet`,
  `dkCmdBufBindSamplerDescriptorSet`, `dkMakeTextureHandle`). Keep one image
  descriptor per image the host makes and a sampler descriptor per distinct
  sampler state (`configure_sampler` in `d3d8_gl.c` lists the inputs:
  filters, address modes, LOD bias - into `DkSampler.lodBias`, which is why
  phase 4 dropped the shaders' bias - the maximum mip level, anisotropy,
  border colour). Changing descriptors in use needs
  `DkInvalidateFlags_Descriptors`; append, don't rewrite in place.

### Render targets, the mip composite, the screen

`host_dk.c` already makes a render target's image by its address
(`target_get`). Sampling one needs it made sampleable (check
`DkImageFlags` and the hardware compression flag on images that are both
rendered to and sampled). The mip composite (`mip_composite_get` in
`d3d8_gl.c`: the water renders a texture one mip level at a time, each a
surface) becomes image-to-image copies into a mipmapped image
(`dkCmdBufCopyImage`). A barrier (`dkCmdBufBarrier(..., DkBarrier_Fragments,
DkInvalidateFlags_Image)`) is needed between rendering into an image and
sampling it.

### Visibility tests

`DkCounter_SamplesPassed`: report the counter into a small result buffer at
`BeginVisibilityTest` and again at `EndVisibilityTest`
(`dkCmdBufReportCounter`); the result is the difference. The game reads
results a frame later (`GetVisibilityTestResult`), so the host keeps them in
CPU-readable memory and the guest asks through an import (or the host
writes them into guest memory); return the latest finished, never wait (see
`d3d8_gl.c`'s query-buffer comments). Divide by the target's scale as
`visibility_unscaled` does.

### The high-res HUD and text, the menus' art

`hud_hires.c` (`hud_hires_png_texture`), `text_hires.c`
(`text_hires_atlas_texture`) and `menu_files.c` (`halo_menus_art_register`)
make GL textures and hand back GL names that `xbox_textures.c` binds. Under
deko3d their GL calls do nothing. Give the deko3d image versions that make
host images instead (copies under `port/switch/guest/`, linked in its
place - see how `d3d8_dk.c` replaces `d3d8_gl.c` in `tools/switch_build.py`;
a function only some other file calls can be replaced by linking a copy of
its file instead of the original).

### Pitfalls already known

- **deko3d ends the program on any failure it reports**, in release builds
  too (phase 0). Check sizes and limits before asking; never ask it for a
  memory block the console might refuse.
- **The queue keeps its state between command lists**: a pass that does not
  set its viewport, scissor and every state it depends on inherits the last
  ones (phase 0, the probe's red square).
- **Only the game thread touches deko3d** (`host_dk.c`'s rule; the shader
  service follows it).
- **Every range a draw reads from the window goes through `window_read`**,
  or the GPU reads stale data the CPU has not written back (phase 3).
- **A draw's resources get `Lock` after its command is written**, or IsBusy
  and the locks wait for the wrong submission (phase 3).
- **Direct3D's half-pixel**: the generated vertex shaders add 0.5; do not
  add it again in the viewport.
- **The game's Xbox enumerants are OpenGL's**: compare `d3d8_gl.c`, which
  passes many of them straight to GL, before writing a mapping table.

### Testing on the console

Deploy with `python3 tools/switch_deploy.py`; the user runs the game and says
when; logs are `/switch/halo/halo.log` and `/switch/halo/debug.txt` (read
both); crash reports in `/atmosphere/crash_reports` (symbolize against
`build/switch/halo.elf`). Screenshots: `debug.screenshot_every` and
`debug.screenshot_directory` in `config.toml` make the OpenGL renderer
write BMPs; give the deko3d renderer the same (read the back buffer's image
into a buffer with `dkCmdBufCopyImageToBuffer`, as the probe reads its
pixel), so a step can be checked from the PC: the same scene under
`renderer = "gl"` and `"deko3d"`, compared. Each step above is its own
test; say which scene to go to and what to look for.

### Acceptance

1. `ninja switch` builds with no new warnings; the OpenGL image and the
   other platforms are unchanged (no edits to `port/linux/src` except
   behind `#ifdef HALO_SWITCH`, argued for under "Progress").
2. Each of the six steps shown working on the console, with screenshots
   compared against the OpenGL renderer's for the menus, a map (Blood Gulch
   or the Silent Cartographer), a scene with water, split screen and a lens
   flare.
3. The game thread's time a frame in the menus and in a map, from the
   host's log, against the OpenGL image's in the same places (phase 1:
   5.8 ms against 0.6 ms in the menus with nothing drawn).
4. Nothing the game draws is missing for longer than its shader's compile
   (a few frames) on a console with a warm cache.
5. `DEKO3D.md`'s "Progress" has a "Phase 6" entry: what was built, each
   step's result, and anything unexpected.

Don't commit; the review does that.

## The game thread

With phase 6 the game plays under deko3d, and much faster than over Mesa (the
comparison phase the plan first had here was dropped: the user's verdict was
enough). What limits it now is the game thread. In a busy online match
(34 players, about 1,950 draws a frame) it was busy 90-96% of the time at
25-39 ms a frame; in the single-player maps it is 5-6 ms. The Switch gives a
homebrew program three cores; the game thread keeps one to itself, and the
other two carry the audio mixer, the shader compiler, the vertical blank
thread and little else. Phases 7 to 10 move work to them, measured first and
in the order the measurements give.

What cannot move: the game's own simulation (AI, physics, objects, scripts,
the network game). It is single-threaded code that assumes nothing else
touches its state, and nothing here changes that.

## Phase 7 — the game thread, measured

### Step 1: the key file written off the game thread (done in the tree)
Every key a draw met for the first time was appended to `console.dkk` on the
game thread - two opens and a write on the card each - and a first match on
a new map hitched for its first minute (frames of 200-400 ms with the game
thread mostly waiting: about 250 opens in the slow stretch, none after). The
draw now queues the record; a writer thread appends what has queued in one
open, at most once a second (`console_append`, `console_writer` in
`dk_shaders.c`). A queue that fills (the import at the first start) is
written by whoever fills it.

### Step 2: a sampling profiler (done in the tree)
`debug.profiler = true` (`host_debug.c`; `debug.profile_hz` the rate, 500
by default): a thread that, that many times a second,
pauses each guest thread (`svcSetThreadActivity`), reads its registers
(`svcGetThreadContext3`, which needs no debugger) and its frame chain, lets
it go on, and counts each stack; every 20 s the counts go to
`/switch/halo/profile/profile_NNN.txt`, written by the profiler's thread.
While a thread is paused the profiler takes no lock and allocates nothing;
the frame walk stays inside the stack's own memory region.
`tools/switch_profile.py` fetches the files, symbolizes them against the
build (the guest's image as linked, the host's by an anchor function's
address, since the host moves from run to run) and prints self and inclusive
time per function, per thread, and collapsed stacks for a flame graph.

### Step 3: a profile of a busy match
A busy online match, two minutes, the profiler at 500 Hz. The game thread's time
split: the game's own simulation and network game; the renderer's guest half
(`draw_prepare`, keys, `dk_texture_get` and its decoding); the host half
(`host_dk_submit` and below: recording, `window_read`'s cache cleans,
staging); sound (`sound_idle`, `sound_render`, obstruction); waits (files,
locks, the GPU).

**Acceptance:** that split in "Progress", with the profiler's own cost (the
frame time with it off and on), and the order phases 8 to 10 are done in.

## Phase 8 — a render thread for the host half

`host_dk_submit` processes the command stream inside the guest's call, on
the game thread: every draw's deko3d recording, the cache cleans, the
texture staging. Instead, the stream is handed to a render thread on a
helper core and the call returns.

### What has to change
- **The rule about guest memory** ("Where the renderer lives"): the host
  reads what a command names when it processes it. Processed later, a
  command may name memory the game has rewritten since. Each kind of
  command's data is sorted into: read by the GPU in place under the
  `Lock` busy tracking (vertex and index buffers in the window, compressed
  textures - unchanged); and guest memory that is reused at once (the
  immediate-mode vertices, the quad list's indices, `device.immediate_*`,
  decoded texels, the rows of the high-res images), which is copied into
  the hand-over's own memory, or kept alive until the render thread is done
  with it.
- **The guest's frees after hand-over** (`dk_stream_free_after_handover`)
  wait for the render thread to have processed that stream, not for the call
  to return.
- **The submission numbers**: the guest numbers a hand-over when it hands it
  over, as now; `host_dk_retired` answers from the fences as now, and a
  busy check of the submission being written hands it over as now. A
  hand-over not yet processed is a submission not yet retired, which is
  already what the numbers say.
- **Two streams in flight at most**: the guest writes the next while the
  render thread processes the last; a third waits.
- **What else touches the host's deko3d state from the game thread**: the
  shader service's tables (`host_dk_shader`), the window chunks
  (`chunks_map`), the first submission's `initialize`, the screenshot's
  wait. Each goes behind the render thread or a lock.

### Steps, each tried on the console
1. The render thread, processing exactly what the call processed, with the
   game thread waiting for it after each hand-over (no overlap yet): the
   plumbing, proved by the game looking and running as before.
2. The copies and the deferred frees, then the overlap.
3. The busy match again, with the profiler.

**Acceptance:** the menus, a map, a busy match look as before; the game
thread's share of the host half (phase 7's profile) gone from it; the
frame time of the busy match in "Progress".

## Phase 9 — textures decoded on a worker

The texture cache decodes every non-compressed texture to BGRA on the game
thread the first time a draw uses it (`dk_texture_get`), which costs most at
a map's start and on first sight of new areas. A worker thread decodes
instead; a draw whose texture is not decoded yet draws with the dummy, the
rule shaders already follow. Done if phase 7's profile shows the decoding,
or the load hitches, worth it.

## Phase 10 — what the profile ranks next

Decided by phase 7's profile, one at a time: file reads the game waits for
during play (the sound and texture caches stream from the map file), read
ahead on another thread; sound obstruction (`compute_sound_obstruction`,
a collision test per sound source per frame), tested less often rather
than threaded; and whatever else the profile shows.

## Phase 11 — collecting keys

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
| Behaviour of Mesa that the GL renderer relies on without saying so | found by play, the parity phase having been dropped |
| A render thread reading guest memory the game has rewritten | phase 8 |
| Two copies of the shared code drifting apart until they are merged | ongoing |

---

## Progress

### Phase 7 (under way)

**After the measurements: the game thread's waits gone; a match held at 60.**
The GPU, timed (timestamps around each submission, `gpu_timing_*` in
`host_dk.c`, logged every 60 frames): 5-8 ms a frame on average in a
match, 9-12 ms the longest - far from the 16.7 a frame has. So the limit
was the CPU and its waits, and these went in, measured together on the same
server's match as the profile before them:

- The host's log written by a thread of its own (`host_main.c`): a line was
  a synchronous write to the card, fsynced if a warning (every line of the
  guest's), on whatever thread logged it, under the lock every guest file
  call takes. Errors and worse are still written and synced where they are
  logged, after what is buffered.
- The draws' comparisons of what the host was told made a word at a time
  (`words_equal` in `d3d8_dk.c`): musl's memcmp goes byte by byte (7.3%).
- `dk_shader_for_draw` keeps each stage's last key and handle: draws in a
  row mostly share them, and the copy and hash are a byte at a time (3.8%,
  now 1.9%).
- A texture entry keeps its menu art's answer until the art registered
  changes (`menu_art_serial` in `menu_files.c`), where every lookup of every
  draw searched the list (1.3%).
- A vertex buffer's ring of storage (`vertex_buffer_rename` in
  `d3d8_resources.c`, behind `HALO_SWITCH`): the first lock a frame of a
  dynamic vertex buffer waited for the GPU to finish the last frame's draws
  from it, which under deko3d (a frame submitted at its end) was most of the
  GPU's time on that frame. A lock that would wait now gives the buffer
  storage the GPU is done with, or a new one, up to four, and the GPU reads
  the old; the game writes every range it draws after that first lock. The
  storage's addresses go through `PLATFORM_*_TO_*` as `CreateVertexBuffer`'s
  do, so they follow the window wherever it is. 13% to 0.
- Three swapchain images, not two: a frame that missed the display's
  refresh waited for the next one.
- Shaders on the card read and loaded by a loader thread
  (`loader_thread` in `host_dk_shaders.c`), the draws skipped meanwhile as
  for one compiling (about 3%, now 0).

The match: 60 frames a second almost throughout (one stretch at 57), where
the one before dipped to 50-58; the game thread at 8-14 ms a frame. In the
profile (114-180 s, 29,468 samples) the vertex buffers' wait and the loads
are 0, the sprites 0.9% (14.3% before), and the wait for a swapchain image
25.6% - which at a steady 60 is the frame's slack, not a frame that missed.

The phases after 6 were redrawn here: the parity and performance phase
(once phase 7) was dropped, and phases 7 to 10 take work off the game
thread; collecting keys is now phase 11. Entries below that say "phase 7"
mean the old one.

**Steps 1 to 3: the profiler works, and the first profile of a match says
the game thread waits more than it works.** The SVCs are allowed. The first
run counted nothing: the thread bookkeeping (`current_thread_id`) passed
`0xFFFFFFFF` for the current thread where libnx's pseudo-handle is
`CUR_THREAD_HANDLE` (`0xFFFF8000`), so every thread was registered as id 0,
which the profiler takes for an empty slot; fixed.

An online match (not one of the 34-player ones; the game thread busy
63-94% at 11-23 ms a frame, 36-60 frames a second), the 40 seconds that
were heaviest, 19,645 samples of the game thread at 500 Hz:

| Where | Share |
|---|---|
| Waiting for a swapchain image (`present` → `dkQueueAcquireImage` → `nwindowDequeueBuffer`) | 16.5% |
| Waiting for the GPU in a lock: the first lock a frame of a dynamic vertex buffer (`_rasterizer_dynamic_vertices_lock` → `halo_resource_wait`, sleeping 500 µs at a time; `build_sprite` and `flag_render_proper`) | 11.0% |
| The game tick (`game_time_update`: objects, physics, the network game) | about 11% |
| `draw_prepare` and below (keys, textures, state) | 11.6% |
| `memcmp` called from the draws (most from `_rasterizer_decals_draw` → `DrawVertices`) | 7.3% |
| `dk_shader_for_draw` itself | 3.8% |
| The host's log, written to the card on the game thread (`__android_log_write` → `fsdev_write`) | 2.8% |
| `recv` on the network socket | 2.4% |
| `menu_art_name`, looked up for every texture of every draw (step 6's code) | 1.3% |
| The host half without the acquire (`host_dk_submit` 22.3% less 16.5%) | about 6% |

What it changes: the two waits, a quarter of the game thread, are waits for
the GPU or the display. If the GPU is what limits a heavy frame, taking
CPU work off the game thread (phase 8's render thread, worth about 6%)
moves the wait rather than the frame rate. So next: the GPU's time a frame,
measured (timestamps around each submission, `DkCounter_Timestamp`), beside
the fps line. Cheap CPU work regardless: the log written by a thread of its
own, `menu_art_name` not searched for every texture, and the `memcmp` in the
draws found and avoided. If the GPU has room to spare, the dynamic vertex
buffers' wait goes by giving each a ring of copies (a lock that would wait
takes the next copy instead, as `D3DLOCK_DISCARD` does elsewhere).

### Phase 6 (under way)

Rules kept: no edit to the OpenGL renderer; under `port/linux/src`, only
read-only accessors behind `#ifdef HALO_SWITCH` (step 6, below), which the
OpenGL image links but does not call.

**Step 5, visibility tests: done and seen on the console - lens flares
show.** A test's begin resets the GPU's samples-passed counter
(`DK_COMMAND_VISIBILITY_BEGIN`, `dkCmdBufResetCounter`); its end has the GPU
write the count to the test's slot of a 4096-slot report buffer, 16 bytes
each, CPU-uncached (`DK_COMMAND_VISIBILITY_END`, `dkCmdBufReportCounter`).
`D3DDevice_GetVisibilityTestResult` reads a slot's latest count through a
new import, `host_dk_visibility` - from the latest test, or while the GPU is
behind an earlier one, as the query buffer of `d3d8_gl.c`'s desktop path
gives - so the game never waits for the GPU. deko3d enables the sample
counter when it sets up the queue (`SampleCounterEnable`), so nothing else
is needed. The count needs no scaling: the screen is drawn at the game's
pixels.

**Step 4, render targets: done and seen on the console, but for split
screen.** The mip composite: a stage whose texture is a target rendered a
level at a time (not linear, not a cube, more than one level, its top level
the size of the target drawn there - `bind_textures`'s test) sends
`DK_COMMAND_COMPOSITE` (the levels' addresses, by
`xgpu_texture_level_offset`) once, and samples it with
`dk_stage_texture.composite` set to its level count. The host
(`composite_sampled`) keeps an RGBA8 image with every level; when a level's
target has been bound since the last copy (`target.bound` against the
composite's stamp), it copies each level the game drew with the 2D engine
(`dkCmdBufBlitImage`) and makes the ones below the last drawn by halving
(linear blits), between a fragment barrier and a full barrier with an image
invalidate; a full barrier comes before a copied target is drawn into again
(`tex.targets_copied`). It copies only after a redraw, where GL copies at
every bind. The water's ripples look right. The screen's scale has nothing
to match: on the Switch `d3d8_gl.c` takes the Android path, scale 1, 852x480
drawn as it is (drawing at 720p would be an improvement on GL, not parity).
Split screen needs no new code - the draw state's scissor is the viewport
and clears are clipped to it, as in GL, from step 3 - but has not been tried
on the console.

**Step 4, render-to-texture: done and seen on the console.** The
sniper's zoom went black: it draws into a texture and draws that texture
back, and the texture cache read the texture's address in the game's memory,
where the GPU never writes. Now `d3d8_dk.c` notes every color surface it
binds as a target (`rendered_note`, `rendered_find`: data and the size last
drawn at), and a stage whose texture's data is one sends that address
(`dk_stage_texture.target`, id 0) instead of an image, with one level and
the linear-texture scale from the target's size, as `bind_textures` in
`d3d8_gl.c` does. The host (`target_sampled` in `host_dk.c`) takes, of the
color targets at that address, the one bound last (`target.bound`, from
`dk.target_clock`) - `xgpu_render_target_find`'s choice - gives it an image
descriptor the first time, and puts a fragment barrier with an image
invalidate between the draws into a target and the first draw that samples
it (`target.drawn`, set when it is bound); a target sampled since the last
barrier gets one before the targets change again
(`textures_targets_changing`). The zoom works. Still to do: the mip
composite (a texture the game renders a level at a time, the water's ripple
map - the water looks "a little weird but not super wrong": one level is
sampled, the levels below it are not made), the screen's scale and split
screen.

**Step 3, a map: run on the console - the game plays.** The first level
load stopped silently: no present, no fault. A two-second report on every
wait (`fence_wait_said` on the host's frame and rollover waits; one in
`halo_resource_wait`; a queue-error check after every submission) named it at
once: "a lock of 0x203bd800 has waited two seconds for submission 153684298
(409 retired, 410 being written)". A resource whose header comes from the
map file keeps what the file has in `Lock` - the game zeroes it only for the
headers it makes - and `halo_resource_busy` read that as a submission still
to come. A `Lock` past the submission being written is now not this
device's: not busy, and set to 0 (one at or below it waits, at most, until
the GPU passes it). The reports stay in. With that the map plays: 58-60
frames a second, the game thread at 5.6-5.9 ms a frame (34-35% of the time),
the most command memory a frame used 1 MB (one piece) and no rollover, some
draws skipped for shaders not ready on first sight (tens a second at most,
compiled in the background), no GPU fault. The rollover is therefore still
untried on the console; the 8 MB is ample for this map.

**Step 6, the high-res HUD and text and the menus' art: run on the console -
the PC menus work.** Pulled ahead of steps 4 and 5, since the PC menus
(`display.menus = "pc"`) cannot be read without them. `xbox_textures_dk.c`
makes the three replacements `texture_entry_result` in `xbox_textures.c`
makes, as images of its own from the same id allocator: the text's atlas
(2048 square, white with the coverage as alpha; after the first upload only
the rows written since are sent), a menu's art (by file name, kept) and a
high-res HUD bitmap (found by `hud_hires_override_find` when an entry is
uploaded, as in GL; kept). PNGs are decoded by `hud_hires_png_pixels`, their
mip levels made by halving (a 2x2 mean), red and blue swapped to BGRA. The
texels go as a new command, `DK_COMMAND_TEXTURE_ROWS` (rows of one level of
a 2D BGRA image), after a `DK_COMMAND_TEXTURE` with no source that only
makes the image, in pieces of at most 1 MB so that one seldom straddles two
window chunks (which sends it through the 8 MB staging slice). `sampler_make`
takes `hires` as `configure_sampler` does: linear, from the mip levels, no
bias. The accessors, behind `HALO_SWITCH`: `text_hires_atlas_rows`
(`text_hires.c`), `hud_hires_png_pixels` (`hud_hires.c`), `menu_art_name`
and `menu_art_png` (`menu_files.c`).

**Step 3, a map: command memory made safe.**
A frame that fills its 8 MB of command memory no longer ends the program
(`host_dk.c`). The frame's region is given to the command buffer in 1 MB
pieces (`command_piece_add`, so the host knows how much a frame has used);
when the last piece is given, `host_dk_submit` - between two commands, where a
command's few KB cannot overrun the megabyte left - submits what is recorded,
waits for the GPU to finish it and records on from the start of the region
(`command_memory_roll`: a stall, said once in the log as a warning, counted).
Every 60 presented frames the log says the most a frame used (to the piece)
and the rollovers so far: "deko3d: command memory, the most a frame used in
the last 60: N KB of 8192 KB (R rollovers in all)" - the size is to be chosen
from that. The callback still ends the program, but only if one command asks
for more than a piece. A submission in the middle of a stream (the roll, and
`staging_copy`'s, which had the same flaw) now fences with the serial *before*
the stream's (`commands_submit_partial`): the guest's busy checks and locks
read a fence as "every draw of that serial is finished", and the stream's
later draws were not yet recorded.

**Step 1, first triangle: run on the console - the draw path works; the
picture's orientation is still to be seen (step 2).** In the menus, per 60
frames: 60 immediate-mode draws drawn (one a frame), 0 other draws, 7,440
skipped as textured (124 a frame), none skipped for a shader not ready or
anything else, 60 frames a second, no GPU fault. The menus are almost all
textured, so the screen stays black until step 2; the one untextured draw a
frame is presumably a dark full-screen layer (a fade). It went through
shaders from the cache, state, vertex data and uniforms with no fault - that
is what step 1 could show. Whether the device flags give the right way up
can only be judged when textured draws appear.

**Step 2, textures: run on the console - the first picture.** The menus'
background and the 3D Halo ring with its textures appear, and the user saw
them render "at amazing speed". From the log, in the menus, every 60
frames: 420 draws and 5,900-6,600 immediate-mode draws (about 7 and 105 a
frame), nothing skipped, no GPU fault, no texture error; 60 frames a second;
**the game thread at 1.2-1.3 ms a frame (7-8% of the time), against 5.8 ms
(35%) for the OpenGL image in the menus** - about four and a half times less,
before any of phase 7's work. The picture is the right way up with the
device flags as they are (`DkDeviceFlags_OriginUpperLeft`, no
`YAxisPointsDown`): step 1's open question is settled.

Built: `guest/xbox_textures_dk.c` (a copy of `xbox_textures.c`, in the
deko3d image only: its decoders and cache, the texels sent to the host -
BC1 to BC3 as they are, read where the game keeps them, the rest decoded to
BGRA), `guest/dk_textures.h`, the texture, texture-free and stage commands,
and in the host a table of images, staging uploads, image and sampler
descriptor sets, deferred release by submission, and an opaque-black dummy
for an empty stage. Fixed on the console: the dummy's four bytes, which are
the host's own, went through the command's 32-bit guest-address field, cut
short, and the first textured draw faulted; `texture_write` takes host texels
apart now.

Still expected, for later steps: textures whose data is a render target's
sample stale game memory (step 4); the high-res HUD, text and the menus' art
draw nothing (step 6); a cached texture is refreshed only by announced writes
(locks, file reads), as the OpenGL renderer's on the Switch. For phase 7: a
texture written in place after draws puts a full barrier first.

Found on the console, and fixed or worked around:
- **A screenshot faulted the GPU.** With `debug.screenshot_every` set (the
  console's `config.toml` had 300 from this phase's testing, so frame 0 took
  one), the frame never finished: first with `dkCmdBufCopyImageToBuffer` from
  the back buffer, then with a blit of it into a pitch-linear image in
  CPU-visible memory. Without screenshots, no fault. The back buffer is made
  with `DkImageFlags_HwCompression` (`target_get`); reading a compressed
  render target this way is the likely cause, not proved. The readback now
  waits two seconds at most and says so (`present`). **Rework before using
  screenshots again** - render targets without hardware compression, or a
  copy into an uncompressed image first - and keep `screenshot_every = 0`
  until then (it is 0 on the console now).
- **A faulted GPU ended the program later, with nothing said**: deko3d aborts
  at the next call needing the queue (`dkQueueAcquireImage`). `present` now
  checks `dkQueueIsInErrorState`, logs the frame once, stops presenting and
  discards what was recorded, so the game goes on and the log has the frame.
- **The unfed registers' values were reused across formats** in a frame (see
  the review note below); fixed.

- `guest/dk_commands.h`: the draw commands. The host *keeps* what it is told
  (`STATE`, `SHADERS`, `VERTEX_FORMAT`, the two parameter blocks), and the
  guest writes each only when it differs from what it last wrote, so a draw
  that changes nothing is one `DRAW` command. The Xbox's blend factors,
  compare functions and stencil operations cross as they are (they are
  OpenGL's) and the host maps them; the rest are neutral enums.
- `guest/d3d8_dk.c`: `draw_prepare` (`prepare_draw`'s key building, the state
  as `apply_raster_state` computes it, the changed vertex constants as one
  run, the other uniforms converted only when their inputs changed),
  `DrawVertices`, `DrawIndexedVertices` (the streams start at the lowest vertex
  the indices name, `vertex_offset` is minus that, as `d3d8_gl.c` does with
  `glDrawElementsBaseVertex`) and `End` (immediate mode: the vertices go in the
  command itself, because the next `Begin` reuses their buffer before the
  stream is handed over). A draw that needs a texture is skipped until step 2.
  Screenshots (`debug.screenshot_every`) and `debug.gpu_stats`.
- `host/host_dk.c`: one uniform buffer for every frame (`dkCmdBufPushConstants`
  runs in order with the draws, so a ring is not needed), the tables from the
  Xbox's enumerants to deko3d's, the state put into the queue at the next draw
  when a frame start, a clear or a change of targets has left it not as told,
  the vertex format as deko3d wants it (streams numbered densely; the
  unfed registers' current values are one more stream, of stride 0, in the
  upload buffer), and the draw. Ranges the draw reads go through `window_read`,
  or `upload_copy` when that gives nothing.
- `host/host_dk_shaders.c`: `host_dk_shader`, a handle's `DkShader`.
- Command memory per frame went from 4 to 8 MB (the constants are in the
  command stream).
- Review: the unfed registers' values (one stride-0 stream in the upload
  buffer) were copied once a frame and reused for every later format that
  frame, so a `SetVertexData` between draws - how the HUD and menus set a
  colour - would have drawn with the frame's first values; a new format now
  makes a new copy. Still to settle on the console: the winding under
  deko3d's upper-left origin (culling copies desktop OpenGL's, which relies
  on `glClipControl`; if a map's geometry vanishes in step 3, look there
  first), and the index range each indexed draw scans (no cache, unlike
  `d3d8_gl.c`'s: without the mirror there is no write generation to tell a
  cached range is stale - phase 7's question).

### Phase 5, steps 2 to 4

Built (by the agent that did step 1, finished and fixed in review):

- **UAM in the host** (`host/host_dk_compiler.cpp`, `tools/switch_build.py`):
  one object with UAM's library, its symbols renamed and all but
  `host_dk_compile_glsl` made local, as in the probe; a lock around it. The
  DKSH is written through `WriteDksh` (`uam.patch`) into a file the host
  opens, so the card's lock (`host_sd_lock`, which the game's file calls wait
  on) is held for the write only, not for the compile. Without meson, bison,
  flex or mako the host links `host/host_dk_compiler_stub.c` instead, which
  fails and says so once (checked: with mako hidden, configure notes it and
  plans the host's link with the stub). The NRO grew from 6.3 to 8.4 MB (as `tools/switch_deploy.py` reports it).
- **The shader service** (`host/host_dk_shaders.c`): the card's cache in
  `<data root>/shader_cache/<UAM commit>/`, listed once at start into a set
  (other UAM versions' folders removed); one compile thread, placed off the
  game thread's core (`host_thread_place_on_helper_core`, `host_thread.c`)
  one step below its priority, writing `.tmp` and renaming; a 16 MB code
  memory block; handles; the imports `host_dk_shader_find` (loads a shader
  on the card), `host_dk_shader_known` (says what the host has, without
  loading) and `host_dk_shader_compile`.
- **Keys** (`guest/dk_shaders.c`, `guest/dk_shaders.h`):
  `DK_SHADER_GENERATOR_VERSION` in every hash; a vertex program's hash
  computed in `D3DDevice_CreateVertexShader`; key files in
  `z:\shader_keys\` - `console.dkk`, this console's, and `keys.dkk`, a
  shared one: two fixed names, because the guest cannot list a folder
  (`getdents64` is not served on the Switch); the OpenGL records imported
  into `console.dkk` once; the startup pass at the 60th frame, eight keys a
  frame; a map's keys raised in the queue, and loaded, when it finishes
  loading (`d3d8_gl_map_loaded`); and `dk_shader_for_draw` for phase 6.

On the console (Horizon 21.2):

| Test | Result |
|---|---|
| First launch, empty cache | the 1,197 OpenGL records imported as 744 keys into `console.dkk` (193,452 bytes: 744 records of 260 bytes and a 12-byte header); all 744 compiled in the background (the files' times on the card span about a minute; that run's log was not kept - the version test below, compiling the same 744, took about 47 s) |
| Frame rate while compiling | 59.8-60 fps throughout, the game thread at 0.6-1.0 ms a frame (as with no compile); the compile thread on core 2, the game thread keeping core 1. The longest single frames rose to 42-58 ms while it compiled, from 22-30 ms after: an occasional one- or two-frame hitch, likely the card's lock |
| Second launch | 744 keys from `console.dkk`, 744 shaders on the card, the pass done in 1.6 s with all 744 ready and nothing compiled, at 60 fps; `console.dkk` unchanged |
| Generator version raised to 2 | every key hashed anew (first hash `f7a82e4248c3d12f6`, from `011925a88a8f14ad`), all 744 queued and compiled again in about 47 s, 0 failed; put back to 1 |

Fixed in review:
- **The known-shader set could hang the game.** Linear probing over 8,192
  slots with no bound: a full set probes for ever, at start, in the card's
  listing - and nothing removes the old names a generator change leaves, so
  about eleven version raises would have done it. The set now takes at most
  seven eighths of its slots and says once when it is full.
- **A cached file that would not load was read again at every find**, which a
  draw would make every frame: it is marked broken now (and compiled again)
  or, whole but with no room, unloadable (and not tried again).
- **A write that failed counted as done** (`ferror` and `fclose` were not
  checked), and a failed compile's `.tmp` was left on the card.
- **A shader being compiled answered "unknown"**, so a draw meeting it in
  that time would have queued it again; it answers "queued" now.
- **`dk_shader_for_draw`** ignored "queued", so until a shader was ready every
  draw needing it would have generated its GLSL, queued it and appended its
  key to `console.dkk` again, every frame; it raises the queued one now, and
  appends a key only the first time the console meets it. It also hashed a
  pixel key with its `count_samples` as given (the imported keys have it at
  0), and its handle cache was a 512-entry list searched in full on every
  draw past 512 shaders: a hash table now.
- **The startup pass loaded every shader on the card** (through `find`):
  four seconds of the menus at 39 frames a second, frames up to 96 ms. It
  asks `host_dk_shader_known` now, which loads nothing (1.6 s at 60 frames a
  second); a shader loads with its map, under the loading screen, or at its
  first draw. The pass also leaked the GLSL of every vertex key it checked
  (it generated it to see whether the program existed).

Not exercised yet, waiting for phase 6: `dk_shader_for_draw` (no draws), the
loading of a map's shaders at its load (no map was loaded in these runs:
the menus only), and broken or unloadable files in practice.

Unexplained: after the generator-version test the card held only the 744
version-1 files, not the 744 of version 2 as well, though the run's log
shows them compiled and renamed into place. The game deletes nothing in
that folder (only other UAM versions' folders); most likely they were
removed by hand when the test was put back. Nothing depends on it.

### Phase 5, step 1

Measured in the probe on the console, against phase 4's corpus (744 shaders,
`sdmc:/halo_dk_shaders`), each time including writing the DKSH to the card:

| | Time | DKSH files differing from the one-thread run's |
|---|---|---|
| phase 4: one thread, the front end set up and torn down every compile | 68.6 s | - |
| one thread, the front end initialised once and kept | **43.1 s** | - |
| two threads, on cores 0 and 1 (three runs) | 75.4, 76.7, 73.8 s | 18, 17, 19 |
| three threads, on cores 0, 1 and 2 (three runs) | 96.0, 96.2, 95.3 s | 14, 17, 17 |

**Decision: one compile thread, the front end kept.** Keeping it is the
gain - every compile used to rebuild Mesa's built-in functions, which
`glsl_frontend_exit` released - and more threads only cost: the workers
were on separate cores (the probe logs each worker's core), yet two were
slower than one and three slower still, most likely because Mesa takes its
global type table's lock (`glsl_type::hash_mutex`) all the time, and the
threads write to the card at once.

The differing files: only vertex shaders, and in each exactly **one byte**,
toggling within a pair of values (0x20 and 0x40, 0x84 and 0x88, 0xe1 and
0xe2), in both directions from file to file - not a pattern of corruption,
but of an equivalent choice (two registers, two slots) that depends on the
order of things in memory. Not proved (compiling the corpus twice on one
thread and comparing would tell), and moot with one thread.

What `uam.patch` now does, beyond phase 0's library target, and why:
- **The front end once**: `glsl_frontend_exit` does nothing, so Mesa's types
  and built-ins live for the program; the context is made the first time a
  thread compiles.
- **Real locks** in `mesa-imported/c11/threads.h` (it was fake: every lock
  did nothing), on pthreads. No lock in UAM is recursive.
- **Per-thread state**: the front end's `gl_context` (a `__thread` pointer to
  one allocated per thread), `tgsi_ureg.c`'s `error_tokens`, and
  `st_glsl_to_tgsi.cpp`'s `in_array` (a counter raised while an array
  constant is turned into code and read for every constant; shared, two
  compiles would put each other's constants in the wrong register file).
  The other writable statics found are debug switches and debug printing,
  never reached in a compile. Harmless with one thread, kept so that the
  compiler stays correct if anything ever calls it from two.

Found on the way:
- **Thread-local variables broke under meson's `-fPIC`.** meson compiles a
  static library with `-fPIC` after the cross file's `-fPIE`, and under it
  GCC reads a `__thread` variable in a way that assumes the thread pointer
  is in a register; devkitA64's `-mtp=soft` gets it from a call
  (`__aarch64_read_tp`) whose result overwrote the variable's offset, so the
  first compile read the thread pointer plus itself and faulted. UAM is now
  built with `-Db_staticpic=false` (`tools/switch_build.py`), which keeps
  `-fPIE` and local-exec access; the linked probe adds the right offset.
- **A changed `uam.patch` did not reach an existing clone**: `fetch_uam`
  skipped any clone marked patched, so `ninja switch-uam` rebuilt from stale
  sources. The marker now holds the patch's hash, and a clone made from
  another patch is cloned afresh.
- **A pthread made by libnx runs on the process's default core** and is
  never moved; the probe's workers each move onto their own core
  (`worker_place`), as the host's compile thread must (step 3).

### Phase 4

Built as the spec says: `guest/nv2a_vsh_dk.c` and `guest/nv2a_psh_dk.c`
(`nv2a_dk_vertex_shader_to_glsl`, `nv2a_dk_pixel_shader_to_glsl`), differing
from the OpenGL generators only in the dialect; `guest/dk_shaders.h` (the
bindings, the locations, and the blocks as structs of 3072, 48 and 528
bytes, checked at compile time); the dump in `d3d8_dk.c`
(`debug.gpu_dump_shaders`); `tools/dk_shader_check.py`; and the probe's
corpus pass.

The corpus, dumped by the deko3d image on a console from the game and from
the 1,197 program records of the OpenGL image's play
(`/switch/halo/save/z/shader_programs.bin`): **100 vertex shaders** - the
game's 67 programs, each as the immediate-mode variant (packed mask 0), and
33 more for the packed masks the records show - and **644 pixel shaders**,
one a distinct key.

**On the PC** (`uam` 1.1.0, `tools/dk_shader_check.py`): all 744 compile,
with no warnings. Before the console run, the 67 programs of the game's
microcode table were also put through the generator natively (each with no
attribute packed and with every one), and all 134 compiled.

**On the console** (the probe; each time includes writing the DKSH to the
card):

| | Shaders | Failed | Total | Average | Slowest |
|---|---|---|---|---|---|
| vertex | 100 | 0 | 14.0 s | 140 ms | 236 ms (`vs_32_0000000e`) |
| pixel | 644 | 0 | 54.6 s | 85 ms | 192 ms (`ps_788808af`) |
| both | 744 | 0 | 68.6 s | | |

What it means for phase 5:
- Compiling every known shader on a console with an empty cache takes
  about **70 seconds**, on one core. That is once a console (and again only
  when the generators or UAM change), but it is too long to sit through
  unexplained: the pass needs its progress screen, and is worth splitting
  over the cores the game does not use - if UAM's compiler can run on two
  threads at once, which nothing has tested (it is Mesa's GLSL compiler,
  with global state of its own). Compiling in the background while the game
  runs, skipping draws until their shaders are ready, is the other way, and
  the choice is the user's.
- A shader first met in play costs 85 to 236 ms: a few frames in which the
  draws that need it are skipped.

Found on the way:
- The game presents once while its rasterizer starts, before it makes its
  vertex shaders, so the dump runs a second in (`device.frame >= 60`), not
  at the first frame; the first dump had no vertex shaders.
- Writing a file from the guest logs `guest call (29) failed: errno 25` once
  a file: musl asks whether a new stream is a terminal. Harmless; a dump
  logs some 750 of them.
- The records hold keys that differ only in `count_samples` (the OpenGL
  image's occlusion counting), which make identical GLSL under two names;
  and pixel files are named by a 32-bit hash, so two keys with one hash
  would overwrite each other's file. Neither matters to the proof.

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

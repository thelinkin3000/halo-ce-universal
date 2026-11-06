# Porting GulchCE Dynamic Lighting to SlipstreamCE

Written 2026-11-06.

## Purpose and approach

Port GulchCE's dynamic lighting in layers, not as a merge: CPU light selection first, then world lighting, then model lighting per backend, and occlusion last behind its own toggle.

The source is GulchCE `main` at `4159d0d1`, cloned to `/home/carlo/projects/GulchCE`. GulchCE only built this for desktop and web OpenGL, and removed it from Android after reports of dark models and missing geometry. So this is a port to new renderers, and every phase has to prove itself on our devices.

Use this document as a runbook. Work through the phases in order, take only the commits and files each one names, and don't start a phase until the previous one meets its exit criteria.

## Target platforms and constraints

Two renderers need the work: Vulkan, which covers Android and PS5, and deko3d on the Switch. GulchCE's OpenGL model path can't be reused on either.

| Platform | Renderer | Files | Guest CPU | Effect on the port |
| --- | --- | --- | --- | --- |
| PS5 | Vulkan | `port/android/guest/d3d8_vk.c`, `nv2a_*_vk.c` | x32 (x86) | Should get the SSE2 visibility path; confirm it compiles. Most GPU headroom. |
| Android | Vulkan or GLES, by `display.renderer` | Same as PS5 | arm64_32 | Scalar visibility path only. |
| Switch | deko3d only (checked: `tools/switch_build.py` links no GL image, so there is no Mesa option) | `port/switch/guest/d3d8_dk.c`, `nv2a_*_dk.c` | ARM | Scalar visibility path only. Tightest GPU budget. |

**Keep it off the GL renderers.** Android's GLES path is where GulchCE saw dark models and missing geometry. Our GL images (Android's and PS5's `halo_guest.elf`; the Switch has none) link GulchCE's inert stub, `port/android/guest/dynamic_lighting_disabled.c`, and hide the menu rows.

**Desktop is out of scope.** This fork ships only on Android, Switch and PS5, so nothing here should fix or plan around Windows or Linux builds.

**The GPU budget is already tight.** In Chill Out with 64 players, 6 lights cost the Switch 11.3 ms on specular passes and 3.7 ms on diffuse passes (`source/objects/object_lights.c:1845`). That's why we cap them with `display.dynamic_light_limit` (8) and `display.specular_light_limit` (2). GulchCE's numbers all come from an RTX 5070 Ti, so they say nothing about these devices.

## Source inventory

Import the feature as a snapshot of `4159d0d1`, not by cherry-picking: its roughly 40 commits are interleaved with merges, and later ones rewrite earlier ones. Our tree and GulchCE share history up to `da8d45562` (2026-10-30), so most engine changes carry over.

### Toggles and the modules behind them

Paths are under `port/linux/game/` in GulchCE. All switches default to off.

| Menu row | Config keys (`display.*`) | Modules | Read first |
| --- | --- | --- | --- |
| Projectile Lights | `plasma_lighting` | `plasma_lighting.c`, `rocket_lighting.c`, `covenant_lighting.c`, `ambient_emitter_lighting.c` (flame streams) | `7c50c361`, `5cc72528`, `2af3ddac` |
| Muzzle Flash Lights | `muzzle_lighting` | `muzzle_lighting.c` | `a4fa0bc6`, `2522c432` |
| Weapon Glow (group) | `weapon_glow`, `fuel_rod_lighting`, `ammo_counter_lighting`, `needler_lighting` | `authored_emitter_lighting.c`, `covenant_emitter_lighting.c`, `ammo_counter_lighting.c`, `needler_lighting.c` | `be382e27`, `17191d7a`, `9b3d82d8` |
| Plasma Prong Lighting | `plasma_prong_lighting` | `authored_emitter_lighting.c` | `3469db86` |
| Shield Emitters (group) | `jackal_shield_lighting`, `covenant_shield_lighting` | `covenant_emitter_lighting.c` | `be382e27`, `76296c6b` |
| Vehicle Lights | `vehicle_lighting` | `authored_emitter_lighting.c`, `ambient_emitter_lighting.c` | `9b3d82d8` |
| World Emitters | `world_lighting` | `ambient_emitter_lighting.c` | `9b3d82d8` |
| Light Occlusion | `plasma_occlusion` | `plasma_occlusion.c`, `plasma_collision*.c` | `771cb8ff`, `c05f2388`, `3469db86` |
| Shield Glow, Glow Intensity | `shield_glow`, `shield_glow_intensity` | `shield_glow.c` (we already have an older version) | `ecf2cd69` |

Supporting files: `plasma_falloff.c` (world falloff), `ammo_counter_falloff.c`, `lighting_config.c` (settings cache), `lighting_menu.c` (group rows) and `port/linux/src/plasma_model_shader.c`.

**Don't import** `port/linux/src/lighting_settings_migration.inl`. Our players have no old lighting settings to reset, except a Shield Glow choice worth keeping. Skipping it also avoids the worst review finding.

### Engine and renderer changes

I ran `git apply --check` on the full `da8d45562..4159d0d1` diff of each file against our `f8dbc4411`. Those diffs can include unrelated GulchCE changes, so take only the lighting hunks.

| File | GulchCE lines (+/-) | Applies to ours | What it holds |
| --- | --- | --- | --- |
| `source/objects/object_lights.c` | 147/4 | Clean | Update, radius, lifecycle and colour hooks; the light-move bridge |
| `source/items/weapons.c` | 32/18 | Clean | `muzzle_lighting_shot` on a successful shot |
| `source/physics/collision_bsp.c`, `.h` | 89/14, 33/0 | Clean | `collision_bsp_test_vector_from_node` and the batch cache |
| `source/rasterizer/xbox/rasterizer_xbox_environment.c` | 13/0 | Clean | Falloff begin and end around world diffuse and specular passes |
| `source/rasterizer/xbox/rasterizer_xbox_models.c` | 18/1 | Clean | AR screen and Needler material hooks |
| `source/rasterizer/xbox/rasterizer_xbox_transparent_geometry.c` | 16/0 | Clean | Needler transparent-draw hook |
| `source/rasterizer/xbox/rasterizer_xbox.c` | 9/0 (2 lighting lines) | Taken by hand | Model light slot and reset hooks. Missing from the first version of this table |
| `port/linux/game/render_interpolation.c` | 1 line | Taken by hand | `plasma_lighting_frame_end` at frame end. Missing from the first version of this table |
| `port/linux/game/shield_glow.c` | 255/203 | Clean | Shared-lighting version of Shield Glow |
| `port/linux/game/menu_functions.c` | 599/13 | **Conflict** | Three small `lighting_menu_*` calls; add them by hand |
| `port/linux/src/d3d8_gl.c` | 311/33 | **Conflict** | Don't port; it's the GL model path |
| `port/linux/src/xgpu.h` | 23/2 | **Conflict** | Only the key field is needed, in our backends' own keys |
| `port/linux/src/nv2a_psh.c`, `nv2a_vsh.c` | 94/8, 34/2 | Clean | Don't port; use them as the reference for the Vulkan and deko3d translators |

The menu rows in `tools/port_settings.py` also go in by hand. Our Shield Glow row sits in Extra Effects, and GulchCE tags its rows `desktop web`.

### Tests and reference docs

GulchCE has about 20 C harnesses with Python runners in `tools/harness/tests/`, such as `plasma_lighting.c` and `muzzle_lighting.c`. Our harness lives at the same path, so import them with their modules.

Start reading with `docs/dynamic_lighting_release.md`, `docs/reading/rendering.md` and `docs/reading/emitter_expansion.md`. Treat their performance results as desktop-only.

## Porting phases

There are eight phases, and each one leaves all three platforms shippable with the feature off. World lighting comes first because it runs through the D3D8 layer our backends already implement. Occlusion comes last because it's the main CPU cost and has a safe fallback until then: with no occlusion texture, `plasma_falloff_bind` uses the plain falloff texture.

### Phase 0: Set up

1. Work on a `dynamic-lighting` branch. Commit and push only when asked.
2. Record the source revision, `4159d0d1`, in the first commit message. Both projects are AGPLv3, so keep GulchCE's file headers and credit it in the release notes.
3. Settle the decisions listed under "Risks and open decisions".

### Phase 1: Inert modules and hooks

**Take:** every module from the inventory except occlusion and collision, the stub, the lighting hunks in `object_lights.c`, `weapons.c` and the three rasterizer files, and the config keys.

1. Add the modules to the Vulkan and deko3d guest images in `tools/android_build.py`, `tools/ps5_build.py` and `tools/switch_build.py`.
2. Link `dynamic_lighting_disabled.c` into the GL images.
3. Register the keys as `_platform_all`, as `display.shield_glow` is. GulchCE's `_platform_desktop` would hide them on Android.
4. Temporarily stub the occlusion functions, with `plasma_occlusion_texture` returning NULL, so no rays are traced yet.

**Exit criteria:**

- [x] All three platforms build, including the GL images.
- [ ] With everything off, no feature lights, shader keys or texture allocations appear, and a capture matches the current build. (Code and harness "off" cases only; the capture needs a device.)
- [x] Imported harnesses for these modules pass.

**Status (Phase 1):** built and harnessed, not run on a device. GulchCE's other hunks in the same files (MCC, third person, shield look) were left out. The current Shield Glow stays in every image until Phase 5, reached through the temporary `shield_glow_bridge.c`. `plasma_occlusion_stub.c` stands in for occlusion until Phase 6.

### Phase 2: World lighting for projectiles and muzzle flashes

**Enable:** `plasma_lighting` and `muzzle_lighting` through config keys only, using `plasma_falloff.c`.

1. Confirm the 3D falloff texture reaches the shader on both backends, with linear filtering.
2. Confirm enlarged radii flow correctly into `port_dynamic_lights_choose` and `rasterizer_environment_port_light_scissor`. The chooser ranks by radius over distance, so bigger lights can displace stock ones.
3. Apply the falloff and muzzle fixes from "Fixes to apply during the port".

**Exit criteria:**

- [ ] Soft world spill appears on PS5, Android and Switch, and stock effects are unchanged.
- [ ] Lights aren't clipped at their scissor box.
- [ ] Per-light GPU cost is recorded for each platform.

**Status (Phase 2):** code paths read and the two fixes in, with harnesses. All three exit criteria need a device, so they stay open. What reading the code settled:

- The falloff texture needs no new shader on either backend. It is a 3D texture on stage 2 (diffuse) or stage 1 (specular), which is what the stock distance attenuation already is, so the pixel key does not change. Only the sampler does: `plasma_falloff_bind` sets `MINFILTER` to linear over the stock `MAGFILTER` linear, and both backends read those states per draw (`sampler_state_make`, `sampler_make`).
- deko3d's `dk_texture_reach` finds the falloff texture's own reach from its decoded texels, so its sphere clip fits it. Its table holds 16 textures: with more, a texture falls out and that draw loses the sphere clip, which costs fill but doesn't change the picture.
- Enlarged radii reach both `rasterizer_environment_port_light_scissor` (through the submitted `light->radius`) and `port_dynamic_lights_choose` (through `light_compute_bounding_sphere`, after the radius is written). So bigger lights can displace stock ones under the 8-light cap, as the risk table says. No rank change has been made; that needs a device to judge.

### Phase 3: Model lighting on Vulkan, then deko3d

**Build:** the backend work under "Renderer work". Finish Vulkan first, because it covers two platforms.

**Exit criteria:**

- [ ] Models pick up the falloff in both vertex and per-pixel lighting.
- [ ] Draws with no selected light use the stock shader key, so stock key counts don't change.
- [ ] No model disappears or goes dark while an enhanced shader compiles.
- [ ] The GulchCE Android symptoms are checked explicitly: first-person geometry and model brightness.

**Status (Phase 3):** written for both backends and checked as far as this machine can, none of it on a GPU. The first three criteria stay open because they ask about pictures and running key counts, but here is what was checked:

- Every real vertex program goes through the Vulkan and deko3d generators in `test_plasma_model_shader.py`, and every variant compiles with glslang and UAM. That shows the GLSL is valid, not that it looks right.
- Stock shader text is byte-identical to the tree before this port for all 67 real programs, packed and unpacked, lit and unlit, on both backends, by a one-off comparison (not a kept test). So stock hashes and caches don't move.
- `test_plasma_model_fallback.py` runs the real Vulkan `draw_shaders` against a fake shader service, with negative controls: no selected light never asks for the variant, a variant that is compiling or failed draws stock, and a draw doesn't spend the frame waiting for a variant. deko3d's choice is only checked by reading its order in the source.

An independent read-only review of this code found no high-severity defect and five lesser ones. Four are fixed on deko3d: a variant no longer waits or spends the map-start stall budget (`dk_shader_variant_for_draw`); a failed variant no longer recompiles forever (a second send means the first failed, and the program stays stock); a field image that couldn't be made reads a white 3D dummy, not the 2D one; and the reach table now evicts rewritten fields before anything written once. The fifth stays open: on Vulkan, if a variant's *pipeline* (not its shaders) fails to create, its draws are skipped because the guest isn't told, as a stock pipeline's would be. Fixing it needs a host-to-guest report. The trigger should be rare, and it's listed in the status file.

**Decided (occlusion bindings before Phase 6):** build the shader without them. `XGPU_PLASMA_MODEL_FALLOFF` variants have no samplers and `plasma_visibility` returns 1, and only `XGPU_PLASMA_MODEL_OCCLUSION` variants bind fields. So Light Occlusion off costs no sampler or field, and no fallback texture is needed before Phase 6.

### Phase 4: Remaining sources and the menu

**Enable:** Weapon Glow, Plasma Prong Lighting, Shield Emitters, Vehicle Lights, World Emitters and the Covenant profiles.

1. Add `lighting_menu.c`, its three calls in `menu_functions.c`, and the rows in `tools/port_settings.py`. Then regenerate the menus.
2. Apply the group-save and Needler fixes.
3. Check that `ammo_counter_lighting_draw_begin` works: it doubles output through `D3DRS_PSRGBOUTPUTS7`, so each backend's pixel shader key has to honour it.

**Exit criteria:**

- [ ] Every row saves, cancels, restores defaults and survives a restart, by touch and by controller.
- [ ] The GL images don't show the rows. (By code only: the stub's `lighting_menu_available` returns 0 and `dynamic_lighting_row_show` hides the row, so nobody can reach the screen. Not seen on a device.)

**Status (Phase 4):** modules and the `lighting_menu` calls are in, with the group-save and Needler fixes, and the harnesses pass. The `lighting_menu` harness found a real bug in the port: our `setting_write` is a separate route from `setting_save`, so it needed its own call as well. Our setting calls also use `config_profile_*`, so `lighting_menu_write` covers only the two groups and the other rows go the usual way. The save, cancel and defaults route is tested with a fake config for the spinner logic, but not by touch or controller. Menus: `tools/ce_menus.py` needs the game's tags, so only `port_settings.settings_files()` was regenerated. It reproduced all 20 other settings files byte for byte, so the generator and the committed XML agree. No Needler check on a CE map, since that needs a device.

**`ammo_counter_lighting_draw_begin`, by reading the code:** both backends honour it. `D3DRS_PSRGBOUTPUTS7` is 52, below `D3DRS_PS_MAX` and `D3DRS_SIMPLE_MAX`, so `SetRenderState` stores it and both `pixel_key_make`s copy it into the key. Model materials set 8 combiners (`rasterizer_xbox_models.c`), so stage 7 runs, and both translators read flag `0x10` as times two. The doubled draw is one more pixel key (and pipeline on Vulkan), and it isn't covered by the stock fallback: it's an ordinary shader, so on first use it's skipped or waited for as any new one is.

### Phase 5: Shield Glow integration

**Take:** GulchCE's `shield_glow.c` and `overshield_effect.c` changes (`ecf2cd69`), then move both rows to the new screen.

**Exit criteria:**

- [ ] `network.shield_effects` still overrides local settings, and a client with its setting off follows the host.
- [x] Network version and `network_format.txt` are unchanged. (`tools/network_format.py` says "the wire format is the one recorded"; no network source is touched.)

**Status (Phase 5):** `shield_glow.c` taken and adapted, and both rows moved. `network_shield_effects()` still decides inside `shield_glow_enabled`, shown by `test_shield_lighting.py` and `test_shield_effects.py`, but the "client follows the host" criterion also needs two machines, so it stays open. Differences from GulchCE:

- GulchCE's `shield_glow.c` depends on its Shield Look feature, which we don't have, so the file is adapted to use the unit's own shader. `test_shield_lighting.py` and its fixture lost the matching Shield Look lines.
- Only `overshield_effect_reset` and `overshield_effect_light_sample` were taken from GulchCE's `overshield_effect.c`. Its surface changes (green outer layer, new arms route) aren't lighting. `overshield_effect_reset` now runs at map and light-array changes in every image, including GL, which is a small change to the GL images' overshield state: it no longer carries over a map change.
- Moving the rows leaves Extra Effects with nothing on the Switch and PS5, since overshield and HDR are the OpenGL renderer's. So that entry now shows only on `desktop android web`.
- **GL images lose Shield Glow.** They link the stub as decided, and its rows are hidden with the rest. Before this phase, Android's GL renderer had the old Shield Glow, including when a host asked for it online. See the status file's decisions.

### Phase 6: Occlusion

**Take:** `plasma_occlusion.c`, `plasma_collision*.c` and the `collision_bsp` changes, then remove the Phase 1 stub.

1. Apply the three CPU fixes before measuring anything.
2. Measure the scalar path on ARM. `plasma_occlusion.c` has only an SSE2 fast path, so decide whether a NEON version is worth writing from the numbers.
3. Measure the cost of rewriting field textures, described under "Renderer work".

**Exit criteria:**

- [ ] Light stops at walls and doesn't leak through vehicles.
- [x] Stock collision callers still start at the root and get identical results, shown by GulchCE's paired-ray test. (`collision_vector_entry.c` and `collision_vector_cache.c` in `test_plasma_collision.py`, over the real `collision_bsp.c`. Asset-free BSPs, not a real map.)
- [ ] Worst-case CPU cost is recorded per platform, with a decision to ship, limit or hide the toggle.

**Status (Phase 6):** in and harnessed, with no device run. The temporary stub is gone. Steps 2 and 3 need devices, so there's no measurement and no NEON decision yet. `test_scalar_path` runs the whole visibility case over the scalar path the ARM images use, on x86, so it shows the logic and says nothing about ARM's speed.

Changes from GulchCE:

- **Light Occlusion gates every ray.** As decided, `enabled()` and `plasma_occlusion_submit` both require `display.plasma_occlusion`, so muzzle and vehicle lights trace nothing with it off. The catch is that with it off they light through walls and their own vehicle. `plasma_occlusion.c`'s harness now turns the toggle on for those cases, gains "traces no rays" checks, and drops one check that only made sense with the old rule ("vehicle switch cannot select unrelated lights").
- **`D3DLOCK_NOOVERWRITE` on field rewrites.** Not in the guide. A lock with flags 0 makes the Switch wait for the GPU (`lock_wait`) on every rewritten field. Reading `xbox_textures_dk.c` shows the GPU draws a decoded copy sent in order with the draws, so the wait protects nothing. Not run.
- **`plasma_model_lighting_mask` answers 0 first.** It's now asked for every draw of a model program, including with the feature off.

### Phase 7: Release

1. Review the combined diff, and check that the off path still matches stock.
2. Write the release notes in the Build commit, as usual, crediting GulchCE.

## Renderer work

Model lighting is the only part that needs new backend code; world lighting should work through what exists. None of this has been run yet, so Phases 2 and 3 have to confirm it.

```text
+---------------------------------------------------------------+
| Shared game code, imported from GulchCE                       |
|   Light selection: plasma, muzzle, emitter and shield modules |
|   Occlusion (Phase 6): CPU rays into 16-cube visibility fields|
+---------------------------------------------------------------+
            |                                   |
            v                                   v
+------------------------------+  +-----------------------------+
| World lighting: verify       |  | Model lighting: NEW CODE    |
|   plasma_falloff.c calls the |  |   Shader key bit, GLSL and  |
|   D3D8 API                   |  |   uniforms                  |
|   Volume textures through    |  |   Two extra sampler3D       |
|   d3d8_resources.c           |  |   bindings                  |
|   No new backend code        |  |   Written once per backend  |
|   expected                   |  |                             |
+------------------------------+  +-----------------------------+
            |                                   |
            v                                   v
+---------------------------------------------------------------+
| Backends the feature runs on                                  |
|   Vulkan: Android and PS5        deko3d: Switch               |
+---------------------------------------------------------------+
```

The GL images aren't shown, because they link the inert stub and never run this feature.

### World lighting: verify, don't rebuild

`plasma_falloff.c` and `plasma_occlusion.c` only call the D3D8 API (`D3DDevice_CreateVolumeTexture`, `LockBox`, `SetTexture`). Our shared `port/linux/src/d3d8_resources.c` implements those calls. Vulkan has `VK_IMAGE_3D`, and deko3d has `DK_TEXTURE_3D`. The stock attenuation is itself a volume texture on the same stage, so the route is used every frame today. Checked by reading both backends (not run): the sampler type comes from the texture's depth, the texels are unswizzled with the same bit order `rasterizer_xbox_bitmap_swizzle3d_long` writes, and the filter follows the stage's state.

### Model lighting: add to both backends

GulchCE's version is OpenGL-specific: `plasma_occlusion_gl.c`, `plasma_model_shader.c` and about 300 lines in `d3d8_gl.c`. What was built differs from the first plan in these ways, each so that stock draws stay as they were:

| Step | GulchCE reference | Vulkan | deko3d |
| --- | --- | --- | --- |
| Variant in the shader key | `key->plasma_model` in `d3d8_gl.c:4176` | `key->plasma_model`, taken from the key's padding so its layout and stock hashes don't change, and a marker in the vertex hash | Bits above the attributes in `dk_vertex_key.packed_mask`. Stock keys and `DK_KEY_FORMAT` don't change |
| Vertex and pixel GLSL | `plasma_model_emit_*` | `nv2a_vsh_vk.c` for vertex lighting, `nv2a_psh_vk.c` (`model_lighting`) for per-pixel | `nv2a_vsh_dk.c` only: deko3d has no per-pixel lighting, so every model is vertex lit |
| Uniforms | `glGetUniformLocation` per program | Appended to the vertex and pixel parameter blocks, declared only by variants | Appended to the vertex parameter block, declared only by variants |
| Two `sampler3D` occlusion bindings | GL units 6 and 7 | A second descriptor set (`VK_PLASMA_SET`) with its own pipeline layout, used only by occlusion variants, so stock pipelines keep their layout and cache entries | Vertex-stage bindings 0 and 1, bound only once a field exists |
| Binding per draw | `plasma_occlusion_bind_model` | `VK_COMMAND_PLASMA_OCCLUSION` when the images change, and a flag in `vk_command_draw`'s reserved word | `DK_COMMAND_PLASMA_OCCLUSION` when the images change |

The cost to stock draws is that each parameter block is 48 bytes longer, which every put carries (vertex 48 to 96 on both backends, pixel 720 to 768 on Vulkan). The alternative was another uniform binding for every draw. A 3D image's upload barrier on Vulkan also now covers the vertex stage. Stock 3D images are uploaded only when a map loads.

### Three differences from desktop

**A missing shader skips the draw.** GulchCE compiles enhanced variants lazily and stalls for a moment. Checked against the code: Vulkan's `shader_handle` first compiles for up to a frame's time per frame (`host_vk_shader_make`) and skips the draw after that, and deko3d queues it and skips, except that it waits within a map's first seconds (`stall_allowed`). So a model could vanish the first time a light touches it. The variant now compiles in the background, and the draw uses the stock shader until it's ready. A failed variant stays stock (`plasma_failed` on Vulkan; deko3d's service keeps answering 0). A variant never waits on either backend. On Vulkan a new pipeline is still made at the draw, as every pipeline is, so that can cost a hitch, and a pipeline that fails to create skips its draws.

**Rewriting textures costs page faults.** Our backends spot rewritten textures through the memory watch, which uses `mprotect` and a `SIGSEGV` handler per 4 KiB page on Android, with its own version in `host_memory.c` on Switch and PS5. Eight fields with three 16 KiB textures each is 96 pages per full rewrite. GulchCE already skips byte-identical uploads, but moving lights will change them. If this shows up in measurements, add a direct upload path for feature-owned textures.

**Enhanced keys add to compile load.** The Switch compiles known keys in the background and players share keys. Add the common enhanced variants to that list only after Phase 3 settles their format, so the key files change once.

## Fixes to apply during the port

Fix these as each file arrives, because several matter more on our hardware than on GulchCE's. Line numbers refer to GulchCE at `4159d0d1`. "Confirmed" means the code was read and matches; nothing has been run.

| Phase | Location | Problem | Fix | Status |
| --- | --- | --- | --- | --- |
| 1 | `lighting_settings_migration.inl:74` | Settings can reset on every launch if the marker never gets written | Don't import the file | Confirmed |
| 2 | `muzzle_lighting.c:185` | A pulse is dropped if no render happens within 2 ticks, about 100 ms, so flashes vanish in hitches | Keep the pulse until it's rendered or replaced by a newer shot | Fixed, with a harness case |
| 2 | `plasma_falloff.c:93` | A failed texture creation resets occlusion mid-view and retries for every light | Release only the texture, and remember the failure until the next map | Fixed, with a harness case |
| 4 | `lighting_menu.c:54` | A failed group save leaves earlier members changed | Restore earlier members on failure | Fixed, with a harness case |
| 4 | `needler_lighting.c:28` | Held Needler glow ignores `weapons\needler\mp_needler` | Add the tag, then check it on a CE map | Tag added, with a harness case; the CE map check needs a device |
| 4 | `covenant_emitter_lighting.c:396` | Teleporter lights may be blocked by their own model | Test with occlusion on; extend the ignore list if needed | Unconfirmed, depends on map tags |
| 6 | `plasma_occlusion.c:433` | The scene revision is computed per field and per view, even when it can't change the decision | Compute it only when the 12 ms cadence is due and the light is stationary | Fixed, with a harness case. A retrace still takes the revision it stores |
| 6 | `plasma_occlusion.c:444` | Any extent change forces a retrace outside the 12 ms cadence | Rescale cached depths, or store hit distances | Fixed by rescaling, with a harness case. A ray that missed stays clear until the next trace if the extent grew. How often extents change is unmeasured |
| 6 | `plasma_collision_models.c:47` | The BSP cache is fully cleared for each node of each object | Clear only the entries that node count can use | Fixed, with a harness case. The saving isn't measured |

Leave `rocket_lighting.c` as a separate module for now, even though the review found it nearly duplicates `covenant_lighting.c`. Keeping the file layout close to GulchCE's makes its later fixes easy to bring over. Merge them once the port is stable.

GulchCE's shader and renderer integration wasn't part of that review. Treat the backend work under "Renderer work" as unreviewed, and review it when it's written.

## Validation

A phase passes only on real devices, because GulchCE's tests never ran on our renderers. Its harnesses prove module logic, and its visual and performance results don't transfer.

### Rules for test devices

- **Ask first.** Say what you're about to run and wait before launching or driving anything on a phone.
- **Android:** the wireless phones use the Linux `adb`. Never uninstall the app or reboot the phone, and never copy or back up maps, since the app extracts them from the ISO on the phone.
- **S21 FE:** the standing permission covers only the Vulkan-parity work, so ask before using it here.
- **PS5:** ask who launches each run. If the user started it, fetch the results and don't relaunch.
- **Switch:** the FTP test console is a Lite, so runs are 720p. Docked 1080p results come from someone else's console. Game errors are in `/switch/halo/debug.txt`, not `halo.log`. Never restart with `envSetNextLoad`.

### Checks per phase

| Phase | Automated | On device | Compare against |
| --- | --- | --- | --- |
| 1 | Builds for all images; imported harnesses | One map with everything off on each platform | Current build: identical shader keys and no feature lights |
| 2 | `plasma_lighting`, `plasma_falloff`, `muzzle_lighting`, `rocket_lighting` | Fire plasma and rockets at a wall, then in a firefight | Toggle off in the same scene |
| 3 | `plasma_model_shader`, adapted to the Vulkan and deko3d translators | Lit models in first and third person, with per-pixel lighting on and off | GulchCE's Android symptoms |
| 4 | `authored_emitter_lighting`, `covenant_emitter_lighting`, `ambient_emitter_lighting`, `lighting_menu` | Each row on, by touch and controller, on Xbox, CE and MCC maps | Toggle off, and a restart |
| 5 | `test_shield_lighting.py`, `test_shield_effects.py` | Two players with different local settings | Host policy and `network_format.txt` |
| 6 | `plasma_occlusion`, `plasma_collision*`, `plasma_visibility`, paired-ray diagnostic | Walls, doors and vehicles, then a busy match | Occlusion off in the same scene |

### What to measure

For each toggle on each platform, record the following against the off baseline in the same scene:

- Frame time on the CPU and GPU. Use a fixed scene for repeatable numbers, and a bot match for worst-case numbers.
- Dynamic lights kept and dropped, from the existing `object_lights.c` log line every 300 frames.
- Rays traced and textures rewritten per frame (Phase 6).
- Shader compile time and skipped draws the first time a variant appears.

Different bot battles aren't directly comparable, as GulchCE's own reports note. Only treat a difference as real if it shows up with identical inputs or across repeated runs.

## Risks and open decisions

The biggest risk is GPU cost on phones and the Switch, and no measurement exists yet. Phase 2 gives the first real numbers, so review the plan there before committing to the backend work in Phase 3.

### Risks

| Risk | Why it matters | Mitigation |
| --- | --- | --- |
| Too slow on Switch and phones | Each kept light redraws the level's surfaces, and larger radii touch more of them | Per-toggle costs in Phase 2; stop or limit a platform there |
| GulchCE's Android rendering faults return | The cause was never found, so Vulkan can't be assumed safe or unsafe | Stock shader keys when no light is selected; explicit check in Phase 3 |
| Models vanish while shaders compile | Our backends skip draws with no ready shader | Stock-variant fallback, required by Phase 3's exit criteria |
| Occlusion too costly on ARM | No SSE2 path, and the 1536-ray refreshes were tuned on a desktop CPU | Comes last, stays optional, and can be hidden per platform |
| Enlarged lights push out stock ones | The chooser ranks by radius over distance under an 8-light cap | Check in Phase 2; rank feature lights by their stock radius if needed |
| GulchCE keeps changing this code | It has been changing daily, with commits as recent as 2026-11-06 | Pin to `4159d0d1`; bring later fixes over with `git diff 4159d0d1..<new> -- <paths>` |

These effects have competitive implications too. `63fd3677` made shield effects host-controlled because lit players were easier to see. The other toggles stay local, as they are in GulchCE, so one player can have light spill that another doesn't.

### Decisions for you

- [ ] **Platform order.** Recommended: PS5, then Android on the Adreno 750, then Switch. PS5 has the most headroom, and the Vulkan work carries over to Android.
- [ ] **Scope for Switch.** Recommended: decide after Phase 2. Shipping world lighting without occlusion there may be the realistic limit.
- [ ] **Occlusion on ARM.** Recommended: decide from Phase 6 measurements, and offer it only where it holds frame rate.
- [x] **Always-on blocking.** Decided: muzzle and vehicle lights follow Light Occlusion, so off means no rays. The catch is that flashes can then light through vehicles.
- [x] **Local or host-controlled.** Decided: the new toggles stay local, and Shield Glow stays under `network.shield_effects`.
- [ ] **Performance limits.** Set the frame-time cost per platform that a toggle may add. No numbers are proposed here, because nothing has been measured on these devices.

To limit a toggle on a platform once decided, name its key in `platform_off` in `port/linux/game/lighting_config.c` and tag its row in `tools/port_settings.py`.

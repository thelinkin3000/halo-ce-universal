# Dynamic Lighting Port: Status

Written 2026-11-06, at the end of an unattended run over Phases 1 to 6 of [the guide](dynamic_lighting_port.md). The source is GulchCE at `4159d0d1`.

**The code for Phases 1 to 6 is written, it builds for every image, and its harnesses pass, but none of it has run on a device or a GPU.** No phone, Switch or PS5 was touched. Nothing here shows what the lighting looks like or what it costs. Every effect still needs a device to prove it, starting with whether the off path matches the current build.

Nothing is pushed. Phase 7 (release notes, Build commit) was not part of this run.

## 1. What's implemented

| Phase | Commit | What it holds |
| --- | --- | --- |
| 1 | `6572541d2` | Light selection modules, their harnesses, lighting hunks in the engine files, the config keys (all off), and the stub in the GL images |
| 2 | `f7088c6fd` | Muzzle pulse and falloff failure fixes |
| 3 | `47f2689aa` | Model variant on Vulkan: GLSL, key, uniforms, stock fallback, separate descriptor set |
| 3 | `90a22361b` | Model variant on deko3d, and the shader and fallback harnesses |
| 4 | `357635edd` | Dynamic Lighting screen, group-save and Needler fixes, row hidden on GL |
| 5 | `8b877b0ac` | Shield Glow on the shared lighting, adapted; rows moved |
| 6 | `67008f560` | Occlusion, collision batching, three CPU fixes, Light Occlusion gating every ray |
| 1–6 | `26ad060e6` | One place to limit a toggle per platform; nothing is limited yet |
| 3 | `00843efbe` | Fixes to deko3d from a review of the backend code |

To limit a toggle on a platform, name its key in `platform_off` in `port/linux/game/lighting_config.c`, and tag its row in `tools/port_settings.py`.

### Exit criteria

| Phase | Verified here | Open, needs a device |
| --- | --- | --- |
| 1 | All images build; harnesses pass | The off path against the current build |
| 2 | Nothing: these criteria are all about the picture and its cost | Spill, scissor clipping, per-light GPU cost |
| 3 | Partly: the text and compile checks in section 2 | Falloff on real models, stock key counts while running, no vanishing, GulchCE's Android symptoms |
| 4 | Partly: save, cancel and defaults logic with a fake config | Touch and controller; rows hidden on GL |
| 5 | Wire format unchanged | A client following the host |
| 6 | Stock collision callers unchanged (paired-ray test, synthetic BSPs) | Walls and vehicles; worst-case CPU cost and the ship, limit or hide decision |

## 2. What ran, and the results

### Builds

Built with `--release`, `ninja android android_apk switch ps5 build/ps5/halo_guest.elf`, at `00843efbe`. All exit 0.

| Image | Result | Links the modules | Links the stub |
| --- | --- | --- | --- |
| Android Vulkan (`halo_guest_vk.elf`) | Built | Yes | No |
| Android GL (`halo_guest.elf`) | Built | No | Yes |
| Android APK (debug) | Built | | |
| PS5 Vulkan (`halo_guest_vk.elf`), title | Built | Yes | No |
| PS5 GL (`halo_guest.elf`) | Built | No | Yes |
| Switch (`halo_guest.elf`, `halo.nro`) | Built | Yes | No |

"Links" comes from each image's linker map. The Switch has no GL image, which the guide wrongly said it had.

### Harnesses

Run on x86 in a 32-bit build, so they test logic and generated text, not ARM speed or GPU output.

| Run | Result |
| --- | --- |
| `tools/harness`, before this port (`237b84e6f`, clean worktree) | 159 failed, 1312 passed, 1 skipped |
| `tools/harness`, at `00843efbe` | 159 failed, 1439 passed, 1 skipped, 7 subtests passed |
| Tests failing now that passed before the port | 0 |
| The 18 lighting harness files added by this port | Pass |
| Other suites (`test_ps5_build`, `test_guest_layout`, `test_check_host_imports`, `test_host_*`, `test_android_*`, `test_linux_guest_build`) | 7 failed, 112 passed, with the same 7 failures on the clean tree |
| `test_touch_menu.py`, `test_light_storage.py`, `test_death_timing.py` | Pass |
| `tools/network_format.py` | "the wire format is the one recorded" |

The 159 and the 7 existed before this port and sit in unrelated tests (for example `test_transparent_enclosure`, `test_server_toml`, `test_bots_lobby`, `test_android_render_policy`). I didn't touch them. Some of those files also cover code near this work, such as `test_config_write` and `test_object_bounds_cache`, so they gave no signal here.

### Specific checks

- **Shaders compile.** All the real model programs, with each variant, go through the Vulkan generator into glslang and the deko3d generator into UAM. The Vulkan pixel variants compile too. A deliberately broken variant is rejected, so the compile step can fail. This shows valid GLSL, not a right picture.
- **Stock shader text is unchanged.** A one-off comparison against the pre-port tree, over all real programs: 291 Vulkan shaders and 288 deko3d shaders, byte-identical. It isn't a kept test.
- **Fallback.** The real Vulkan `draw_shaders` runs against a fake shader service, with negative controls. On deko3d the fallback is only checked by reading its order in the source.
- **Review.** A separate read-only pass over the backend code found no high-severity defects and five lesser ones. Four are fixed, compiled only; the fifth is in section 3.

### Renderer claims the guide asked me to check

All by reading code, none run.

| Claim | Result |
| --- | --- |
| Volume textures reach the shaders with linear filtering | **Holds on both.** Same stage and sampler type as the stock attenuation, so no new shader is needed |
| Draws with an unready shader are skipped | **Holds, with detail.** Vulkan first compiles for up to a frame's time, and deko3d waits within a map's first seconds. The fallback is built, and variants never wait |
| `D3DRS_PSRGBOUTPUTS7` is honoured | **Holds on both.** It's in the key, model materials use 8 combiners, and both translators read the flag as times two |
| Occlusion bindings before Phase 6 | **Settled:** variants are built without samplers unless a field is bound |

## 3. What failed or was skipped

- **Everything needing a device,** by instruction: all pictures, costs, the off-path capture, input by touch and controller, two-machine play, and the Needler check on a CE map.
- **Measurements.** None exist, so there's no NEON decision and no decision on a direct upload path for fields.
- **Teleporter self-blocking** (`covenant_emitter_lighting.c:396`) is untouched, because it depends on map tags. It's in the test plan.
- **Known defect, not fixed:** on Vulkan, if a variant's shaders compile but its *pipeline* fails to create, its draws are skipped from then on, so that model vanishes while the light touches it. The guest isn't told. A stock pipeline failure does the same today. See decisions.
- **Known cost, not fixed:** a new Vulkan variant pipeline is still made at the draw, which can hitch.
- **Enhanced-key precompile list not updated.** Variants go into `console.dkk` when first met, but not into the shared `keys.dkk`, which needs a device to generate.
- **`tools/ce_menus.py` not run,** because it needs the game's tags. Only `port_settings.settings_files()` was regenerated, and it reproduced the other settings files byte for byte.
- **Not imported:** `lighting_settings_migration.inl`, as the guide said, plus GulchCE's performance and capture tooling (`lighting_performance_*`, `plasma_visual_*`), which is built for its desktop.

## 4. Where this departs from the guide or GulchCE

**From the guide:**

- The Switch has no Mesa image. Guide corrected.
- `rasterizer_xbox.c` and `render_interpolation.c` also need hunks, which the guide's table missed. Corrected.
- Occlusion bindings use a separate descriptor set with its own pipeline layout, not extra bindings in the shared set, so stock pipelines keep their layout and cache entries.
- deko3d has no per-pixel lighting, so its variant is vertex only.
- Extra Effects now shows only on `desktop android web`, since moving the Shield Glow rows left it empty on the Switch and PS5.

**From GulchCE:**

- Muzzle and vehicle lights follow Light Occlusion, as you decided. Off, they light through walls and their own vehicle.
- Shield Glow is adapted without Shield Look, which we don't have. Only the overshield's reset and light sample are taken, so its surface is unchanged.
- `D3DLOCK_NOOVERWRITE` on field rewrites. A plain lock would make the Switch wait for the GPU on each one, and reading the code shows the GPU draws a decoded copy. Not run.
- The deko3d 3D reach table is larger (16 to 64), logs once per texture, and evicts rewritten fields first.

**Changes to stock behaviour with everything off:**

- Vertex parameter blocks are 48 bytes longer on both backends, and Vulkan's pixel block too. So stock draws carry 48 more bytes per put.
- `plasma_model_lighting_mask` is called on every draw of a model program, and the hooks run per light.
- A Vulkan 3D image's upload barrier also covers the vertex stage. Stock 3D images upload only when a map loads.
- The overshield state resets at map changes, in every image.
- About 1.2 MB of static field storage in the Vulkan and deko3d images, by reading the structures, not measured.
- **The GL images lose Shield Glow.** See decisions.

**Harness changes** (no check was weakened to get a pass; each is also in its commit message):

| Test | Change | Why |
| --- | --- | --- |
| `plasma_lighting` | Declares `interpolation_drawing` | Our frame end clears it too |
| `muzzle_lighting` | "Old undrawn event discarded" became "a hitch keeps the flash" | That's the guide's fix; the old behaviour is now a negative control |
| `plasma_occlusion` | Turns the toggle on for muzzle and vehicle cases; removes "vehicle switch cannot select unrelated lights" | Your decision makes that check no longer apply. No-ray checks are added |
| `lighting_menu` | Config goes through `config_profile_*`; no third-person policy | Our layout |
| `shield_lighting` and fixture | Shield Look lines removed | We don't have it |
| `plasma_model_shader` | Rewritten to target the Vulkan and deko3d translators | GulchCE's targets its GL translators |
| `harness.build` | Takes compiler flags. Exact float checks use SSE math | x87 excess precision made them meaningless |

## 5. Device test plan

Run these in order, since each step assumes the last passed. **Ask-first rules apply.** The platform order follows the guide's recommendation (PS5, then Android, then Switch) until you decide otherwise.

Build with `ninja android_apk`, `ninja switch` or `ninja ps5`. "Log" means `debug.txt` and `halo.log`; on the Switch, game errors are in `/switch/halo/debug.txt`. All lighting keys are `display.*` and default to false.

Useful on every step: `debug.gpu_stats = true` regularly logs skipped draws by reason.

| # | Platform | Config | Look for | Failure means |
| --- | --- | --- | --- | --- |
| 1 | PS5, Android Vulkan, Switch | Defaults, same map as Build 138 | Same picture, frame time and `gpu_stats`. No new shader compiles against an existing cache. No `3D texture` log lines beyond Build 138's. `console.dkk` doesn't grow | The off path isn't stock. Stop here |
| 2 | Android GL (`display.renderer = "gl"`) | Defaults, then all lighting keys true | Same picture both ways. No Dynamic Lighting row. Shield Glow gone | The stub or hide is wrong |
| 3 | All three, Vulkan or deko3d | Menu only, touch and controller | Row present, and screen laid out with no overlap. Save, Cancel, Defaults, restart. Extra Effects gone on Switch and PS5 | Row layout (only checked as XML) or save route |
| 4 | PS5 | `plasma_lighting` | Fire plasma and rockets at a wall: soft spill, no hard box edge | Falloff or scissor. A box edge points at `rasterizer_environment_port_light_scissor` |
| 5 | PS5 | `plasma_lighting`, firefight | `lights:` log line every 300 frames: dropped count against off | Big lights displacing stock ones (chooser risk) |
| 6 | PS5 | `muzzle_lighting`, with a hitch if you can force one | Flash on every shot, including right after a hitch | Pulse fix |
| 7 | PS5 | `plasma_lighting`, `per_pixel_lighting` off, then on | Model next to a bolt gets soft light in both. First touch: no vanishing or flicker, and `skipped ... shader not ready` doesn't rise on the variant's account | Variant or fallback. In the host log, shader errors mean GLSL; a skipped pipeline is the known defect |
| 8 | Android Vulkan (Adreno 750, then Turnip) | Step 7's | Same, plus **first-person weapon geometry and model brightness**: GulchCE's Android symptoms | The cause was never found upstream, so stop and report |
| 9 | Android Vulkan, older phone | Step 7's | Same, plus compile hitches | Driver dependence |
| 10 | Switch | `plasma_lighting`, `muzzle_lighting` | Same as 4, 6 and 7, on vertex lighting. New keys in `console.dkk` only with the feature on | deko3d variant, or vertex-stage binding |
| 11 | All | `weapon_glow`, `fuel_rod_lighting`, `ammo_counter_lighting`, `needler_lighting`, `plasma_prong_lighting`, one at a time | AR screen brighter only on its mask, other materials untouched. Needler on Xbox, CE and MCC maps, including `mp_needler` | `PSRGBOUTPUTS7` restore, or tag names |
| 12 | All | `jackal_shield_lighting`, `covenant_shield_lighting`, `vehicle_lighting`, `world_lighting` | Each lights what it says, and stock effects stay. Xbox, CE and MCC maps | Authored emitter profiles |
| 13 | Two machines | Host `network.shield_effects` set, client setting off, then `"players"` | Client follows host; `"players"` uses each player's own | Host policy |
| 14 | PS5 | `plasma_occlusion`, with `plasma_lighting` | Light stops at walls and doesn't leak through vehicles. Doors fade, no popping | Field or BSP batching. Try the world pass and the models separately |
| 15 | PS5 | Occlusion off, `muzzle_lighting` and `vehicle_lighting` on | Profile shows no rays. Flash lights through walls and hull, as decided | Gate wrong. If it looks bad, revisit the decision |
| 16 | PS5 | `plasma_occlusion`, `world_lighting` | Teleporter still lights its surroundings | Needs its model added to the ignore list |
| 17 | Android, then Switch | Step 14's | Same picture on the scalar path, and a sane frame time. On Switch, watch for game-thread waits | Scalar path, or the lock flag. Basis for the NEON decision |
| 18 | Each platform, fixed scene then bot match, per toggle against off | `debug.benchmark = "standard"`, `debug.profile_run` | CPU and GPU frame time, lights kept and dropped, rays and rewrites, compile time | The numbers for the limits below |

## 6. Decisions I need from you

1. **Shield Glow on GL.** Android's GL renderer had the old Shield Glow, and online hosts could ask for it. As you decided, GL images link the stub, so it's gone, and a GL player sees no glow when a host enables it. I recommend accepting that if few people play on GL; otherwise I can keep the old `shield_glow.c` in the GL images only.
2. **Failed variant pipeline on Vulkan.** I recommend leaving it unless step 7 or 8 shows it happening, since the host-to-guest report it needs is new plumbing for a rare case.
3. **The 48 extra uniform bytes on stock draws.** I recommend accepting them; step 1 shows whether they matter. The alternative is a dedicated binding only variants use, which adds more host code.
4. **Platform order, Switch scope, occlusion on ARM, and per-platform frame-time limits.** These are still yours, and steps 17 and 18 give the numbers. I set no limits.
5. **After the first device runs:** whether to rank feature lights by their stock radius (step 5), whether field rewrites need a direct upload path (step 18), and whether to add variants to the shared `keys.dkk`.

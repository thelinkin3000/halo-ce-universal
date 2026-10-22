# Android GLES render scale prototype

Base: OpenCE Ports build 107, commit `68e03267817b0d40e929c8f32cb4a9d6194f57c8`.

The change keeps the game's 480-line logical coordinates and field of view. The OpenGL ES render targets multiply build 107's `display.resolution_limit` scale by `display.render_scale`, clamped between 0.5 and 1.0 (default 1.0). At the default multiplier, native and high-resolution choices retain their upstream behavior. With `display.resolution_limit = "480"`, `display.screen_width = 640` and `display.render_scale = 0.75`, the actual back buffer is 480 by 360 pixels, then enlarged at presentation. It does not modify the Vulkan renderer. Android OpenGL ES also accepts `display.max_fps = 30` or `60`; `0` (the default), or another value, keeps it uncapped. The cap waits before presentation using the monotonic clock and nanosleep, retries interruptions, and never catches up late frames with presentation bursts. Limiter waiting precedes the existing SDL graphics-driver stall timer. A cap does not make slow frames faster.

Rebuild `halo_guest.elf` and its matching `halo_guest.relocs` together against the same source revision as the APK host libraries. Do not mix build 106 guest assets with build 107 host libraries. The harnesses below compile selected production C functions; they do not validate a complete guest build or measure device performance. Run the default scale and 0.75 on the same scene to measure the effect before choosing a preset.

The Android GLES path also reuses the existing Web VAO cache for draws whose vertex streams all come from the stable mirror. Its key includes the declaration, stream set, VBOs, offsets and strides. Immediate or streamed draws keep the device VAO. Android retains at most 4096 entries and falls back for new keys when full, without adding a `glDeleteVertexArrays` import. Web keeps its original clear/delete policy.

## Reproduce on Windows

Use Python 3.12, Git Bash, the installed Android NDK 28.2.13676358 and Ninja from Android SDK CMake 3.22.1. Use LLVM 22.1.0 for the guest compiler to retain the committed LLVM 22 profile; NDK clang 19 cannot read it. The NDK supplies the linker, archive tools, GLES headers and AArch64 builtins. No Linux Android SDK, native SDL build or glslang build is required.

The isolated checkout includes `source`, `tools`, `port/linux`, `port/include`, `port/third_party`, `port/android/{guest,include,host,probe}`, `port/switch/{guest,host}`, all of `port/assets`, and `pgo`. The complete assets preserve the original embedded menus, fonts, titles, buttons and HUD. The Switch files are read by the shared host ABI checker. `xbox/include` and Xbox SDK headers are not used.

Stage the official SDL headers once:

```powershell
git clone --depth 1 --filter=blob:none --sparse --branch release-3.4.16 https://github.com/libsdl-org/SDL.git build/android/third_party/SDL3
git -C build/android/third_party/SDL3 sparse-checkout set include
```

This pins SDL to `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c`. The build helper downloads musl 1.2.5 from musl.libc.org if absent and verifies the source archive SHA-256 against the upstream build script's pinned checksum.

Run from this checkout, substituting the installed paths:

```powershell
$guestClang = "C:/path/to/llvm-22/bin/clang.exe"
python build_guest_windows.py --ndk "$env:ANDROID_HOME/ndk/28.2.13676358" --clang $guestClang --pgo use --ninja "$env:ANDROID_HOME/cmake/3.22.1/bin/ninja.exe" --jobs 8
python tools/check_host_imports.py
python tools/test_android_render_scale.py --wsl Ubuntu-20.04
python tools/test_android_frame_limit.py --wsl Ubuntu-20.04
python tools/test_android_vao_cache.py --wsl Ubuntu-20.04
python tools/test_android_vao_cache.py --wsl Ubuntu-20.04 --web
```

The helper invokes the upstream `generate_guest_image` for GLES only, wraps shell commands through Git Bash (discovered beside `git` on PATH, or selected with `--bash`), and uses native Windows LLVM tools. It generates the original MSVC semantics headers and runs the upstream guest image validator. The local render scale test compiles the actual production screen selection function and verifies 22 scaling, clamping, finite-value, logical-width and upstream native/high-resolution composition cases. The pacing test compiles the production Android clock, limiter and swap functions with controlled time; 21 scenarios cover 30/60/uncapped, interrupted and late sleeps, overruns, configuration caching, clock failure/recovery and retained driver-stall logging. A failed monotonic read resets the deadline and skips the limiter for that frame, without mixing clock origins.

Outputs: `build/android/halo_guest.elf`, `build/android/halo_guest.relocs`, and the validator stamp `build/android/guest/halo_guest.checked`.

The VAO harness uses the production cache and attribute setters with a stateful GL model. A controlled trace of 400 mirrored draws with six attributes makes 2400 pointer calls per baseline frame, 2400 on a cold cached frame and zero on its warm repeat. It checks VBO/offset/stride/declaration changes, integer formats, per-VAO index buffers, invalidation epochs, immediate fallback, failed allocation, failed VAO generation and the Android/Web capacity policies. These are local functional/call-count results; actual eligible draw counts and FPS must be measured on the phone.

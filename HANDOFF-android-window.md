# Android memory window work — handoff

Branch: `main` on `origin` (`thelinkin3000/halo-ce-universal`), rebased onto
`upstream` (`cybersecurity/halo-ce-universal`). Fork point `aad8719f`.

Upstream is moving several commits an hour at times and has not merged this
work. Everything below is written to survive a rebase: what each change does,
how to tell it is still intact, and which claims are verified versus guessed.

---

## The one-paragraph version

On Android the Java runtime has usually mapped its own memory across the
`0x80000000` where this port puts the Xbox's 128 MB memory window, and across
`0x40000000` where the guest image is linked to run. The host now searches for
free space and tells the guest where it landed; the guest's own address
arithmetic — and the game's reads of addresses embedded in map data — had to be
taught to follow. Two devices: a **Nokia 8** (`NB1GAD1791306511`, window gets
moved, the hard case) and a **Lenovo TB321FU** (`HA27WN3Z`, window stays put).
Both reach levels with 0 crashes and 0 log assertions.

---

## The eleven commits, oldest first

| Commit | What it does |
|---|---|
| `42090ba8` | Host places the window in a free range instead of assuming `0x80000000` |
| `41aa2e32` | Accessors for map-data addresses (`TAG_DATA_ADDRESS`, `TAG_BLOCK_ADDRESS_AT`) |
| `feff12ee` | Routes 36 call-site files through those accessors |
| `04cd8f8e` | Diagnostic probe (see "dead code" below) |
| `5d1cb163` | `-iquote` so the Windows build finds the shared port headers |
| `ff462940` | Drops the ART-reclaim attempt; moves the window instead |
| `5030531a` | **Empty commit, no changes. Safe to drop.** |
| `d2f0d087` | The main fix-up: nine places still assuming the window never moved |
| `326717b5` | Reverts the game-state move that broke the back buffer; save layout stamp |
| `2850f2e5` | Loads the image before the display comes up; honest conflict reporting |
| `87f08ff6` | Hides the Android system bars properly |

`04cd8f8e` added `port/android/host/host_probe.c` (~390 lines). It is **never
called** — `host_probe_start` has no callers — but it still compiles into the
host. Consider deleting it.

---

## The invariants, and how to check each survived a rebase

All eleven of our commits sit **on top** of upstream's, so upstream cannot
textually overwrite us. The risk is *semantic*: upstream changing code our
fixes depend on. Check these after every rebase:

```bash
# 1. window search + guest-address translation
grep -c 'to_host\|to_guest' port/android/host/host_memory.c          # expect 6
# 2. write tracking follows the real window, not the linked one
grep -c 'address >= window_base' port/android/host/host_memory.c     # expect 1
# 3. physical/virtual conversion is not a bit-31 OR
grep -c 'PLATFORM_CONTIGUOUS_BASE + (unsigned long)(physical)' port/linux/src/platform.h   # expect 1
# 4. XPhysicalAlloc takes an offset, not a masked address
grep -c 'xbox_address) - 0x80000000UL' port/linux/include/halo_port_window.h             # expect 2
# 5. saved games carry the address they were written for
grep -c layout_address "source/saved games/game_state.c"              # expect 2
# 6. Present skips a frame rather than faulting
grep -c 'if (back_buffer)' port/linux/src/d3d8_gl.c                  # expect 1
# 7. image claimed BEFORE the display comes up (line numbers drift; compare order)
img=$(grep -n 'host_load_image' port/android/host/host_main.c | head -1 | cut -d: -f1)
vid=$(grep -n 'SDL_InitSubSystem' port/android/host/host_main.c | head -1 | cut -d: -f1)
[ "$img" -lt "$vid" ] \
  && echo "OK: image ($img) first, display ($vid) after" \
  || echo "FAIL: display ($vid) initialised before the image ($img)"
# 8. system bars
grep -c hideSystemBars port/android/app/src/main/java/com/halo/decomp/HaloActivity.java   # expect 3
# 9. animation frame data through the accessor (header macro, highest leverage)
grep -c 'TAG_DATA_ADDRESS((animation)->default_data)' source/models/model_animation_definitions.h  # 1
# 10. environment vertices through the accessor
grep -c 'TAG_DATA_ADDRESS((material)->compressed_vertex_data)' source/objects/object_lights.c      # 2
# 11. item permutations through the accessor
grep -c 'TAG_BLOCK_ADDRESS_AT(permutations)' source/game/game_engine.c                           # 2
```

**The whole-tree sweep** — catches any *new* place upstream added that reads a
map-data address without going through an accessor. This is the check that
actually matters after a rebase, because upstream's HUD work is adjacent to the
text/vertex code we've been bitten by:

```bash
grep -rnE "\)->address|\]\.address" source/ port/linux/ --include=*.c --include=*.h \
  | grep -vE 'TAG_BLOCK_ADDRESS_AT|TAG_DATA_ADDRESS|PORT_WINDOW_REBASE'
```

Expected: only network/IP `address` fields and our own `index_ranges[].address`
GL cache. **Anything naming `tag_data`, `tag_block` or `tag_reference` is a bug
that only shows on a device whose window moved.**

Also confirm upstream hasn't touched the headers we depend on — if they have,
re-read the fixes above rather than trusting them:

```bash
git diff --name-only aad8719f..upstream/main | \
  grep -E 'halo_port_window|tag_groups\.h|platform\.h|halo_port_capacity|halo_android_abi|guest\.ld'
```

Empty as of `c55e4e2b`.

---

## Rebasing (the treadmill)

```bash
git fetch upstream
git rebase upstream/main
git push --force-with-lease origin main     # rewrites published history
```

Rebases so far have been clean, even with overlapping files — upstream's work
has been netcode and HUD, ours is memory layout. Overlap has stayed at 0–5
files, usually small. Build and device-test before pushing:

```bash
ninja android_apk
adb install -r -d <apk>
```

The push triggers CI (`.github/workflows/build.yml`, fires on any push). That
run is the **only** check that Linux and Windows compile — this machine lacks
32-bit glibc headers, so even `#include <stdio.h>` fails under `-m32`. Treat CI
green as the desktop-build signal.

---

## Layout constants, with the arithmetic that matters

Window is 128 MB at `0x80000000` when free, else a 256 MB-aligned free range.

```
tag cache      0x803A6000 + 0x1600000  ->  ends 0x819A6000
level data     ends 0x819A6000  (maps are linked to exactly this boundary)
game state     0x81A00000 .. 0x82A00000   (16 MB)
top-down       texture 0x1600000 + sound 0x400000 -> reaches down to 0x86600000
```

**Do not move the game state above `0x86600000`.** `326717b5` moved it to
`0x86000000`, which put its top at `0x87000000` — inside the range the window
hands out top-down. The game state allocation failed, the back buffer then had
no memory, and `D3DDevice_Present` dereferenced NULL. The guard added in
`326717b5` means that now fails as a skipped frame with a log line rather than
a crash, which is what makes the next such mistake diagnosable.

Measured against the maps on the tablet (`tag_data_size` at offset `0x14` of
each map header), the largest shipped map is **b40 at 16,995,988 bytes**
(`0x1035694`) — comfortably inside the 22 MB cache. **This corrects an earlier
claim:** commit `326717b5`'s message says maps need `0x167C800` and that large
maps are expected to fail. That is wrong; it was never measured. Left in place
per instruction, but do not act on it.

---

## Open problems

### 1. Guest image is still fixed at `0x40000000` (blocks some devices)

A device reported:

```
cannot reserve the guest image range at 40000000 (File exists)
  in the way: 1a040000-42c00000 rw-p [anon:dalvik-main space (region space)]
```

ART's main heap covers the image's address and is mapped by Zygote before any
app code runs. There is **no** way to move it: `android.app.ActivityManager` in
`android-35` exposes only `getMemoryClass`, `getLargeMemoryClass`,
`getMemoryInfo`, `getMyMemoryState`, `getProcessMemoryInfo`, `setWatchHeapLimit`
— no base-address control, no `setProcessHeapGrowthLimits`, no `vmHeapSize`,
and `dalvik.vm.*` is Zygote-only. `android:largeHeap` exists but changes only
the size, so where the heap lands is undocumented and device-specific.

The image is `ET_EXEC`, not `PIC`, so unlike the window it cannot be relocated.
**Planned fix:** build it position-independent and have the host place it with
the same `find_gap` the window uses. Feasibility is **verified** — `-fPIC` on
`arm64_32-apple-watchos` yields `adrp`/`add` plus `.long sym+off` for data
pointers, `android_asm_convert.py` passes that through as standard ELF, and
the assembler emits `R_AARCH64_ABS32`. Remaining work:

1. `-fPIC` into `GUEST_ABI_FLAGS` (`tools/android_build.py`, ~line 53)
2. `port/android/guest/guest.ld`: link at base 0, add `PHDRS` with `PT_DYNAMIC`
3. link as `ET_DYN`
4. `port/android/host/host_loader.c`: choose a base via `find_gap`, copy
   segments to `base + p_vaddr`, apply `R_AARCH64_RELATIVE` relocations

The guest is `arm64_32` (32-bit pointers in a 64-bit process), so it is confined
to the low 4 GB regardless — "position independent" here means *find a free range
in the low 4 GB*, exactly what the window already does.

Risk: anything `-fPIC` doesn't convert becomes silent corruption. Verify on
**both** devices and check models/terrain/text specifically, not just "it boots".

### 2. Vertex explosion on a couple of levels / a couple of devices

Reported by users. **Memory sizing is ruled out** — see the measurements above;
the maps fit by construction. Remaining suspects are the vertex decode path
(compressed vertex formats, the BGRA swizzle in `stream_upload_swizzled`, stride
handling) or a rebasing miss specific to level geometry.

Useful diagnostic already written and **stashed** as `stash@{0}
diag-bsp-vertex-explosion` — one `platform_log` in `scenario_structure_bsp_load`
(`source/cache/cache_files.c`) printing tag-data size/address, level size and
destination, tag-cache end and game-state address. It is how the numbers above
were obtained. Re-apply with `git stash pop stash@{0}`.

Cheap trick for loading a specific map without touching the UI: write the map
name into `save/z/last_solo.txt` — the game resumes it on next launch.

### 3. Untested configuration

The **Nokia has been off USB for much of the session**. Every recent rebase has
been verified only on the tablet, where the window stays at `0x80000000` and
most of our code paths are no-ops. The relocated-window path — where every bug
in this work actually manifested — is currently unverified against current
upstream. Plugging it in and running a level is the highest-value next check.

### 4. Saves are discarded on upgrade

`326717b5` stamps each save with the game state's address and ignores it on a
mismatch. That is deliberate: translation can't be done safely on a raw 16 MB
dump mixing floats, ints and pointers. The cost is that everyone's existing
saves are dropped once, on first run of a build with the stamp.

---

## Stashes

| Stash | What |
|---|---|
| `stash@{0}` `diag-bsp-vertex-explosion` | The BSP layout diagnostic above. Re-apply when picking up vertex explosion. |
| `stash@{1}` `cleanup-both` | Cosmetic tidy (unused include, comment on `to_guest`, explicit include in `game_engine.c`). Semantically identical; not needed. |
| `stash@{2}` | **Not ours** — predates this work. Debug signing config, a dropped-buffer counter in `host_gl.c`, and a temporary window-forcing knob. Leave alone. |

---

## Mistakes worth not repeating

- **Asserted causes from reasoning instead of measuring.** Called the tablet's
  level-load crash "pre-existing" when comparing against our own branch, not
  upstream — it was a genuine regression. Called the Nokia's missing models a
  "loading artifact" when they were real.
- **Probed the wrong variable twice.** `vertex_buffer_base_address` is never
  called; `hardware_format` is a D3D resource pointer, not a data address. Both
  rounds cost a device run and taught nothing.
- **Built and installed an APK while the sources were unstashed/stashed
  inconsistently** — produced a build that didn't match the tree. Check
  `git status` before building.
- **`git stash push` hit an unrelated stash** and produced merge conflicts. Check
  `git stash list` first; it now holds other people's work.
- **Repeated an unverified number** ("maps need `0x167C800`") into a commit
  message. It was wrong. The measurement that disproved it is above.

---

## Useful commands

```bash
# devices
export PATH="/mnt/c/Users/carlo/AppData/Local/Android/Sdk/platform-tools:$PATH"
adb.exe devices                      # note: Windows adb via WSL; paths must be Windows-style
adb.exe -s HA27WN3Z logcat -d | grep -E 'I halo|E halo'
adb.exe -s HA27WN3Z shell "grep -c 'EXCEPTION assert' /storage/emulated/0/Android/data/com.halo.decomp/files/debug.txt"

# build / install
ninja android_apk
cp port/android/app/build/outputs/apk/debug/app-debug.apk /mnt/c/Users/carlo/halo-test.apk
# adb.exe can't read WSL paths; drive it from a .bat on the Windows side

# CI status (no gh CLI here)
# https://api.github.com/repos/thelinkin3000/halo-ce-universal/actions/runs?per_page=1
```

**`asserts=0` and `crashes=0` on both devices is the bar.** A non-zero assert
count has twice pointed at the real bug (models reading as zeros: 46 asserts on
the Nokia, 0 on the tablet).
# Language-neutral subtitles for the OpenCE guest

Only source code and integration notes are included. No Portuguese captions,
translation lookup, patched ELF, NRO, game maps or audio are bundled.
This is a standalone C99 subtitle core adapted from the experimental subtitle
work. It is not yet wired into the upstream guest or verified on Switch.

## Native integration

Add `subtitles.c` to the guest build and include `subtitles.h`. It uses the C
runtime (malloc/realloc/free, qsort, memcpy/memset, strlen/strcmp/strchr/memchr)
and fixed-width integer types. No compiler-specific ABI or binary hooks are used.
Keep all calls on the same game thread; this API does not provide locking.

1. Create a context with `subtitles_create()` during game initialization.
2. Load the user-selected UTF-8 caption file using the port's filesystem code,
   then pass its bytes to `subtitles_load()`. The core copies the data. A failed
   load preserves the previous caption set. Handle the return value in the UI.
3. After `sound_new_impulse` successfully creates a sound, obtain its actual
   tag name, pitch range and permutation from the sound datum. Call
   `subtitles_sound_start()` with the sound-instance ID and current game time.
   Use the selected permutation, not a guessed index. Assign a higher priority
   to story/radio than combat if desired; the core has no language/path heuristic.
4. Notify `subtitles_sound_stop()` when a sound instance stops, including before
   its ID is reused. Call `subtitles_reset()` on level changes, save loads,
   seeks, or time discontinuities. Convert the original 30 Hz game clock to
   milliseconds in the integration, preserving monotonic time for the session.
5. Once per rendered frame (the earlier extension used `cinematic_render`),
   call `subtitles_render()` with a synchronous draw callback. Pass visible=0
   for disabled subtitles or menus; suppressing display still updates expiry.
6. Destroy the context on shutdown. Successful file reloads reset active voices.

The draw callback must use native HUD/font types and preserve/restore the
engine's font, scale, shadow and clipping state. The original binary experiment
needed the single-player messaging font (offset 0x54 in that exact guest);
use the named native field instead of copying a binary offset. Draw bounds,
panel, wrapping, scaling, split-screen, font fallback and settings belong to
the port's integration. No renderer or menu setting is included here.

## Caption format

Each nonempty, non-comment row contains six tab-separated columns:

`sound_tag | pitch_range | permutation | begin_ms | end_ms | text`

The `|` characters above describe column boundaries: actual files use tabs.
Sound tag paths must match exactly, including case and literal backslashes.
Offsets are relative to sound start, expressed in milliseconds. Several
nonoverlapping rows may describe successive captions for one sound key.
End is exclusive. Lines beginning with # are comments. UTF-8 BOM and CRLF are
accepted. Text supports escapes `\n`, `\r`, `\t` and `\\` (a literal backslash).
No caption file is included. Files can use any language; the core does not
choose the user's language, download captions or translate audio.

UTF-8 is decoded strictly into UTF-16, including supplementary characters.
Actual script coverage depends on the font and renderer. Right-to-left layout,
Arabic shaping, combining marks and other complex scripts require appropriate
engine support; UTF decoding alone does not implement text shaping.

Limits: 8 MiB input, 20,000 cues, four simultaneous tracked voices, one displayed
caption. Tag/pitch/permutation lookup uses a sorted table and binary search.
Higher priority wins; the newest sound breaks ties. Lower priority cannot evict
four active higher-priority sounds. Gaps allow another eligible voice to display.
Malformed input, overlapping captions for the same key, and allocation failures
reject the whole load. The file is external to the game assets and may be
replaced without rebuilding the core.

## Maintainer review

This source package was prepared with Codex assistance for integration review.
The generic core has local host tests; the native sound/draw calls must still be
connected and tested in the port. The earlier patched ELF's CPU/console results
do not constitute Switch validation of this rewritten core. Upstream code has
not been modified here, and compatibility with a particular current revision
is not asserted. Licensing for an upstream contribution should be agreed with
the contributor; this package asserts no license over upstream or game content.

## Changes since the first handoff

The experimental native renderer now uses a footer panel at Y=419..478 and text bounds Y=444..478, in the engine's 480-line reference coordinates. `subtitle_footer_layout` exposes these coordinates without depending on binary addresses. Preserve the engine's single-player messaging font, color, justification, clipping and shadow state around drawing. Width must be the engine's logical screen width, not the framebuffer width in physical pixels. Measure lines and grow the panel upward when a cue cannot fit: do not silently clip longer translations. The helper does not render, wrap, or scale the text.

`display.cinematic_letterbox=true` preserves the original look. Set false to remove only the overlays. Some scenes may expose geometry or animation that the authored bars covered. Keep cinematic state/clock unchanged.

Keep priority, stop notification, scenario reset and UTF-8 validation from the first handoff. Use user-selected language paths rather than hardcoded ptbr. Never bake a translated caption set into this core.

A separate Portuguese-only tutorial fallback from the binary experiment and all menu translations are intentionally excluded from this language-neutral contribution. Menu title artwork requires localization separately from text lookups.

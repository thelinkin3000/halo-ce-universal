# Android campaign checkpoints

The touch pause menu offers **Guardar y salir**. It persists the last completed
single-player checkpoint and closes the game only after the engine confirms the
write. It does not capture the exact position at which the button was pressed.
If there is no checkpoint yet, the game stays open and explains the problem.

On a cold launch, a saved checkpoint offers **Continuar**, **Nueva partida** and
**Cancelar**. Continue loads its map and difficulty without rewriting `init.txt`.
Starting a new game keeps the previous saved file until another successful save.
Returning to an already running game and multiplayer invitations bypass this
choice. A failed restore displays an error and requires exiting; it never claims
to have continued a newly started map.

The cold launcher offers **Descargar y jugar demo** when demo maps are missing.
It downloads the archived OXM promotional demo directly on Android, checks its
pinned size and SHA256, and adds only missing demo files. Existing maps and
settings are retained. With data installed, **Jugar demo** starts a new demo
without loading or replacing saved checkpoints. Demo startup journals `init.txt`
privately and temporarily replaces only its map command. Input stays blocked
until the engine acknowledges the matching `new` request, then the original
script is restored. An interrupted launch is recovered before the next cold
start. Existing game sessions must be saved and closed before changing to the
demo. No maps are bundled in the APK.

**Guardar y salir** offers up to ten named saves, or an explicitly confirmed
replacement of a selected save. Each archive atomically contains its name,
timestamp and checkpoint; payload CRC32 and SHA256 are checked. The previous
working checkpoint is retained before requesting another native save. The
app exits only after the matching native acknowledgement and successful
archival. **Jugar / Continuar** lists the saves with map, difficulty and date.
Selecting one preserves the working checkpoint before activating its copy.
Incompatible or damaged entries remain visible and cannot be continued.

## Compatibility and storage

The save slot is `save/z/mobile-checkpoint.bin` inside the app's external files
directory. Android uninstall or clearing app data removes it. Ordinary force
closing/backgrounding does not invoke Save and Quit.

The payload is a 20 MiB engine checkpoint image containing pointers. A save must
use the same native guest image, relocations, actual renderer and memory layout.
Changing the APK's native code or switching GLES/Vulkan can make it incompatible.
The launcher checks the asset fingerprint; the engine additionally checks CRC32,
image layout, map allocations, pools and arrays before running load callbacks.
Incompatible or damaged saves are retained and reported, never silently loaded.

Saving is supported only for local solo campaign scenarios with one player and
an existing checkpoint, outside cinematics. Multiplayer/co-op is excluded.

## File protocol

Java atomically publishes `mobile-checkpoint.request` or `.boot` under `save/z`:

```
<32 lowercase hex id> <save|continue|new> <GLES fingerprint> <Vulkan fingerprint>\n
```

Each fingerprint is SHA-256 over the guest ELF followed by its relocation bytes.
The game thread polls at most every 250 ms and writes an atomic `.status` file:

```
<id>\n<result>\n
```

Only a matching operation ID can complete the Java UI. Save waits up to 15 seconds;
Continue waits up to 120 seconds. Timeout/error keeps the save slot and rejects
late UI completion. The native write uses a temporary file, flush, fsync, close
and rename; a failed write does not replace the previous slot. No new JNI calls
or host imports are needed.

The little-endian 512-byte header contains `OCESAVE1`, version (offset 8), header
size (12), payload size (16), IEEE CRC32 (20), difficulty (24), renderer (28,
GLES=0/Vulkan=1), ASCII fingerprint (32, 64 bytes) and zero-terminated map name
(96, 256 bytes); remaining bytes are zero. The payload follows at offset 512.

## Validation

`tools/test_android_checkpoint.py` extracts the production checkpoint functions
and tests them with explicit engine/file-operation stubs, including failed writes,
corrupt images, renderer/layout mismatches and cold launch routing. Android host
tests cover the file protocol, pause/continue UI, timeouts and lifecycle guards.
These tests do not replace saving, closing and continuing on a physical device.

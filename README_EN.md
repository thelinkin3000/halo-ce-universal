# Language-neutral subtitle source v2

Source handoff for OpenCE, based on the repository checkout `78f3ed8` (Build 102).
No captions, Portuguese translation dictionary, game assets, ELF, NRO or maps are included.

## Contents

- `01_add_subtitle_core.patch`: standalone UTF-8/UTF-16 subtitle core from the earlier handoff plus the footer layout helper.
- `02_optional_cinematic_letterbox.patch`: optional stock-on setting for the black cinematic bars. It does not change FOV, camera, zoom or scripted cinematic state.
- `files/`: the same four source files for direct inspection.
- `INTEGRATION.md`: native integration points and remaining work.
- `tests/`: host checks of parsing, priority, stop, visibility, time and layout.

Apply from the repository root: `git apply --check 01_add_subtitle_core.patch`, then `git apply 01_add_subtitle_core.patch`. The optional patch is applied separately the same way. Do not copy the wrapper directory to the repository.

These patches are integration material, not a complete wired subtitle feature. The core is not added to the guest build by this patch, and no native renderer/sound calls are installed automatically. The optional setting must also be documented in the platform READMEs if accepted upstream. The code must be built and tested on every target selected by the maintainer.

The exact binary experiment was tested on Switch by the user: caption rendering, optional no-bars cutscenes, footer position, translated menus. This portable core and optional source patch are different code paths: those console observations do not validate this source integration. Host tests here do not verify the GPU or native ABI.

No new license grant is asserted over upstream or third-party material. Agree the source contribution license with the contributor before merging.

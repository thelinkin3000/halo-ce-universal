# Building

You do not need the Xbox SDK. The port supplies the SDK declarations that
the game uses. Refer to [port/include/xdk](../port/include/xdk/README.md).

To build the game:

1. Install Python and [ninja](https://ninja-build.org/).
2. Install the tools for your platform. Refer to the README for
   [Android](../port/android/README.md), the [Switch](../port/switch/README.md)
   or the [PS5](../port/ps5/README.md#building).
3. In the root folder of the repository, enter `python configure.py`. For the
   PS5, enter `python configure.py --ps5`.
4. Enter `ninja` with the target for the platform:

| Target | Result |
| --- | --- |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` |
| `ninja switch` | `build/switch/halo.nro` and `build/switch/halo_guest.elf` |
| `ninja ps5` | `build/ps5/title/PPSA99500` |

The Switch needs two toolchains and neither can be replaced by the other.
devkitPro builds the host (`devkitA64`, `libnx`, `switch-sdl2`,
`switch-mesa`, `switch-libdrm_nouveau`), and the Android NDK builds the game
image, which is ILP32 AArch64 - 32-bit pointers, because the Xbox data
formats embed them and have to keep their layout. devkitA64 cannot build it,
being LP64 only. So the Switch build wants the same NDK as the Android build,
found as it is there, plus devkitPro:

```
sudo dkp-pacman -S --needed devkitA64 libnx switch-sdl2 switch-mesa switch-libdrm_nouveau
```

`configure.py` finds devkitPro through `DEVKITPRO`, or `/opt/devkitpro`, or
`~/devkitpro`. Both files are needed to run: the NRO is the program, and the
ELF is the game image it loads from the card.

`tools/ci_build.py` makes the same builds as GitHub Actions. For example,
enter `python tools/ci_build.py android release`.

## Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--lto=thin`, `--lto=off` | Less link-time optimization. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--profile` | A profiling build, on Android. It records CPU times. Refer to "Profiling builds". |

## Optimization profiles

The Android and Switch builds use a profile of the game, in `pgo/`, to
optimize the code.

The profile needs clang 22 or later. With an older clang, the builds do not
use the profile.

## Profiling builds

A profiling build records where the game spends its time. Of the supported
platforms, it is available on Android.

1. Enter `python configure.py --profile`, then build as usual.
2. Start a recording. Enter `profile_record` in the console (or the telnet
   console), or set `debug.profile_record = true` in `config.toml`. To
   record each game of a session, also set
   `debug.profile_record_when = "game"`.
3. Stop the recording. Enter `profile_stop`, load another map, or quit.

The game writes numbered `.part<n>.json` files to
`/sdcard/Android/data/ar.com.slipstreamce/files/profiles/` and logs their
paths. Parts are retained after the recording stops. Enter `adb pull` to copy
them to a computer.

`tools/net_report.py` is a helper for developers. Enter
`python tools/net_report.py <recording.part1.json>` to read a recording
as tables. The command also writes `<recording>.summary.txt`, a short text
file.

| Command | Result |
| --- | --- |
| `profile_record [seconds]` | Starts a recording. With a number of seconds, the recording stops after that time. |
| `profile_stop` | Stops the recording. |

`debug.profile_memory` sets the memory of a recording in MB (default 256,
from 4 to 1024). A recording has no length limit:
the game writes a part each time the memory is full. Refer to
[docs/settings.md](settings.md).

A profiling build plays with normal builds.

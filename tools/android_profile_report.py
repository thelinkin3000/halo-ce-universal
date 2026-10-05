#!/usr/bin/env python3
"""Reads profile.bin (debug.profile_hz; port/android/host/host_debug.c) and prints where the time went.

    adb pull /sdcard/Android/data/com.halo.decomp/files/profile.bin
    tools/android_profile_report.py profile.bin [--logcat log.txt] [--elf build/android/halo_guest.elf]

Per guest thread, the share of its samples in:

  game      guest code outside the renderer
  renderer  the GL ES renderer (d3d8_gl.c, xbox_textures.c, nv2a_*.c)
  boundary  the guest's import stubs and the host's forwarding (guest_gl.c, libmain.so, the stubs)
  driver    the GL driver: the vendor's libGLES*/libEGL and what it calls (libc, the vdso, other vendor libraries)
  other     everything else (SDL's audio thread, network, the system's libraries) and the time a thread
            spends blocked in a system call (a sample taken right after an svc instruction: idle in the kernel)

and its CPU time per second (schedstat). With a logcat file that holds the renderer's
"frame N:" lines (debug.gpu_stats), the frames per second, and so the milliseconds of CPU per
frame of each thread.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

GUEST_BASE = 0x40000000
GUEST_END = GUEST_BASE + 0x01000000
SAMPLE = struct.Struct("<IIQQ8Q")  # tid, reserved, pc, lr, 8 frames
CPU = struct.Struct("<IIQ")
NAME = struct.Struct("<II16s")
HEADER = struct.Struct("<8sIIII")

RENDERER_FILES = ("d3d8_gl.c", "xbox_textures.c", "nv2a_vsh.c", "nv2a_psh.c", "nv2a_")
BOUNDARY_FILES = ("guest_gl.c", "guest_posix.c", "imports.s")
DRIVER_HINTS = ("libGLES", "libEGL", "libvulkan", "/vendor/", "libgsl", "libadreno", "libllvm", "libc.so", "libm.so",
                "libdl.so", "[vdso]", "libcutils", "libnativewindow", "libui.so", "libgui", "libsync", "libbase",
                "libhardware", "libutils", "liblog.so", "libc++", "libandroid.so")


def read_profile(path):
    data = Path(path).read_bytes()
    magic, hz, sample_size, maps_size, _ = HEADER.unpack_from(data, 0)
    if not magic.startswith(b"HPRF1"):
        sys.exit("not a profile.bin")
    if sample_size != SAMPLE.size:
        sys.exit(f"sample size {sample_size}, expected {SAMPLE.size}")
    offset = HEADER.size
    maps_text = data[offset:offset + maps_size].decode(errors="replace")
    offset += maps_size
    samples, cpu, names = [], [], {}
    block_index = 0
    while offset + 8 <= len(data):
        kind, count = struct.unpack_from("<II", data, offset)
        offset += 8
        if kind == 1:
            size = SAMPLE.size
            for _ in range(count):
                if offset + size > len(data):
                    break
                samples.append(SAMPLE.unpack_from(data, offset) + (block_index,))
                offset += size
            block_index += 1
        elif kind == 2:
            for _ in range(count):
                if offset + CPU.size > len(data):
                    break
                cpu.append(CPU.unpack_from(data, offset))
                offset += CPU.size
        elif kind == 3:
            for _ in range(count):
                if offset + NAME.size > len(data):
                    break
                tid, _, raw = NAME.unpack_from(data, offset)
                names[tid] = raw.split(b"\0")[0].decode(errors="replace")
                offset += NAME.size
        else:
            break
    return hz, maps_text, samples, cpu, names


def parse_maps(text):
    ranges = []
    for line in text.splitlines():
        if line.startswith("#dl "):
            _, base, *name = line.split(" ", 2)
            DL_OBJECTS.append((int(base, 16), name[0] if name else ""))
            continue
        match = re.match(r"([0-9a-f]+)-([0-9a-f]+) \S+ \S+ \S+ \S+\s*(.*)", line)
        if match:
            ranges.append((int(match.group(1), 16), int(match.group(2), 16), match.group(3).strip()))
    return ranges


def symbolize(addresses, elf):
    """address -> (function, file) for guest addresses, with llvm-symbolizer."""
    result = {}
    if not addresses:
        return result
    tools = ["llvm-symbolizer", "llvm-symbolizer-18", "llvm-symbolizer-19"]
    ndk = os.environ.get("ANDROID_NDK_HOME")
    if ndk:  # the NDK's own, which every Android build here has
        tools += [str(p) for p in Path(ndk).glob("toolchains/llvm/prebuilt/*/bin/llvm-symbolizer")]
    process = None
    for tool in tools:
        try:
            process = subprocess.run([tool, f"--obj={elf}", "--functions=short", "--demangle"] + [hex(a) for a in addresses],
                                     capture_output=True, text=True, check=True)
            break
        except (OSError, subprocess.CalledProcessError):
            process = None
    if process is None:
        print("llvm-symbolizer not found: guest samples are all counted as 'game'", file=sys.stderr)
        return result
    blocks = process.stdout.strip().split("\n\n")
    for address, block in zip(addresses, blocks):
        lines = block.strip().splitlines()
        function = lines[0] if lines else "?"
        location = lines[1] if len(lines) > 1 else "?"
        result[address] = (function, location.rsplit(":", 2)[0].split("/")[-1])
    return result


def parse_linker_map(path):
    """(start, end, object file name) of every input .text section of the guest image, from lld's -Map."""
    sections = []
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return sections
    for line in lines:
        match = re.match(r"\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S+\.o):\(\.text", line)
        if match:
            start, size = int(match.group(1), 16), int(match.group(2), 16)
            sections.append((start, start + size, match.group(3).rsplit("/", 1)[-1]))
    sections.sort()
    return sections


def object_of(pc, sections):
    import bisect
    index = bisect.bisect_right(sections, (pc, 1 << 64, "")) - 1
    if index >= 0 and sections[index][0] <= pc < sections[index][1]:
        return sections[index][2]
    return "?"


GENERIC_LIBS = ("libc.so", "libm.so", "libdl.so", "[vdso]", "libc++", "libbase", "libutils", "liblog.so", "libcutils")


DL_OBJECTS = []  # (base, name) of every loaded object, from the "#dl" lines the profiler appends to the maps


_CACHE = {}


def lib_of(pc, maps):
    key = ("lib", pc >> 12)
    if key not in _CACHE:
        _CACHE[key] = _lib_of(pc & ~0xfff, maps)
    return _CACHE[key]


def _lib_of(pc, maps):
    for start, end, path in maps:
        if start <= pc < end:
            if path.endswith(".apk") or not path:
                # mapped from the APK: the loaded object whose base is the nearest below
                best = ""
                for base, name in sorted(DL_OBJECTS):
                    if base <= pc and pc - base < (1 << 28):
                        best = name
                return best or path
            return path
    return ""


def owner(pc, maps, sections):
    key = ("owner", pc)
    if key not in _CACHE:
        _CACHE[key] = _owner(pc, maps, sections)
    return _CACHE[key]


def _owner(pc, maps, sections):
    """The category of one address: guest code by its object file, the host library, a vendor or system library."""
    if GUEST_BASE <= pc < GUEST_END:
        name = object_of(pc, sections)
        if name in ("guest_gl.o", "guest_posix.o", "imports.o"):
            return "boundary"
        if name in ("d3d8_gl.o", "xbox_textures.o", "nv2a_vsh.o", "nv2a_psh.o"):
            return "renderer"
        return "game"
    name = lib_of(pc, maps)
    if not name:
        return None  # not code of any mapping (a garbled frame): the next one on the chain
    if "libmain.so" in name:
        return "boundary"
    if "libSDL3" in name:
        return "other"
    if any(g in name for g in GENERIC_LIBS):
        return None  # a library function: its caller says whose time it is
    if any(h in name for h in DRIVER_HINTS):
        return "driver"
    return "other"


def classify(pc, chain, maps, sections):
    """The category of a sample: its own address, or for a generic library function (libc and the like) the
    first caller on the frame chain that is not one (the link register, then the recorded return addresses);
    a library function whose callers are not to be found (system libraries are built without frame pointers, so
    the chain past them is often garbage) counts as the driver's, which is where most of them are called from."""
    for address in (pc, *chain):
        if not address:
            continue
        kind = owner(address, maps, sections)
        if kind:
            return kind
    return "driver"


def logcat_fps(path, start=0.0, end=1e9):
    """Frames per second from the renderer's 'frame N:' lines of a logcat file (time-stamped), between
    `start` and `end` seconds after the profiler started."""
    points = []
    origin = None
    for line in Path(path).read_text(errors="replace").splitlines():
        match = re.match(r"\d+-\d+ (\d+):(\d+):([\d.]+).*(profiling guest threads|frame (\d+):)", line)
        if not match:
            continue
        seconds = int(match.group(1)) * 3600 + int(match.group(2)) * 60 + float(match.group(3))
        if match.group(4) == "profiling guest threads":
            origin = seconds
        elif origin is not None and start <= seconds - origin <= end:
            points.append((seconds, int(match.group(5))))
    if len(points) < 2 or points[-1][0] <= points[0][0]:
        return None
    return (points[-1][1] - points[0][1]) / (points[-1][0] - points[0][0])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile")
    parser.add_argument("--elf", default="build/android/halo_guest.elf")
    parser.add_argument("--logcat", help="logcat output with the renderer's frame lines, for frames per second")
    parser.add_argument("--fps", type=float, help="frames per second, if known")
    parser.add_argument("--from", dest="first", type=int, default=0, help="only from this second of the run (a multiple of 10)")
    parser.add_argument("--to", dest="last", type=int, default=10**9, help="only up to this second of the run")
    args = parser.parse_args()

    hz, maps_text, samples, cpu, names = read_profile(args.profile)
    samples = [x for x in samples if args.first <= x[-1] * 10 < args.last]
    cpu = [c for c in cpu if args.first <= c[1] <= args.last]
    maps = parse_maps(maps_text)
    sections = parse_linker_map(args.elf + ".map")
    if not sections:
        print(f"no linker map at {args.elf}.map: guest code is all counted as 'game'", file=sys.stderr)
    guest = sorted({x[2] for x in samples if GUEST_BASE <= x[2] < GUEST_END and not x[1]})
    symbols = symbolize(guest, args.elf)
    fps = args.fps or (logcat_fps(args.logcat, args.first, args.last) if args.logcat else None)

    per_thread = defaultdict(Counter)
    top = defaultdict(Counter)
    for tid, in_syscall, pc, lr, *_rest in samples:
        kind = "other" if in_syscall else classify(pc, (lr, *_rest[:-1]), maps, sections)
        per_thread[tid][kind] += 1
        if in_syscall:
            top[tid]["(in a system call: blocked)"] += 1
        elif GUEST_BASE <= pc < GUEST_END:
            top[tid][symbols.get(pc, ("?", "?"))[0]] += 1
        else:
            for start, end, path in maps:
                if start <= pc < end:
                    top[tid][path.split("/")[-1] or "[anon]"] += 1
                    break
    cpu_by_thread = defaultdict(list)
    for tid, second, nanoseconds in sorted(cpu, key=lambda c: c[1]):
        cpu_by_thread[tid].append((second, nanoseconds))

    print(f"{len(samples)} samples at {hz} Hz; frames per second: {fps:.1f}" if fps else f"{len(samples)} samples at {hz} Hz")
    kinds = ("game", "renderer", "boundary", "driver", "other")
    print(f"{'thread':<24}{'samples':>8}" + "".join(f"{k:>10}" for k in kinds) + f"{'cpu ms/s':>10}" + (f"{'cpu ms/frame':>14}" if fps else ""))
    for tid in sorted(per_thread, key=lambda t: -sum(per_thread[t].values())):
        counts = per_thread[tid]
        total = sum(counts.values())
        series = cpu_by_thread.get(tid, [])
        rate = 0.0
        if len(series) >= 2:
            span = series[-1][0] - series[0][0]
            if span > 0:
                rate = (series[-1][1] - series[0][1]) / 1e6 / span
        label = f"{names.get(tid, '?')} ({tid})"
        line = f"{label:<24}{total:>8}" + "".join(f"{100.0 * counts[k] / total:>9.1f}%" for k in kinds) + f"{rate:>10.1f}"
        if fps:
            line += f"{rate / fps:>14.2f}"
        print(line)
    print()
    for tid in sorted(per_thread, key=lambda t: -sum(per_thread[t].values()))[:3]:
        total = sum(per_thread[tid].values())
        print(f"{names.get(tid, '?')} ({tid}): most sampled")
        for what, count in top[tid].most_common(8):
            print(f"  {100.0 * count / total:5.1f}%  {what}")


if __name__ == "__main__":
    main()

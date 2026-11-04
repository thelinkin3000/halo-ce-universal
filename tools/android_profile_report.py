#!/usr/bin/env python3
"""Reads profile.bin (debug.profile_hz; port/android/host/host_debug.c) and prints where the time went.

    adb pull /sdcard/Android/data/ar.com.slipstreamce/files/profile.bin
    tools/android_profile_report.py profile.bin [--logcat log.txt] [--elf build/android/halo_guest.elf]

or a profiled run's folder (debug.profile_run: Download/halo_profiles/DDMMYYYY_HHMMSS on the phone, sent as it is):

    tools/android_profile_report.py 23102026_201500/

which holds the profile, the run's log (halo.log, read for where the game image was loaded) and run.txt (whose
renderer chooses the image, halo_guest_vk.elf under Vulkan); gpu.txt in it is the GPU's time by part of the frame,
to read as it is. The image must be the build's that made the profile: --apk takes it out of that build's APK (a
release's, from its zip), where it is whole; without the build's linker map (halo_guest_vk.elf.map, beside a local
build's image), guest code is all counted as 'game', its functions still named.

Per guest thread, the share of its samples in:

  game      guest code outside the renderer
  renderer  the GL ES renderer (d3d8_gl.c, xbox_textures.c, nv2a_*.c)
  boundary  the guest's import stubs and the host's forwarding (guest_gl.c, libmain.so, the stubs)
  driver    the GL driver: the vendor's libGLES*/libEGL and what it calls (libc, the vdso, other vendor libraries)
  other     everything else (SDL's audio thread, network, the system's libraries) and the time a thread
            spends blocked in a system call (a sample taken right after an svc instruction: idle in the kernel)

and its CPU time per second (schedstat). The frames per second, and so the milliseconds of CPU per frame of each
thread, from the frames the profile kept (the Vulkan renderer's presents), or from a logcat file that holds the
renderer's "frame N:" lines (debug.gpu_stats). The slowest frames are listed with where the threads were during each.
"""

import argparse
import bisect
import os
import tempfile
import zipfile
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

# where the guest image is in the profiled process (main sets them: the image's own link address and size from the
# ELF, moved to where the logcat's "guest image" line says the host loaded it), and how far it was moved
GUEST_BASE = 0x3c000000
GUEST_END = GUEST_BASE + 0x03000000
GUEST_SHIFT = 0
SAMPLE = struct.Struct("<IIQQ8Q")  # tid, reserved, pc, lr, 8 frames
CPU = struct.Struct("<IIQ")
FRAME = struct.Struct("<QII")
NAME = struct.Struct("<II16s")
HEADER = struct.Struct("<8sIIII")

RENDERER_FILES = ("d3d8_gl.c", "xbox_textures.c", "nv2a_vsh.c", "nv2a_psh.c", "nv2a_", "d3d8_vk.c", "xbox_textures_vk.c")
RENDERER_OBJECTS = ("d3d8_gl.o", "xbox_textures.o", "nv2a_vsh.o", "nv2a_psh.o", "d3d8_vk.o", "xbox_textures_vk.o",
                    "nv2a_vsh_vk.o", "nv2a_psh_vk.o")
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
    samples, cpu, names, frames = [], [], {}, []
    block_index = 0
    while offset + 8 <= len(data):
        kind, count = struct.unpack_from("<II", data, offset)
        offset += 8
        if kind == 1:
            size = SAMPLE.size
            for _ in range(count):
                if offset + size > len(data):
                    break
                # (its number in the run, which a frame's end names: its last sample's)
                samples.append(SAMPLE.unpack_from(data, offset) + (len(samples), block_index))
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
        elif kind == 4:
            for _ in range(count):
                if offset + FRAME.size > len(data):
                    break
                ended, microseconds, _ = FRAME.unpack_from(data, offset)
                frames.append((ended, microseconds))
                offset += FRAME.size
        else:
            break
    return hz, maps_text, samples, cpu, names, frames


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
    tools = ["llvm-symbolizer", "llvm-symbolizer-22", "llvm-symbolizer-18", "llvm-symbolizer-19"]
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
    nearest = None
    for address, block in zip(addresses, blocks):
        lines = block.strip().splitlines()
        function = lines[0] if lines else "?"
        location = lines[1] if len(lines) > 1 else "?"
        # (the image's symbols have no size, so the symbolizer seldom places an address in one: the nearest symbol
        # at or below it, which is the function for code laid out one function after another)
        if function in ("??", "?"):
            if nearest is None:
                nearest = symbol_table(tool, elf)
            index = bisect.bisect_right(nearest[0], address) - 1
            if index >= 0:
                function = nearest[1][index]
        result[address] = (function, location.rsplit(":", 2)[0].split("/")[-1])
    return result


def symbol_table(symbolizer, elf):
    """(addresses, names) of the image's code symbols, sorted, from llvm-nm beside the symbolizer that worked."""
    nm = str(Path(symbolizer).with_name(Path(symbolizer).name.replace("llvm-symbolizer", "llvm-nm")))
    addresses, names = [], []
    for tool in (nm, "llvm-nm", "llvm-nm-22", "llvm-nm-18", "nm"):
        try:
            output = subprocess.run([tool, "-n", "--defined-only", elf], capture_output=True, text=True,
                                    check=True).stdout
            break
        except (OSError, subprocess.CalledProcessError):
            output = ""
    for line in output.splitlines():
        parts = line.split()
        # (code only: a data symbol or a literal's label is never a function)
        if len(parts) == 3 and parts[1] in "TtNn" and not parts[2].startswith("."):
            addresses.append(int(parts[0], 16))
            names.append(parts[2])
    return addresses, names


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
    pc -= GUEST_SHIFT
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
        if name in RENDERER_OBJECTS:
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


def elf_extent(path):
    """(lowest address, highest end) of the ELF's loadable segments: where the image was linked."""
    data = Path(path).read_bytes()
    phoff, = struct.unpack_from("<Q", data, 0x20) if data[4] == 2 else struct.unpack_from("<I", data, 0x1c)
    if data[4] == 2:
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    else:
        phentsize, phnum = struct.unpack_from("<HH", data, 0x2a)
    low, high = None, 0
    for index in range(phnum):
        at = phoff + index * phentsize
        if data[4] == 2:
            kind, _, _, vaddr, _, _, memsz = struct.unpack_from("<IIQQQQQ", data, at)
        else:
            kind, _, vaddr, _, _, memsz = struct.unpack_from("<IIIIII", data, at)
        if kind == 1:
            low = vaddr if low is None else min(low, vaddr)
            high = max(high, vaddr + memsz)
    return low, high


def logcat_image_base(path):
    """Where the host loaded the guest image, from its "guest image X-Y" line (the last, if the run started again)."""
    base = None
    for line in Path(path).read_text(errors="replace").splitlines():
        match = re.search(r"guest image ([0-9a-f]+)-[0-9a-f]+, \d+ imports", line)
        if match:
            base = int(match.group(1), 16)
    return base


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile", help="profile.bin, or a profiled run's folder (debug.profile_run)")
    parser.add_argument("--elf", help="the game image (default: build/android/halo_guest_vk.elf, or halo_guest.elf "
                        "for a run on GL ES)")
    parser.add_argument("--logcat", help="logcat output with the renderer's frame lines, for frames per second")
    parser.add_argument("--fps", type=float, help="frames per second, if known")
    parser.add_argument("--from", dest="first", type=int, default=0, help="only from this second of the run (a multiple of 10)")
    parser.add_argument("--to", dest="last", type=int, default=10**9, help="only up to this second of the run")
    parser.add_argument("--apk", help="the APK of the build that made the profile: the game image is taken from it")
    parser.add_argument("--slow", type=int, default=8, help="how many of the slowest frames over 33 ms to list (0: none)")
    args = parser.parse_args()

    folder = Path(args.profile) if Path(args.profile).is_dir() else None
    if folder:
        args.profile = str(folder / "profile.bin")
        if not args.logcat and (folder / "halo.log").is_file():
            args.logcat = str(folder / "halo.log")
        run = (folder / "run.txt").read_text(errors="replace") if (folder / "run.txt").is_file() else ""
        for line in run.splitlines()[:4]:
            print(line, file=sys.stderr)
        if not args.elf:
            args.elf = "build/android/halo_guest.elf" if "renderer: GL ES" in run else "build/android/halo_guest_vk.elf"
    if args.apk:
        # (the image the renderer ran: the folder's run.txt says which, else Vulkan's)
        wanted = Path(args.elf).name if args.elf else "halo_guest_vk.elf"
        unpacked = tempfile.mkdtemp(prefix="halo_profile_")
        with zipfile.ZipFile(args.apk) as apk:
            args.elf = apk.extract(f"assets/{wanted}", unpacked)
    if not args.elf:
        args.elf = "build/android/halo_guest_vk.elf"

    global GUEST_BASE, GUEST_END, GUEST_SHIFT
    linked, linked_end = elf_extent(args.elf)
    loaded = (logcat_image_base(args.logcat) if args.logcat else None) or linked
    GUEST_SHIFT = loaded - linked
    GUEST_BASE, GUEST_END = loaded, loaded + (linked_end - linked)
    if GUEST_SHIFT:
        print(f"the guest image was loaded at {loaded:08x}, {GUEST_SHIFT:+x} from where it was linked", file=sys.stderr)
    elif not args.logcat:
        print(f"no --logcat: the guest image is taken to be where it was linked ({linked:08x})", file=sys.stderr)

    hz, maps_text, samples, cpu, names, frames = read_profile(args.profile)
    every_sample = samples
    samples = [x for x in samples if args.first <= x[-1] * 10 < args.last]
    cpu = [c for c in cpu if args.first <= c[1] <= args.last]
    maps = parse_maps(maps_text)
    sections = parse_linker_map(args.elf + ".map")
    if not sections:
        print(f"no linker map at {args.elf}.map: guest code is all counted as 'game'", file=sys.stderr)
    guest = sorted({x[2] for x in samples if GUEST_BASE <= x[2] < GUEST_END and not x[1]})
    # (named where the image was linked)
    named = symbolize([pc - GUEST_SHIFT for pc in guest], args.elf)
    symbols = {pc: named[pc - GUEST_SHIFT] for pc in guest if pc - GUEST_SHIFT in named}
    fps = args.fps
    if not fps and len(frames) > 1:
        fps = 1e6 * len(frames) / max(1, sum(microseconds for _, microseconds in frames))
    if not fps and args.logcat:
        fps = logcat_fps(args.logcat, args.first, args.last)

    per_thread = defaultdict(Counter)
    top = defaultdict(Counter)
    for tid, in_syscall, pc, lr, *_rest in samples:
        kind = "other" if in_syscall else classify(pc, (lr, *_rest[:-2]), maps, sections)
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

    if frames and args.slow:
        slow_frames(frames, every_sample, names, maps, args)


def slow_frames(frames, samples, names, maps, args):
    """The slowest frames, each with where the threads were while it was made: the samples taken between the end of
    the frame before it and its own (each frame keeps how many samples had been taken by its end)."""
    lengths = sorted(microseconds for _, microseconds in frames)
    print()
    print(f"{len(frames)} frames: {lengths[len(lengths) // 2] / 1000:.1f} ms the median, "
          f"{lengths[int(len(lengths) * 0.99)] / 1000:.1f} ms the 99th percentile, {lengths[-1] / 1000:.1f} ms the longest; "
          f"{sum(1 for x in lengths if x > 33333)} over 33 ms")
    order = [index for index in sorted(range(1, len(frames)), key=lambda index: -frames[index][1])
             if frames[index][1] > 33333][:args.slow]
    by_number = {sample[-2]: sample for sample in samples}
    wanted = set()
    for index in order:
        wanted.update(range(frames[index - 1][0], frames[index][0]))
    guest = sorted({by_number[n][2] for n in wanted if n in by_number and GUEST_BASE <= by_number[n][2] < GUEST_END})
    named = symbolize([pc - GUEST_SHIFT for pc in guest], args.elf)
    for index in sorted(order):
        start, end = frames[index - 1][0], frames[index][0]
        print(f"frame {index}: {frames[index][1] / 1000:.1f} ms, {end - start} samples")
        where = defaultdict(Counter)
        for number in range(start, end):
            sample = by_number.get(number)
            if not sample:
                continue
            tid, in_syscall, pc = sample[0], sample[1], sample[2]
            if in_syscall:
                what = "(in a system call: blocked)"
            elif GUEST_BASE <= pc < GUEST_END:
                what = named.get(pc - GUEST_SHIFT, ("?", "?"))[0]
            else:
                what = next((path.split("/")[-1] or "[anon]" for low, high, path in maps if low <= pc < high), "?")
            where[tid][what] += 1
        for tid in sorted(where, key=lambda t: -sum(where[t].values()))[:3]:
            total = sum(where[tid].values())
            top = ", ".join(f"{what} {100 * count // total}%" for what, count in where[tid].most_common(4))
            print(f"  {names.get(tid, '?')} ({tid}): {top}")


if __name__ == "__main__":
    main()

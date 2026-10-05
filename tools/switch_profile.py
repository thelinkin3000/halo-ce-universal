#!/usr/bin/env python3
"""Read the Switch port's profiles: where the game's threads spend their time.

config.toml's debug.profiler = true (port/switch/host/host_debug.c) has the
console sample each of the guest's threads debug.profile_hz times a second (500) and write the
counts to /switch/halo/profile/profile_NNN.txt, one file every 20 seconds. This
pulls them off the card (--fetch), turns their addresses into functions with
the build's own images, and prints, per thread, the functions the samples were
in (self) and the functions they were under (inclusive).

Usage: python3 tools/switch_profile.py --fetch           # every file on the card
       python3 tools/switch_profile.py --fetch 3 4 5     # windows 3 to 5 only
       python3 tools/switch_profile.py profile_004.txt   # files already here
       ... --thread all   (default: the game thread)
       ... --top 40       (rows per table)
       ... --folded out.txt   (collapsed stacks, for flamegraph.pl or speedscope)

The images must be the ones the console ran (build/switch, as deployed): a
profile of another build names the wrong functions, and nothing can tell.
"""

import argparse
import ftplib
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "switch"
LOCAL = Path("/tmp/halo-logs/profile")
TOOLS = Path("/opt/devkitpro/devkitA64/bin")
DEFAULT_HOST = "192.168.2.173"
DEFAULT_PORT = 5000
REMOTE = "/switch/halo/profile"


def fetch(host, port, windows):
    LOCAL.mkdir(parents=True, exist_ok=True)
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=10)
    ftp.login()
    names = sorted(name.rsplit("/", 1)[-1] for name in ftp.nlst(REMOTE))
    names = [name for name in names if re.match(r"profile_\d+\.txt$", name)]
    if windows:
        names = [name for name in names if int(name[8:11]) in windows]
    paths = []
    for name in names:
        path = LOCAL / name
        with open(path, "wb") as out:
            ftp.retrbinary(f"RETR {REMOTE}/{name}", out.write)
        paths.append(path)
    ftp.quit()
    print(f"fetched {len(paths)} profile(s) to {LOCAL}", file=sys.stderr)
    return paths


def parse(path):
    profile = {"threads": {}, "stacks": [], "image": None, "anchor": None, "seconds": 0.0, "hz": 0, "dropped": 0}
    for line in path.read_text().splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "window":
            profile["seconds"] = float(words[3])
            profile["hz"] = int(words[5])
        elif words[0] == "image":
            profile["image"] = words[1]
        elif words[0] == "anchor":
            profile["anchor"] = (words[1], int(words[2], 16))
        elif words[0] == "dropped":
            profile["dropped"] = int(words[1])
        elif words[0] == "thread":
            profile["threads"][int(words[1])] = {"kind": words[2], "samples": int(words[6]), "failed": int(words[8])}
        elif words[0] == "stack":
            profile["stacks"].append((int(words[1]), int(words[2]), [int(word, 16) for word in words[3:]]))
    return profile


def tool(name):
    path = TOOLS / f"aarch64-none-elf-{name}"
    return str(path) if path.exists() else name


def load_ranges(elf):
    """the executable LOAD segments of an image, as (start, end) pairs"""
    output = subprocess.run([tool("readelf"), "-lW", str(elf)], capture_output=True, text=True, check=True).stdout
    ranges = []
    for line in output.splitlines():
        words = line.split()
        if words and words[0] == "LOAD" and "E" in words[6:-1]:
            start, size = int(words[2], 16), int(words[5], 16)
            ranges.append((start, start + size))
    return ranges


def symbol_address(elf, name):
    output = subprocess.run([tool("nm"), str(elf)], capture_output=True, text=True, check=True).stdout
    for line in output.splitlines():
        words = line.split()
        if len(words) == 3 and words[2] == name:
            return int(words[0], 16)
    return None


def symbolize(elf, addresses):
    """function names for addresses in an image, by addr2line in one call"""
    addresses = sorted(addresses)
    if not addresses:
        return {}
    text = "\n".join(f"0x{address:x}" for address in addresses) + "\n"
    output = subprocess.run([tool("addr2line"), "-f", "-C", "-e", str(elf)], input=text, capture_output=True,
                            text=True, check=True).stdout.splitlines()
    return {address: (output[index * 2] if index * 2 < len(output) else "??")
            for index, address in enumerate(addresses)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", help="profile files, or window numbers with --fetch")
    parser.add_argument("--fetch", action="store_true", help="pull the profiles off the console first")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--thread", default="game", help="game (default), all, or a thread slot number")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--folded", type=Path, help="also write collapsed stacks here")
    args = parser.parse_args()

    if args.fetch:
        paths = fetch(args.host, args.port, {int(word) for word in args.files})
    else:
        paths = [Path(word) for word in args.files] or sorted(LOCAL.glob("profile_*.txt"))
    if not paths:
        print("no profiles (set profiler = true under [debug] in config.toml, play, then --fetch)", file=sys.stderr)
        return 1
    profiles = [parse(path) for path in paths]

    image = BUILD / profiles[0]["image"]
    host_elf = BUILD / "halo.elf"
    guest_ranges = load_ranges(image)
    anchor_name, anchor_runtime = profiles[0]["anchor"]
    anchor_link = symbol_address(host_elf, anchor_name)
    host_shift = anchor_runtime - anchor_link if anchor_link is not None else None
    if any(profile["anchor"] != profiles[0]["anchor"] or profile["image"] != profiles[0]["image"]
           for profile in profiles):
        print("warning: these profiles are from more than one run; the host's addresses are read with the first's",
              file=sys.stderr)

    # which samples, and their stacks with the return addresses moved into the call
    wanted = []
    threads = defaultdict(lambda: {"samples": 0, "failed": 0, "kind": "?"})
    seconds = 0.0
    dropped = 0
    for profile in profiles:
        seconds += profile["seconds"]
        dropped += profile["dropped"]
        for slot, thread in profile["threads"].items():
            threads[slot]["samples"] += thread["samples"]
            threads[slot]["failed"] += thread["failed"]
            threads[slot]["kind"] = thread["kind"]
        for count, slot, stack in profile["stacks"]:
            kind = profile["threads"].get(slot, {}).get("kind")
            if args.thread == "all" or (args.thread == "game" and kind == "game") or args.thread == str(slot):
                wanted.append((count, slot, [stack[0]] + [address - 4 for address in stack[1:]]))

    def where(address):
        if any(start <= address < end for start, end in guest_ranges):
            return "guest", address
        if host_shift is not None:
            return "host", address - host_shift
        return None, address

    by_image = defaultdict(set)
    for _, _, stack in wanted:
        for address in stack:
            kind, linked = where(address)
            if kind:
                by_image[kind].add(linked)
    names = {}
    for kind, addresses in by_image.items():
        for linked, name in symbolize(image if kind == "guest" else host_elf, addresses).items():
            names[(kind, linked)] = name if kind == "guest" else f"[host] {name}"

    def name_of(address):
        kind, linked = where(address)
        return names.get((kind, linked), f"0x{address:x}") if kind else f"0x{address:x}"

    total = sum(count for count, _, _ in wanted)
    print(f"{len(profiles)} window(s), {seconds:.0f} s, {profiles[0]['hz']} Hz, image {profiles[0]['image']}"
          f"{f', {dropped} stacks dropped (table full)' if dropped else ''}")
    for slot in sorted(threads):
        thread = threads[slot]
        print(f"  thread {slot} ({thread['kind']}): {thread['samples']} samples"
              f"{f', {thread['failed']} failed' if thread['failed'] else ''}")
    print(f"selected ({args.thread}): {total} samples\n")
    if not total:
        return 0

    self_counts = Counter()
    inclusive = Counter()
    folded = Counter()
    for count, slot, stack in wanted:
        frames = [name_of(address) for address in stack]
        self_counts[frames[0]] += count
        for name in set(frames):
            inclusive[name] += count
        folded[";".join(reversed(frames))] += count

    print(f"{'self':>7} {'%':>6}  function (where the samples were)")
    for name, count in self_counts.most_common(args.top):
        print(f"{count:7d} {100.0 * count / total:5.1f}%  {name}")
    print(f"\n{'incl':>7} {'%':>6}  function (the samples under it)")
    for name, count in inclusive.most_common(args.top):
        print(f"{count:7d} {100.0 * count / total:5.1f}%  {name}")
    if args.folded:
        args.folded.write_text("".join(f"{stack} {count}\n" for stack, count in folded.most_common()))
        print(f"\ncollapsed stacks written to {args.folded}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

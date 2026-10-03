"""Turn the addresses in halo.log into names.

Two images are involved and they want different files. The host is the NRO
itself, loaded wherever the launcher put it, so an address in the log is the
host's file address already. The guest is a separate ELF that the host loads at
a fixed address (GUEST_BASE), so a guest address has that taken off before it
will mean anything to nm.

    python3 tools/symbolize.py host 0x1234 0x5678
    python3 tools/symbolize.py guest 0x4001234

Under -O2 and without a line table for the guest, this names functions and
nothing finer. For the host the line table is there, so -l is worth passing:

    python3 tools/symbolize.py host -l 0x1234
"""

import bisect
import subprocess
import sys
from pathlib import Path

BUILD = Path("build/switch")
HOST_ELF = BUILD / "halo.elf"
GUEST_ELF = BUILD / "halo_guest.elf"

# Where the host loads the guest image. Set in port/switch/host/host_loader.c.
GUEST_BASE = 0x40000000


def symbols(elf: Path):
    """Sorted (address, name) for the ELF's defined text symbols."""
    out = subprocess.run(
        ["aarch64-linux-gnu-nm", "-C", "--defined-only", "-n", str(elf)],
        capture_output=True, text=True, check=True).stdout
    found = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in "tTwW":
            found.append((int(parts[0], 16), parts[2]))
    found.sort()
    return found


def lookup(table, address: int):
    index = bisect.bisect_right(table, (address, chr(0x10FFFF))) - 1
    if index < 0:
        return None, 0
    start, name = table[index]
    return name, address - start


def main(argv):
    which = argv[0] if argv else "host"
    rest = argv[1:] if argv else []
    lines = False
    if "-l" in rest:
        lines = True
        rest.remove("-l")

    if which == "guest":
        # No adjustment: the guest image is linked at its load address, so
        # .text starts at 0x40000000 in the ELF already and an address out of
        # the log is an address in the ELF.
        elf, adjust = GUEST_ELF, 0
    else:
        elf, adjust = HOST_ELF, 0

    if not elf.exists():
        sys.exit("missing %s - build first (ninja switch)" % elf)

    table = symbols(elf)
    for text in rest:
        address = int(text, 16)
        file_address = address - adjust
        if lines:
            resolved = subprocess.run(
                ["aarch64-linux-gnu-addr2line", "-f", "-C", "-e", str(elf), hex(file_address)],
                capture_output=True, text=True).stdout.split()
            if resolved and resolved[0] != "??":
                print("%s  %s" % (text, " at ".join(resolved[:2])))
                continue
        name, offset = lookup(table, file_address)
        if name is None:
            print("%s  <not in %s>" % (text, elf.name))
        else:
            print("%s  %s + 0x%x" % (text, name, offset))


if __name__ == "__main__":
    main(sys.argv[1:])
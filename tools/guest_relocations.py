"""The Android guest image's relocation table (tools/android_build.py).

The guest image is linked to run at HALO_GUEST_IMAGE_BASE, but on some
devices the Java runtime has already mapped memory there by the time the app
starts, so the host loads it wherever there is room instead
(port/android/host/host_loader.c). Moving it by a whole number of pages
leaves its code as it is: branches and the ADRP pairs that form addresses are
relative to the instruction, and the low twelve bits an ADRP pair adds do not
change. Only the 32-bit pointers in its data (R_AARCH64_ABS32: pointer
tables, function pointers, the image header) hold the address it was linked
at, and the host adds the distance it moved to each of them.

This reads an image linked with --emit-relocs and writes those pointers'
places as a table:

    uint32_t magic        'HRLC'
    uint32_t base         the address the image was linked at
    uint32_t count
    uint32_t offsets[count]   from base, of each 32-bit pointer

and fails if the image holds any other kind of absolute address, which the
host could not move, or a 32-bit pointer to anything but the image itself
(an undefined weak symbol's 0, say), which moving would break.

    python3 tools/guest_relocations.py image.elf table.relocs
"""

import struct
import sys
from pathlib import Path

MAGIC = 0x434C5248  # 'HRLC'

SHT_RELA = 4
SHF_ALLOC = 0x2

R_AARCH64_ABS32 = 258
# relative to the instruction or its page, so unchanged when the image moves
# by whole pages
POSITION_INDEPENDENT = {
    0,    # R_AARCH64_NONE
    260,  # R_AARCH64_PREL64
    261,  # R_AARCH64_PREL32
    262,  # R_AARCH64_PREL16
    273,  # R_AARCH64_LD_PREL_LO19
    274,  # R_AARCH64_ADR_PREL_LO21
    275,  # R_AARCH64_ADR_PREL_PG_HI21
    276,  # R_AARCH64_ADR_PREL_PG_HI21_NC
    277,  # R_AARCH64_ADD_ABS_LO12_NC (the low bits of a page-aligned move are 0)
    278,  # R_AARCH64_LDST8_ABS_LO12_NC
    279,  # R_AARCH64_TSTBR14
    280,  # R_AARCH64_CONDBR19
    282,  # R_AARCH64_JUMP26
    283,  # R_AARCH64_CALL26
    284,  # R_AARCH64_LDST16_ABS_LO12_NC
    285,  # R_AARCH64_LDST32_ABS_LO12_NC
    286,  # R_AARCH64_LDST64_ABS_LO12_NC
    299,  # R_AARCH64_LDST128_ABS_LO12_NC
}


def relocations(image: bytes):
    """(section name, offset, type) of every relocation into a loaded section"""
    (e_shoff,) = struct.unpack_from("<Q", image, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", image, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", image, e_shoff + index * e_shentsize)
                for index in range(e_shnum)]
    names_offset = sections[e_shstrndx][4]

    def name(section):
        start = names_offset + section[0]
        return image[start:image.index(b"\0", start)].decode()

    for section in sections:
        sh_type, sh_offset, sh_size, sh_info = section[1], section[4], section[5], section[7]
        if sh_type != SHT_RELA or not sections[sh_info][2] & SHF_ALLOC:
            continue
        for entry in range(sh_offset, sh_offset + sh_size, 24):
            r_offset, r_info = struct.unpack_from("<QQ", image, entry)
            yield name(section), r_offset, r_info & 0xFFFFFFFF


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    image = Path(sys.argv[1]).read_bytes()
    (entry,) = struct.unpack_from("<Q", image, 0x18)
    (e_phoff,) = struct.unpack_from("<Q", image, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", image, 0x36)
    # (p_vaddr, p_offset, p_filesz, p_memsz) of each loaded segment
    segments = [(vaddr, offset, filesz, memsz)
                for kind, _, offset, vaddr, _, filesz, memsz, _ in
                (struct.unpack_from("<IIQQQQQQ", image, e_phoff + index * e_phentsize) for index in range(e_phnum))
                if kind == 1]
    # the image's lowest loaded page, which the host's base stands for, and
    # its end, page-aligned as the header's image_end is
    base = min(segment[0] for segment in segments) & ~0xFFF
    end = (max(segment[0] + segment[3] for segment in segments) + 0xFFF) & ~0xFFF

    def word(address):
        for vaddr, offset, filesz, _ in segments:
            if vaddr <= address < vaddr + filesz:
                return struct.unpack_from("<I", image, offset + address - vaddr)[0]
        return None

    offsets = []
    unmovable = {}
    for section, offset, kind in relocations(image):
        if kind == R_AARCH64_ABS32:
            value = word(offset)
            if value is None or not base <= value <= end:
                unmovable.setdefault((section, kind), offset)
                continue
            offsets.append(offset - base)
        elif kind not in POSITION_INDEPENDENT:
            unmovable.setdefault((section, kind), offset)
    if not offsets and entry:
        print(f"{sys.argv[1]}: no relocations; was it linked with --emit-relocs?", file=sys.stderr)
        return 1
    if unmovable:
        for (section, kind), offset in sorted(unmovable.items()):
            print(f"{sys.argv[1]}: relocation type {kind} in {section} (at {offset:#x}) "
                  "is an absolute address the host cannot move (or a pointer out of the image)", file=sys.stderr)
        return 1
    offsets.sort()
    Path(sys.argv[2]).write_bytes(struct.pack(f"<III{len(offsets)}I", MAGIC, base, len(offsets), *offsets))
    return 0


if __name__ == "__main__":
    sys.exit(main())

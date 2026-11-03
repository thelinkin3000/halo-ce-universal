"""The maps' hashes kept across runs (map_share_files.c): their table finds a path again whatever its case as it grows,
and a line of d:\\map_hashes.txt is read back as it was written, a line that is not one passed over."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["table", "lines", "bad-lines"]

# a fault in map_share_files.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "case-kept": (("if (character >= 'A' && character <= 'Z')", "if (0)"), "map_hash_key", "table"),
    "any-path-added": (("if (csstrlen(path) >= MAP_SHARE_PATH_SIZE)\n\t\treturn NULL;", ""), "map_hash_add", "table"),
    "uppercase-digits": (("if (character >= 'a' && character <= 'f')", "if (character >= 'A' && character <= 'f')"),
                         "hex_value", "bad-lines"),
    "size-overflows": (("if (digits == 10 || *size > (0xFFFFFFFFUL - (unsigned long)(*text - '0')) / 10)",
                        "if (0)"), "map_hash_line_read", "bad-lines"),
    "empty-path": (("if (*text++ != ' ' || !*text ||", "if (*text++ != ' ' ||"), "map_hash_line_read", "bad-lines"),
    "time-halves-swapped": (("(unsigned long)(entry->time >> 32),\n\t\t(unsigned long)(entry->time & 0xFFFFFFFFUL)",
                             "(unsigned long)(entry->time & 0xFFFFFFFFUL),\n\t\t(unsigned long)(entry->time >> 32)"),
                            "map_hash_line_write", "lines"),
    "nibbles-swapped": (("digits[index * 2] = hex[entry->hash[index] >> 4];",
                         "digits[index * 2] = hex[entry->hash[index] & 15];"), "map_hash_line_write", "lines"),
}

GLOBALS = """
struct map_share_files_globals
{
	struct map_hash *hashes;
	long hash_count;
	long hash_room;
	long *hash_slots;
	long hash_slot_count;
};
static struct map_share_files_globals map_share_files_globals;
"""


def generated(fault=None, faulty_function=None):
    header = read("port/linux/game/map_share.h")
    source = read("port/linux/game/map_share_files.c")
    config = ""
    for name in ("MAP_SHARE_PATH_SIZE", "MAP_SHARE_HASH_BYTES"):
        config += f"#define {name} {constant(header, name)}\n"
    config += f"#define MAP_HASHES_FIRST_ROOM {constant(source, 'MAP_HASHES_FIRST_ROOM')}\n"
    start = source.index("struct map_hash\n{")
    text = source[start:source.index("};", start) + 2] + "\n" + GLOBALS
    for name in ("map_hash_key", "map_hashes_slot", "map_hash_find", "map_hash_add", "hex_value", "map_hash_line_read",
                 "map_hash_line_write"):
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("map_share_hashes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("map_share_hashes", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"map_share_files.c with a fault ({control}) passed '{case}': the test cannot see it"

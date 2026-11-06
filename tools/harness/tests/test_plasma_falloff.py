"""Exercise the real attenuation texture generation and scoped binding."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run


def generated(fault=None):
    code = re.sub(r'^#include.*$', '', read('port/linux/game/plasma_falloff.c'), flags=re.M)
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', ['curve', 'scope', 'specular', 'failure', 'tint'])
def test_case(case):
    status, output = run(build('plasma_falloff', generated()), case)
    assert status == 0, output


def test_edge_negative_control():
    status, output = run(build('plasma_falloff', generated(
        ('return remaining * remaining;', 'return remaining;'))), 'curve')
    assert status == CHECK_FAILED, output


def test_tint_negative_control():
    status, output = run(build('plasma_falloff', generated(
        ('plasma_specular_tint = real_rgb_color_to_pixel32(&tint);',
         'plasma_specular_tint = 0x00FFFFFFUL;'))), 'tint')
    assert status == CHECK_FAILED, output

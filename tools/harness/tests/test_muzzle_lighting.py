"""Real pulse ownership and render-time fade, without game assets."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run


def generated(fault=None):
    code = re.sub(r'^#include.*$', '', read('port/linux/game/muzzle_lighting.c'), flags=re.M)
    code += function(read('source/objects/object_lights.c'), 'light_port_glow_move')
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', ['pulse', 'profiles', 'off', 'capacity', 'lifecycle', 'failures', 'world', 'move', 'warthog', 'scorpion'])
def test_case(case):
    status, output = run(build('muzzle_lighting', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('before,after,case', [
    ('real now = render_interpolation_game_time_sec(game_time_get());', 'real now = (real)game_time_get() / 30.0f;', 'pulse'),
    ('pulse->connected = FALSE;', 'pulse->connected = TRUE;', 'off'),
    ('light->position = *position;', '(void)position;', 'move'),
])
def test_negative_control(before, after, case):
    # The connected assignment has two legitimate sites; mutate cleanup only.
    if before == 'pulse->connected = FALSE;':
        before = 'light_disconnect_from_map(pulse->light);\n\t\t\tpulse->connected = FALSE;'
        after = 'light_disconnect_from_map(pulse->light);\n\t\t\tpulse->connected = TRUE;'
    status, output = run(build('muzzle_lighting', generated((before, after))), case)
    assert status == CHECK_FAILED, output

"""Enhanced model shaders fall back to stock while they compile, on Vulkan and deko3d."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

VK = 'port/android/guest/d3d8_vk.c'
DK = 'port/switch/guest/d3d8_dk.c'


def generated(fault=None):
    source = read(VK)
    start = source.index('static struct\n{\n\t/* the variant this draw uses')
    code = source[start:source.index("/* a pixel shader's: the generator's version")]
    code += function(source, 'draw_shaders')
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', ['no_light', 'other_program', 'compiling', 'ready', 'failed', 'stock_not_ready'])
def test_vulkan(case):
    status, output = run(build('plasma_model_fallback', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('before,after,case', [
    ('if (!*vertex)\n\t\t\t\tplasma_draw.bits = 0;', '(void)0;', 'compiling'),
    ('mask, FALSE, plasma_draw.bits, NULL, FALSE, &plasma_failed);', 'mask, FALSE, plasma_draw.bits, NULL, TRUE, &plasma_failed);', 'compiling'),
    ('program->plasma_failed = TRUE;', '(void)0;', 'failed'),
    ('if (immediate || program->lighting.lights != 2 || program->plasma_failed)', 'if (immediate || program->plasma_failed)', 'other_program'),
    ('if (!mask)\n\t\treturn;', 'if (0)\n\t\treturn;', 'no_light'),
])
def test_vulkan_negative_controls(before, after, case):
    status, output = run(build('plasma_model_fallback', generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_deko3d_falls_back():
    """Its choice, read from the source: the variant's key first, the stock key if that is not ready, and the
    parameters zeroed so that they are the stock draw's. (The service it calls is the same one a stock draw waits on.)"""
    code = function(read(DK), 'draw_prepare')
    variant = code.index('plasma_key.packed_mask |= (uint32_t)plasma_draw.bits << DK_VERTEX_KEY_PLASMA_SHIFT;')
    asked = code.index('vertex_shader = dk_shader_variant_for_draw(&plasma_key, &unknown);')
    cleared = code.index('memset(&plasma_draw, 0, sizeof(plasma_draw));')
    stock = code.index('vertex_shader = dk_shader_for_draw(DK_SHADER_STAGE_VERTEX, &vertex_key);')
    assert variant < asked < cleared < stock
    assert re.search(r'if \(!vertex_shader\)\s+memset\(&plasma_draw', code)
    assert re.search(r'if \(!vertex_shader\)\s+vertex_shader = dk_shader_for_draw\(DK_SHADER_STAGE_VERTEX, &vertex_key\)', code)

"""The dynamic lighting's model variant, through the real translators and compilers.

The game's stock vertex programs go through the Vulkan and deko3d generators.
Stock text must have nothing of the variant, and what they write must compile
with glslang (Vulkan) and UAM (deko3d). Adapted from GulchCE's test of its GL
translators: this proves the text and that it compiles, not what a GPU draws.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import HARNESS, mutated, read, structure

ROOT = Path(__file__).resolve().parents[3]
GLSLANG = ROOT / "build" / "host-glslang" / "StandAlone" / "glslang"
UAM = Path("/opt/devkitpro/tools/bin/uam")
BACKENDS = {
    "vulkan": ("port/android/guest/nv2a_vsh_vk.c", "port/android/guest/nv2a_psh_vk.c", "port/android/guest/vk_shaders.h"),
    "deko3d": ("port/switch/guest/nv2a_vsh_dk.c", None, "port/switch/guest/dk_shaders.h"),
}
_work = tempfile.TemporaryDirectory(prefix="plasma-model-shader-")


def strip_includes(code):
    return re.sub(r'^#include "?(xgpu|port_config|vk_shaders|dk_shaders|plasma_model_shader).*$', '', code, flags=re.M)


def xdk_constants():
    """The Direct3D numbers the pixel translator reads, from the same header the game compiles with."""
    header = read("port/include/xdk/xdk_pdb.h")
    return "".join(f"#define {name} {value}\n" for name, value in
                   re.findall(r"^\s*(D3D(?:RS|CMP|FOG|BLEND)_\w+) = (\d+),", header, flags=re.M))


def prelude():
    header = read("port/linux/src/xgpu.h")
    return ("#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n"
            "typedef unsigned long DWORD;\ntypedef int BOOL;\n#define TRUE 1\n#define FALSE 0\n"
            "#define XGPU_VERTEX_ATTRIBUTE_COUNT 16\n#define XGPU_VERTEX_CONSTANT_COUNT 192\n"
            "#define XGPU_VERTEX_CONSTANT_BIAS 96\n#define XGPU_MODEL_LIGHT_COUNT 12\n"
            "#define XGPU_PLASMA_MODEL_FALLOFF 1u\n#define XGPU_PLASMA_MODEL_OCCLUSION 2u\n"
            "enum { _xgpu_sampler_none, _xgpu_sampler_2d, _xgpu_sampler_3d, _xgpu_sampler_cube };\n"
            + xdk_constants() + structure(header, "xgpu_text") + "\n" + structure(header, "nv2a_vertex_lighting") + "\n"
            + structure(header, "nv2a_pixel_shader_key") + "\n"
            "void xgpu_text_append(struct xgpu_text *text, const char *format, ...);\n"
            "BOOL nv2a_vertex_shader_lighting(const DWORD *instructions, unsigned long instruction_count,\n"
            "\tstruct nv2a_vertex_lighting *lighting);\n"
            "const char *config_string(const char *name);\nint config_boolean(const char *name);\n")


def real_programs():
    words = [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]+',
             re.sub(r'/\*.*?\*/', '', read('source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc'), flags=re.S))]
    programs = [words[i + 1:i + 1 + 4 * (w >> 16)] for i, w in enumerate(words) if w & 0xffff == 0x2078 and 0 < w >> 16 <= 136]
    return ('static const unsigned long programs[][544] = {\n' +
            ',\n'.join('{' + ','.join(f'{w}UL' for w in p) + '}' for p in programs) + '};\n'
            'static const unsigned long lengths[] = {' + ','.join(str(len(p) // 4) for p in programs) + '};\n')


def lighting_object(work):
    """The recognizer the game uses (nv2a_vsh.c, the GL renderer's: both backends' images link it)."""
    source = work / "lighting.c"
    source.write_text("#define HALO_GLES 1\n" + prelude() +
                      "static BOOL xgpu_reversed_depth;\n"
                      "static struct { const char *shading_language; } xgpu_capabilities = {\"300 es\"};\n" +
                      strip_includes(read("port/linux/src/nv2a_vsh.c")))
    return source


def built(backend, fault=None):
    vertex, pixel, shaders = BACKENDS[backend]
    work = Path(tempfile.mkdtemp(dir=_work.name))
    code = ("#define TEST_VULKAN 1\n" if backend == "vulkan" else "") + prelude()
    code += read(shaders).replace('#include "xgpu.h"', '') + "\n" + read("port/linux/include/plasma_model_shader.h")
    code += strip_includes(read("port/linux/src/plasma_model_shader.c")) + strip_includes(read(vertex))
    if pixel:
        code += strip_includes(read(pixel))
    if fault:
        code = mutated(code, *fault)
    code += real_programs()
    (work / "under_test.inc").write_text(code)
    executable = work / "test"
    command = ["clang", "-m32", "-std=gnu99", "-O1", "-w", "-I", str(HARNESS / "include"), "-I", str(work),
               str(HARNESS / "tests" / "plasma_model_shader.c"), str(lighting_object(work)), "-o", str(executable), "-lm"]
    result = subprocess.run(command, capture_output=True, text=True)
    assert not result.returncode, result.stderr[-3000:]
    return executable


def ask(executable, *arguments):
    result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout, result.stderr


def compile_vulkan(text, stage, where):
    path = where / f"shader.{stage}"
    path.write_text(text)
    return subprocess.run([str(GLSLANG), "-V", "--target-env", "vulkan1.0", "-o", "/dev/null", str(path)],
                          capture_output=True, text=True, timeout=120)


def compile_deko3d(text, stage, where):
    path = where / f"shader.{stage}"
    path.write_text(text)
    return subprocess.run([str(UAM), "-s", stage, str(path), "-o", "/dev/null"], capture_output=True, text=True, timeout=120)


@pytest.mark.parametrize("backend", BACKENDS)
def test_text_of_real_programs(backend):
    status, output, error = ask(built(backend))
    assert status == 0, output + error


@pytest.mark.parametrize("backend,before,after", [
    ("vulkan", 'if (plasma_model_parameters[%d] > 0.5)', 'if (plasma_model_parameters[%d] > 0.6)'),
    ("deko3d", 'if (plasma && (!nv2a_vertex_shader_lighting', 'if (0 && (!nv2a_vertex_shader_lighting'),
    ("vulkan", 'if (plasma)\n\t\t\tplasma_model_emit_vertex', 'if (0)\n\t\t\tplasma_model_emit_vertex'),
])
def test_negative_controls(backend, before, after):
    status, output, error = ask(built(backend, (before, after)))
    assert status != 0, output + error


@pytest.mark.skipif(not GLSLANG.exists(), reason="glslang is not built (tools/vk_shader_check.py builds it)")
def test_vulkan_shaders_compile():
    executable = built("vulkan")
    _, listing, _ = ask(executable, "programs")
    programs = [int(line) for line in listing.split()]
    assert len(programs) >= 2
    with tempfile.TemporaryDirectory() as folder:
        where = Path(folder)
        for program in programs:
            for plasma, packed, lit in ((0, 2, 0), (0, 2, 1), (1, 0, 0), (1, 2, 0), (3, 0, 0), (3, 2, 0)):
                _, text, _ = ask(executable, "vertex", program, plasma, packed, lit)
                result = compile_vulkan(text, "vert", where)
                assert not result.returncode, f"program {program} plasma {plasma}: {result.stdout}{result.stderr}"
        for lights, plasma in ((2, 0), (2, 1), (2, 3)):
            _, text, _ = ask(executable, "pixel", lights, plasma)
            result = compile_vulkan(text, "frag", where)
            assert not result.returncode, f"pixel {lights}, {plasma}: {result.stdout}{result.stderr}"


@pytest.mark.skipif(not UAM.exists(), reason="devkitPro's uam is not installed")
def test_deko3d_shaders_compile():
    executable = built("deko3d")
    _, listing, _ = ask(executable, "programs")
    programs = [int(line) for line in listing.split()]
    assert len(programs) >= 2
    with tempfile.TemporaryDirectory() as folder:
        where = Path(folder)
        for program in programs:
            for plasma, packed in ((0, 2), (1, 0), (1, 2), (3, 0), (3, 2)):
                _, text, _ = ask(executable, "vertex", program, plasma, packed, 0)
                result = compile_deko3d(text, "vert", where)
                assert not result.returncode, f"program {program} plasma {plasma}: {result.stdout}{result.stderr}"


@pytest.mark.skipif(not GLSLANG.exists(), reason="glslang is not built (tools/vk_shader_check.py builds it)")
def test_a_broken_variant_fails_to_compile():
    """A negative control for the compile step itself."""
    executable = built("vulkan", ('" if (plasma_model_parameters[2 + slot] < 0.5) return 1.0;\\n"', '" if (plasma_model_parameters[2 + slot] < 0.5) return 1;;;nope\\n"'))
    _, listing, _ = ask(executable, "programs")
    with tempfile.TemporaryDirectory() as folder:
        _, text, _ = ask(executable, "vertex", listing.split()[0], 3, 2, 0)
        assert compile_vulkan(text, "vert", Path(folder)).returncode


@pytest.mark.skipif(not GLSLANG.exists(), reason="glslang is not built (tools/vk_shader_check.py builds it)")
def test_a_pixel_shader_without_the_variants_functions_fails_to_compile():
    executable = built("vulkan", ('if (point_lights && plasma)\n\t\tplasma_model_emit_pixel', 'if (0)\n\t\tplasma_model_emit_pixel'))
    with tempfile.TemporaryDirectory() as folder:
        _, text, _ = ask(executable, "pixel", 2, 1)
        assert compile_vulkan(text, "frag", Path(folder)).returncode

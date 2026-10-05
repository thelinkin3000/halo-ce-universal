"""Check the Vulkan renderer's dumped shaders on the PC.

The proof of port/android/VULKAN.md's phase 4: every shader the Vulkan image's
GLSL generators (port/android/guest/nv2a_vsh_vk.c, nv2a_psh_vk.c) produce for
the game's real shaders must compile, lay its blocks out as
port/android/guest/vk_shaders.h says, pass spirv-val, and say the same as the
GL ES generators' text for the same shader. The shaders are the folder the
Vulkan image writes when debug.gpu_dump_shaders names one (pull it with adb):

    python3 tools/vk_shader_check.py /path/to/shaders

For each .vert and .frag (the .gl.* files are the GL ES twins):

- compiled with glslangValidator -V --target-env vulkan1.0 (built from the
  glslang configure.py fetched, into build/host-glslang, the first time);
- reflected (-q): every block's binding, size and members' offsets, and every
  sampler's binding, are what vk_shaders.h's #defines say;
- validated with spirv-val, from the PATH or built (into build/host-spirv-tools,
  from the SPIRV-Tools glslang pins, which it fetches) the first time; if that
  cannot be had, it says so and the device's validation layer stands for it;
- compared with its GL ES twin: the Vulkan text must be exactly the twin with
  the changes phase 4 makes (the version and precision lines, the loose
  uniforms made blocks with an offset for every member, the locations, the
  point size, the screen offset and the alpha reference as vec4 components,
  texture_lod_bias and the gl_Position y flip and depth remap gone) and
  nothing else, whitespace aside. A shader that compiles and draws black is
  the failure this is for: it compiles, so only a comparison finds it.

Then the corpus's numbers: files, failures by kind, distinct pixel keys, and
how many keys differ in each key field (a field that never varies across
hundreds of keys is a key that was not filled in).

Exits 1 on any failure.
"""

import difflib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GLSLANG_SOURCE = ROOT / "build" / "android" / "third_party" / "glslang"
GLSLANG_BUILD = ROOT / "build" / "host-glslang"
SPIRV_SOURCE = ROOT / "build" / "spirv-tools-src"
SPIRV_BUILD = ROOT / "build" / "host-spirv-tools"
HEADER = ROOT / "port" / "android" / "guest" / "vk_shaders.h"


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def find_glslang():
    built = GLSLANG_BUILD / "StandAlone" / "glslangValidator"
    if built.exists():
        return str(built)
    if not (GLSLANG_SOURCE / "CMakeLists.txt").exists():
        print("glslang is not fetched: run configure.py first (it clones it into %s)" % GLSLANG_SOURCE)
        return None
    print("building glslangValidator into %s (once)" % GLSLANG_BUILD)
    configure = run(["cmake", "-S", str(GLSLANG_SOURCE), "-B", str(GLSLANG_BUILD), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release", "-DENABLE_OPT=OFF", "-DENABLE_CTEST=OFF", "-DENABLE_HLSL=OFF",
                     "-DBUILD_EXTERNAL=OFF", "-DENABLE_GLSLANG_BINARIES=ON"])
    if configure.returncode == 0:
        build = run(["ninja", "-C", str(GLSLANG_BUILD), "glslang-standalone"])
        if build.returncode == 0 and built.exists():
            return str(built)
        print(build.stdout[-2000:], build.stderr[-2000:])
    else:
        print(configure.stdout[-2000:], configure.stderr[-2000:])
    return None


def find_spirv_val():
    on_path = shutil.which("spirv-val")
    if on_path:
        return on_path
    built = SPIRV_BUILD / "tools" / "spirv-val"
    if built.exists():
        return str(built)
    try:
        import json
        pins = {commit["name"]: commit["commit"] for commit in
                json.loads((GLSLANG_SOURCE / "known_good.json").read_text())["commits"]}
        print("building spirv-val into %s (once)" % SPIRV_BUILD)
        for name, repo, key in (("spirv-tools", "SPIRV-Tools", "spirv-tools"),
                                ("spirv-headers", "SPIRV-Headers", "spirv-tools/external/spirv-headers")):
            folder = SPIRV_SOURCE / name
            if not (folder / ".git").exists():
                folder.mkdir(parents=True, exist_ok=True)
                steps = (["git", "init", "-q", "."],
                         ["git", "fetch", "-q", "--depth", "1", "https://github.com/KhronosGroup/%s.git" % repo, pins[key]],
                         ["git", "checkout", "-q", "FETCH_HEAD"])
                for step in steps:
                    if run(step, cwd=folder).returncode != 0:
                        raise RuntimeError("cannot fetch %s" % repo)
        configure = run(["cmake", "-S", str(SPIRV_SOURCE / "spirv-tools"), "-B", str(SPIRV_BUILD), "-G", "Ninja",
                         "-DCMAKE_BUILD_TYPE=Release", "-DSPIRV-Headers_SOURCE_DIR=%s" % (SPIRV_SOURCE / "spirv-headers"),
                         "-DSPIRV_SKIP_TESTS=ON", "-DSPIRV_WERROR=OFF"])
        if configure.returncode != 0 or run(["ninja", "-C", str(SPIRV_BUILD), "spirv-val"]).returncode != 0 \
                or not built.exists():
            raise RuntimeError("cannot build spirv-val")
        return str(built)
    except Exception as error:  # noqa: BLE001 - best effort, said below
        print("spirv-val is not available (%s): the validation layer's run on the device stands for it" % error)
        return None


def header_defines():
    values = {}
    for line in HEADER.read_text().splitlines():
        match = re.match(r"#define\s+(VK_\w+)\s+(\d+)\s*$", line)
        if match:
            values[match.group(1)] = int(match.group(2))
    return values


def expected_layout(defines):
    """Blocks as (name, binding, size, {member: offset}) from vk_shaders.h."""
    d = defines
    vertex_constants = ("vertex_constants", d["VK_BINDING_VERTEX_CONSTANTS"], d["VK_VERTEX_CONSTANTS_SIZE"],
                        {"c": d["VK_VERTEX_CONSTANTS_C"]})
    vertex_parameters = ("vertex_parameters", d["VK_BINDING_VERTEX_PARAMETERS"], d["VK_VERTEX_PARAMETERS_SIZE"],
                         {"viewport_scale": d["VK_VERTEX_PARAMETERS_VIEWPORT_SCALE"],
                          "viewport_offset": d["VK_VERTEX_PARAMETERS_VIEWPORT_OFFSET"],
                          "point_and_screen": d["VK_VERTEX_PARAMETERS_POINT_AND_SCREEN"]})
    pixel_members = {}
    for member in ("ps_c0", "ps_c1", "ps_final_c0", "ps_final_c1", "fog_color", "fog_parameters", "alpha_reference",
                   "bump_matrix", "bump_luminance", "texture_scale"):
        pixel_members[member] = d["VK_PIXEL_PARAMETERS_" + member.upper().replace("PS_C", "PS_C")]
    pixel_parameters = ("pixel_parameters", d["VK_BINDING_PIXEL_PARAMETERS"], d["VK_PIXEL_PARAMETERS_SIZE"],
                        pixel_members)
    return {"vert": [vertex_constants, vertex_parameters], "frag": [pixel_parameters]}


def header_text(stage, defines):
    """The declarations the Vulkan text must have between its #version and its body, written from vk_shaders.h."""
    d = defines
    if stage == "vert":
        return (
            "layout(std140, set = 0, binding = %d) uniform vertex_constants\n{\n"
            "layout(offset = %d) vec4 c[%d];\n};\n"
            "layout(std140, set = 0, binding = %d) uniform vertex_parameters\n{\n"
            "layout(offset = %d) vec4 viewport_scale;\n"
            "layout(offset = %d) vec4 viewport_offset;\n"
            "layout(offset = %d) vec4 point_and_screen;\n};\n"
            "layout(location = %d) out vec4 xD0;\nlayout(location = %d) out vec4 xD1;\n"
            "layout(location = %d) out vec4 xB0;\nlayout(location = %d) out vec4 xB1;\n"
            "layout(location = %d) out vec4 xT0;\nlayout(location = %d) out vec4 xT1;\n"
            "layout(location = %d) out vec4 xT2;\nlayout(location = %d) out vec4 xT3;\n"
            "layout(location = %d) out float xFog;\n" % (
                d["VK_BINDING_VERTEX_CONSTANTS"], d["VK_VERTEX_CONSTANTS_C"], d["VK_VERTEX_CONSTANT_COUNT"],
                d["VK_BINDING_VERTEX_PARAMETERS"], d["VK_VERTEX_PARAMETERS_VIEWPORT_SCALE"],
                d["VK_VERTEX_PARAMETERS_VIEWPORT_OFFSET"], d["VK_VERTEX_PARAMETERS_POINT_AND_SCREEN"],
                d["VK_LOCATION_D0"], d["VK_LOCATION_D1"], d["VK_LOCATION_B0"], d["VK_LOCATION_B1"],
                d["VK_LOCATION_T0"], d["VK_LOCATION_T1"], d["VK_LOCATION_T2"], d["VK_LOCATION_T3"],
                d["VK_LOCATION_FOG"]))
    p = "VK_PIXEL_PARAMETERS_"
    return (
        "layout(std140, set = 0, binding = %d) uniform pixel_parameters\n{\n"
        "layout(offset = %d) vec4 ps_c0[8];\nlayout(offset = %d) vec4 ps_c1[8];\n"
        "layout(offset = %d) vec4 ps_final_c0;\nlayout(offset = %d) vec4 ps_final_c1;\n"
        "layout(offset = %d) vec4 fog_color;\nlayout(offset = %d) vec4 fog_parameters;\n"
        "layout(offset = %d) vec4 alpha_reference;\nlayout(offset = %d) vec4 bump_matrix[4];\n"
        "layout(offset = %d) vec4 bump_luminance[4];\nlayout(offset = %d) vec4 texture_scale[4];\n};\n"
        "layout(location = %d) in vec4 xD0;\nlayout(location = %d) in vec4 xD1;\n"
        "layout(location = %d) in vec4 xB0;\nlayout(location = %d) in vec4 xB1;\n"
        "layout(location = %d) in vec4 xT0;\nlayout(location = %d) in vec4 xT1;\n"
        "layout(location = %d) in vec4 xT2;\nlayout(location = %d) in vec4 xT3;\n"
        "layout(location = %d) in float xFog;\n"
        "layout(location = %d) out vec4 fragment_color;\n" % (
            d["VK_BINDING_PIXEL_PARAMETERS"], d[p + "PS_C0"], d[p + "PS_C1"], d[p + "PS_FINAL_C0"],
            d[p + "PS_FINAL_C1"], d[p + "FOG_COLOR"], d[p + "FOG_PARAMETERS"], d[p + "ALPHA_REFERENCE"],
            d[p + "BUMP_MATRIX"], d[p + "BUMP_LUMINANCE"], d[p + "TEXTURE_SCALE"],
            d["VK_LOCATION_D0"], d["VK_LOCATION_D1"], d["VK_LOCATION_B0"], d["VK_LOCATION_B1"],
            d["VK_LOCATION_T0"], d["VK_LOCATION_T1"], d["VK_LOCATION_T2"], d["VK_LOCATION_T3"],
            d["VK_LOCATION_FOG"], d["VK_LOCATION_FRAGMENT_COLOR"]))


def normalise(text):
    """Lines with their whitespace collapsed, blank ones dropped."""
    lines = []
    for line in text.splitlines():
        line = re.sub(r"\s+", " ", line).strip()
        if line:
            lines.append(line)
    return lines


def expected_from_gl(stage, gl_text, defines):
    """What the Vulkan text must be, from its GL ES twin and vk_shaders.h: the twin's body with the changes of phase 4,
    after the header those #defines write (as normalised lines)."""
    lines = gl_text.splitlines()
    body = []
    if stage == "vert":
        # the GL text: #version, precision, uniforms, outs, then `invariant gl_Position;` and the rest
        for index, line in enumerate(lines):
            if line.strip() == "invariant gl_Position;":
                body = lines[index:]
                break
        else:
            return None, "the GL ES text has no `invariant gl_Position;`"
        text = "\n".join(body)
        text = re.sub(r"\bpoint_size\b", "point_and_screen.x", text)
        text = re.sub(r"\bscreen_offset\b", "point_and_screen.y", text)
        text = re.sub(r"\n\s*gl_Position\.y = -gl_Position\.y;", "", text)
        text = re.sub(r"\n\s*gl_Position\.z = 2\.0 \* gl_Position\.z - gl_Position\.w;", "", text)
        samplers = ""
    else:
        # the GL text: #version, precision, in x9, the output, uniforms, sampler uniforms, then the body
        samplers = ""
        start = None
        for index, line in enumerate(lines):
            match = re.match(r"uniform (sampler\w+) tex(\d);", line.strip())
            if match:
                samplers += "layout(set = 0, binding = %d) uniform %s tex%s;\n" % (
                    defines["VK_BINDING_TEXTURE0"] + int(match.group(2)), match.group(1), match.group(2))
                start = index + 1
        if start is None:
            return None, "the GL ES text has no samplers"
        body = lines[start:]
        text = "\n".join(body)
        text = re.sub(r",\s*texture_lod_bias\[\d\]", "", text)
        text = re.sub(r"\balpha_reference\b(?!\.)", "alpha_reference.x", text)
        for forbidden in ("atomicCounter", "visible_samples", "early_fragment_tests"):
            if forbidden in "\n".join(lines):
                return None, "the GL ES text counts samples (%s): the keys have count_samples 0" % forbidden
    return normalise("#version 450\n" + header_text(stage, defines) + samplers + text), None


def parse_reflection(output):
    """(blocks, uniforms) from glslangValidator -q: blocks {name: (binding, size)}, uniforms {name: (offset, binding)}."""
    section = None
    blocks, uniforms = {}, {}
    for line in output.splitlines():
        if line.endswith("reflection:"):
            section = line
            continue
        match = re.match(r"([\w\[\].]+): offset (-?\d+), type \w+, size (\d+), index (-?\d+), binding (-?\d+)", line)
        if not match:
            continue
        name = re.sub(r"\[\d+\]", "", match.group(1))
        if section == "Uniform block reflection:":
            blocks[name] = (int(match.group(5)), int(match.group(3)))
        elif section == "Uniform reflection:":
            uniforms[name] = (int(match.group(2)), int(match.group(5)))
    return blocks, uniforms


def check_reflection(stage, output, layout, defines):
    errors = []
    blocks, uniforms = parse_reflection(output)
    expected = {block[0]: block for block in layout[stage]}
    for name, (binding, size) in blocks.items():
        if name not in expected:
            errors.append("unexpected block %s" % name)
            continue
        _, want_binding, want_size, _ = expected[name]
        if binding != want_binding or size != want_size:
            errors.append("block %s is at binding %d, %d bytes; vk_shaders.h says %d, %d" % (
                name, binding, size, want_binding, want_size))
    for name, (offset, binding) in uniforms.items():
        member = None
        for block in layout[stage]:
            if name in block[3]:
                member = block
        if member is not None:
            if offset != member[3][name]:
                errors.append("%s is at offset %d; vk_shaders.h says %d" % (name, offset, member[3][name]))
            continue
        match = re.match(r"tex(\d)$", name)
        if match:
            want = defines["VK_BINDING_TEXTURE0"] + int(match.group(1))
            if binding != want:
                errors.append("%s is at binding %d; vk_shaders.h says %d" % (name, binding, want))
            continue
        errors.append("unexpected uniform %s" % name)
    return errors


# the key's fields after the combiner words (struct nv2a_pixel_shader_key, xgpu.h, as the 32-bit guest lays it out)
KEY_TAIL = (("texture_modes", 4), ("sampler_type[0]", 1), ("sampler_type[1]", 1), ("sampler_type[2]", 1),
            ("sampler_type[3]", 1), ("alpha_kill[0]", 1), ("alpha_kill[1]", 1), ("alpha_kill[2]", 1),
            ("alpha_kill[3]", 1), ("color_sign[0]", 1), ("color_sign[1]", 1), ("color_sign[2]", 1),
            ("color_sign[3]", 1), ("alpha_test_function", 4), ("fog_enable", 1), ("fog_table_mode", 1),
            ("count_samples", 1), ("coverage_alpha", 1))
KEY_TAIL_SIZE = sum(size for _, size in KEY_TAIL)


def key_statistics(keys):
    """Distinct values of each key field across the corpus's distinct keys."""
    if not keys:
        return []
    size = len(keys[0])
    words = (size - KEY_TAIL_SIZE) // 4
    fields = []
    columns = defaultdict(set)
    for key in keys:
        if len(key) != size:
            continue
        for word in range(words):
            columns["combiner_state[%d]" % word].add(struct.unpack_from("<I", key, word * 4)[0])
        at = words * 4
        for name, width in KEY_TAIL:
            columns[name].add(int.from_bytes(key[at:at + width], "little"))
            at += width
    varying_words = sum(1 for name, values in columns.items() if name.startswith("combiner_state") and len(values) > 1)
    for name, _ in KEY_TAIL:
        fields.append((name, len(columns[name])))
    return fields, words, varying_words


def main():
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print("usage: vk_shader_check.py <folder of dumped shaders>")
        return 2
    folder = Path(sys.argv[1])
    if not folder.is_dir():
        print("no shader folder at %s" % folder)
        return 2
    glslang = find_glslang()
    if not glslang:
        return 2
    spirv_val = find_spirv_val()
    defines = header_defines()
    layout = expected_layout(defines)

    files = [path for path in sorted(folder.glob("*.vert")) + sorted(folder.glob("*.frag"))
             if ".gl." not in path.name]
    if not files:
        print("no .vert or .frag files in %s" % folder)
        return 2

    counts = Counter()
    failures = Counter()
    failed_files = []
    with tempfile.TemporaryDirectory() as scratch:
        for path in files:
            stage = "vert" if path.suffix == ".vert" else "frag"
            counts[stage] += 1
            problems = []
            text = path.read_text()
            output = Path(scratch) / (path.name + ".spv")
            compiled = run([glslang, "-V", "--target-env", "vulkan1.0", "-o", str(output), str(path)])
            if compiled.returncode != 0:
                failures["compile"] += 1
                problems.append("compile: " + (compiled.stdout + compiled.stderr).strip())
            else:
                reflected = run([glslang, "-V", "--target-env", "vulkan1.0", "-q", "-o", "/dev/null", str(path)])
                errors = check_reflection(stage, reflected.stdout, layout, defines)
                if errors:
                    failures["reflection"] += 1
                    problems.append("reflection: " + "; ".join(errors))
                if spirv_val:
                    validated = run([spirv_val, "--target-env", "vulkan1.0", str(output)])
                    if validated.returncode != 0:
                        failures["spirv-val"] += 1
                        problems.append("spirv-val: " + (validated.stdout + validated.stderr).strip())
            twin = path.with_name(path.stem + ".gl" + path.suffix)
            if not twin.exists():
                failures["no twin"] += 1
                problems.append("no GL ES twin (%s)" % twin.name)
            else:
                expected, why = expected_from_gl(stage, twin.read_text(), defines)
                if expected is None:
                    failures["comparison"] += 1
                    problems.append("comparison: " + why)
                else:
                    actual = normalise(text)
                    if actual != expected:
                        failures["comparison"] += 1
                        diff = difflib.unified_diff(expected, actual, "expected from the GL ES text", path.name,
                                                    lineterm="", n=1)
                        problems.append("comparison: differs\n" + "\n".join(list(diff)[:40]))
            if problems:
                failed_files.append(path.name)
                print("FAIL %s:" % path.name)
                for problem in problems:
                    for line in problem.splitlines():
                        print("    " + line)

    keys = [path.read_bytes() for path in sorted(folder.glob("ps_*.key"))]
    print()
    print("files: %d vertex shaders, %d pixel shaders (%d with a key); %d failed" % (
        counts["vert"], counts["frag"], len(keys), len(failed_files)))
    for kind, count in sorted(failures.items()):
        print("failures: %s x%d" % (kind, count))
    print("spirv-val: %s" % ("run on every module that compiled" if spirv_val else "not available"))
    if keys:
        fields, words, varying = key_statistics(keys)
        print("distinct pixel keys: %d; of the key's %d combiner words, %d vary" % (len(set(keys)), words, varying))
        for name, distinct in fields:
            note = ""
            if name == "count_samples":
                note = "  (the Vulkan device makes it 0)" if distinct == 1 else "  <- must be 0 everywhere"
            elif distinct == 1 and len(keys) >= 100:
                note = "  <- never varies in %d keys: not filled in?" % len(keys)
            print("  %-22s %d distinct value(s)%s" % (name, distinct, note))
    return 1 if failed_files else 0


if __name__ == "__main__":
    sys.exit(main())

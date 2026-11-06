"""Exercise the actual visibility field with synthetic collision geometry."""
import re
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, SSE_MATH, build, read, run


def test_visibility_field():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == 0, output


def test_fade_underflow_guard_is_required():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    guard = 'if (value < 1.0e-20f) value = 0;'
    assert code.count(guard) == 1
    status, output = run(build('plasma_occlusion', (('under_test.inc', code.replace(guard, '')),)), 'visibility')
    assert status == CHECK_FAILED and 'blocked history settles' in output, output


def test_clear_depth_shortcut_is_exact():
    from harness import function
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    body = function(code, 'depth_visibility')
    reference = body.replace('depth_visibility(', 'reference_depth_visibility(', 1).replace('if (clear)', 'if (clear && FALSE)')
    fast = body.replace('depth_visibility(', 'fast_depth_visibility(', 1)
    wrapper = r'''
static real depth_visibility(real depths[6][OCC_SIZE * OCC_SIZE], real_vector3d const *ray, real extent, int resolution)
{
    real result = fast_depth_visibility(depths, ray, extent, resolution);
    CHECK(result == reference_depth_visibility(depths, ray, extent, resolution), "clear depth shortcut retains float result");
    return result;
}
'''
    code = code.replace(body, reference + '\n' + fast + wrapper).replace('#if defined(__SSE2__)', '#if 0')
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),), SSE_MATH), 'visibility')
    assert status == 0, output


def test_stale_pass_upload_is_detected():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    assert code.count('field->texture_mask = 0;') == 2
    code = code.replace('field->texture_mask = 0;', '(void)0;')
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == CHECK_FAILED, output


def test_directional_mask_is_required():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    code = code.replace('return t*t*(3.0f - 2.0f*t);', '(void)t; return 1.0f;')
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == CHECK_FAILED, output


def test_needler_filtered_foot_pool_is_detected():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    # The previous long fade must no longer satisfy the lower-spill contract.
    old = code.replace('(-height - 0.06f) / 0.06f', '(-height - 0.06f) / 0.18f')
    status, output = run(build('plasma_occlusion', (('under_test.inc', old),)), 'visibility')
    assert status == CHECK_FAILED, output


def test_stationary_history_matches_reprojection():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    # Compare every updated cell, including moving lights, door changes, changed
    # extents and the 30/60/120 Hz sequences exercised by the fixture.
    marker = '\tfield->valid = 1;'
    assert code.count(marker) == 1
    code = code.replace(marker, marker + '\n\tfor (i = 0; i < OCC_COUNT; i++) printf("CELL %.9g\\n", field->visibility[i]);')
    samples = []
    reference = code.replace('if (stationary)', 'if (stationary && FALSE)')
    reference = reference.replace('if (stationary && !retraced)', 'if (stationary && !retraced && FALSE)')
    # The reference also needs coordinates for stationary fields now that
    # reprojection prepares its separable axes before visiting the cells.
    reference = reference.replace('if (!stationary && field->valid', 'if (field->valid')
    for version in (code, reference):
        status, output = run(build('plasma_occlusion', (('under_test.inc', version),)), 'visibility')
        assert status == 0, output[-2000:]
        samples.append([float(x) for x in re.findall(r'CELL (\S+)', output)])
    assert len(samples[0]) > 4096
    assert samples[0] == pytest.approx(samples[1], abs=0.00001)


def test_precomputed_history_matches_cell_coordinates():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    fast = 'history = sample_history_rows(projected, x, y, &history_axes[2][z]);'
    assert code.count(fast) == 1
    # Check the actual fast result against the original per-cell coordinate
    # arithmetic throughout movement, extent changes and all three rates.
    reference = '''sample_field(previous,
        0.5f + (light->position.x + ((2.0f * (x + 0.5f) / OCC_SIZE - 1) * extent) - field->center[0]) / (2 * field->extent),
        0.5f + (light->position.y + ((2.0f * (y + 0.5f) / OCC_SIZE - 1) * extent) - field->center[1]) / (2 * field->extent),
        0.5f + (light->position.z + ((2.0f * (z + 0.5f) / OCC_SIZE - 1) * extent) - field->center[2]) / (2 * field->extent))'''
    code = code.replace(fast, '{ ' + fast + '\nCHECK(history == ' + reference + ', "precomputed coordinates preserve every sample"); }')
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),), SSE_MATH), 'visibility')
    assert status == 0, output


def test_interpolation_matches_weighted_visibility():
    from harness import function
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    body = function(code, 'sample_axes')
    wrapper = r'''
static real sample_axes(const real *values, const struct sample_axis *x,
    const struct sample_axis *y, const struct sample_axis *z)
{
    int i,j,k;
    double expected=0;
    real result=fast_sample_axes(values,x,y,z);
    for(k=0;k<2;k++) for(j=0;j<2;j++) for(i=0;i<2;i++)
        expected += (double)values[((k?z->b:z->a)*OCC_SIZE+(j?y->b:y->a))*OCC_SIZE+(i?x->b:x->a)] *
            (i?x->t:1-x->t)*(j?y->t:1-y->t)*(k?z->t:1-z->t);
    CHECK(fabs(result-expected)<0.000001, "seven interpolations agree with weighted visibility");
    return result;
}
'''
    code = code.replace(body, body.replace('sample_axes(', 'fast_sample_axes(', 1) + wrapper)
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == 0, output

def test_half_scale_resampling_is_exact():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    anchor = '\t\t\t\t\tvalue = 1 - field->admission + field->admission * value;\n\t\t\t\t\tif (directional)'
    assert code.count(anchor) == 1
    check = r'''
    if (pass && remaining > 0 && scale == 0.5f) {
    real expected=sample_field(field->visibility, 0.5f+(2.0f*(x+0.5f)/16-1)*0.25f,
        0.5f+(2.0f*(y+0.5f)/16-1)*0.25f, 0.5f+(2.0f*(z+0.5f)/16-1)*0.25f);
    CHECK(value==expected, "half-scale resampling preserves the original float result");
    }
'''
    code = code.replace(anchor, check + anchor)
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == 0, output


def test_specialized_masks_match_generic_pixels():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    start = code.index('\telse\n\t{', code.index('The ordinary model mask needs'))
    opening = code.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (code[end] == '{') - (code[end] == '}')
        end += 1
    # Execute the actual generic conversion beside the flat path, comparing
    # all uploaded cells rather than only representative sample positions.
    generic = code[opening + 1:end - 1].replace('pixels[i]', 'reference_pixels[i]')
    check = ('\nif (!directional && (!pass || scale == 0.5f || scale == 1.0f)) { unsigned long reference_pixels[OCC_COUNT];\n' +
             generic + '\nCHECK(!memcmp(pixels, reference_pixels, sizeof(pixels)), "specialized mask matches every generic pixel");\n}\n')
    marker = '\tif (!(field->uploaded_mask & (1 << pass))'
    assert code.count(marker) == 1
    code = code.replace(marker, check + marker)
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == 0, output


def test_runtime_texture_comparison_preserves_uploads(tmp_path, monkeypatch):
    from lighting_texture_check import instrument
    code = read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c')
    code = instrument(code).replace('if (memcmp(fast, generic, sizeof(fast))) mismatches++;',
        'CHECK(!memcmp(fast, generic, sizeof(fast)), "runtime comparison preserves every texture byte");')
    clocks = '''
typedef struct { long long QuadPart; } LARGE_INTEGER;
static void QueryPerformanceCounter(LARGE_INTEGER *v) { static long long tick; v->QuadPart=++tick; }
static void QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart=1000000; }
'''
    code = clocks + re.sub(r'^#include.*$', '', code, flags=re.M)
    monkeypatch.chdir(tmp_path)
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == 0, output


def test_packed_visibility_matches_scalar_fields():
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    samples = []
    marker = '\tfield->valid = 1;'
    code = code.replace(marker, marker + '\n\tfor (i = 0; i < OCC_COUNT; i++) printf("CELL %.9g\\n", field->visibility[i]);')
    for version in (code, code.replace('#if defined(__SSE2__)', '#if 0')):
        status, output = run(build('plasma_occlusion', (('under_test.inc', version),)), 'visibility')
        assert status == 0, output[-2000:]
        samples.append(re.findall(r'CELL (\S+)', output))
    assert len(samples[0]) > 4096
    assert samples[0] == samples[1]


def test_runtime_visibility_comparison_preserves_fields(tmp_path, monkeypatch):
    from lighting_visibility_check import instrument
    code = read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c')
    code = instrument(code).replace('if (memcmp(output, reference, sizeof(reference))) mismatches++;',
        'CHECK(!memcmp(output, reference, sizeof(reference)), "runtime comparison preserves every visibility byte");')
    clocks = '''
typedef struct { long long QuadPart; } LARGE_INTEGER;
static void QueryPerformanceCounter(LARGE_INTEGER *v) { static long long tick; v->QuadPart=++tick; }
static void QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart=1000000; }
'''
    code = clocks + re.sub(r'^#include.*$', '', code, flags=re.M)
    monkeypatch.chdir(tmp_path)
    status, output = run(build('plasma_occlusion', (('under_test.inc', code),)), 'visibility')
    assert status == 0, output


def test_scalar_path():
    """The ARM images have no SSE2 path: the whole case over the scalar reconstruction."""
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    assert code.count('#if defined(__SSE2__)') == 1 or '#if defined(__SSE2__)' in code
    status, output = run(build('plasma_occlusion', (('under_test.inc', code.replace('#if defined(__SSE2__)', '#if 0')),)), 'visibility')
    assert status == 0, output


@pytest.mark.parametrize('before,after,expected', [
    # the scene asked on every call again (the review's finding)
    ('if (!retrace && due)\n', 'if (!retrace && (due || 1))\n', 'before the cadence'),
    # an extent change retraces at once again (the review's finding)
    ('retrace = !field->valid ||', 'retrace = field->trace_extent != extent || !field->valid ||', 'does not trace'),
    ('rescale_depths(field->depths, field->trace_extent, extent);', '(void)0;', 'same hits'),
])
def test_fix_negative_controls(before, after, expected):
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    assert code.count(before) == 1, before
    status, output = run(build('plasma_occlusion', (('under_test.inc', code.replace(before, after)),)), 'visibility')
    assert status == CHECK_FAILED and expected in output, output


def test_light_occlusion_off_negative_control():
    """Muzzle and vehicle lights tracing with the toggle off, GulchCE's behaviour: both gates opened."""
    code = re.sub(r'^#include.*$', '', read('port/linux/game/ammo_counter_falloff.c') + '\n' + read('port/linux/game/plasma_occlusion.c'), flags=re.M)
    gate = 'lighting_config_boolean("display.plasma_occlusion") &&\n'
    assert code.count(gate) == 2
    status, output = run(build('plasma_occlusion', (('under_test.inc', code.replace(gate, '1 &&\n')),)), 'visibility')
    assert status == CHECK_FAILED and 'traces no rays' in output, output

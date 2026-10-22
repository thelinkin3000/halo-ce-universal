"""Exercise the actual Android screen selection function without the game/GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--wsl", help="Windows: distro providing gcc")
options = parser.parse_args()
root = Path(__file__).resolve().parent.parent
source = (root / "port/linux/src/d3d8_gl.c").read_text()
function = source[source.index("static void screen_mode_choose("):source.index("long halo_screen_width(void)")]
program = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define HALO_ANDROID 1
static long requested_width;
static double requested_scale;
static float requested_resolution = 1.0f, screen_supersampling;
static long config_integer(const char *key) { (void)key; return requested_width; }
static double config_real(const char *key) { (void)key; return requested_scale; }
static float platform_resolution_scale(void) { return requested_resolution; }
''' + function + r'''
static void check(long width, double scale, long expected_width, float expected_scale) {
    long selected_width; float selected_scale[2];
    requested_width = width; requested_scale = scale;
    screen_supersampling = 4.0f;
    screen_mode_choose(&selected_width, selected_scale);
    assert(selected_width == expected_width);
    assert(selected_scale[0] == expected_scale && selected_scale[1] == expected_scale);
    assert(screen_supersampling == 1.0f);
}
int main(void) {
    check(640, 1.0, 640, 1.0f);
    check(640, 0.75, 640, 0.75f);
    check(1068, 0.75, 1068, 0.75f);
    check(640, 0.5, 640, 0.5f);
    check(640, 0.0, 640, 0.5f);
    check(640, -100.0, 640, 0.5f);
    check(640, 100.0, 640, 1.0f);
    check(640, NAN, 640, 1.0f);
    check(640, INFINITY, 640, 1.0f);
    check(640, -INFINITY, 640, 1.0f);
    check(10, 0.75, 640, 0.75f);
    check(9999, 0.75, 1600, 0.75f);
    check(1069, 0.75, 1068, 0.75f);
    setenv("HALO_DISPLAY_WIDTH", "960", 1);
    check(0, 0.75, 960, 0.75f);
    check(-1, 0.75, 960, 0.75f);
    requested_resolution = 1.5f; /* Upstream 720-line selection. */
    check(640, 1.0, 640, 1.5f);
    check(640, 0.75, 640, 1.125f);
    requested_resolution = 2.25f; /* Upstream 1080-line selection. */
    check(640, 1.0, 640, 2.25f);
    check(640, 0.5, 640, 1.125f);
    requested_resolution = 3.0f;
    check(640, 1.0, 640, 3.0f);
    requested_resolution = 4.5f;
    check(640, 1.0, 640, 4.5f);
    requested_resolution = 719.0f / 480.0f; /* Native panel height, no rounding. */
    check(640, 1.0, 640, requested_resolution);
    puts("PASS: 22 Android render scale/width cases, including upstream native/high-resolution composition");
    return 0;
}
'''

def target_path(path):
    if not options.wsl:
        return str(path)
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]

prefix = ["wsl", "-d", options.wsl, "--"] if options.wsl else []
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="render-scale-test-", dir=root / "build") as temporary:
    c_file = Path(temporary) / "test.c"
    executable = Path(temporary) / "test"
    c_file.write_text(program, encoding="utf-8", newline="\n")
    subprocess.run([*prefix, "gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", target_path(c_file), "-lm", "-o", target_path(executable)], check=True)
    subprocess.run([*prefix, target_path(executable)], check=True)

"""Run production Android pacing functions with a controlled monotonic clock."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--wsl", help="Windows: distro providing gcc")
options = parser.parse_args()
root = Path(__file__).resolve().parent.parent
source = (root / "port/linux/src/sdl_platform.c").read_text()
helpers = source[source.index("static int frame_clock_ns(Uint64 *value)"):source.index("#else\n/* with vsync off")]
swap = source[source.index("void platform_video_swap(void)"):source.index("void platform_mouse_capture(")]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define HALO_ANDROID 1
typedef unsigned long long Uint64;
static Uint64 now_ns, last_swap_ns, extra_sleep_ns, swap_delay_ns;
static double logged_swap_seconds;
static unsigned long revision = 1;
static long maximum = 30;
static unsigned reads, sleeps, swaps, interrupts_left, clock_failure, sleep_failure, stall_logs;
static void *platform_window;
static unsigned long config_changes(void) { return revision; }
static long config_integer(const char *name) { (void)name; reads++; return maximum; }
static Uint64 SDL_GetTicks(void) { return now_ns / 1000000ULL; }
static void SDL_GL_SwapWindow(void *window) {
    (void)window; now_ns += swap_delay_ns; last_swap_ns = now_ns; swaps++;
}
static void platform_log(const char *format, ...) {
    va_list args;
    assert(strstr(format, "graphics driver"));
    va_start(args, format); logged_swap_seconds = va_arg(args, double); va_end(args);
    stall_logs++;
}
static int fake_clock_gettime(clockid_t id, struct timespec *value) {
    assert(id == CLOCK_MONOTONIC);
    if (clock_failure) { errno = EIO; return -1; }
    value->tv_sec = (time_t)(now_ns / 1000000000ULL);
    value->tv_nsec = (long)(now_ns % 1000000000ULL);
    return 0;
}
static int fake_nanosleep(const struct timespec *duration, struct timespec *remaining) {
    Uint64 requested = (Uint64)duration->tv_sec * 1000000000ULL + (Uint64)duration->tv_nsec;
    assert(remaining == NULL && requested > 0 && requested < 1000000000ULL);
    sleeps++;
    if (sleep_failure) { errno = EINVAL; return -1; }
    if (interrupts_left) { interrupts_left--; now_ns += requested / 2; errno = EINTR; return -1; }
    now_ns += requested + extra_sleep_ns;
    return 0;
}
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
''' + helpers + swap + r'''
int main(void) {
    const Uint64 frame30 = 1000000000ULL / 30, frame60 = 1000000000ULL / 60;
    Uint64 previous; unsigned previous_sleeps, previous_reads;
    now_ns = 1000000000ULL;
    platform_video_swap(); /* First frame never waits. */
    assert(last_swap_ns == now_ns && sleeps == 0 && reads == 1);
    previous = last_swap_ns;
    now_ns += 10000000ULL;
    platform_video_swap();
    assert(last_swap_ns == previous + frame30 && sleeps == 1 && reads == 1);
    previous = last_swap_ns;
    now_ns += 20000000ULL;
    platform_video_swap();
    assert(last_swap_ns == previous + frame30 && reads == 1);
    now_ns += 100000000ULL; /* A late frame restarts scheduling. */
    previous_sleeps = sleeps;
    platform_video_swap();
    assert(last_swap_ns == now_ns && sleeps == previous_sleeps);
    previous = last_swap_ns;
    platform_video_swap(); /* No catch-up burst after the late frame. */
    assert(last_swap_ns == previous + frame30 && sleeps == previous_sleeps + 1);
    previous = last_swap_ns;
    previous_sleeps = sleeps;
    interrupts_left = 2;
    platform_video_swap();
    assert(last_swap_ns == previous + frame30 && sleeps == previous_sleeps + 3);
    previous = last_swap_ns;
    extra_sleep_ns = 5000000ULL;
    platform_video_swap();
    assert(last_swap_ns == previous + frame30 + extra_sleep_ns);
    previous = last_swap_ns;
    extra_sleep_ns = 0;
    platform_video_swap();
    assert(last_swap_ns == previous + frame30); /* Oversleep does not cause catch-up. */
    maximum = 0; revision++; previous_sleeps = sleeps;
    platform_video_swap(); platform_video_swap();
    assert(sleeps == previous_sleeps && reads == 2);
    maximum = 60; revision++; previous = now_ns;
    platform_video_swap();
    assert(last_swap_ns == previous && reads == 3);
    now_ns += 5000000ULL;
    platform_video_swap();
    assert(last_swap_ns == previous + frame60 && reads == 3);
    maximum = 24; revision++; previous_sleeps = sleeps;
    platform_video_swap(); platform_video_swap();
    assert(sleeps == previous_sleeps && reads == 4);
    maximum = -1; revision++; previous_sleeps = sleeps;
    platform_video_swap();
    assert(sleeps == previous_sleeps && reads == 5);
    maximum = 30; revision++; now_ns = 10000000000ULL;
    platform_video_swap(); /* Establish a deadline before clock failure. */
    clock_failure = 1; now_ns += 5000000ULL; previous_sleeps = sleeps;
    platform_video_swap();
    assert(sleeps == previous_sleeps); /* Clock failure cannot sleep on another origin. */
    clock_failure = 0;
    now_ns = 5000000ULL; /* Even a radically different recovered origin cannot reuse the old deadline. */
    platform_video_swap();
    assert(sleeps == previous_sleeps && last_swap_ns == now_ns);
    previous = last_swap_ns;
    platform_video_swap();
    assert(last_swap_ns == previous + frame30 && sleeps == previous_sleeps + 1);
    sleep_failure = 1; previous_sleeps = sleeps; previous_reads = reads;
    platform_video_swap();
    assert(sleeps == previous_sleeps + 1 && reads == previous_reads); /* No retry loop on EINVAL. */
    assert(swaps == 20);
    sleep_failure = 0; maximum = 30; revision++; now_ns = 20000000000ULL;
    platform_video_swap();
    extra_sleep_ns = 2500000000ULL;
    platform_video_swap(); /* Limiter/scheduler waiting is not a driver stall. */
    assert(stall_logs == 0);
    extra_sleep_ns = 0; swap_delay_ns = 2500000000ULL;
    platform_video_swap();
    assert(stall_logs == 1 && logged_swap_seconds == 2.5);
    swap_delay_ns = 1999000000ULL;
    platform_video_swap();
    assert(stall_logs == 1);
    swap_delay_ns = 2000000000ULL;
    platform_video_swap();
    assert(stall_logs == 2 && logged_swap_seconds == 2.0 && swaps == 25);
    puts("PASS: 21 Android pacing/stall scenarios, production functions, EINTR/overrun/clock recovery/config cache");
    return 0;
}
'''

def target_path(path):
    if not options.wsl:
        return str(path)
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]

prefix = ["wsl", "-d", options.wsl, "--"] if options.wsl else []
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="frame-limit-test-", dir=root / "build") as temporary:
    c_file = Path(temporary) / "test.c"
    executable = Path(temporary) / "test"
    c_file.write_text(program, encoding="utf-8", newline="\n")
    subprocess.run([*prefix, "gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", target_path(c_file), "-o", target_path(executable)], check=True)
    subprocess.run([*prefix, target_path(executable)], check=True)

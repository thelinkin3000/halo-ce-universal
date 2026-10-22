"""Run asset-free C89 tests against the real Android checkpoint functions.

    python tools/test_android_checkpoint.py --cc clang

Linux/macOS use a normal host C compiler. Windows uses clang's MSVC target,
MSVC Build Tools and the Windows SDK, discovered with vswhere/environment.
For a standalone clang missing its resource headers, pass --resource-dir.
--link, --msvc-root, --sdk-root and --sdk-version override Windows discovery.
--build-dir retains the generated C program, executable and JSON evidence.

The production protocol, snapshot function and CRC functions are extracted
unchanged. Payloads are synthetic; engine callbacks/validators and deliberate
I/O failures are stubs. The allocator rejects free(NULL), as cseries'
debug_free does. These tests require no game assets or Android device.
"""
import argparse
import contextlib
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import zlib

REPO = Path(__file__).resolve().parents[1]

def tracked(name):
    path = REPO / name
    if path.is_file():
        return path.read_text()
    return subprocess.check_output(["git", "show", "HEAD:" + name], cwd=REPO).decode()

def function(source, name):
    import re
    match = re.search(r"(?:^|\n)(?:static )?(?:void|boolean|int|DWORD|unsigned long)\s+(?:WINAPI )?" + name + r"\s*\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(name)
    start = source.index("{", match.start())
    level = 1
    end = start + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"

state = tracked("source/saved games/game_state.c")
xbox = tracked("source/saved games/game_state_xbox.c")
header = tracked("source/saved games/game_state.h")
struct = header[header.index("struct game_state_header\n"):header.index("/* ---------- prototypes/GAME_STATE.C */")]
crc = tracked("source/memory/crc.c")
mobile = state[state.index("#ifdef HALO_ANDROID\n/* port: one mobile checkpoint"):]

prefix = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
__declspec(dllimport) void __stdcall ExitProcess(unsigned long);
__declspec(dllimport) int __stdcall MoveFileExA(char const *, char const *, unsigned long);
static void test_exit(int result) { ExitProcess(result); }
#define host_sync _commit
#define host_fileno _fileno
#else
#include <unistd.h>
static void test_exit(int result) { exit(result); }
#define host_sync fsync
#define host_fileno fileno
#endif
#define TRUE 1
#define FALSE 0
#define HALO_ANDROID 1
#define HALO_GLES 1
#define GAME_STATE_SIZE 4096
#define PORT_IMAGE_SHIFT 0UL
#define WINAPI
typedef unsigned char byte;
typedef int boolean;
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); test_exit(1); } } while (0)
#define match_assert(a,b,c) CHECK(c)
static struct { boolean initialized; unsigned long table[256]; } crc_globals;
'''

stubs = r'''
static byte live[GAME_STATE_SIZE], snapshot[GAME_STATE_SIZE];
static struct { void *base_address; boolean saved_game_valid; } game_state_globals;
static struct { int file_open, file_valid_for_read; long buffer_size; int handle; } xbox_game_state_globals;
static struct { void *snapshot; int snapshot_valid; } game_state_writer;
static int writer_waits, fallback_reads, fallback_success = 1;
#define FILE_BEGIN 0
#define INVALID_SET_FILE_POINTER 0xffffffffUL
static void game_state_writer_wait(void) { writer_waits++; }
static unsigned long SetFilePointer(int handle, long offset, void *high, int origin) { return 0; }
static int ReadFile(int handle, void *image, long size, unsigned long *read, void *unused) {
    fallback_reads++; if (!fallback_success) return 0;
    memcpy(image, snapshot, size); *read = size; return 1;
}
static struct { short type; } scenario_value;
static void *scenario_dummy;
static int playing = 1, connection, cinematic, arrays_valid = 1, header_valid = 1;
static int current_renderer;
static int android_checkpoint_renderer(void) { return current_renderer; }
static int before_calls, after_calls, save_calls, stop_calls, start_calls, persistent = 1;
static short player_spawn_count = 1, selected_difficulty = 1;
static char selected_map[256];
#define global_scenario (&scenario_value)
#define _game_connection_local 0
#define _scenario_type_solo 0
static int game_in_progress(void) { return playing; }
static short game_connection(void) { return connection; }
static int cinematic_in_progress(void) { return cinematic; }
static int game_state_header_valid(struct game_state_header *h, int halt) { return header_valid; }
static int game_state_image_valid(void *image, long size) { return arrays_valid; }
static void game_state_call_before_load_procs(void) { before_calls++; }
static void game_state_call_after_load_procs(void) { after_calls++; }
static void game_state_save(void) { save_calls++; }
static void game_state_note_event(char const *event) { }
static void game_difficulty_level_set(short difficulty) { }
static short main_get_difficulty(void) { return selected_difficulty; }
static void main_set_map_name(char const *map) { strcpy(selected_map, map); persistent = 1; }
static void main_set_difficulty(short difficulty) { selected_difficulty = difficulty; }
static void main_disallow_persistent_storage(void) { persistent = 0; }
static void main_stop_time(void) { stop_calls++; }
static void main_start_time(void) { start_calls++; }
#define csmemcpy memcpy
static int fail_open, fail_write, fail_sync, fail_close, fail_rename, writes;
static char const *disk_path(char const *path) { return strncmp(path, "z:\\", 3) ? path : path + 3; }
static FILE *test_open(char const *path, char const *mode) { return fail_open ? NULL : fopen(disk_path(path), mode); }
static size_t test_write(void const *p, size_t size, size_t count, FILE *file) {
    writes++; if (fail_write && writes == fail_write) return 0;
    return fwrite(p, size, count, file);
}
static int test_sync(int fd) { return fail_sync ? -1 : host_sync(fd); }
static int test_close(FILE *file) { int result = fclose(file); return fail_close ? -1 : result; }
static int test_rename(char const *from, char const *to) {
#ifdef _WIN32
    return fail_rename ? -1 : !MoveFileExA(disk_path(from), disk_path(to), 1);
#else
    return fail_rename ? -1 : rename(disk_path(from), disk_path(to));
#endif
}
static int test_remove(char const *path) { return remove(disk_path(path)); }
/* Production includes cseries.h: its free macro requires an allocation. */
static int engine_free_calls;
static void test_engine_free(void *pointer) {
    CHECK(pointer != NULL); engine_free_calls++; free(pointer);
}
#define free test_engine_free
#define fopen test_open
#define fwrite test_write
#define fclose test_close
#define fsync test_sync
#define fileno host_fileno
#define rename test_rename
#define remove test_remove
'''

tests = r'''
#undef fopen
#undef fwrite
#undef fclose
#undef remove
#undef free
static struct android_checkpoint_request request;
static byte good[512 + GAME_STATE_SIZE];
static void save_good(void) {
    FILE *f = fopen("mobile-checkpoint.bin", "rb");
    CHECK(f != NULL); CHECK(fread(good, 1, sizeof(good), f) == sizeof(good)); CHECK(!fclose(f));
}
static void restore_good(void) {
    FILE *f = fopen("mobile-checkpoint.bin", "wb");
    CHECK(f != NULL); CHECK(fwrite(good, 1, sizeof(good), f) == sizeof(good)); CHECK(!fclose(f));
}
static void unchanged(void) {
    byte current[sizeof(good)]; FILE *f = fopen("mobile-checkpoint.bin", "rb");
    CHECK(f != NULL); CHECK(fread(current, 1, sizeof(current), f) == sizeof(current)); CHECK(!fclose(f));
    CHECK(!memcmp(good, current, sizeof(good)));
}
static void write_request(char const *path, char const *operation) {
    FILE *f = fopen(path, "wb"); CHECK(f != NULL);
    fprintf(f, "%s %s %s %s\n", request.id, operation, request.fingerprint[0], request.fingerprint[1]);
    CHECK(!fclose(f));
}
static void status_is(char const *value) {
    char id[64], status[64]; FILE *f = fopen("mobile-checkpoint.status", "rb"); CHECK(f != NULL);
    CHECK(fgets(id, sizeof(id), f) != NULL); CHECK(fgets(status, sizeof(status), f) != NULL); CHECK(!fclose(f));
    CHECK(!strncmp(id, request.id, 32)); CHECK(!strncmp(status, value, strlen(value))); CHECK(status[strlen(value)] == '\n');
}
static void read_is(char const *status) {
    void *image = NULL; CHECK(!strcmp(android_checkpoint_read(&request, &image), status));
    CHECK((image != NULL) == !strcmp(status, "ready")); free(image);
}
int main(void) {
    struct game_state_header *h = (struct game_state_header *)snapshot;
    struct android_checkpoint_request parsed;
    FILE *f; void *image; byte copy[GAME_STATE_SIZE]; int i, frees_before;
    memset(&request, 0, sizeof(request)); memset(request.id, 'a', 32); strcpy(request.operation, "save");
    memset(request.fingerprint[0], 'b', 64); memset(request.fingerprint[1], 'c', 64);
    game_state_globals.base_address = live; game_state_globals.saved_game_valid = TRUE;
    xbox_game_state_globals.file_open = 1; xbox_game_state_globals.file_valid_for_read = 1;
    xbox_game_state_globals.buffer_size = GAME_STATE_SIZE; game_state_writer.snapshot = snapshot; game_state_writer.snapshot_valid = 1;
    strcpy(h->map_name, "levels\\demob30\\demob30"); strcpy(h->build_number, "01.01.14.2342");
    h->difficulty = 1; h->player_count = 1; h->layout_address = (unsigned long)live; h->image_shift = PORT_IMAGE_SHIFT;
    snapshot[400] = 42; memset(live, 7, sizeof(live));
    CHECK(android_checkpoint_snapshot(copy, sizeof(copy))); CHECK(copy[400] == 42); CHECK(writer_waits == 1);
    game_state_writer.snapshot_valid = 0;
    CHECK(android_checkpoint_snapshot(copy, sizeof(copy))); CHECK(fallback_reads == 1); CHECK(copy[400] == 42);
    fallback_success = 0; CHECK(!android_checkpoint_snapshot(copy, sizeof(copy))); fallback_success = 1;
    CHECK(!android_checkpoint_snapshot(copy, sizeof(copy) - 1)); game_state_writer.snapshot_valid = 1;
    CHECK(!strcmp(android_checkpoint_write(&request), "saved")); save_good();
    CHECK(!memcmp(good, "OCESAVE1", 8)); CHECK(android_checkpoint_get32(good + 8) == 1);
    CHECK(android_checkpoint_get32(good + 12) == 512); CHECK(android_checkpoint_get32(good + 16) == GAME_STATE_SIZE);
    CHECK(android_checkpoint_get32(good + 24) == 1); CHECK(android_checkpoint_get32(good + 28) == 0);
    CHECK(!memcmp(good + 32, request.fingerprint[0], 64)); CHECK(good[512 + 400] == 42); CHECK(live[400] == 7);
    game_state_globals.saved_game_valid = 0; CHECK(!strcmp(android_checkpoint_write(&request), "no_checkpoint")); unchanged();
    game_state_globals.saved_game_valid = 1; connection = 1; CHECK(!strcmp(android_checkpoint_write(&request), "unsupported"));
    connection = 0; player_spawn_count = 2; CHECK(!strcmp(android_checkpoint_write(&request), "unsupported"));
    player_spawn_count = 1; cinematic = 1; CHECK(!strcmp(android_checkpoint_write(&request), "unsupported"));
    cinematic = 0; scenario_value.type = 1; CHECK(!strcmp(android_checkpoint_write(&request), "unsupported")); scenario_value.type = 0;
    for (i = 0; i < 5; i++) {
        fail_open = i == 0; fail_write = i == 1 ? 2 : 0; fail_sync = i == 2; fail_close = i == 3; fail_rename = i == 4; writes = 0;
        CHECK(!strcmp(android_checkpoint_write(&request), "io_error"));
        fail_open = fail_write = fail_sync = fail_close = fail_rename = 0; unchanged();
    }
    read_is("ready");
    fail_open = 1; read_is("io_error"); fail_open = 0;
    fail_close = 1; read_is("io_error"); fail_close = 0;
    current_renderer = 1; read_is("incompatible");
    CHECK(!strcmp(android_checkpoint_write(&request), "saved"));
    f = fopen("mobile-checkpoint.bin", "rb"); CHECK(f != NULL);
    CHECK(fread(copy, 1, 96, f) == 96); fclose(f);
    CHECK(android_checkpoint_get32(copy + 28) == 1); CHECK(!memcmp(copy + 32, request.fingerprint[1], 64));
    read_is("ready"); current_renderer = 0; restore_good();
    request.fingerprint[0][0] = 'd'; read_is("incompatible"); request.fingerprint[0][0] = 'b';
    good[28] = 1; restore_good(); read_is("incompatible"); good[28] = 0;
    good[24] = 4; restore_good(); read_is("corrupt"); good[24] = 1;
    good[351] = 1; restore_good(); read_is("corrupt"); good[351] = 0;
    good[511] = 1; restore_good(); read_is("corrupt"); good[511] = 0;
    good[512 + 400] ^= 1; restore_good(); read_is("corrupt"); good[512 + 400] ^= 1;
    restore_good(); f = fopen("mobile-checkpoint.bin", "ab"); fputc(0, f); fclose(f); read_is("corrupt");
    f = fopen("mobile-checkpoint.bin", "wb"); fwrite(good, 1, sizeof(good) - 1, f); fclose(f); read_is("corrupt"); restore_good();
    game_state_globals.base_address = live + 4; read_is("incompatible"); game_state_globals.base_address = live;
    write_request("mobile-checkpoint.request", "save"); CHECK(android_checkpoint_request_read(ANDROID_CHECKPOINT_REQUEST, &parsed));
    CHECK(!strcmp(parsed.id, request.id)); CHECK(!strcmp(parsed.fingerprint[0], request.fingerprint[0]));
    f = fopen("mobile-checkpoint.request", "wb"); fprintf(f, "%s save %s %s extra\n", request.id, request.fingerprint[0], request.fingerprint[1]); fclose(f);
    CHECK(!android_checkpoint_request_read(ANDROID_CHECKPOINT_REQUEST, &parsed)); remove("mobile-checkpoint.request");
    f = fopen("mobile-checkpoint.request", "wb"); fputs("incomplete", f); fclose(f);
    CHECK(!android_checkpoint_request_read(ANDROID_CHECKPOINT_REQUEST, &parsed)); remove("mobile-checkpoint.request");
    write_request("mobile-checkpoint.request", "save"); android_checkpoint_poll(1000); status_is("saved");
    CHECK(stop_calls == 1 && start_calls == 1); write_request("mobile-checkpoint.request", "save");
    android_checkpoint_poll(1100); f = fopen("mobile-checkpoint.request", "rb"); CHECK(f != NULL); fclose(f);
    android_checkpoint_poll(1250); status_is("saved");
    write_request("mobile-checkpoint.boot", "new"); android_checkpoint_boot_prepare(); CHECK(!persistent); status_is("new"); unchanged();
    frees_before = engine_free_calls; CHECK(android_checkpoint_pending_image == NULL);
    write_request("mobile-checkpoint.boot", "continue"); android_checkpoint_boot_prepare();
    CHECK(engine_free_calls == frees_before);
    CHECK(android_checkpoint_pending_image != NULL); CHECK(!strcmp(selected_map, h->map_name)); CHECK(selected_difficulty == h->difficulty);
    CHECK(before_calls == 0); CHECK(live[400] == 7); CHECK(!persistent);
    /* Replacing a pending load still releases its non-null allocation once. */
    write_request("mobile-checkpoint.boot", "continue"); android_checkpoint_boot_prepare();
    CHECK(engine_free_calls == frees_before + 1); CHECK(android_checkpoint_pending_image != NULL);
    android_checkpoint_after_new_map(); status_is("resumed"); CHECK(live[400] == 42);
    CHECK(engine_free_calls == frees_before + 2);
    CHECK(before_calls == 1 && after_calls == 1 && save_calls == 1); CHECK(android_checkpoint_pending_image == NULL);
    memset(live, 7, sizeof(live)); write_request("mobile-checkpoint.boot", "continue"); android_checkpoint_boot_prepare();
    arrays_valid = 0; android_checkpoint_after_new_map(); status_is("corrupt"); CHECK(before_calls == 1); CHECK(live[400] == 7); arrays_valid = 1;
    write_request("mobile-checkpoint.boot", "continue"); android_checkpoint_boot_prepare(); header_valid = 0;
    android_checkpoint_after_new_map(); status_is("incompatible"); CHECK(before_calls == 1); CHECK(live[400] == 7);
    printf("PASS %d checks\n", checks); return 0;
}
#ifdef _WIN32
void mainCRTStartup(void) { ExitProcess(main()); }
#endif
'''

source = prefix + struct + "\n" + "\n".join(function(crc, name) for name in ("crc_new", "build_crc_table", "crc_checksum_buffer")) + stubs + function(xbox, "android_checkpoint_snapshot") + mobile + tests

def windows_tools(args):
    if args.msvc_root:
        msvc = Path(args.msvc_root)
    elif os.environ.get("VCToolsInstallDir"):
        msvc = Path(os.environ["VCToolsInstallDir"])
    else:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        if not vswhere.is_file():
            raise SystemExit("MSVC Build Tools not found; pass --msvc-root.")
        install = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip())
        version = (install / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt").read_text().strip()
        msvc = install / "VC/Tools/MSVC" / version
    sdk_root = args.sdk_root or os.environ.get("WindowsSdkDir")
    if not sdk_root:
        import winreg
        candidates = []
        for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows Kits\Installed Roots", 0, winreg.KEY_READ | view) as key:
                    candidates.append(Path(winreg.QueryValueEx(key, "KitsRoot10")[0]))
            except OSError:
                pass
        sdk_root = next((path for path in candidates if (path / "Lib").is_dir()), None)
        if not sdk_root:
            raise SystemExit("Windows SDK not found; pass --sdk-root.")
    sdk = Path(sdk_root)
    version = args.sdk_version or os.environ.get("WindowsSDKVersion", "").rstrip("\\/")
    if not version:
        versions = [p.name for p in (sdk / "Lib").iterdir() if (p / "ucrt/x64/ucrt.lib").is_file()]
        version = max(versions, key=lambda value: tuple(int(part) for part in value.split(".")))
    return msvc, sdk / "Lib" / version


def run_tests(args, build):
    build.mkdir(parents=True, exist_ok=True)
    (build / "checkpoint.c").write_text(source)
    cc = args.cc or os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        raise SystemExit("C compiler not found; pass --cc clang.")
    flags = ["-std=gnu89", "-Werror=declaration-after-statement", "-Wno-pointer-to-int-cast"]
    if args.resource_dir:
        flags += ["-resource-dir", args.resource_dir]
    executable = build / ("checkpoint.exe" if os.name == "nt" else "checkpoint")
    if os.name == "nt":
        msvc, sdk = windows_tools(args)
        subprocess.run([cc, *flags, "-c", str(build / "checkpoint.c"), "-o", str(build / "checkpoint.obj")], check=True)
        linker = args.link or str(msvc / "bin/Hostx64/x64/link.exe")
        subprocess.run([linker, "/nologo", "/entry:mainCRTStartup", "/subsystem:console", "/out:" + str(executable), str(build / "checkpoint.obj"), "/libpath:" + str(sdk / "ucrt/x64"), "/libpath:" + str(sdk / "um/x64"), "/libpath:" + str(msvc / "lib/x64"), "ucrt.lib", "vcruntime.lib", "libcmt.lib", "kernel32.lib"], check=True)
    else:
        subprocess.run([cc, *flags, str(build / "checkpoint.c"), "-o", str(executable)], check=True)
    run = subprocess.run([str(executable)], cwd=build, text=True, capture_output=True)
    print(run.stdout.strip())
    if run.returncode:
        raise SystemExit(run.stderr or "Checkpoint C harness failed")
    data = (build / "mobile-checkpoint.bin").read_bytes()
    expected = int.from_bytes(data[20:24], "little")
    assert expected == zlib.crc32(data[512:])
    print("PASS CRC32 matches Python zlib")
    (build / "result.json").write_text(json.dumps({"runtime": run.stdout.strip(), "crc32_matches_zlib": True, "source": "game_state.c and game_state_xbox.c extracted; engine callbacks and file failures stubbed", "phone_tested": False}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("cc", "resource-dir", "link", "msvc-root", "sdk-root", "sdk-version", "build-dir"):
        parser.add_argument("--" + option)
    args = parser.parse_args()
    with contextlib.ExitStack() as stack:
        build = Path(args.build_dir).resolve() if args.build_dir else Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="opence-checkpoint-")))
        run_tests(args, build)

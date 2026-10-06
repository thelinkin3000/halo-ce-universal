"""Ninja rules for the Nintendo Switch build (``ninja switch``).

The Switch port (port/switch/README.md) runs the same ILP32 AArch64 guest
image as the Android port (port/android/README.md) inside an ordinary
devkitA64 homebrew program. This graph builds

- the guest image, build/switch/halo_guest.elf: the Android guest, AArch64
  code with 32-bit pointers built by the same clang for arm64_32 and the
  same linker script, with the deko3d renderer (port/switch/DEKO3D.md) in
  place of the OpenGL one;
- the host, build/switch/halo.nro: the Android host library
  (port/android/host) with three files replaced by port/switch/host, so that
  it answers the guest's SDL3 calls with devkitPro's SDL2 and links against
  Mesa's static EGL and GLES instead of the NDK's shared libGLESv3.so.

Two toolchains are involved and neither can be replaced by the other. The
guest needs clang's arm64_32 target and the NDK's llvm-ar and ld.lld, and
devkitA64 is LP64 only, so it cannot build the guest. The host needs
devkitA64's compiler and libnx. The NDK is used only for the guest.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .android_build import (ANDROID_API, GUEST_ABI_FLAGS, GUEST_CODE_FLAGS, MUSL_DIRECTORIES, MUSL_EXCLUDE,
                            MUSL_FILES, MUSL_THREAD_PREFIXES, MUSL_URL, MUSL_VERSION, VARIADIC_PROTOTYPE_FILES,
                            ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES,
                            _musl_sources, _find_ndk, fetch_third_party)
from .linux_build import (LINUX_PROFILE, MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, XDK_INCLUDE,
                          compile_launcher, game_defines_and_includes, game_sources, miniupnpc_sources,
                          musl_math_sources, pgo_mode, pgo_profile,
                          profile_use_flags, updater_defines, xdk_headers)
from .embed_assets import hud_assets_build, hud_configure_inputs
from .ninja_syntax import Writer

PORT_DIR = Path("port/switch")
# the guest half is the Android port's, unchanged: same image, same ABI, so
# the guest sources, the libc, the runtime and the import table are its files
ANDROID_PORT_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/switch")
THIRD_PARTY = Path("build/android/third_party")
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")
EXPAT_DIR = Path("port/third_party/expat")
MONOCYPHER_DIR = Path("port/third_party/monocypher")
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
SDL_DIR = THIRD_PARTY / "SDL3"

# devkitPro's static graphics and window libraries. Mesa is static (the
# switch-mesa package ships libEGL.a and libGLESv2.a), so there is no
# libEGL.so to load at run time and none has to be shipped.
# libglapi is Mesa's own dispatch table and has to be named; Mesa is C++
# (nv50_ir and its exceptions), so the host is linked with the C++ driver
# to bring in the standard library, which is what LittleGPTracker's Switch
# Makefile does for the same reason.
HOST_LIBRARIES = ["SDL2", "EGL", "GLESv2", "glapi", "drm_nouveau", "deko3d", "nx", "pthread", "m"]

# UAM, deko3d's shader compiler, built as a library for the console so that
# the deko3d renderer can compile shaders at run time (port/switch/DEKO3D.md).
# devkitPro ships it only as a PC tool. Pinned, and patched (uam.patch) to
# build a static library beside the tool and to check for mako without
# distutils, which Python 3.12 dropped. Its build runs Python's mako, bison
# and flex on the build machine.
UAM_URL = "https://github.com/devkitPro/uam.git"
UAM_COMMIT = "5a5afc2bae8b55409ab36ba45be63fcb73f68993"
UAM_DIR = BUILD / "third_party" / "uam"
UAM_PATCH = PORT_DIR / "uam.patch"


def fetch_uam() -> bool:
    """Clone UAM at its pinned commit and apply the port's patch (configure time, and again
    whenever the patch changes: the marker records which patch the clone has)."""
    import hashlib
    marker = UAM_COMMIT + " " + hashlib.sha256(UAM_PATCH.read_bytes()).hexdigest() + "\n"
    if (UAM_DIR / ".patched").is_file() and (UAM_DIR / ".patched").read_text() == marker:
        return True
    if UAM_DIR.exists():
        shutil.rmtree(UAM_DIR)
    UAM_DIR.parent.mkdir(parents=True, exist_ok=True)
    print(f"Cloning UAM {UAM_COMMIT[:12]}")
    subprocess.run(["git", "init", "-q", str(UAM_DIR)], check=True)
    subprocess.run(["git", "-C", str(UAM_DIR), "fetch", "-q", "--depth", "1", UAM_URL, UAM_COMMIT], check=True)
    subprocess.run(["git", "-C", str(UAM_DIR), "checkout", "-q", "FETCH_HEAD"], check=True)
    subprocess.run(["git", "-C", str(UAM_DIR), "apply", str(UAM_PATCH.resolve())], check=True)
    (UAM_DIR / ".patched").write_text(marker)
    return True


def _uam_build(n: Writer, devkitpro: Path, host_cc: Path, host_arch: str) -> Optional[Path]:
    """Ninja rules for build/switch/uam/libuam.a, or None (and a comment) if UAM cannot be built here."""
    if not shutil.which("meson") or not shutil.which("bison") or not shutil.which("flex"):
        n.comment("Switch build: no UAM (it needs meson, bison and flex)")
        return None
    if subprocess.run([sys.executable, "-c", "import mako"], capture_output=True).returncode != 0:
        n.comment("Switch build: no UAM (it needs Python's mako: pip install --user mako)")
        return None
    try:
        fetch_uam()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Switch build: cannot fetch UAM ({error})", file=sys.stderr)
        return None

    build_dir = BUILD / "uam"
    cross_file = build_dir / "cross.ini"
    flags = ", ".join(f"'{flag}'" for flag in [*host_arch.split(), "-D__SWITCH__", "-isystem",
                                                str(devkitpro / "libnx" / "include")])
    link_flags = ", ".join(f"'{flag}'" for flag in [f"-specs={devkitpro / 'libnx' / 'switch.specs'}",
                                                     *host_arch.split(), f"-L{devkitpro / 'libnx' / 'lib'}", "-lnx"])
    build_dir.mkdir(parents=True, exist_ok=True)
    cross_file.write_text(
        "[binaries]\n"
        f"c = '{host_cc}'\n"
        f"cpp = '{host_cc.parent / 'aarch64-none-elf-g++'}'\n"
        f"ar = '{host_cc.parent / 'aarch64-none-elf-gcc-ar'}'\n"
        f"strip = '{host_cc.parent / 'aarch64-none-elf-strip'}'\n"
        "\n[built-in options]\n"
        f"c_args = [{flags}]\ncpp_args = [{flags}]\n"
        f"c_link_args = [{link_flags}]\ncpp_link_args = [{link_flags}]\n"
        "\n[properties]\nneeds_exe_wrapper = true\n"
        "\n[host_machine]\nsystem = 'horizon'\ncpu_family = 'aarch64'\ncpu = 'cortex-a57'\nendian = 'little'\n",
        encoding="utf-8")

    # meson's build of the library is a thin archive (members by path), so it
    # is repacked into an ordinary one that can be linked from anywhere.
    #
    # b_staticpic=false: meson compiles a static library with -fPIC by default,
    # after the cross file's -fPIE, and -fPIC gives thread-local variables an
    # access model that needs the thread pointer in a register - while
    # devkitA64's -mtp=soft gets it from a call (__aarch64_read_tp) whose result
    # overwrites the variable's offset. The code then reads the thread pointer
    # plus itself, and the first compile faults (uam.patch's per-thread state;
    # the probe found it). -fPIE, as the host is built, uses local-exec, which
    # works.
    library = build_dir / "libuam.a"
    meson_dir = build_dir / "meson"
    ar = host_cc.parent / "aarch64-none-elf-ar"
    n.rule(
        name="switch_uam",
        command=(f"rm -rf {meson_dir} && meson setup -Db_staticpic=false --cross-file {cross_file.resolve()} "
                 f"{meson_dir.resolve()} {UAM_DIR.resolve()} > /dev/null && ninja -C {meson_dir} libuam.a > /dev/null && "
                 f"rm -f $out && cd {meson_dir} && {ar} rcs {library.resolve()} $$({ar} t libuam.a)"),
        description="SWITCH UAM $out",
    )
    n.build(outputs=library, rule="switch_uam", implicit=[UAM_PATCH, UAM_DIR / ".patched", cross_file])
    n.build(outputs="switch-uam", rule="phony", inputs=[library])
    return library


def _devkitpro() -> Optional[Path]:
    root = os.environ.get("DEVKITPRO")
    if root and Path(root).is_dir():
        return Path(root)
    for candidate in (Path("/opt/devkitpro"), Path.home() / "devkitpro"):
        if candidate.is_dir():
            return candidate
    return None


def _devkita64(devkitpro: Path) -> Optional[Path]:
    root = os.environ.get("DEVKITA64")
    if root and Path(root).is_dir():
        return Path(root)
    for candidate in (devkitpro / "devkitA64", devkitpro / "devkitARM64"):
        if candidate.is_dir():
            return candidate
    return None


def switch_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "host", PORT_DIR / "guest", ANDROID_PORT_DIR / "host_imports.list",
            LINUX_DIR / "src", UAM_PATCH, *hud_configure_inputs()]


def generate_switch_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not (PORT_DIR / "host").is_dir():
        return

    devkitpro = _devkitpro()
    devkita64 = _devkita64(devkitpro) if devkitpro else None
    if not devkitpro or not devkita64:
        n.comment("Switch build: no devkitPro found (set DEVKITPRO and DEVKITA64)")
        return
    portlibs = devkitpro / "portlibs" / "switch"
    # What the console shows for this program in its menus. Fixed rather than
    # taken from the environment: $USER on a build machine is whoever's account
    # ran it, which would put a different publisher on every release and on CI
    # whatever the runner happens to be called.
    nacp_title = "Halo: Combat Evolved"
    nacp_author = "thelinkin3000"
    nacp_version = "1.0.0"
    # The icon does not go in the NACP: it is an asset, embedded by elf2nro
    # below. A 256x256 JPEG, converted from the same master the Android icon
    # comes from (port/android/art/android-icon.png, via tools/android_icon.py),
    # so the launcher on the console and the one on the phone show the same
    # picture rather than two pictures that happen to be a port of each other.
    # absolute, for the same reason the paths below are: elf2nro reports
    # "Failed to open input!" for a relative one
    nro_icon = (Path.cwd() / PORT_DIR / "art" / "icon.jpg").resolve()
    if not (portlibs / "lib" / "libSDL2.a").is_file():
        n.comment("Switch build: devkitPro has no SDL2 (dkp-pacman -S switch-sdl2 switch-mesa)")
        return

    ndk = Path(sln.android_ndk) if getattr(sln, "android_ndk", None) else _find_ndk()
    if not ndk or not ndk.is_dir():
        n.comment("Switch build: no NDK found (the guest is ILP32; devkitA64 cannot build it)")
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Switch build disabled: cannot fetch musl/SDL3 ({error})", file=sys.stderr)
        return

    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    toolchain = ndk / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64"
    sysroot_include = toolchain / "sysroot" / "usr" / "include"
    ndk_bin = toolchain / "bin"
    guest_cc = getattr(sln, "android_guest_cc", None) or "clang"
    # devkitPro's aarch64-none-elf cross compiler, not the aarch64-linux-gnu
    # one: this is a bare-metal homebrew program with libnx and newlib, not
    # a Linux process.
    host_cc = Path(os.environ.get("DEVKITPRO", str(devkitpro))) / "devkitA64" / "bin" / "aarch64-none-elf-gcc"
    if not host_cc.is_file():
        host_cc = devkita64 / "bin" / "aarch64-none-elf-gcc"
    if not host_cc.is_file():
        n.comment(f"Switch build: no aarch64-none-elf-gcc under {devkita64}")
        return

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    gl_include = guest_dir / "gl_include"
    arch = ANDROID_PORT_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    host_obj_dir = BUILD / "host" / "obj"
    nro = BUILD / "halo.nro"
    python = "$python"

    n.comment("Switch build (ninja switch); see port/switch/README.md")
    n.variable("switch_guest_cc", guest_cc)
    n.variable("switch_ndk_bin", str(ndk_bin))
    n.variable("switch_host_cc", str(host_cc))
    # the C++ driver, for linking against Mesa
    n.variable("switch_host_gxx", str(host_cc.parent / "aarch64-none-elf-g++"))
    n.variable("switch_specs", str(devkitpro / "libnx" / "switch.specs"))
    # libnx is a separate tree from the portlibs, and the link needs both;
    # the paths are spelled out per -L below rather than in one variable,
    # because a space-separated list is not what -L takes
    switch_includes = " ".join(
        f"-I{path}" for path in (portlibs / "include", devkitpro / "libnx" / "include",
                                 devkitpro / "switch" / "include", devkitpro / "devkitA64" / "newlib" / "include")
    )

    # ---------- the guest image, built exactly as the Android port builds it

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="switch_alltypes",
        command=f"sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="SWITCH MUSL $out",
    )
    n.build(outputs=alltypes, rule="switch_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="switch_syscall_h",
        command=f"cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="SWITCH MUSL $out",
    )
    n.build(outputs=syscall_h, rule="switch_syscall_h", inputs=[arch / "bits" / "syscall.h.in"])
    n.rule(
        name="switch_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="SWITCH MUSL $out",
    )
    n.build(outputs=version_h, rule="switch_version_h")

    gl_stamp = gl_include / "stamp"
    n.rule(
        name="switch_gl_include",
        command=(f"mkdir -p {gl_include} && ln -sfn {sysroot_include}/GLES2 {gl_include}/GLES2 && "
                 f"ln -sfn {sysroot_include}/GLES3 {gl_include}/GLES3 && "
                 f"ln -sfn {sysroot_include}/KHR {gl_include}/KHR && touch $out"),
        description="SWITCH GL HEADERS",
    )
    n.build(outputs=gl_stamp, rule="switch_gl_include")

    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name="switch_gl_stubs",
        command=(f"{python} tools/android_gl_stubs.py {LINUX_DIR}/src/gl.h {sysroot_include}/GLES3/gl32.h "
                 f"{sysroot_include}/GLES2/gl2ext.h {guest_gl_c} {gl_imports}"),
        description="SWITCH GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule="switch_gl_stubs",
            implicit=[Path("tools/android_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="switch_posix_stubs",
        command=f"{python} tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description="SWITCH POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="switch_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    imports_s = gen_dir / "imports.s"
    host_table_c = BUILD / "host" / "host_import_table.c"
    host_imports_list = ANDROID_PORT_DIR / "host_imports.list"
    # the Switch host's own: the deko3d renderer's (port/switch/guest/d3d8_dk.c)
    switch_imports_list = PORT_DIR / "host_imports.list"
    n.rule(
        name="switch_imports",
        command=f"{python} tools/android_imports.py --translate-descriptors --host-table {host_table_c} {imports_s} $in",
        description="SWITCH IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="switch_imports",
            inputs=[host_imports_list, switch_imports_list, posix_imports, gl_imports],
            implicit=[Path("tools/android_imports.py")])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, gl_stamp,
                         semantics_header, platform_semantics_header]

    n.newline()
    n.rule(
        name="switch_guest_cc",
        command=(f"{compile_launcher(sln)}$switch_guest_cc -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 f"{python} tools/android_asm_convert.py $out.darwin.s $out.s && "
                 f"$switch_guest_cc --target=aarch64-linux-android -c $out.s -o $out"),
        description="SWITCH CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="switch_guest_asm",
        command=f"$switch_guest_cc --target=aarch64-linux-android -c $in -o $out",
        description="SWITCH AS $out",
    )

    # HALO_ANDROID tells the guest that the Xbox contiguous window is
    # relocatable and that the host will say where in the boot structure
    # (port/linux/src/platform.h). Without it PLATFORM_CONTIGUOUS_BASE is a
    # compile-time constant, and the guest ignores the host entirely.
    #
    # The Switch needs it for the same reason Android has it, only more
    # starkly: 0x80000000 is reported free by the kernel and refused by
    # svcMapMemory at every size, so the window cannot live there and must
    # be placed somewhere else and the game data moved with it.
    #
    # The name is Android's and stays that way - it is the port's own flag,
    # shared through port/linux/src with every other port that relocates the
    # window, and changing it would be a larger edit than the Switch needs.
    guest_abi = " ".join(GUEST_ABI_FLAGS + ["-DHALO_ANDROID", "-DHALO_SWITCH"] +
                         (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))

    # musl is configured for an Apple target and passes
    # -fno-define-target-os-macros, which tells clang not to define that
    # target's OS macros. Which clangs have it is not something to assume:
    # Ubuntu's clang 18 has it and builds this port, the NDK's clang 17.0.2
    # does not, and neither does the clang a Debian-based image installs. All
    # three were tried from this one build file.
    #
    # So the compiler is asked instead. Dropping the flag is safe here because
    # the two macros it suppresses, __APPLE__ and __MACH__, are already
    # undefined on the next line of GUEST_ABI_FLAGS; what it does beyond that
    # is decide whether a target macro is predefined, and this guest is not
    # Apple's and does not care.
    #
    # Only the Switch build is changed. The Android build keeps the flag
    # because the compiler it uses has always had it.
    if "-fno-define-target-os-macros" in guest_abi:
        probe = subprocess.run(
            [guest_cc, "--target=arm64_32-apple-watchos", "-fno-define-target-os-macros",
             "-E", "-x", "c", os.devnull, "-o", os.devnull],
            capture_output=True)
        if probe.returncode != 0:
            n.comment("Switch build: this clang has no -fno-define-target-os-macros; leaving it out")
            guest_abi = guest_abi.replace(" -fno-define-target-os-macros", "")
    # No -g on the guest, and that is a decision rather than an oversight.
    #
    # The guest has a symbol table either way, so a backtrace through the game
    # names its functions; what -g would add is source lines. It was tried and
    # does not survive the link. The guest is ILP32, so clang's debug
    # information has address size 4, and lld does not relocate the debug
    # information of address-size-4 objects: every compile unit in the linked
    # image keeps abbr_offset 0 and so reads the first one's abbreviations.
    # The result is a 34 MB image whose line table is not merely missing but
    # wrong, which is worse than none - a debugger would confidently show the
    # wrong line.
    #
    # It is specific to address size, not to this port's linker script: the
    # same objects linked with addr_size 8 (plain aarch64-linux) are relocated
    # correctly, and the guest's own objects linked without a script are not.
    # No DWARF version changes it (2, 4 and 5 all behave the same) and lld has
    # no option to merge them differently. Making it work means patching the
    # abbreviation offsets after the link, which needs the linker's own
    # layout of the merged table - it is not the sum of the inputs - so it is
    # a piece of build machinery to write, not a flag to turn on.
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [Path("tools/android_asm_convert.py"), *generated_headers]
    profile = pgo_profile(sln, LINUX_PROFILE if pgo_mode(sln) == "train" else None, [LINUX_PROFILE], guest_cc)
    profile_flags = " ".join(profile_use_flags(profile))
    if profile:
        tool_implicit.append(profile)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="switch_guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]

    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{ANDROID_PORT_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="switch_ar",
        command=f"rm -f $out && $switch_ndk_bin/llvm-ar rcs $out @$out.rsp",
        description="SWITCH AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="switch_ar", inputs=musl_objects)

    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        # the headers of the port's own game units (port/linux/game), for the
        # game sources that call them
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    objects: List[Path] = []
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {ANDROID_PORT_DIR}/include/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{ANDROID_PORT_DIR}/guest/runtime",
        f"-I{ANDROID_PORT_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}", f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}  # replaced by guest_memory_watch.c
    # the OpenGL renderer's device, texture cache and anti-aliasing passes:
    # the deko3d renderer's take their place, below
    opengl_only = {"d3d8_gl.c", "xbox_textures.c", "xgpu_post.c"}
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only or source.name in opengl_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    for source in hud_assets_build(n, "switch", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    # the menus' XML parser (port/third_party/expat; menu_files.c)
    for name in ("xmlparse.c", "xmlrole.c", "xmltok.c"):
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    # internet play's signatures, for public games' listings
    # (port/third_party/monocypher; p2p_crypto.c)
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    # the port's zlib, built as the Android port builds it (tools/android_build.py)
    for name in ZLIB_SOURCES:
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES,
                                                               "-U__ARM_FEATURE_CRC32"])))

    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", profile_flags, *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))

    runtime_dir = ANDROID_PORT_DIR / "guest" / "runtime"
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{runtime_dir}", f"-I{ANDROID_PORT_DIR}/include",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{ANDROID_PORT_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{runtime_dir}", f"-I{ANDROID_PORT_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes,
    ])
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="switch_guest_asm", inputs=imports_s)
    objects.append(imports_o)

    linker_script = ANDROID_PORT_DIR / "guest" / "guest.ld"
    n.rule(
        name="switch_guest_link",
        command=(f"$switch_ndk_bin/ld.lld -m aarch64linux -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc} "
                 "$$($switch_host_cc -print-libgcc-file-name)"),
        description="SWITCH LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )

    # The deko3d renderer (port/switch/DEKO3D.md), the Switch's only one: its
    # device, port/switch/guest/d3d8_dk.c, takes the place of d3d8_gl.c
    dk_objects = list(objects)
    dk_objects.append(guest_object(PORT_DIR / "guest" / "d3d8_dk.c", platform_cflags))
    # the texture cache, which decodes as xbox_textures.c does and sends the
    # texels to the host (DEKO3D.md, phase 6, step 2)
    dk_objects.append(guest_object(PORT_DIR / "guest" / "xbox_textures_dk.c", platform_cflags))
    # the deko3d renderer's GLSL generators (DEKO3D.md, phase 4); the
    # OpenGL renderer's nv2a_vsh.c and nv2a_psh.c stay in the image, and the
    # copies' names do not collide with them
    dk_objects.append(guest_object(PORT_DIR / "guest" / "nv2a_vsh_dk.c", platform_cflags))
    dk_objects.append(guest_object(PORT_DIR / "guest" / "nv2a_psh_dk.c", platform_cflags))
    # the shader cache's guest half (DEKO3D.md, phase 5, step 4): the keys,
    # the key files, the import of the OpenGL records, the startup pass
    dk_objects.append(guest_object(PORT_DIR / "guest" / "dk_shaders.c", platform_cflags))
    n.build(outputs=image, rule="switch_guest_link", inputs=dk_objects, implicit=[libguestc, linker_script])

    # ---------- the host, built with devkitA64

    n.newline()
    n.rule(
        name="switch_host_cc",
        command="$switch_host_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="SWITCH HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # The architecture flags, and -fPIE with them, are needed at link time
    # as well as compile time. Without them ld fails with "read-only
    # segment has dynamic relocations", which libnx alone reproduces with a
    # four-line program; switch.specs does not add them.
    host_arch = "-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE"
    uam_library = _uam_build(n, devkitpro, host_cc, host_arch)
    host_cflags = " ".join([
        "-O2", "-g", "-Wall", "-Wno-unused-function", "-D_GNU_SOURCE", "-D__SWITCH__",
        # Frame pointers, which -O2 omits. host_debug.c walks the x29 chain to
        # print the host's own stack when it gives up, and there is no signal
        # handler to do it for us: the console has no syscall for installing
        # one, so the trace has to be taken by the code that is failing.
        "-fno-omit-frame-pointer",
        # fPIE is required, and not for a style reason. Link-time, it is what
        # lets the loader resolve the relocations libnx and SDL2 leave
        # behind; without it ld fails with "read-only segment has dynamic
        # relocations", which is how this port's first link failed. A
        # four-line program linking only libnx reproduces it. devkitA64's
        # switch.specs does not add it.
        host_arch,
        # devkitA64: the guest's own thread and socket code are newlib's
        f"-I{ANDROID_PORT_DIR}/include", f"-I{PORT_DIR}/host", f"-I{PORT_DIR}/include", f"-I{SDL_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{TOML_DIR}", f"-I{libc_include}", switch_includes,
    ])
    # The Android host's own files, with this port's replacements (the
    # three in port/switch/host take the place of port/android/host's) and
    # posix_net.c and posix_upnp.c left out: host_net.c has the socket calls
    # over libnx, and answers the desktop-only and UPnP ones itself.
    host_sources = list(sorted((PORT_DIR / "host").glob("*.c")))
    # the stack switch into the guest, which has to be assembly: it moves the
    # stack pointer, which C cannot express
    host_assembly = list(sorted((PORT_DIR / "host").glob("*.S")))
    # UAM's compiler in the host (DEKO3D.md, phase 5, step 2), or the stub in
    # its place (host_dk_compiler_stub.c) when this build has no UAM: without
    # one the deko3d renderer has only the shaders already on the card, and
    # the OpenGL renderer depends on UAM in no build at all
    if uam_library is None:
        n.comment("Switch build: the deko3d renderer will have no shaders it has not already got "
                  "(no UAM: it needs meson, bison, flex and mako)")
    else:
        host_sources = [source for source in host_sources if source.name != "host_dk_compiler_stub.c"]
    host_sources += [
        LINUX_DIR / "src" / "posix_files.c",
        TOML_DIR / "tomlc17.c",
    ]
    # the disc image reader, so a retail .iso on the card can be unpacked into
    # the maps folder by the port itself rather than copied there by hand. It is
    # the same source the desktop port and the guest use, built here without
    # the game's platform layer (see the guard in xiso.c).
    extractor_cflags = f"{host_cflags} -DHALO_EXTRACTOR_STANDALONE"
    host_objects: List[Path] = []
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        cflags = host_cflags
        # the self-updater knows its build number and flavour (as the
        # desktop's and Android's do), and only it, so a new number rebuilds
        # one file
        if source.name == "host_update.c":
            cflags = f"{host_cflags} {updater_defines(getattr(sln, 'port_release', False))}"
        # the shader cache names its folder for the compiler whose DKSH files
        # it keeps, so a new UAM does not read an old compiler's (and this is
        # known even in a stub build, whose folder is then the same one)
        if source.name == "host_dk_shaders.c":
            cflags = f'{host_cflags} -DHOST_DK_UAM_COMMIT=\\"{UAM_COMMIT[:12]}\\"'
        n.build(outputs=obj, rule="switch_host_cc", inputs=source, variables={"cflags": cflags},
                implicit=[syscall_h])
        host_objects.append(obj)
    # UAM's compiler, linked into the host as the probe links it (DEKO3D.md,
    # phase 5, step 2; the probe's Makefile is the control for this). One C++
    # file, host_dk_compiler.cpp, is compiled with UAM's own include paths
    # and defines (as UAM's meson build gives them) and linked with UAM's
    # library into a single relocatable object, every symbol they define is
    # renamed (which renames the C++ COMDAT groups too, whose names are what
    # really clash with Mesa's) and all but host_dk_compile_glsl are made
    # local. That is what lets UAM sit beside the Mesa that SDL2 links in:
    # both are Mesa's GLSL compiler, and linked plainly they collide. The
    # object is built without -fPIC, as UAM's library is (step 1: under it,
    # a thread-local variable faulted with devkitA64's -mtp=soft), which is
    # also what the host's own -fPIE asks for.
    if uam_library is not None:
        uam_cflags = " ".join([
            "-std=c++11", "-DNDEBUG", "-DDESKTOP", "-D_USE_MATH_DEFINES", "-D_GNU_SOURCE", "-DHAVE_POSIX_MEMALIGN",
            "-D__SWITCH__", host_arch, f"-isystem {devkitpro / 'libnx' / 'include'}",
            f"-I{UAM_DIR / 'source'}", f"-I{UAM_DIR / 'mesa-imported'}",
            f"-I{BUILD / 'uam' / 'meson' / 'mesa-imported'}",
            f"-I{BUILD / 'uam' / 'meson' / 'mesa-imported' / 'glsl'}",
            f"-I{BUILD / 'uam' / 'meson' / 'mesa-imported' / 'glsl' / 'glcpp'}",
        ])
        compiler_obj = host_obj_dir / "host_dk_compiler.cpp.o"
        n.rule(
            name="switch_host_cxx",
            command=f"$switch_host_gxx -MMD -MF $out.d {uam_cflags} -c $in -o $out",
            description="SWITCH HOST CXX $out",
            depfile="$out.d",
            deps="gcc",
        )
        n.build(outputs=compiler_obj, rule="switch_host_cxx", inputs=PORT_DIR / "host" / "host_dk_compiler.cpp",
                implicit=[uam_library])
        combined = host_obj_dir / "uam_combined.o"
        symbols = host_obj_dir / "uam_symbols.txt"
        host_uam_object = host_obj_dir / "uam_compiler.o"
        nm_bin = host_cc.parent / "aarch64-none-elf-nm"
        objcopy_bin = host_cc.parent / "aarch64-none-elf-objcopy"
        ld_bin = host_cc.parent / "aarch64-none-elf-ld"
        n.rule(
            name="switch_uam_combine",
            command=(
                f"{ld_bin} -r -o {combined} {compiler_obj} --whole-archive {uam_library} --no-whole-archive && "
                f"{nm_bin} --defined-only {combined} | awk '{{print $$3}}' | "
                f"grep -v -e '^host_dk_compile_glsl$$' -e '^\\$$' | sort -u | "
                f"awk '{{print $$1\" uam_\"$$1}}' > {symbols} && "
                f"{objcopy_bin} --redefine-syms={symbols} --keep-global-symbol=host_dk_compile_glsl "
                f"{combined} $out"
            ),
            description="SWITCH UAM COMBINE $out",
        )
        n.build(outputs=host_uam_object, rule="switch_uam_combine", inputs=[compiler_obj], implicit=[uam_library])
        host_objects.append(host_uam_object)
    extractor = host_obj_dir / "xiso.c.o"
    n.build(outputs=extractor, rule="switch_host_cc", inputs=LINUX_DIR / "src" / "xiso.c",
            variables={"cflags": extractor_cflags}, implicit=[syscall_h])
    host_objects.append(extractor)

    # the host dispatches on the guest's own syscall numbers, read from the
    # generated header rather than written out, so its objects depend on it
    n.rule(
        name="switch_host_as",
        command="$switch_host_cc -c " + host_arch + " -o $out $in",
        description="SWITCH HOST AS $out",
    )
    # the menus' fonts and panel, which host_ui_assets.S includes as they are
    ui_assets = [Path("port/assets/fonts/OpenCE-Regular.ttf"), Path("port/assets/fonts/Overpass-750.ttf"),
                 PORT_DIR / "art" / "menu_panel.svg"]
    for source in host_assembly:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="switch_host_as", inputs=source,
                implicit=ui_assets if source.name == "host_ui_assets.S" else None)
        host_objects.append(obj)

    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="switch_host_cc", inputs=host_table_c, variables={"cflags": host_cflags})
    host_objects.append(table_obj)

    # The host is linked to an ELF and then packaged into an NRO.
    #
    # Both steps are needed and the second is easy to forget: an NRO is not
    # an ELF. It is a 32-byte header, a 16 KB NACP holding the title and
    # author, an asset section, and then the ELF. Write the ELF out with the
    # .nro name and the result is not an NRO at all - the Homebrew Menu
    # reports it as not supporting the current ABI, and Sphaira does not
    # show it properly. port/switch/probe/hello2 is the control: it is the
    # same program packaged the same way, and it runs.
    elf = BUILD / "halo.elf"
    nacp = BUILD / "halo.nacp"
    n.rule(
        name="switch_host_link",
        command=(f"$switch_host_gxx -specs=$switch_specs -g {host_arch} -o $out $in "
                 + " ".join(f"-L{path}" for path in (portlibs / "lib", devkitpro / "libnx" / "lib")) + " "
                 + " ".join(f"-l{lib}" for lib in HOST_LIBRARIES)
                 + " -Wl,-Map,$out.map"),
        description="SWITCH HOST LINK $out",
    )
    n.build(outputs=elf, rule="switch_host_link", inputs=host_objects)

    # devkitPro's own tools, which live beside the compiler
    n.rule(
        name="switch_nacp",
        command=f"{devkitpro / 'tools' / 'bin' / 'nacptool'} "
                f"--create '{nacp_title}' '{nacp_author}' {nacp_version} $out",
        description="SWITCH NACP $out",
    )
    n.build(outputs=nacp, rule="switch_nacp")

    n.rule(
        name="switch_nro",
        # elf2nro is given absolute paths: with relative ones it reports
        # success and writes nothing, which ninja does not notice.
        #
        # --icon goes after the two paths, not before them. Given first it
        # answers "Failed to open input!" and builds nothing, which is a
        # confusing way to say that an option has to follow the files it
        # applies to.
        command=(f"{devkitpro / 'tools' / 'bin' / 'elf2nro'} "
                 f"{(Path.cwd() / str(elf)).resolve()} {(Path.cwd() / str(nro)).resolve()}"
                 f" --icon={nro_icon} --nacp={(Path.cwd() / str(nacp)).resolve()}"),
        description="SWITCH NRO $out",
    )
    n.build(outputs=nro, rule="switch_nro", inputs=[elf, nacp, nro_icon])

    n.build(outputs="switch", rule="phony", inputs=[nro, image])
    n.newline()
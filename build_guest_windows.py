"""Build only the Android GLES guest using the installed Windows NDK and Git Bash."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
from types import SimpleNamespace
import urllib.request

from tools.android_build import generate_guest_image, MUSL_SHA256, MUSL_URL, MUSL_VERSION
from tools.guest_arch import ARM64_32
from tools.ninja_syntax import Writer, serialize_paths

ROOT = Path(__file__).resolve().parent
os.chdir(ROOT)
parser = argparse.ArgumentParser()
parser.add_argument("--ndk", required=True)
parser.add_argument("--clang")
parser.add_argument("--bash", help="Git Bash executable; otherwise discovered beside git on PATH")
parser.add_argument("--ninja", required=True)
parser.add_argument("--jobs", default="8")
parser.add_argument("--pgo", choices=("use", "off"), default="use")
args = parser.parse_args()
git = shutil.which("git")
bash_candidates = [Path(args.bash)] if args.bash else (
    [Path(git).parent.parent / "bin/bash.exe", Path(git).parent / "bash.exe"] if git else [])
bash = next((path.resolve() for path in bash_candidates if path.is_file()), None)
if bash is None:
    parser.error("Git Bash was not found; supply --bash with its executable path")
ndk = Path(args.ndk).resolve()
ndkbin = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
clang = str(Path(args.clang).resolve() if args.clang else ndkbin / "clang.exe").replace("\\", "/")
third_party = Path("build/android/third_party")
musl = third_party / f"musl-{MUSL_VERSION}"
if not musl.is_dir():
    third_party.mkdir(parents=True, exist_ok=True)
    archive = third_party / f"musl-{MUSL_VERSION}.tar.gz"
    with urllib.request.urlopen(MUSL_URL, timeout=60) as response:
        archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != MUSL_SHA256:
        raise SystemExit("musl archive SHA-256 mismatch")
    with tarfile.open(archive) as source:
        source.extractall(third_party, filter="data")

separator = "@@HALO_BASH_RULE@@"
runner = Path("build/run_bash_rule.py")
runner.parent.mkdir(exist_ok=True)
runner.write_text(f'''import os, pathlib, subprocess, sys
parts = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").split("@@HALO_BASH_RULE@@")
if len(parts) != 3:
    raise SystemExit("invalid build rule response file")
if parts[1]:
    pathlib.Path(parts[1]).write_text(parts[2], encoding="utf-8", newline="\\n")
environment = os.environ.copy()
shell = pathlib.Path({str(bash)!r})
environment["PATH"] = os.pathsep.join([str(shell.parent.parent / "usr/bin"), str(shell.parent), environment["PATH"]])
environment["MSYS2_ARG_CONV_EXCL"] = "*"
result = subprocess.run([str(shell), "-c", parts[0]], env=environment)
raise SystemExit(result.returncode)
''', encoding="utf-8", newline="\n")

def normal(value):
    if isinstance(value, (str, Path)):
        return str(value).replace('\\"', "@@ESCAPED_QUOTE@@").replace("\\", "/").replace("@@ESCAPED_QUOTE@@", '\\"')
    if isinstance(value, list):
        return [normal(item) for item in value]
    return value

class BashWriter(Writer):
    def variable(self, key, value, indent=0):
        return super().variable(key, normal(value), indent)

    def rule(self, name, command, **kwargs):
        response = kwargs.pop("rspfile", "")
        response_content = kwargs.pop("rspfile_content", "")
        command = normal(command)
        # MSYS symlinks cannot be followed by the native Windows compiler.
        if name == "android_gl_include":
            headers = str(ndkbin.parent / "sysroot/usr/include").replace("\\", "/")
            command = ("mkdir -p build/android/guest/gl_include && "
                       + " && ".join(f"cp -R '{headers}/{name}' build/android/guest/gl_include/" for name in ("GLES2", "GLES3", "KHR"))
                       + " && touch $out")
        kwargs["rspfile"] = "$bash_command_file"
        kwargs["rspfile_content"] = f"{command}{separator}{normal(response)}{separator}{response_content}"
        super().rule(name, f'"{sys.executable}" {runner.as_posix()} "$bash_command_file"', **kwargs)

    def build(self, outputs, rule, **kwargs):
        outputs = normal(outputs)
        first = serialize_paths(outputs)[0]
        variables = dict(kwargs.pop("variables", None) or {})
        variables["bash_command_file"] = first + ".shell-command"
        for key in ("inputs", "implicit", "implicit_outputs", "order_only"):
            if key in kwargs:
                kwargs[key] = normal(kwargs[key])
        return super().build(outputs, rule, variables=variables, **kwargs)

with Path("build/guest-windows.ninja").open("w", encoding="utf-8", newline="\n") as output:
    n = BashWriter(output, width=200)
    n.variable("python", sys.executable)
    n.variable("android_guest_cc", clang)
    n.variable("android_ndk_bin", ndkbin.as_posix())
    n.rule("msvc_semantics", "$python tools/linux_msvc_semantics.py --output $out $scan", description="MSVC SEMANTICS $out")
    n.build("build/linux/halo_msvc_semantics.h", "msvc_semantics", variables={"scan": "--all-inlines --tags source --inlines source --inlines port/include/xdk"})
    n.build("build/linux/platform_msvc_semantics.h", "msvc_semantics", variables={"scan": "--inlines port/include/xdk"})
    config = json.loads(Path("port/linux/port.json").read_text())
    guest = generate_guest_image(
        n, SimpleNamespace(port_release=True, port_profile=False, port_pgo=args.pgo), config,
        arch=ARM64_32, prefix="android", label="ANDROID GLES", build=Path("build/android"), third_party=third_party,
        guest_cc=clang, gl_headers=ndkbin.parent / "sysroot/usr/include",
        ar="$android_ndk_bin/llvm-ar", ld="$android_ndk_bin/ld.lld", objdump="$android_ndk_bin/llvm-objdump",
        builtins_cc=f"{ndkbin.as_posix()}/clang --target=aarch64-linux-android28", environment="android",
        platform_defines=["-DHALO_GLES=1", "-DHALO_ANDROID=1"],
        extra_imports=[Path("port/android/android_imports.list"), Path("port/android/host_imports_vk.list")],
        relocatable=True, vulkan=False)
    n.default([guest["checked"]])

raise SystemExit(subprocess.call([args.ninja, "-f", "build/guest-windows.ninja", "-j", args.jobs]))

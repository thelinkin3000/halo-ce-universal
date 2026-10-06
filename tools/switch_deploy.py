#!/usr/bin/env python3
"""Copy the Switch build to the console over FTP.

The console runs an FTP server from its Homebrew Menu, so a build can be put
on the card and run without taking it out. That makes the edit-build-run
loop a matter of one command and a wait, rather than a card reader.

Usage: python3 tools/switch_deploy.py [--host 192.168.2.173] [--port 5000]
                                     [--destination /switch/halo]
                                     [--user anonymous] [--password ...]
                                     [build/switch/halo.nro ...]

With no files, it uploads what the port needs: the program, the two guest
images, OpenGL's and deko3d's (config.toml's display.renderer chooses), and
internet play's brokers.txt, as a release carries it.
The game's own data (maps/) is not touched - it is large, it does not change
between builds, and it belongs to the player rather than to the build.
"""

import argparse
import ftplib
import sys
from pathlib import Path

DEFAULT_HOST = "192.168.2.173"
DEFAULT_PORT = 5000
DEFAULT_DESTINATION = "/switch/halo"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="*", type=Path,
                        help="files to upload; the default is the program and the guest images")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--destination", default=DEFAULT_DESTINATION)
    parser.add_argument("--user", default="anonymous")
    parser.add_argument("--password", default=None,
                        help="anonymous by default, which is what the console's server uses")
    args = parser.parse_args()

    # (and internet play's brokers, which a release carries beside the
    # game: without them the server browser lists nothing)
    files = args.files or [Path("build/switch/halo.nro"), Path("build/switch/halo_guest.elf"),
                          Path("port/assets/network/brokers.txt")]
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        print("not built: " + ", ".join(missing), file=sys.stderr)
        return 1

    try:
        ftp = ftplib.FTP()
        ftp.connect(args.host, args.port, timeout=30)
        ftp.login(args.user, args.password or "anonymous@")
    except (OSError, ftplib.Error) as error:
        print("cannot reach the console's FTP server at %s:%d: %s" % (args.host, args.port, error),
              file=sys.stderr)
        return 1

    try:
        ftp.cwd(args.destination)
    except ftplib.error_perm as error:
        print("no %s on the card: %s" % (args.destination, error), file=sys.stderr)
        ftp.quit()
        return 1

    for path in files:
        # Straight over the old file, with no rename dance.

        # The first version uploaded to name.new and renamed over the top,
        # on the reasoning that a failure part way through would leave the
        # old program runnable. That reasoning was wrong in the case that
        # mattered: this server refuses to rename onto an existing file, so
        # the fallback deleted the target first - which opens a window in
        # which neither the old name nor the new one exists, and loses the
        # program if anything goes wrong in it. A direct STOR replaces the
        # file in one step, which is checked to work here and has no such
        # window. The cost is that a transfer cut short leaves a truncated
        # file rather than an old one, which is why the size is compared
        # afterwards.
        print("%s -> %s/%s (%.1f MB)" % (path, args.destination, path.name, path.stat().st_size / 1048576.0))
        with path.open("rb") as handle:
            ftp.storbinary("STOR " + path.name, handle)
        expected = path.stat().st_size
        actual = ftp.size(path.name)
        if actual != expected:
            print("warning: %s is %d bytes on the card, %d expected; the transfer was cut short"
                  % (path.name, actual, expected), file=sys.stderr)
    ftp.quit()
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
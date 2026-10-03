# Handoff: Halo CE → Nintendo Switch port

Written as a starting point for a fresh session. Read it before touching
anything; most of it is the result of experiments that are expensive to repeat.

## The task

Port this decompilation of Halo: Combat Evolved to the Nintendo Switch, as a
devkitPro homebrew program. The game is a guest image — the decompilation
compiled as ILP32 AArch64 (32-bit pointers, because the Xbox data formats
embed them) — running inside a host that is the Android host library with
some files replaced.

## Where it is

The port **runs the game further than it has ever run** and **still exits
during startup, every time, without a word about why**. Both halves of that
sentence are the state of the thing.

Verified from the console's own log:

- Guest image loaded at `0x40000000`, imports resolved, nothing missing
- The game completes its entire startup: settings load and are **saved to
  `config.toml` (6325 bytes written and read back — the first guest file
  with real content this port has ever produced)**, the frame is drawn
  (852×480), SDL 2.28.5 and OpenGL ES 3.2 on Mesa/NV120 come up, eight
  controllers enumerate, and then…
- **The game calls `exit(0)` about 0.3 s after the last controller appears**,
  with no error, no map ever opened, and no `debug.txt` written. `main()`
  does not return (the runtime logs "the game's main() returned" and the line
  is never seen), so the exit comes from inside the game. Every exit path
  found by reading the source either logs first (and nothing logged) or is
  unreachable. The exit's call site is now printed by the guest and resolves
  in the guest image — **symbolize it with `tools/symbolize.py guest <addr>`
  on the next run; that is where the port stands.**
- **No console lockup in any run since the SD serialization went in.** The
  hard freezes are gone. That was the biggest thing wrong before.

Against that: runs reach 6–7 s and quit. The game has never once opened a
map file, drawn more than the startup frame, or written `debug.txt`.

## The two things that are wrong

**1. The game exits during startup, silently.** Everything initializes;
then `exit(0)`. No error is recorded, no map is opened, `main()` never
returns. The call site of `exit` is logged now and symbolizable; the guest
error channel (`debug.txt`, which needs `d:\debug.txt` translated by
`platform_translate_path`) has never received a byte, so the game's own
diagnosis of whatever is going wrong is invisible.

**2. The Xbox window lands somewhere different every run.** `0x80000000`
is always refused (`0xdc01`), the pinned fallback `0x20000000` is refused
sometimes, and the guest-visible base has been `0x10000000`, `0x20000000`,
`0x30000000`, `0x50000000`. The guest takes whatever it is told (it
reserves exactly the address it is given), so this is not a correctness
problem yet, but every run is a different run.

## Where the crash reports are

```
sdmc:/atmosphere/crash_reports/      <- ours: *_05446530aca7e000.log
sdmc:/atmosphere/fatal_reports/      <- qlaunch complaining its child died; not ours
```

`crash_reports` has a full per-thread dump. Addresses in it are module-relative
in the report itself (`halo + 0x7ff8`), so `tools/symbolize.py host -l 0x7ff8`
names them directly; an absolute address needs the load base the report printed.
Pull with `python3 tools/switch_logs.py --atmosphere` or by hand.

## The test loop

Build, push, launch from Sphaira, read the log.

```
cd /home/carlo/projects/halo-ce-switch
export DEVKITPRO=/opt/devkitpro DEVKITA64=$DEVKITPRO/devkitA64
export ANDROID_NDK_HOME=$HOME/projects/Wiicompiled/.android-sdk/ndk/26.1.10909125
export PATH=$DEVKITPRO/tools/bin:$PATH
ninja switch                 # ~2 min (host and guest)
python3 tools/switch_deploy.py
python3 tools/switch_logs.py --keep
```

**All three environment variables are needed.** Without `ANDROID_NDK_HOME`,
`build.ninja` regenerates without switch targets and `ninja switch` fails with
`unknown target 'switch'`. Recover with `python3 configure.py`.

Launch from **Sphaira**. **Launching kills the console's FTP server** (it is a
homebrew NRO) — restart it after each run, and pull the log between runs.

## What was wrong, and is now fixed

Ordered by how much damage each one did. The first two ended runs; the rest
were silent corruption waiting to happen.

1. **The card driver does not survive concurrent access; the whole console
   froze.** Every log line was written and `fsync`ed while a guest file call
   could be inside the same driver, and during the startup allocator churn
   that is hundreds of writes a second. Runs ended with the console itself
   locked up, `halo.log` cut off mid-line, and — the proof — a heartbeat
   file written with no lock and no `fsync` ending in **binary garbage six
   seconds before the main log stopped**. Every entry into the SD driver now
   takes one lock (`host_sd_lock`, host_main.c), shared by the logger, all
   guest file calls, the file-backed `mmap` read, and the heartbeat. No
   lockup since.
2. **`config.toml` was saved by a loop that could never finish: 12,000
   `writev` calls a second, forever, file 0 bytes.** musl's `__stdio_write`
   sends *two* iovecs — its own FILE buffer first, which is empty on a fresh
   flush, then the caller's data — and the shim treated a zero-length first
   vector as "wrote zero bytes, stop", returning 0 for a call carrying
   kilobytes. musl retries a short write forever and only aborts on a
   negative result. A zero-length vector is now skipped (host_syscall.c).
   This also explains why the file had been written *once* historically: it
   works whenever musl's first vector happens to be non-empty.
3. **`readv`/`writev` turned errors into zero.** A `-1` from `write()` broke
   the loop with `total == 0`, so a failing write looked like a short one
   and was retried silently. Errors now propagate.
4. **`preadv` called `writev`.** Every positional read overwrote the file
   with the guest's own (uninitialized) buffer.
5. **`fcntl` was answered with the wrong arguments** (it switched on the
   value instead of the command) and passed a union by value into a variadic
   call; it also handed the guest host descriptors, defeating the guest
   descriptor table. Rewritten: command and argument read from the right
   registers, descriptors translated, `F_DUPFD` results filed as guest
   descriptors.
6. **`dup` shared one host descriptor between two guest slots**, so the
   first close left the second naming a descriptor the host could recycle —
   possibly onto the log. `dup` now duplicates the real descriptor.
7. **`O_*` flags never matched.** The guest is musl and speaks Linux values;
   newlib's differ for everything past the access mode, and some collide: the
   guest's `O_APPEND` (`0x400`) *is* newlib's `O_TRUNC`, so an open-for-append
   truncated the file. Flags are now translated bit by bit in `openat` and
   `fcntl(F_SETFL)`.
8. **`fstat` returned newlib's `struct stat` where musl wants the kernel's
   `struct kstat`.** musl converts in libc, so every offset was wrong — in
   particular `st_atim.tv_sec` sat where `st_size` belongs, so the guest read
   file modification times as file sizes (~2.6 billion). `kstat_from_stat()`
   now produces the layout musl expects.
9. **The guest's file buffers are memory the card driver cannot see.**
   Writes sourced from the port's own `svcMapMemory` mappings come back 0
   with no error (host-side writes from the heap are fine, and a write from
   pool memory was verified working in the startup self-test, so this one was
   a red herring in the end — kept as a bounce anyway, it costs a copy).
10. **`gettid` asked the console with an invalid pseudo-handle** (`0xffffffff`
    instead of `0xffff8000`), so every thread reported tid 0; `SYS_gettid`
    was answered by newlib's unimplemented `getpid`, so musl's TLS setup saw
    a *failed* syscall (`call (178) failed: errno 88`). Both now use
    `svcGetThreadId`, `getpid`/`getppid` answer 1, `set_tid_address` returns
    the real id.
11. **The futex waiter count was decremented twice** (by the waiter and by
    `futex_wake`), so wake counts drifted negative. `futex_wake` no longer
    adjusts it.
12. **`guest mmap` passed the guest's fd number straight to the host's
    file-reading mmap**, bypassing the descriptor table — a file-backed
    mapping would have read whatever the host's fd pointed at. Translated now.
13. **A 16 KB scratch buffer on `mmap`'s stack** killed a thread with a 4 KB
    stack just entering the function (Data Abort at `0x0fffffd0`; the crash
    report's `Stack Region` is 4 KB and `PC` is `mmap`). It is `static` now,
    used only under the SD lock. The rule this port learned: **never put a
    buffer bigger than a page on the stack in the host** — some threads here
    have kilobytes of stack, not megabytes.
14. **`dup3` bypassed the guest fd table**, `clock_nanosleep` returned the
    wrong-signed errno, `SYS_openat` reported a stale errno for the shim's
    own refusals, `host_image_find_symbol`'s undefined-symbol fallback was
    dead code, `thread_main` dereferenced an unchecked `calloc`, and the
    `mmap` log line printed `flags` where it said `prot`.

Two of these deserve a note about the reasoning, because both were believed
with more confidence than they deserved:

- **`to_guest()` "fix" that broke the window.** Translating fixed-mapping
  results into the guest's window range looked obviously correct. It is not:
  the guest is told the window's *real* base in its boot structure and works
  in real addresses; it asked to map at `0x30000000`, was handed
  `0x80000000`, treated the mismatch as failure, unmapped the window it had
  just reserved, and printed "cannot reserve the Xbox contiguous memory
  window". The translation was reverted (host_memory.c). Read
  `port/linux/src/xbox_files.c`'s caller checks (`result == wanted`) before
  assuming an address transformation is wanted.
- **The "driver cannot see pool memory" theory** cost a run: a direct write
  from pool memory to a card file works. The bounce buffer stayed because it
  is harmless, not because it fixed anything.

## Still wrong

- **The silent exit.** See above. The call site is symbolizable now.
- **`debug.txt` is still empty.** The game's own error log would name
  whatever startup is unhappy about, and it has never received a byte. The
  writes go through `fopen("d:\\debug.txt")` → `halo_linux_fopen` →
  `platform_translate_path` → `fopen` → `openat`; **no openat for it appears
  in any log**, which means the game's `error()` has never run — the exit is
  not an error path the game knows about.
- **`getdents64` is unanswerable** (no libc call, no service), so the guest
  cannot list directories. Not hit yet, but the attract-mode film code
  (`source/interface/attract_mode.c`) checks films with `file_exists` on
  constructed paths, and there is no `bink/` folder on the card.
- **Window placement is non-deterministic** across runs (see above).
- **The teardown crash.** A run that ends (`exit(0)`) sometimes takes the
  console with it: Instruction Abort on a host thread during `_exit` at an
  address in neither image (`0x16bc22000`, X7 = the same value). It is
  post-mortem noise, but it is what the player sees last.

## How to read a run's end

- `the game exited (N)` — the guest called exit; the next line up names the
  call site. Symbolize it.
- `the game's main() returned N` — `main()` returned (shell init refused or
  the main loop ended). Its absence is what rules those out.
- nothing after a heartbeat — the log stops; check whether the heartbeat file
  kept time (logger wedged) or stopped too (process/console gone).

## Instruments

- `port/switch/DEBUGGING.md` — gdb over Atmosphère's stub (port 22225).
- `tools/symbolize.py` — `host -l 0x1234` for host file offsets (the crash
  reports print them as `halo + offset`; an address in a log is the host's
  file address already), `guest 0x402a26cc` for the guest image.
- `tools/switch_logs.py` — pulls runs off the card, one summary line each
  (length, syscalls, window, drew, how it ended) and deletes them; `--keep`
  leaves them, `--no-fetch` re-summarises. Heartbeat and the pulled logs land
  in `/tmp/halo-logs/`.
- The guest watcher heartbeats every second into both `halo.log` and
  `heartbeat.log`: a second file holding nothing but the syscall count, so a
  run that stops can be read for whether the logger wedged or the console
  went. It takes the SD lock like everything else — built without it, it
  corrupted within three seconds, which is itself the evidence that the card
  driver takes no concurrency. Per-allocation mmap chatter was deleted from
  the log: at several hundred `fsync`ed lines a minute it was itself load on
  the card.

## Where things are

- Worktree `/home/carlo/projects/halo-ce-switch`, branch `switch`
- `port/switch/host/` — `host_mman.c` (mmap over the console's memory
  services), `host_syscall.c` (the guest's libc — this session's main work),
  `host_debug.c` (watcher, heartbeat, backtrace), `host_sdl2.c` +
  `host_sdl3_events.c`, `host_main.c`, `guest_stack.S`, `host_futex.c`
- `port/switch/include/` — shim headers newlib lacks
- `port/switch/probe/mem/` — hardware probes (standalone)
- `port/switch/README.md` — design, memory model, install
- `port/switch/DEBUGGING.md` — gdb
- `reference/` — the Anbernic RG35XX port, reference only

## If you want to change direction

The host's memory layer (`port/switch/host/host_mman.c`,
`host_memory.c`) was the hardest to win and is self-contained.

The most promising structural change is the one not yet taken: **route the
game's file APIs through the import table instead of through musl.** The
guest already calls 193 host functions directly; the Xbox file layer
(`port/linux/src/xbox_files.c`, compiled into the guest) currently reaches
the card through musl → `SYS_openat`/`SYS_writev` → the shim → newlib →
libnx. Every bug in this session lived at that seam — flag mismatches,
stat layouts, empty iovecs, buffer visibility. Moving `CreateFileA`/`ReadFile`/
`WriteFile` (and `fopen`) to direct host imports removes musl from the data
path entirely, at the cost of a handful of new import functions. musl keeps
doing what it is good at: malloc, threads, TLS.

Internet play is stubbed in `host_net_stub.c` and needs a socket layer (lwIP
over libnx), deliberately left out so the first working build would not
depend on it.
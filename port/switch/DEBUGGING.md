# Debugging a running port with gdb over the LAN

The port runs as an NRO under Atmosphère, and Atmosphère ships a standalone GDB
stub that listens on the network. Nothing in the port has to be linked or
changed to use it; it is firmware-side. This file is the recipe, and the
reasoning behind the two decisions in it.

## 1. Turn the stub on

On the card, `sd:/atmosphere/config/system_settings.ini`:

```ini
[atmosphere]
enable_htc = u8!0x0
enable_standalone_gdbstub = u8!0x1
```

`enable_standalone_gdbstub` only matters when `enable_htc` is 0, which is the
default and should stay that way: with htc on, the stub is ignored and the
gdb server is expected to be driven over usb instead.

Reboot. The console must be on the same network as the PC — the port already
talks to 192.168.2.173 over FTP for deployment, so that address is the console.

## 2. Connect

```
gdb-multiarch
(gdb) target extended-remote 192.168.2.173:22225
(gdb) info os processes
```

Port 22225 is Atmosphère's, chosen to not collide with anything else. Note that
connecting is not attaching: `info os processes` lists the running processes
and the title or program id next to each.

## 3. Catch the port at startup

Attaching to something already spinning is rarely what you want, because the
interesting part happens before the first frame. Make gdb wait instead, and
then start the NRO from hbmenu (or Sphaira, or whatever launcher):

```
(gdb) monitor wait homebrew
Attach to 0x2a0
(gdb) attach 0x2a0
```

`monitor wait homebrew` blocks until homebrew starts and reports its pid, then
`attach` to it. `monitor wait application` is the same thing for titles, and
`monitor wait 0x<program id>` for one specific title.

## 4. Symbols

The host is compiled `-O2 -g`, so `build/switch/halo.elf` is the file to load,
and it is 15 MB because it still has its debug info (the 6 MB `.nro` is the
stripped copy the console actually runs; do not load that one).

```
(gdb) symbol-file build/switch/halo.elf
(gdb) monitor get info
(gdb) monitor get mappings
```

`monitor get info` prints the address space layout and the loaded modules with
their base addresses, which is what tells you whether the guest loaded and at
where.

The guest is a separate ELF that the host loads from the card and jumps into.
It is built without `-g` (see `tools/switch_build.py` for why: the ILP32 line
table does not survive the link and is worse than none), but its symbols are
there, so a guest stack trace is still readable by name:

```
(gdb) add-symbol-file build/switch/halo_guest.elf 0x40000000
(gdb) info registers pc
(gdb) x/8i $pc
(gdb) bt
```

## What is worth knowing before blaming the stub

- **Breakpoints work**: software, hardware, watchpoints and single-step are all
  implemented in the stub. `hbreak` is worth reaching for in code that maps
  memory, since a software breakpoint writes into the page it is in.
- **dmnt conflicts.** A `dmnt` client (EdiZon and anything else that talks to
  `dmnt`, and any sysmodule that does) collides with the standalone stub and
  ends in `std::abort()`. Move `/atmosphere/contents` out of the way first.
- **Sockets can hang.** The stub serves over the network stack, so a process
  that is stuck in socket code can wedge the stub along with it. The port stubs
  its socket layer out entirely (`host_net_stub.c` returns -1 for everything),
  which is convenient here.
- **Detach before closing.** Leaving gdb attached when the program exits leaves
  the stub waiting. `detach`, then quit.
- **The log is still the first tool.** `halo.log` on the card, pulled over FTP,
  answers most questions faster than a breakpoint does. Use gdb for the ones the
  log cannot: what is on the stack, what is in a register, where a thread
  actually is.
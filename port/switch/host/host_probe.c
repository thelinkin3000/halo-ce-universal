/*
HOST_PROBE.C

Not built for the Switch: see below. The Android port's version of this file
is a development aid, and it needs two things the console does not have.

It walks the guest's memory window looking for 32-bit words that are pointers
back into the window, so that the port can be written against what is really
there rather than against a guess about where the Xbox game data's pointers
live. Two things make that reading unsafe, and the Android version deals with
both:

- the guest maps and unmaps while the probe walks, so a range that was
readable may be gone by the time it is read. It installs a fault handler and
longjmps past the page, so the worst case is a page missing;
- the part of the window the guest has not mapped is reserved, and reading
it faults. The probe takes its ranges from /proc/self/maps and reads only
what is mapped.

Neither substitute exists here. libnx exposes no syscall for installing a
signal handler, so there is nothing to longjmp from, and there is no
/proc/self/maps - the ranges the window's pages occupy would have to come
from the loader, which knows which pages it handed out but does not keep a
list the probe could ask for.

So the probe is absent rather than crippled. Nothing the game needs depends
on it: it is a tool for working out where the port's relocation has to go,
and by the time this port runs that question is already answered. The
memories the port does keep are the window's base address
(host_memory_window_base) and a window of its own, which is all the loader
and the guest's boot structure need.

Bringing it back would mean walking the window page by page with the guest
paused, rather than racing it: a fault while reading a page the loader
believes it mapped would be a bug worth a crash rather than something to
catch. That is the shape it should take if it is wanted.
*/
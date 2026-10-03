/*
SYS_MMAN.H

The POSIX memory calls, for the Switch port.

The Android host library (port/android/host) is written against mmap,
munmap, mprotect and the MAP_/PROT_ constants. The Switch is not Linux and
devkitPro's newlib has no <sys/mman.h> at all, so this declares what the
shared sources use and host_mman.c provides it over the console's own memory
services.

What the layer underneath actually does is nothing like Linux's, and two
differences matter to anything that calls this:

- No mapping is ever both writable and executable. svcSetMemoryPermission
  refuses Perm_X (it answers 0xd801), and its own documentation says so.
  PROT_EXEC is therefore a separate kind of mapping, made with
  svcCreateCodeMemory (0x4B) and svcControlCodeMemory (0x4C), and an
  ordinary page cannot be promoted into one.
- Because of that, a mapping that is to become executable is created as a
  code memory object with its writable view already at the address asked
  for, so its contents can be written normally; mprotect is what turns it
  into its read-execute view at the same address. Turning it back undoes
  that, and the contents survive both ways, because the two views are one
  memory.

Both behaviours were confirmed on hardware: see port/switch/probe.
*/

#ifndef __HALO_SWITCH_SYS_MMAN_H
#define __HALO_SWITCH_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

/* protection */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* flags */
#define MAP_SHARED    0x0001
#define MAP_PRIVATE   0x0002
#define MAP_FIXED     0x0010
#define MAP_ANONYMOUS 0x0020
#define MAP_NORESERVE 0x0040
/* not in POSIX; Linux added it, and host_memory.c uses it to map a range
only if it is free (port/android/host/host_memory.c) */
#define MAP_FIXED_NOREPLACE 0x100000

/* advice, accepted and ignored: the console has nothing to tune */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       5

#define MAP_FAILED ((void *)-1)

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int madvise(void *address, size_t length, int advice);

#endif
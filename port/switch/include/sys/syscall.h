/*
SYS_SYSCALL.H

The syscall numbers, for the Switch port.

host_syscall.c dispatches the guest's Linux syscalls by number. Almost all of
them it reaches through libc - write, read, open and the rest are ordinary
calls in newlib - and the numbers are only spelled out for the two that libc
has no equivalent of, and which therefore need implementing here:

	SYS_futex   the guest's threads synchronise with futexes (host_futex.c)
	SYS_madvise  a no-op here (madvise(), in host_mman.c)

On Android the dispatcher called syscall() for these; there is no syscall() on
the Switch, so the two are handled directly and this header exists only to
give the switch statement its labels.
*/

#ifndef __HALO_SWITCH_SYS_SYSCALL_H
#define __HALO_SWITCH_SYS_SYSCALL_H

/* These are the guest's numbers, which are Linux's, because the guest is
Linux-shaped code built against a musl that expects them. They do not
correspond to anything on the console. */
#define SYS_madvise 28
#define SYS_futex 98

#endif

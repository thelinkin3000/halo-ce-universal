/*
SYS_UIO.H

Scattered input and output, for the Switch port.

The guest's file helpers are written against readv, writev and preadv. Newlib
provides the functions but not this header, so it is declared here with the
same shapes Linux uses.

The guest's iovec entries are not the same as the host's: the guest runs
ILP32 code, so its struct iovec holds 32-bit fields. host_syscall.c keeps
them apart on purpose - it builds a host vector from the guest's rather than
passing the guest's through - and the definitions below are the host's,
which is what the libc functions here expect.
*/

#ifndef __HALO_SWITCH_SYS_UIO_H
#define __HALO_SWITCH_SYS_UIO_H

#include <stddef.h>
#include <sys/types.h>

struct iovec
{
	void *iov_base;
	size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *vectors, int count);
ssize_t writev(int fd, const struct iovec *vectors, int count);
ssize_t preadv(int fd, const struct iovec *vectors, int count, off_t offset);

#endif

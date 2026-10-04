/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h
sends every call here).

Most calls pass straight through: the guest's pointers are valid host
addresses, and its integer arguments arrive properly extended to 64 bits.
The exceptions are

- structures whose ILP32 layout differs from the kernel's: timespec and
  timeval (the guest's time_t is 32-bit, as in the MSVC runtime), iovec;
- memory mappings, which must stay below 4 GB (host_memory.c);
- the standard output and error streams, which go to logcat;
- process exit, and calls that have no meaning for the guest (signal
  handlers, which the host owns).
*/

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>

/* The numbers the guest passes are musl's, and musl is compiled for the
Android port from port/android/guest/libc/arch/arm64_32/bits/syscall.h.in,
which the build turns into bits/syscall.h with __NR_ renamed to SYS_. This
file includes that generated header rather than writing the numbers out, so
the host's dispatch and the guest's libc cannot drift apart: they are the
same file, and any change to one is a change to both. */
#include <bits/syscall.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <unistd.h>

#include <switch.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include "posix.h"

long host_futex(uint32_t address, int operation, uint32_t value, uint64_t timeout_address,
	uint64_t address2, uint32_t value3);

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* The structure the guest's fstat wants is the kernel's kstat, not
 * newlib's: musl issues the call into a kstat buffer and converts in libc
 * (musl src/stat/fstatat.c), so the shim has to produce the kernel's
 * layout. Handing newlib's struct stat over instead put its st_atim.tv_sec
 * where kstat keeps st_size, the guest read the file's modification time
 * as its size - some 2.6 billion - and every positional write it computed
 * from that landed two and a half gigabytes past the end of a zero-byte
 * file, where the driver quietly writes nothing. */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad1;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

static void kstat_from_stat(struct guest_kstat *out, const struct stat *in)
{
	memset(out, 0, sizeof(*out));
	out->st_dev = in->st_dev;
	out->st_ino = in->st_ino;
	out->st_mode = in->st_mode;
	out->st_nlink = in->st_nlink;
	out->st_uid = in->st_uid;
	out->st_gid = in->st_gid;
	out->st_rdev = in->st_rdev;
	out->st_size = in->st_size;
	out->st_blksize = in->st_blksize;
	out->st_blocks = in->st_blocks;
	out->st_atime_sec = in->st_atim.tv_sec;
	out->st_atime_nsec = in->st_atim.tv_nsec;
	out->st_mtime_sec = in->st_mtim.tv_sec;
	out->st_mtime_nsec = in->st_mtim.tv_nsec;
	out->st_ctime_sec = in->st_ctim.tv_sec;
	out->st_ctime_nsec = in->st_ctim.tv_nsec;
}

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

/* The id of the thread this is running on is answered by gettid() below,
 * from the console rather than from newlib. The dispatcher once answered
 * SYS_gettid through newlib's getpid, which is not implemented on this
 * console: it returns -1 and leaves errno at whatever it happened to be,
 * and result_of() turns that into a *failed* system call - which is what
 * the guest saw:

 *   guest call (178) failed: errno 88
 *
 * gettid is not a call the guest can do without. musl asks for it while
 * setting up threads and TLS, and an error there leaves it working from a
 * thread structure it believes was never built. */
static long result_of(long value)
{
	return value == -1 ? -errno : value;
}

/* ---------- standard output and error */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	pthread_mutex_lock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			__android_log_write(fd == 2 ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "halo", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	pthread_mutex_unlock(&log_lock);
}

/* ---------- the few C library calls the dispatcher needs and newlib has not

The console's C library is not Linux's, and the dispatcher is written against
Linux's names. Four of the gaps are small enough to fill here, and filling
them here is better than changing the shared source, which every other port
reads. */



/* ---------- the guest's descriptors

	 * The guest does not have descriptors of its own. It runs in this
	 * process, so every open() it makes returns a real one from this
	 * process's table, and it inherits ours as its own 0, 1, 2, 3 - the
	 * first three because those really are its standard streams, and the
	 * rest because nothing stopped it being handed them.
	 *
	 * That is not a tidiness problem. The host's log file is a descriptor
	 * in that table. A guest that cannot tell its descriptors from the
	 * host's can write its own data into the log - which is exactly what
	 * happened, leaving halo.log ending in binary - and can close a
	 * descriptor the runtime is relying on, which is a plausible way to
	 * take the console down hard enough that it will not reach its home
	 * screen.
	 *
	 * So the guest is given descriptors of its own: small integers that
	 * index this table, and mean nothing to the host. An open() is made
	 * here as before and the result is filed in a slot the guest is given
	 * instead; every other call is translated on the way in and forgotten
	 * on the way out. The guest can then only reach files it opened
	 * itself, because there is no number that reaches anything else.
	 *
	 * Standard streams are the exception and are shared deliberately: the
	 * guest's console output is meant to go to the same place the host's
	 * does, and those three are the only descriptors it may name without
	 * having opened them. */

#define GUEST_MAXIMUM_FDS 256

static int guest_fds[GUEST_MAXIMUM_FDS];
static pthread_mutex_t guest_fds_lock = PTHREAD_MUTEX_INITIALIZER;
static int guest_fds_prepared;

/* 0, 1 and 2 are the same descriptors on both sides and are never filed in
 * the table; everything else has to be in the table to be usable. */
static void guest_fds_prepare(void)
{
	int index;

	if (guest_fds_prepared)
		return;
	for (index = 0; index < GUEST_MAXIMUM_FDS; index++)
		guest_fds[index] = -1;
	guest_fds_prepared = 1;
}

static int host_descriptor(int guest_fd);

/* The public spelling of host_descriptor, for host_memory.c's mmap. */
int host_guest_fd(int guest_fd)
{
	return host_descriptor(guest_fd);
}

/* The host descriptor behind a guest one, or -1 if the guest never opened
 * it. Standard streams pass through unchanged. */
static int host_descriptor(int guest_fd)
{
	int result;

	if (guest_fd >= 0 && guest_fd <= 2)
		return guest_fd;
	pthread_mutex_lock(&guest_fds_lock);
	guest_fds_prepare();
	result = (guest_fd >= 0 && guest_fd < GUEST_MAXIMUM_FDS) ? guest_fds[guest_fd] : -1;
	pthread_mutex_unlock(&guest_fds_lock);
	return result;
}

/* A host descriptor to a guest one, the lowest free slot. -1 if the table is
 * full, which is reported to the guest as EMFILE - its own limit, and the
 * truth. */
static int guest_descriptor(int host_fd)
{
	int guest_fd = -1;
	int index;

	if (host_fd >= 0 && host_fd <= 2)
		return host_fd;
	pthread_mutex_lock(&guest_fds_lock);
	guest_fds_prepare();
	for (index = 3; index < GUEST_MAXIMUM_FDS; index++)
	{
		if (guest_fds[index] < 0)
		{
			guest_fds[index] = host_fd;
			guest_fd = index;
			break;
		}
	}
	pthread_mutex_unlock(&guest_fds_lock);
	return guest_fd;
}

/* A host descriptor filed at a guest number the caller chose, for dup3.
 * Whatever the guest had at that number is replaced. */
static int guest_descriptor_at(int guest_fd, int host_fd)
{
	if (guest_fd < 3 || guest_fd >= GUEST_MAXIMUM_FDS)
		return -1;
	pthread_mutex_lock(&guest_fds_lock);
	guest_fds_prepare();
	guest_fds[guest_fd] = host_fd;
	pthread_mutex_unlock(&guest_fds_lock);
	return guest_fd;
}

static void guest_descriptor_forget(int guest_fd)
{
	int host_fd;

	if (guest_fd < 3 || guest_fd >= GUEST_MAXIMUM_FDS)
		return;
	pthread_mutex_lock(&guest_fds_lock);
	guest_fds_prepare();
	host_fd = guest_fds[guest_fd];
	guest_fds[guest_fd] = -1;
	pthread_mutex_unlock(&guest_fds_lock);
	if (host_fd >= 0)
	{
		host_sd_lock();
		close(host_fd);
		host_sd_unlock();
	}
}

/* For log lines: "yes" if the guest could name it at all. */
static int guest_descriptor_is_open(int guest_fd)
{
	return host_descriptor(guest_fd) >= 0;
}

/* ---------- the libc the console's newlib does not have

	 * devkitA64's newlib has open, read, write, close, stat, unlink, mkdir,
	 * rename, access, dup and fcntl. It has none of the *at family, no
	 * pread or pwrite, and no pipe2, dup3, fdatasync, umask or flock -
	 * which is most of what musl reaches for when it opens a file.
	 *
	 * They are provided here in terms of what newlib does have. Until they
	 * existed, openat and read fell through to the syscall() above, which
	 * answers -ENOSYS for everything it does not recognise, so the guest
	 * could not open a file, read a file, or even close a descriptor it had
	 * been given. Writing worked, because write and writev have handlers of
	 * their own. That is why the guest could talk and still never manage to
	 * open anything - and why the game's own debug.txt was always empty. */

#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

/* The guest's open flags are Linux's, because it is musl; newlib's values
 * for everything past the access mode are different, and some collide:
 * the guest's O_APPEND (0x400) is newlib's O_TRUNC, so an append opened
 * for appending truncated the file instead, and the guest's O_TRUNC was
 * newlib's O_CREAT. Translated bit by bit, because neither set is the
 * other's superset. */
static int open_flags_from_guest(int flags)
{
	int result = flags & 3; /* O_RDONLY/O_WRONLY/O_RDWR agree */

	if (flags & 0x40)   result |= O_CREAT;    /* guest O_CREAT */
	if (flags & 0x80)   result |= O_EXCL;     /* guest O_EXCL */
	if (flags & 0x200)  result |= O_TRUNC;    /* guest O_TRUNC */
	if (flags & 0x400)  result |= O_APPEND;   /* guest O_APPEND */
	if (flags & 0x800)  result |= O_NONBLOCK; /* guest O_NONBLOCK */
	if (flags & 0x101000) result |= O_SYNC;   /* guest O_SYNC/O_DSYNC */
	if (flags & 0x80000) result |= O_CLOEXEC; /* guest O_CLOEXEC */
	return result;
}

int openat(int directory, const char *path, int flags, ...)
{
	mode_t mode = 0;

	flags = open_flags_from_guest(flags);
	if (flags & O_CREAT)
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = (mode_t)va_arg(arguments, int);
		va_end(arguments);
	}
	/* A path relative to a descriptor cannot be resolved without the
	 * *at calls this libc does not have. musl's own open() always passes
	 * AT_FDCWD, and a relative path would be a bug in the caller.
	 *
	 * The test is that the directory is a real descriptor - zero or more -
	 * and not a comparison against AT_FDCWD. newlib's <fcntl.h> does not
	 * define AT_FDCWD at all, so this file defines it, and something else
	 * in the include path defines it first with a different value: the
	 * first version of this refused every call, including the game's own
	 * sdmc:/switch/halo/config.toml, and said the directory was -100 while
	 * comparing against something that was not. A descriptor is never
	 * negative, which is true whatever the constant happens to be. */
	if (directory >= 0)
	{
		host_logf(HOST_LOG_WARN, "openat: relative path %s with directory %d is not supported", path, directory);
		return -EBADF;
	}
	{
		int result;

		host_sd_lock();
		result = open(path, flags, mode);
		host_sd_unlock();
		return result;
	}
}

static ssize_t bounced_write(int fd, const void *buffer, size_t size);
static ssize_t bounced_read(int fd, void *buffer, size_t size);

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	off_t here;
	ssize_t read_bytes;
	host_sd_lock();
	here = lseek(descriptor, 0, SEEK_CUR);
	if (here < 0)
		goto out;
	if (lseek(descriptor, offset, SEEK_SET) < 0)
	{
		here = -1;
		goto out;
	}
	read_bytes = bounced_read(descriptor, buffer, count);
	lseek(descriptor, here, SEEK_SET);
	here = read_bytes;
out:
	host_sd_unlock();
	return here;
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	off_t here;
	ssize_t written;

	host_sd_lock();
	here = lseek(descriptor, 0, SEEK_CUR);
	if (here < 0)
		goto out;
	if (lseek(descriptor, offset, SEEK_SET) < 0)
	{
		here = -1;
		goto out;
	}
	written = bounced_write(descriptor, buffer, count);
	lseek(descriptor, here, SEEK_SET);
	here = written;
out:
	host_sd_unlock();
	return here;
}

int fstatat(int directory, const char *path, struct stat *information, int flags)
{
	if (directory >= 0)
		return -EBADF;
	/* follow_symlinks is the Linux spelling; newlib spells it the
	 * S_ISLNK-free way and has no lstat worth relying on here, so the
	 * flag is accepted and the path is followed. */
	(void)flags;
	{
		int result;

		host_sd_lock();
		result = stat(path, information);
		host_sd_unlock();
		return result;
	}
}

ssize_t readlinkat(int directory, const char *path, char *buffer, size_t count)
{
	if (directory >= 0)
		return -EBADF;
	{
		ssize_t result;

		host_sd_lock();
		result = (ssize_t)readlink(path, buffer, count);
		host_sd_unlock();
		return result;
	}
}

int unlinkat(int directory, const char *path, int flags)
{
	if (directory >= 0)
		return -EBADF;
	{
		int result;

		host_sd_lock();
		result = (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
		host_sd_unlock();
		return result;
	}
}

int mkdirat(int directory, const char *path, mode_t mode)
{
	if (directory >= 0)
		return -EBADF;
	{
		int result;

		host_sd_lock();
		result = mkdir(path, mode);
		host_sd_unlock();
		return result;
	}
}

int faccessat(int directory, const char *path, int mode, int flags)
{
	(void)flags;
	if (directory >= 0)
		return -EBADF;
	{
		int result;

		host_sd_lock();
		result = access(path, mode);
		host_sd_unlock();
		return result;
	}
}

int dup3(int from, int to, int flags)
{
	(void)flags;
	if (from == to)
		return -EINVAL;
	if (dup2(from, to) < 0)
		return -1;
	return to;
}

int pipe2(int fds[2], int flags)
{
	/* Not answerable. newlib has no pipe() either, and there is no service
	 * behind one: a pipe is a kernel object this console's libc does not
	 * expose. Refused plainly, so musl knows to take its fallback rather
	 * than waiting on two descriptors that will never exist. */
	(void)fds;
	(void)flags;
	host_logf(HOST_LOG_WARN, "the guest asked for a pipe, which this console's libc cannot make");
	return -ENOSYS;
}

int fdatasync(int descriptor)
{
	int result;

	host_sd_lock();
	result = fsync(descriptor);
	host_sd_unlock();
	return result;
}

mode_t umask(mode_t mask)
{
	/* newlib has no umask. The guest asks for one and expects the old
	 * value back; reporting the usual default and changing nothing keeps
	 * it working, and this port creates every file with an explicit mode. */
	static mode_t current = 022;
	mode_t previous = current;

	current = mask;
	return previous;
}

int flock(int descriptor, int operation)
{
	/* Advisory locking has no meaning on this console's filesystem. */
	(void)descriptor;
	(void)operation;
	return 0;
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

long syscall(long number, ...);

/* newlib has nanosleep but not the clock-nanosleep spelling, which takes the
clock to sleep on; the console's threads all sleep on the monotonic clock, so
a request for another one is refused rather than quietly answered with the
wrong clock. */
/* The clocks the guest is answered from.

libnx's CLOCK_MONOTONIC counts from the same epoch as the wall clock - some
2.6 billion seconds - and the guest's time_t is 32 bits, so the value it was
handed wrapped to minus 1.69 billion. The emulated vertical blank
(d3d8_gl.c) builds its deadlines from that, this compared them with the real
value, every deadline was 136 years past, and the thread called
clock_nanosleep three and a half million times a second for as long as the
game ran. The monotonic clock is therefore the console's system tick, which
counts from boot: monotonic as the name promises, and small enough for 32-bit
seconds for 68 years. */
static int host_clock_read(clockid_t clock, struct timespec *value)
{
	if (clock == CLOCK_MONOTONIC)
	{
		u64 nanoseconds = armTicksToNs(armGetSystemTick());

		value->tv_sec = (time_t)(nanoseconds / 1000000000ull);
		value->tv_nsec = (long)(nanoseconds % 1000000000ull);
		return 0;
	}
	return clock_gettime(clock, value);
}

static int host_clock_resolution(clockid_t clock, struct timespec *value)
{
	if (clock == CLOCK_MONOTONIC)
	{
		/* the system tick runs at 19.2 MHz */
		value->tv_sec = 0;
		value->tv_nsec = 53;
		return 0;
	}
	return clock_getres(clock, value);
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remaining)
{
	/* clock_nanosleep returns the errno number itself, not -1: the
	 * dispatcher negates it on the way back to the guest. */
	struct timespec relative = *request;

	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;
	/* an absolute request is a deadline on that clock, not a length */
	if (flags & TIMER_ABSTIME)
	{
		struct timespec now;

		if (host_clock_read(clock, &now) != 0)
			return errno;
		relative.tv_sec = request->tv_sec - now.tv_sec;
		relative.tv_nsec = request->tv_nsec - now.tv_nsec;
		if (relative.tv_nsec < 0)
		{
			relative.tv_nsec += 1000000000L;
			relative.tv_sec--;
		}
		if (relative.tv_sec < 0)
			return 0;
		remaining = NULL;
	}
	if (nanosleep(&relative, remaining) < 0)
		return errno;
	return 0;
}

/* poll, with a timeout the caller expressed as a timespec rather than as
milliseconds. This is what musl's poll reaches for, because the timeout it
wants has microsecond resolution and poll's does not. */
int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
	int milliseconds = -1;

	if (timeout)
	{
		long long value = (long long)timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;

		milliseconds = value > 0x7fffffff ? 0x7fffffff : (int)value;
	}
	(void)mask;
	return poll(fds, count, milliseconds);
}

/* gettid is not POSIX and so is not in newlib; the console's equivalent is
one syscall, and the pseudo-handle 0xffff8000 means "this thread"
(0xffffffff is not a handle the kernel recognises, so the call failed and
every thread reported tid 0). */
pid_t gettid(void)
{
	u64 id = 0;

	/* a made-up positive id beats a failed call: musl is asking while it
	 * builds its TLS, and an error there is not one it can carry on from */
	if (svcGetThreadId(&id, 0xffff8000) != 0)
		return 1;
	return (pid_t)id;
}

/* preadv and pwritev: newlib has readv and writev but not the positional
forms, which the guest's file helpers use to read a map file's contents
without disturbing whatever else is reading it. */
static ssize_t readv_unlocked(int fd, const struct iovec *vectors, int count);
static ssize_t writev_unlocked(int fd, const struct iovec *vectors, int count);

ssize_t preadv(int fd, const struct iovec *vectors, int count, off_t offset)
{
	off_t here;
	ssize_t got;

	host_sd_lock();
	here = lseek(fd, 0, SEEK_CUR);
	if (here < 0)
		goto out;
	if (lseek(fd, offset, SEEK_SET) < 0)
	{
		here = -1;
		goto out;
	}
	got = readv_unlocked(fd, vectors, count);
	lseek(fd, here, SEEK_SET);
	here = got;
out:
	host_sd_unlock();
	return here;
}

ssize_t pwritev(int fd, const struct iovec *vectors, int count, off_t offset)
{
	off_t here;
	ssize_t put;

	host_sd_lock();
	here = lseek(fd, 0, SEEK_CUR);
	if (here < 0)
		goto out;
	if (lseek(fd, offset, SEEK_SET) < 0)
	{
		here = -1;
		goto out;
	}
	put = writev_unlocked(fd, vectors, count);
	lseek(fd, here, SEEK_SET);
	here = put;
out:
	host_sd_unlock();
	return here;
}

/* newlib has neither the scattered nor the positional vector calls, and the
guest's file helpers use both. These are the obvious loops over read and
write; a vector is only ever a convenience for several buffers, and the
descriptors are the host's, so there is nothing subtle in them. */
/* The card driver cannot see the guest's buffers.
 *
 * The guest's memory is the port's own svcMapMemory mappings below 4 GB,
 * not the process's ordinary heap, and a write() whose source lives there
 * comes back 0 with errno 0 - the driver quietly copies nothing. So every
 * file read and write the guest makes goes through a bounce buffer on the
 * host's stack: an extra copy per call, which is nothing next to a card
 * access, and the only kind of memory the driver is known to handle. */
/* Guest memory goes through a bounce buffer on its way to and from the card
(see the driver note above). Every caller holds the SD lock, so one static
buffer serves them all: on the stack it was 4 KB, which some threads cannot
spare, and a read returned after that one 4 KB chunk - legal, but the cache
copy thread then asked again for every 4 KB of a 14 MB map, three seeks and
the card lock each time, and the heartbeat waited four seconds for it. */
#define BOUNCE_SIZE (128 * 1024)

static char bounce_buffer[BOUNCE_SIZE] __attribute__((aligned(4096)));

static ssize_t bounced_write(int fd, const void *buffer, size_t size)
{
	size_t done = 0;

	while (done < size)
	{
		size_t chunk = size - done;
		ssize_t put;

		if (chunk > BOUNCE_SIZE)
			chunk = BOUNCE_SIZE;
		memcpy(bounce_buffer, (const char *)buffer + done, chunk);
		put = write(fd, bounce_buffer, chunk);
		if (put <= 0)
			return done ? (ssize_t)done : put;
		done += (size_t)put;
		if ((size_t)put < chunk)
			break;
	}
	return (ssize_t)done;
}

static ssize_t bounced_read(int fd, void *buffer, size_t size)
{
	size_t done = 0;

	while (done < size)
	{
		size_t chunk = size - done;
		ssize_t got;

		if (chunk > BOUNCE_SIZE)
			chunk = BOUNCE_SIZE;
		got = read(fd, bounce_buffer, chunk);
		if (got <= 0)
			return done ? (ssize_t)done : got;
		memcpy((char *)buffer + done, bounce_buffer, (size_t)got);
		done += (size_t)got;
		/* the end of the file, or a device that gives what it has */
		if ((size_t)got < chunk)
			break;
	}
	return (ssize_t)done;
}

/* the locks are not taken here so that preadv and pwritev can hold one
 * across a seek and a vector call; the public forms below take it */
static ssize_t readv_unlocked(int fd, const struct iovec *vectors, int count)
{
	ssize_t total = 0;
	int index;

	for (index = 0; index < count; index++)
	{
		ssize_t got;

		/* an empty vector is nothing, not the end: musl's stdio leads
		 * with its own buffer, which is empty on a fresh flush, and
		 * stopping there returned 0 for a call that carried kilobytes -
		 * musl retried it twelve thousand times a second, forever */
		if (vectors[index].iov_len == 0)
			continue;
		got = bounced_read(fd, vectors[index].iov_base, vectors[index].iov_len);

		/* an error is an error, not a short read: returning 0 for it
		 * sent musl into a twelve-thousand-calls-a-second retry loop
		 * over a write that was failing every time */
		if (got < 0)
		{
			if (total == 0)
				return -1;
			break;
		}
		if (got == 0)
			break;
		total += got;
		if ((size_t)got < vectors[index].iov_len)
			break;
	}
	return total;
}

static ssize_t writev_unlocked(int fd, const struct iovec *vectors, int count)
{
	ssize_t total = 0;
	int index;

	for (index = 0; index < count; index++)
	{
		ssize_t put;

		if (vectors[index].iov_len == 0)
			continue;
		put = bounced_write(fd, vectors[index].iov_base, vectors[index].iov_len);

		if (put < 0)
		{
			if (total == 0)
				return -1;
			break;
		}
		if (put == 0)
			break;
		total += put;
		if ((size_t)put < vectors[index].iov_len)
			break;
	}
	return total;
}

ssize_t readv(int fd, const struct iovec *vectors, int count)
{
	ssize_t result;

	host_sd_lock();
	result = readv_unlocked(fd, vectors, count);
	host_sd_unlock();
	return result;
}

ssize_t writev(int fd, const struct iovec *vectors, int count)
{
	ssize_t result;

	host_sd_lock();
	result = writev_unlocked(fd, vectors, count);
	host_sd_unlock();
	return result;
}

/* The guest names clocks by Linux's numbers and newlib numbers them its own
way: the guest's CLOCK_REALTIME (0) is newlib's CLOCK_REALTIME_COARSE, which
the console refuses, and its CLOCK_MONOTONIC (1) is newlib's CLOCK_REALTIME,
the wall clock - so the game timed its frames by the wall clock. The coarse
and raw variants are answered by the precise clock of the same kind. */
static long guest_clock(long long linux_clock)
{
	switch (linux_clock)
	{
	case 0: /* CLOCK_REALTIME */
	case 5: /* CLOCK_REALTIME_COARSE */
		return CLOCK_REALTIME;
	case 1: /* CLOCK_MONOTONIC */
	case 4: /* CLOCK_MONOTONIC_RAW */
	case 6: /* CLOCK_MONOTONIC_COARSE */
	case 7: /* CLOCK_BOOTTIME */
	/* the console cannot measure CPU time; elapsed time stands in, so
	 * musl's clock() moves rather than failing */
	case 2: /* CLOCK_PROCESS_CPUTIME_ID */
	case 3: /* CLOCK_THREAD_CPUTIME_ID */
		return CLOCK_MONOTONIC;
	default:
		return -1;
	}
}

/* A file position or length past anything the game's files reach (its
largest, a cache file, is 47 MB) is logged with the guest descriptor it was
asked of: one run left two host files grown to 0x11600000, a guest heap
address, and this says whether the guest asked for it. */
#define LARGE_FILE_OFFSET (64LL * 1024 * 1024)

static void note_large_offset(const char *call, long long guest_fd, long long offset)
{
	if (offset >= LARGE_FILE_OFFSET)
		host_logf(HOST_LOG_WARN, "guest %s on descriptor %lld at %#llx", call, guest_fd, offset);
}

/* ---------- the platform layer's descriptor helpers

The guest's posix_* helpers (port/linux/src/posix.h) are not system calls:
they are host imports, hostposix_<name>, that run posix_files.c here. That
was designed for Android, where the guest's descriptors are the host's. Here
they are not - the guest has its own table (above) - and the three helpers
that take a descriptor were handed the guest's number and used it as the
host's. GetFileSize on ui.map, guest descriptor 12, measured host descriptor
12, an empty savegame.bin, and the game halted on a map smaller than its
header; the cache files' seeks, guest descriptors 4 and 5, moved the
heartbeat and debug.txt to 0x11600000. The import table sends those three
here instead (tools/android_imports.py), to be translated. */

int host_guest_posix_fstat(int descriptor, struct posix_file_information *information)
{
	int host_fd = host_descriptor(descriptor);
	int result;

	if (host_fd < 0)
	{
		errno = EBADF;
		return -1;
	}
	host_sd_lock();
	result = posix_fstat(host_fd, information);
	host_sd_unlock();
	return result;
}

int host_guest_posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	int host_fd = host_descriptor(descriptor);
	int result;

	if (host_fd < 0)
	{
		errno = EBADF;
		return -1;
	}
	host_sd_lock();
	result = posix_seek(host_fd, offset_low, offset_high, whence, position_low, position_high);
	host_sd_unlock();
	return result;
}

int host_guest_posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	int host_fd = host_descriptor(descriptor);
	int result;

	if (host_fd < 0)
	{
		errno = EBADF;
		return -1;
	}
	host_sd_lock();
	result = posix_truncate(host_fd, size_low, size_high);
	host_sd_unlock();
	return result;
}

/* utimensat sets two timestamps and has no newlib equivalent. The console
has no way to ask for a file's timestamps from a program either, so this
answers that it could not, which is what the guest checks for. */
int utimensat(int descriptor, const char *path, const struct timespec times[2], int flags)
{
	(void)descriptor;
	(void)path;
	(void)times;
	(void)flags;
	return -ENOSYS;
}

/* Two cases in the dispatcher below want to make a call without knowing what
it is, so they go through this rather than naming the libc function. There is
no syscall() on the Switch - the console's kernel is reached by libnx, not by
an instruction - so this is a switch over the few numbers those cases pass,
and an error for the rest. It is variadic because the call sites pass three
arguments or four, and the unused ones are simply not there. */
long syscall(long number, ...)
{
	va_list arguments;
	long first, second, third;

	va_start(arguments, number);
	first = va_arg(arguments, long);
	second = va_arg(arguments, long);
	third = va_arg(arguments, long);
	va_end(arguments);
	switch (number)
	{
	case SYS_clock_gettime:
		return host_clock_read((clockid_t)first, (struct timespec *)(uintptr_t)second);
	case SYS_clock_getres:
		return host_clock_resolution((clockid_t)first, (struct timespec *)(uintptr_t)second);
	case SYS_madvise:
		return madvise((void *)(uintptr_t)(uint32_t)first, (size_t)(uint32_t)second, (int)third);
	default:
		return -ENOSYS;
	}
}

static long guest_writev(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	size_t asked = 0;
	int index;

	if (count < 0 || count > 64)
		return -EINVAL;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
		asked += guest_vector[index].length;
	}
	if (fd == 1 || fd == 2)
	{
		long total = 0;

		for (index = 0; index < count; index++)
		{
			log_bytes(fd, host_vector[index].iov_base, host_vector[index].iov_len);
			total += (long)host_vector[index].iov_len;
		}
		return total;
	}
	{
		long result;

		if (positional)
			result = result_of(pwritev(fd, host_vector, count, offset));
		else
			result = result_of(writev(fd, host_vector, count));
		/* A zero written with no error is no answer at all. The first
		 * time it happens, say what the descriptor actually is right
		 * now, and whether a write the host makes on its own fares any
		 * better on the same thread at the same moment. */
		if (result == 0 && asked > 0)
		{
			static int investigated;
			struct stat information;
			int control;

			if (!investigated)
			{
				investigated = 1;
				if (fstat(fd, &information) == 0)
					host_logf(HOST_LOG_WARN,
						"  zero-write fd %d: mode 0%o, size %llu, inode %llu",
						fd, information.st_mode,
						(unsigned long long)information.st_size,
						(unsigned long long)information.st_ino);
				else
					host_logf(HOST_LOG_WARN, "  zero-write fd %d: fstat failed (errno %d)", fd, errno);
				control = open("sdmc:/switch/halo/wtest_control.tmp",
					O_WRONLY | O_CREAT | O_TRUNC, 0666);
				if (control >= 0)
				{
					char big[6209];
					ssize_t control_written = write(control, "ctrl", 4);

					memset(big, 'b', sizeof(big));
					host_logf(HOST_LOG_WARN, "  control write from the same thread -> %d (errno %d)",
						(int)control_written, errno);
					control_written = write(control, big, sizeof(big));
					host_logf(HOST_LOG_WARN, "  control write of 6209 bytes -> %d (errno %d)",
						(int)control_written, errno);
					close(control);
				}
				{
					ssize_t direct = write(fd, "ctrl", 4);

					host_logf(HOST_LOG_WARN, "  direct write to the guest's fd %d -> %d (errno %d)",
						fd, (int)direct, errno);
				}
			}
		}
		return result;
	}
}

static long guest_readv(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	int index;

	if (count < 0 || count > 64)
		return -EINVAL;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
	}
	if (positional)
		return result_of(preadv(fd, host_vector, count, offset));
	return result_of(readv(fd, host_vector, count));
}

/* ---------- time */

/* The futex case is answered by host_futex.c rather than by a syscall:
the console has no futex syscall to call, and what it needs - wait, wake,
and the bitset variants musl actually uses - is built there over libnx's
thread primitives. */
static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout,
	uint64_t address2, uint32_t value3)
{
	return host_futex((uint32_t)address, operation, value, timeout, address2, value3);
}

/* ---------- dispatch */

/* Two cases in the dispatcher below want to make a call without knowing
what it is, so they go through this rather than naming the libc function.
There is no syscall() on the Switch - the console's kernel is reached by
libnx, not by an instruction - so this is a switch over the few numbers
those cases actually pass, and an error for the rest. */

static long long host_syscall_inner(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case SYS_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
		{
			int host_fd = host_descriptor((int)a);
			ssize_t written;

			if (host_fd < 0)
				return result_of(-EBADF);
			host_sd_lock();
			written = bounced_write(host_fd, GUEST(const void *, b), (size_t)(uint32_t)c);
			host_sd_unlock();
			return result_of(written);
		}
	case SYS_writev:
	{
		int host_fd = host_descriptor((int)a);

		return host_fd < 0 ? result_of(-EBADF) :
			guest_writev(host_fd, (uint64_t)b, (int)c, 0, 0);
	}
	case SYS_pwritev:
	{
		int host_fd = host_descriptor((int)a);

		note_large_offset("pwritev", a, d);
		return host_fd < 0 ? result_of(-EBADF) :
			guest_writev(host_fd, (uint64_t)b, (int)c, d, 1);
	}
	case SYS_readv:
	{
		int host_fd = host_descriptor((int)a);

		return host_fd < 0 ? result_of(-EBADF) :
			guest_readv(host_fd, (uint64_t)b, (int)c, 0, 0);
	}
	case SYS_preadv:
	{
		int host_fd = host_descriptor((int)a);

		return host_fd < 0 ? result_of(-EBADF) :
			guest_readv(host_fd, (uint64_t)b, (int)c, d, 1);
	}

	case SYS_clock_gettime:
	case SYS_clock_getres:
	{
		struct timespec value;
		long result;

		if (guest_clock(a) < 0)
			return -EINVAL;
		result = syscall(number, guest_clock(a), &value);

		if (result == 0)
			timespec_out((uint64_t)b, &value);
		return result_of(result);
	}
	case SYS_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case SYS_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint64_t)a, &request))
			return -EFAULT;
		result = result_of(nanosleep(&request, &remaining));
		if (result == -EINTR)
			timespec_out((uint64_t)b, &remaining);
		return result;
	}
	case SYS_clock_nanosleep:
	{
		struct timespec request, remaining;
		int result;

		if (!timespec_in((uint64_t)c, &request))
			return -EFAULT;
		if (guest_clock(a) < 0)
			return -EINVAL;
		/* Linux's TIMER_ABSTIME is 1; newlib's is 4 */
		result = clock_nanosleep((clockid_t)guest_clock(a), (b & 1) ? TIMER_ABSTIME : 0,
			&request, &remaining);
		if (result == EINTR)
			timespec_out((uint64_t)d, &remaining);
		return -result;
	}
	case SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case SYS_ppoll:
	{
		struct timespec timeout;
		int has_timeout = timespec_in((uint64_t)c, &timeout);

		return result_of(ppoll(GUEST(struct pollfd *, a), (nfds_t)(uint32_t)b, has_timeout ? &timeout : NULL, NULL));
	}
	case SYS_utimensat:
	{
		struct timespec times[2];
		const struct guest_timespec *guest_times = GUEST(const struct guest_timespec *, c);

		if (guest_times)
		{
			timespec_in((uint64_t)c, &times[0]);
			timespec_in((uint64_t)c + sizeof(struct guest_timespec), &times[1]);
		}
		return result_of(utimensat((int)a, GUEST(const char *, b), guest_times ? times : NULL, (int)d));
	}

	case SYS_mmap:
		/* Not logged on success: the guest's allocator makes several of
		 * these a second for minutes at a time, and the per-line fsync
		 * storm that logging them produced is what locked the console up.
		 * Failures are still reported by the caller's failure path. */
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case SYS_madvise:
		if ((uint64_t)a + (uint64_t)b > 0x100000000ULL)
			return -EINVAL;
		return result_of(syscall(SYS_madvise, a, b, c));
	case SYS_mremap:
		/* musl falls back to mmap and copying, which is why every
		 * allocation this guest makes goes through mmap rather than brk.
		 * That is slower but should still work, so a refusal here is worth
		 * seeing: it is logged rather than returned quietly, because a
		 * guest allocator that cannot extend is what "cannot grow emulated
		 * TLS" turns out to be. */
		host_logf(HOST_LOG_INFO, "the guest asked to move %llu bytes to %llx",
			(unsigned long long)b, (unsigned long long)d);
		return -ENOMEM;
	case SYS_brk:
		host_logf(HOST_LOG_INFO, "the guest asked brk for %llx", (unsigned long long)a);
		return -ENOMEM;

	case SYS_exit:
	case SYS_exit_group:
		host_exit((int)a);

	case SYS_set_tid_address:
		/* musl hands over its thread pointer here and is told which
		 * thread it is; the console answers, newlib cannot */
		return gettid();
	case SYS_rt_sigaction:
	case SYS_sigaltstack:
		/* the host owns signal handling */
		return 0;
	case SYS_ioctl:
		return -ENOTTY;
	case SYS_statx:
	case SYS_statfs:
	case SYS_fstatfs:
	case SYS_clone:
	case SYS_clone3:
	case SYS_execve:
	case SYS_rt_sigtimedwait:
	case SYS_pselect6:
	case SYS_epoll_pwait:
	case SYS_sysinfo:
	case SYS_sendmsg:
	case SYS_recvmsg:
	case SYS_timer_create:
	case SYS_timer_settime:
	case SYS_timer_gettime:
	case SYS_setitimer:
	case SYS_getitimer:
	case SYS_times:
	case SYS_getrusage:
	case SYS_wait4:
	case SYS_waitid:
		return -ENOSYS;

	/* The file calls musl makes on the guest's behalf.

	 * These used to fall through to the syscall() above, which knows only
	 * clock_gettime, clock_getres and madvise and answers -ENOSYS for
	 * everything else - so the guest could not open a file, read one,
	 * close one or even close a descriptor. Writing worked, because write
	 * and writev have handlers of their own, which is why the guest could
	 * talk and still never manage to open anything.

	 * Every pointer here is a guest pointer and has to go through GUEST():
	 * the guest is ILP32, so it hands over 32 bits where the console's
	 * libc expects a 64-bit address. Passing them straight through would
	 * point newlib at an address that is never mapped. */
	case SYS_openat:
	{
		/* Opened here as before, but the guest is given a number of its own
		 * and the real descriptor is kept in the table. */
		int host_fd = openat((int)a, GUEST(const char *, b), (int)c, (int)d, (int)e);
		int guest_fd;

		/* newlib's open says -1 and sets errno; the openat above says a
		 * negated errno itself when the call is one it cannot take. */
		if (host_fd == -1)
			return result_of(-errno);
		if (host_fd < 0)
			return result_of(host_fd);
		guest_fd = guest_descriptor(host_fd);
		if (guest_fd < 0)
		{
			close(host_fd);
			return result_of(-EMFILE);
		}
		return result_of(guest_fd);
	}
	case SYS_read:
	{
		int host_fd = host_descriptor((int)a);
		ssize_t got;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		got = bounced_read(host_fd, GUEST(void *, b), (size_t)c);
		host_sd_unlock();
		return result_of(got);
	}
	case SYS_close:
	{
		/* Closing a guest descriptor closes the host one behind it and
		 * frees the slot; closing one the guest never opened is EBADF,
		 * which is what close(3) should say now that 3 means nothing. */
		if (!guest_descriptor_is_open((int)a))
			return result_of(-EBADF);
		guest_descriptor_forget((int)a);
		return result_of(0);
	}
	case SYS_lseek:
	{
		int host_fd = host_descriptor((int)a);
		off_t where;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		if ((int)c == SEEK_SET)
			note_large_offset("lseek", a, b);
		where = lseek(host_fd, (off_t)b, (int)c);
		host_sd_unlock();
		return result_of(where);
	}
	case SYS_pread64:
	{
		int host_fd = host_descriptor((int)a);

		return result_of(host_fd < 0 ? -EBADF : pread(host_fd, GUEST(void *, b), (size_t)c, (off_t)d));
	}
	case SYS_pwrite64:
	{
		int host_fd = host_descriptor((int)a);

		note_large_offset("pwrite64", a, d);
		return result_of(host_fd < 0 ? -EBADF : pwrite(host_fd, GUEST(const void *, b), (size_t)c, (off_t)d));
	}
	case SYS_readlinkat:
	{
		/* The buffer is guest memory, so it is copied out and back rather
		 * than handed to readlinkat as an address. The length is what the
		 * guest asked room for. */
		char link[1024];
		ssize_t length = readlinkat((int)a, GUEST(const char *, b), link, sizeof(link) - 1);

		if (length > 0)
		{
			char *out = GUEST(char *, c);

			if (out)
				memcpy(out, link, (size_t)length);
		}
		return result_of(length);
	}
	case SYS_fstat:
	{
		struct stat information;
		int host_fd = host_descriptor((int)a);
		int result;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		result = fstat(host_fd, &information);
		host_sd_unlock();

		if (result == 0)
		{
			struct guest_kstat *out = GUEST(struct guest_kstat *, b);

			if (out)
				kstat_from_stat(out, &information);
		}
		return result_of(result);
	}
	case SYS_newfstatat:
	{
		struct stat information;
		int result = fstatat((int)a, GUEST(const char *, b), &information, (int)d);

		if (result == 0)
		{
			struct guest_kstat *out = GUEST(struct guest_kstat *, c);

			if (out)
				kstat_from_stat(out, &information);
		}
		return result_of(result);
	}
	case SYS_getdents64:
		/* Not answered, and it cannot be from here.

		 * Reading a directory means asking the kernel for its entries in
		 * the console's packed format. devkitPro's libc exposes no
		 * getdents, and libnx exposes no service for it either, so there
		 * is no call to make - unlike every case above, which is an
		 * ordinary libc function with a guest pointer to widen.

		 * Said out loud because a guest that cannot list a directory will
		 * otherwise look for a file it has no way to find. */
		host_logf(HOST_LOG_WARN, "the guest asked to read a directory, which this console has no way to do");
		return result_of(-ENOSYS);
	case SYS_fsync:
	{
		int host_fd = host_descriptor((int)a);
		int result;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		result = fsync(host_fd);
		host_sd_unlock();
		return result_of(result);
	}
	case SYS_fdatasync:
	{
		int host_fd = host_descriptor((int)a);

		return result_of(host_fd < 0 ? -EBADF : fdatasync(host_fd));
	}
	case SYS_ftruncate:
	{
		int host_fd = host_descriptor((int)a);
		int result;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		note_large_offset("ftruncate", a, b);
		result = ftruncate(host_fd, (off_t)b);
		host_sd_unlock();
		return result_of(result);
	}
	case SYS_dup:
	{
		/* A second guest name for the same open file, which is what dup
		 * means. The host descriptor is genuinely duplicated rather than
		 * the slot being shared: two guest names for one host descriptor
		 * would both believe they own it, and the first close would leave
		 * the other naming a number the host could recycle - possibly
		 * onto the log file. */
		int host_fd = host_descriptor((int)a);
		int duplicate, guest_fd;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		duplicate = dup(host_fd);
		host_sd_unlock();
		if (duplicate < 0)
			return result_of(duplicate);
		guest_fd = guest_descriptor(duplicate);
		if (guest_fd < 0)
		{
			host_sd_lock();
			close(duplicate);
			host_sd_unlock();
			return result_of(-EMFILE);
		}
		return result_of(guest_fd);
	}
	case SYS_dup3:
	{
		/* Both numbers are the guest's: the source is translated, and
		 * the target is a table slot to fill, not a host descriptor -
		 * passing them straight to libc's dup3 would have operated on
		 * the host's own descriptors. */
		int host_fd = host_descriptor((int)a);
		int duplicate;

		if ((int)a == (int)b)
			return result_of(-EINVAL);
		if (host_fd < 0)
			return result_of(-EBADF);
		if (b < 3)
		{
			host_logf(HOST_LOG_WARN, "guest dup3 onto a standard stream is not supported");
			return result_of(-EINVAL);
		}
		if (b >= GUEST_MAXIMUM_FDS)
			return result_of(-EBADF);
		guest_descriptor_forget((int)b);
		host_sd_lock();
		duplicate = dup(host_fd);
		host_sd_unlock();
		if (duplicate < 0)
			return result_of(duplicate);
		guest_descriptor_at((int)b, duplicate);
		return result_of((int)b);
	}
	case SYS_pipe2:
	{
		/* The two descriptors come back in guest memory as an int pair. */
		int fds[2];
		int result = pipe2(fds, (int)b);
		int *out = GUEST(int *, a);

		if (result == 0 && out)
			memcpy(out, fds, sizeof(fds));
		return result_of(result);
	}
	case SYS_fcntl:
	{
		/* a = descriptor, b = command, c = its argument. The descriptor
		 * is the guest's own number and is translated like everywhere
		 * else; passing it straight through would let the guest operate
		 * on the host's descriptors, which the table exists to prevent.
		 * A command that makes a new descriptor is given a guest number
		 * of its own rather than handing the host's number back. */
		int host_fd = host_descriptor((int)a);

		if (host_fd < 0)
			return result_of(-EBADF);
		switch ((int)b)
		{
		case F_GETFD:
		case F_GETFL:
#ifdef F_GETOWN
		case F_GETOWN:
#endif
			return result_of(fcntl(host_fd, (int)b));
		case F_SETFD:
			return result_of(fcntl(host_fd, (int)b, (int)c));
		case F_SETFL:
			/* status flags carry the same O_* mismatch as open */
			return result_of(fcntl(host_fd, (int)b, open_flags_from_guest((int)c)));
#ifdef F_DUPFD
		case F_DUPFD:
		{
			int duplicate = fcntl(host_fd, F_DUPFD, (int)c);
			int guest_fd;

			if (duplicate < 0)
				return result_of(duplicate);
			guest_fd = guest_descriptor(duplicate);
			if (guest_fd < 0)
			{
				close(duplicate);
				return result_of(-EMFILE);
			}
			return result_of(guest_fd);
		}
#endif
		default:
			host_logf(HOST_LOG_WARN, "guest fcntl command %d is not supported", (int)b);
			return result_of(-EINVAL);
		}
	}
	case SYS_faccessat:
		return result_of(faccessat((int)a, GUEST(const char *, b), (int)c, 0));
	case SYS_unlinkat:
		return result_of(unlinkat((int)a, GUEST(const char *, b), (int)c));
	case SYS_mkdirat:
		return result_of(mkdirat((int)a, GUEST(const char *, b), (mode_t)c));
	case SYS_fchmod:
	{
		int host_fd = host_descriptor((int)a);
		int result;

		if (host_fd < 0)
			return result_of(-EBADF);
		host_sd_lock();
		result = fchmod(host_fd, (mode_t)b);
		host_sd_unlock();
		return result_of(result);
	}

	/* The rest are answered directly: these are either process
	 * attributes the guest asks about and never uses, or calls with no
	 * meaning on this console. Answering them with the truth, rather than
	 * with -ENOSYS, is what lets musl decide it can carry on. */
	case SYS_getpid:
		/* One process, whatever the console's own idea of the number is;
		 * the guest has no use for it beyond "not zero". */
		return 1;
	case SYS_getppid:
		return 1;
	case SYS_gettid:
		return gettid();
	case SYS_getuid:
	case SYS_geteuid:
	case SYS_getgid:
	case SYS_getegid:
		return result_of(getuid());
	case SYS_uname:
	{
		/* devkitA64's newlib has no <sys/utsname.h>, so the structure is
		 * described here rather than included. musl reads six fields from
		 * it and little else. */
		struct guest_utsname
		{
			char sysname[65];
			char nodename[65];
			char release[65];
			char version[65];
			char machine[65];
			char domainname[65];
		} names;
		char *out = GUEST(char *, a);

		if (!out)
			return result_of(-EFAULT);
		memset(&names, 0, sizeof(names));
		strncpy(names.sysname, "Linux", sizeof(names.sysname) - 1);
		strncpy(names.nodename, "switch", sizeof(names.nodename) - 1);
		strncpy(names.release, "1.0", sizeof(names.release) - 1);
		strncpy(names.machine, "aarch64", sizeof(names.machine) - 1);
		memcpy(out, &names, sizeof(names));
		return result_of(0);
	}
	case SYS_sched_yield:
		return result_of(sched_yield());
	case SYS_getrandom:
		/* Same problem as getdents64: no libc function and no service.
		 * musl's fallback chain accepts a failure here. */
		return result_of(-ENOSYS);
	case SYS_umask:
		return result_of(umask((mode_t)a));
	case SYS_flock:
		return result_of(flock((int)a, (int)b));
	case SYS_membarrier:
		return 0;
	case SYS_rt_sigprocmask:
		return 0;
	case SYS_getcwd:
	{
		char buffer[1024];
		char *out;
		char *found;

		host_sd_lock();
		found = getcwd(buffer, sizeof(buffer));
		host_sd_unlock();
		if (!found)
			return result_of(-errno);
		out = GUEST(char *, a);
		if (out)
		{
			size_t length = strlen(buffer) + 1;

			if ((size_t)b < length)
				length = (size_t)b;
			memcpy(out, buffer, length);
		}
		return result_of((long)strlen(buffer) + 1);
	}
	case SYS_chdir:
	{
		int result;

		host_sd_lock();
		result = chdir(GUEST(const char *, a));
		host_sd_unlock();
		return result_of(result);
	}
	case SYS_getrlimit:
	{
		/* A guess is better than a refusal here. The guest asks what the
		 * stack limit is and then sizes an allocation from the answer;
		 * telling it ENOSYS makes musl fall back to a default it would
		 * rather have been given, and telling it nothing it can use is
		 * worse still. devkitA64's newlib declares no <sys/resource.h>
		 * rlimit, so the two fields are written by hand. */
		char *out = GUEST(char *, b);

		if (!out)
			return result_of(-EFAULT);
		memset(out, 0, 16);
		/* soft limit first, then hard: 32 MB of stack, 64 MB ceiling */
		*(uint64_t *)out = 32ULL * 1024 * 1024;
		*(uint64_t *)(out + 8) = 64ULL * 1024 * 1024;
		return result_of(0);
	}
	case SYS_prlimit64:
		return 0;

	case SYS_kill:
	case SYS_tkill:
	case SYS_tgkill:
		/* No signal syscalls exist on this console, so the guest is told
		 * plainly that the call failed rather than being left to wait for
		 * a signal that will never arrive. */
		return result_of(-ENOSYS);

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -ENOSYS;
	}
}

/* Every call the guest makes passes through here on its way to the switch
 * above, so this is the one place that sees both the request and the answer.
 *
 * Until now the log recorded the requests and never the replies, which is
 * half the story: a run that spun for forty seconds calling writev over and
 * over produced 342 KB of identical lines that said nothing about whether
 * those writes worked. A failure is what matters and a success is silence,
 * so that is what is logged - which also cuts the volume by orders of
 * magnitude, because a working call now costs nothing.
 *
 * Failures are logged every time, not once, on purpose. A guest that retries
 * forever is the case being diagnosed, and one line of that is worse than no
 * line: it hides whether the thing being retried ever succeeded. */
long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	long long result;

	result = host_syscall_inner(number, a, b, c, d, e, f);

	/* a futex wait that found the value changed, or timed out, is how
	 * futexes work, not a failure; at dozens a second it was most of the log,
	 * and every line is a synchronous write to the card */
	if (number == SYS_futex && (result == -EAGAIN || result == -ETIMEDOUT))
		return result;
	/* refused by the console, and said once already (host_mman.c) */
	if (number == SYS_mprotect && result == -ENOTSUP)
		return result;
	if (result < 0 && result > -4096)
	{
		/* A negative small number is -errno; anything else negative is the
		 * guest's own convention for something else. */
		const char *name = NULL;

		switch (number)
		{
		case 56: name = "openat"; break;
		case 63: name = "read"; break;
		case 64: name = "write"; break;
		case 66: name = "writev"; break;
		case 61: name = "getdents64"; break;
		case 79: name = "newfstatat"; break;
		case 80: name = "fstat"; break;
		case 93: name = "exit"; break;
		case 94: name = "exit_group"; break;
		case 98: name = "futex"; break;
		case 35: name = "unlinkat"; break;
		case 34: name = "mkdirat"; break;
		default: break;
		}
		/* A call that names a file says which: its path is the guest's
		 * second argument, a string in guest memory. Without it a run that
		 * opened one missing file two hundred times said only its address. */
		if ((number == 56 || number == 35 || number == 34 || number == 79) && b > 0 && b < 0x100000000LL)
			host_logf(HOST_LOG_WARN, "guest %s (%lld) failed: errno %lld (\"%.160s\", %lld, %lld)",
				name ? name : "call", number, -result, (const char *)(uintptr_t)b, c, d);
		else
			host_logf(HOST_LOG_WARN, "guest %s (%lld) failed: errno %lld (args %lld, %lld, %lld, %lld, %lld, %lld)",
				name ? name : "call", number, -result, a, b, c, d, e, f);
	}
	return result;
}

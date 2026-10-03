/*
SYS_STAT_H

The parts of <sys/stat.h> newlib has not, for the Switch port.

posix_files.c sets file timestamps with utimensat, which takes its two
"leave this one alone" values as magic nanosecond counts rather than as
flags. Newlib declares neither the function nor the constants, so they are
here. The values are Linux's: the guest's musl expects them, and the port
shares that file with the Linux build.

Only what the port actually uses is declared. A fuller replacement would be
more misleading than this, because everything absent from it would fail
subtly rather than loudly.
*/

#ifndef __HALO_SWITCH_SYS_STAT_H
#define __HALO_SWITCH_SYS_STAT_H

#include <time.h>

/* newlib's own <sys/stat.h>, reached with include_next so that this file -
which sits earlier on the include path - does not include itself. */
#include_next <sys/stat.h>

/* utimensat's two ways of saying "leave this timestamp as it is"; the value
is a nanosecond count too large to be one */
#define UTIME_OMIT ((1L << 30) - 2L)
#define UTIME_NOW ((1L << 30) - 1L)

int utimensat(int descriptor, const char *path, const struct timespec times[2], int flags);

#endif

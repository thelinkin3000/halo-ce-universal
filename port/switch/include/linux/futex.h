/*
LINUX_FUTEX.H

The futux constants, for the Switch port.

The guest's musl synchronises its threads with futexes on 32-bit words in
guest memory, and it speaks Linux's futux ABI, so host_syscall.c needs the
operation numbers. They are declared here because there is no
<linux/futex.h>: the console is not Linux and nothing else here wants one.

What acts on them is host_futex.c, which implements them over libnx's thread
primitives rather than over the Linux syscall, because there is no syscall to
call.
*/

#ifndef __HALO_SWITCH_LINUX_FUTEX_H
#define __HALO_SWITCH_LINUX_FUTEX_H

/* the command is the low seven bits; FUTEX_PRIVATE_FLAG (128) and
FUTEX_CLOCK_REALTIME (256) sit above it and mean nothing here */
#define FUTEX_CMD_MASK ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI 12
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET_PRIVATE FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG
#define FUTEX_LOCK_PI_PRIVATE FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG

#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

/* the bits a FUTEX_WAIT_BITSET or FUTEX_WAKE_BITSET matches on */
#define FUTEX_BITSET_MATCH_ANY 0xffffffff

#endif
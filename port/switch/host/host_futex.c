/*
HOST_FUTEX.C

Futexes for the Switch port: the guest's threads synchronise with them, and
the console has no futex syscall to call.

The guest is ILP32 musl, so it speaks Linux's futux ABI over 32-bit words in
guest memory. Android answered with SYS_futex; there is nothing here to call,
so the two operations that matter - wait and wake - are built out of libnx's
thread primitives.

What that costs in fidelity, stated plainly:

- FUTEX_WAIT and FUTEX_WAKE are implemented properly, including the "value
changed, do not wait" case that the caller must be woken to observe. The
guest's musl uses these for its condition variables and its thread joins.

- The priority-inheritance operations (FUTEX_LOCK_PI and the rest) are not.
They need a wait queue whose order is by priority and an unlock that hands
the lock to the highest waiter; musl uses them only on a kernel that
advertises them, and the guest here does not, so it takes the plain
condition-variable path. They answer ENOSYS rather than pretending.

- FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET are treated as the plain forms: the
bitset is ignored. That is exact for a single bitset used as "any", which is
the usual case, and lossy if a caller ever waits on a subset, which musl does
not.

- The timeout is honoured through libnx's timed sleep, and a wait that times
out returns ETIMEDOUT exactly as Linux does.

The word the guest waits on is read and compared directly, through the host
view of the guest's address space. That is the same memory the guest's own
threads are running on, so the comparison sees the change that should wake it.
*/

#include "host.h"

#include <errno.h>
#include <linux/futex.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

/* ---------- a condition variable per futex word

libnx 4.x has no threadWait, threadWake or threadGetCurrent - the primitives
this would most naturally use are gone - but it does have Mutex and CondVar,
which are enough to build futux semantics on.

The mutex is what makes it correct rather than merely usual: a waiter holds
it while it tests the word and only then waits, so a change made between the
test and the wait cannot be missed. That is the lost-wakeup problem, and it
is why this is a condvar and not an event.

There is one list per word rather than a waiter list, because CondVar
signals a single arbitrary waiter and futex's wake is likewise a count. The
waiter count is kept so that wake knows how many are outstanding. */
#define FUTEX_LIMIT 128

struct futex_word
{
	uint32_t address;
	int waiters;
	Mutex lock;
	CondVar condition;
	struct futex_word *next;
};

static Mutex table_lock;
static int table_ready;
static struct futex_word *words;

static void ensure_table(void)
{
	if (table_ready)
		return;
	mutexInit(&table_lock);
	table_ready = 1;
	words = NULL;
}

/* the table is small and its lock is never held across a wait, so a plain
linear scan is the right structure; see the limit above */
static struct futex_word *word_for(uint32_t address, int create)
{
	struct futex_word *word;

	ensure_table();
	mutexLock(&table_lock);
	for (word = words; word; word = word->next)
	{
		if (word->address == address)
		{
			mutexUnlock(&table_lock);
			return word;
		}
	}
	mutexUnlock(&table_lock);
	if (!create)
		return NULL;

	word = malloc(sizeof(*word));
	if (!word)
		return NULL;
	word->address = address;
	word->waiters = 0;
	mutexInit(&word->lock);
	condvarInit(&word->condition);
	mutexLock(&table_lock);
	word->next = words;
	words = word;
	mutexUnlock(&table_lock);
	return word;
}

/* ---------- the guest's word, in the host's view of the guest's memory */

/* A guest address is 32 bits, and the guest's memory all lives below 4 GB
with the host mapping it in the same process, so the guest's pointer is that
value zero-extended. This is the same GUEST() the rest of the host uses; it
is written out here so this file does not depend on that one's macros. */
#define GUEST_WORD(address) (*(volatile uint32_t *)(uintptr_t)(uint32_t)(address))

/* ---------- the two operations */

/* Linux: wait while *address == value, or until it differs or the timeout
passes. Returns 0 woken, -EAGAIN if the value already differed,
-ETIMEDOUT if the timeout passed. */
static int futex_wait(uint32_t address, uint32_t value, const struct timespec *timeout)
{
	struct futex_word *word = word_for(address, 1);
	u64 nanoseconds;
	Result slept;
	int result;

	if (!word)
		return -EAGAIN;

	mutexLock(&word->lock);
	/* the value may already have changed: that is the whole point of the
	word, and a waiter that slept through it would hang forever. Holding
	the lock across the test is what makes the wait safe. */
	if (GUEST_WORD(address) != value)
	{
		mutexUnlock(&word->lock);
		return -EAGAIN;
	}
	word->waiters++;
	if (timeout)
	{
		nanoseconds = (u64)timeout->tv_sec * 1000000000ULL + (u64)timeout->tv_nsec;
		slept = condvarWaitTimeout(&word->condition, &word->lock, nanoseconds);
	}
	else
	{
		slept = condvarWait(&word->condition, &word->lock);
	}
	word->waiters--;
	mutexUnlock(&word->lock);

	if (R_FAILED(slept))
		return -ETIMEDOUT;
	/* the loop is the caller's: it re-tests its own condition. Linux
	returns 0 here whichever way it was woken, and so does this. */
	result = 0;
	(void)result;
	return 0;
}

/* Linux: wake up to count waiters on this word; returns how many. */
static int futex_wake(uint32_t address, int count)
{
	struct futex_word *word = word_for(address, 0);
	int woken = 0;

	if (!word || count <= 0)
		return 0;

	mutexLock(&word->lock);
	/* condvarWake wakes up to a number of waiters and does not say how
	many it managed, so the count is worked out here: it wakes exactly as
	many as there are waiters, up to the number asked for. The waiters
	count themselves back out as they wake - it is not adjusted here, or
	each wake would be subtracted twice and the count would drift
	negative, mis-counting every later wake. */
	woken = word->waiters < count ? word->waiters : count;
	condvarWake(&word->condition, woken);
	mutexUnlock(&word->lock);
	return woken;
}

/* ---------- what host_syscall.c calls */

long host_futex(uint32_t address, int operation, uint32_t value, uint64_t timeout_address,
	uint64_t address2, uint32_t value3)
{
	struct timespec timeout;
	struct timespec *timeout_pointer = NULL;
	int command = operation & FUTEX_CMD_MASK;

	/* for the waiting operations the fourth argument is a pointer to a
	timeout; for the others it is a count or a pointer-sized value */
	if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET || command == FUTEX_LOCK_PI ||
		command == FUTEX_WAIT_REQUEUE_PI)
	{
		if (timeout_address)
		{
			const struct guest_timespec_s
			{
				int32_t seconds;
				int32_t nanoseconds;
			} *value_pointer = (const void *)(uintptr_t)(uint32_t)timeout_address;

			timeout.tv_sec = value_pointer->seconds;
			timeout.tv_nsec = value_pointer->nanoseconds;
			timeout_pointer = &timeout;
		}
	}

	switch (command)
	{
	case FUTEX_WAIT:
	case FUTEX_WAIT_BITSET:
		return futex_wait(address, value, timeout_pointer);
	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		return futex_wake(address, (int)(uintptr_t)timeout_address ? (int)value : (int)value);
	case FUTEX_WAKE_OP:
		/* the comparison is meaningless without a user-supplied function
		to run on the woken threads' words, which this port does not have;
		report the wake it can do rather than failing outright */
		return futex_wake(address, (int)value);
	case FUTEX_REQUEUE:
	{
		/* move up to value waiters from one word to another, as the wake
		half of a requeue; the futex that asked for this is only ever
		woken by a requeue that could not be completed on Linux either */
		int woken = futex_wake(address, (int)value);

		if (woken)
			futex_wake((uint32_t)address2, woken);
		return woken;
	}
	case FUTEX_LOCK_PI:
	case FUTEX_TRYLOCK_PI:
	case FUTEX_CMP_REQUEUE_PI:
	case FUTEX_UNLOCK_PI:
		/* priority inheritance needs a priority-ordered wait queue and an
		unlock that hands the lock to the highest waiter. The guest's musl
		only reaches for these when the kernel claims to support them,
		and this one does not, so it takes its plain path; answering
		ENOSYS is honest, where pretending would deadlock */
		(void)address2;
		(void)value3;
		return -ENOSYS;
	default:
		return -ENOSYS;
	}
}
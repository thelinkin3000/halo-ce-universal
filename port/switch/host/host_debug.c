/*
HOST_DEBUG.C

The Switch version. The Android port's sampler cannot be built here, and the
reason is the same one that took the crash handler out of host_memory.c:
libnx exposes no syscall for installing a signal handler.

What the Android sampler does is worth describing, because it is a good
diagnostic and its absence should be understood rather than noticed later.
config.toml's debug.sample_seconds starts a thread which, every few
seconds, sends a signal to each of the guest's threads; a signal handler
walks that thread's frame pointer chain and logs the program counter, the
link register and up to twelve return addresses. When a game appears stuck
the sampler shows where each of its threads actually is, which is the
question a log of "it hung here" cannot answer. It costs nothing but a
signal every few seconds.

On the Switch there is no way to deliver a signal to another thread, so there
is nothing to sample on. The thread bookkeeping is kept - the guest's thread
ids are still tracked, which costs nothing and is what a future mechanism
would need - but the sampler records only that it was asked for and did not
start.

The way back, if it is wanted, is not a signal. The console can read and
write another thread's context directly, through svcGetDebugThreadContext
and svcSetDebugThreadContext, which libnx uses itself. A sampler built on
that would read each guest thread's registers on a timer without involving
the thread at all, which is arguably better than the signal version because
it cannot disturb what it is watching.
*/

#include "host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <switch.h>

/* ------------------------------------------------------------------ backtrace

The console has no signal syscalls, so there is no handler to install for a
fault and nothing catches one. What can be done is the other half of it: at any
point where the port notices a failure for itself, the host can walk its own
frame chain and say how it got there.

That needs frame pointers, which -O2 omits. The host is therefore compiled with
-fno-omit-frame-pointer (tools/switch_build.py), which costs a little on the
paths that never fail and buys a stack trace on the ones that do.

Only the host's frames appear. Guest frames do not: the guest is entered on its
own stack by guest_stack.S, which sets up a frame of its own, so a walk from a
host function reaches host callers and stops there. Where the guest is is the
watcher's business, below. */

#define BACKTRACE_MAXIMUM_DEPTH 32

void host_backtrace(const char *reason)
{
	uintptr_t *frame = (uintptr_t *)__builtin_frame_address(0);
	/* The chain cannot be trusted blindly: a stack that is exhausted or
	 * corrupt gives a plausible-looking pointer, and following it faults,
	 * which loses the trace and the log line explaining why it was wanted.
	 * So each step has to look like a frame: aligned, above the last one,
	 * and within a plausible distance of the stack it started on. */
	const uintptr_t floor = (uintptr_t)frame - (16 * 1024 * 1024);
	int depth;

	host_logf(HOST_LOG_WARN, "backtrace (%s), innermost first:", reason);
	for (depth = 0; depth < BACKTRACE_MAXIMUM_DEPTH; depth++)
	{
		if (!frame || (uintptr_t)frame & 15)
			break;
		host_logf(HOST_LOG_WARN, "  #%02d %p", depth, (void *)frame[1]);
		if ((uintptr_t)frame[0] & 15 || frame[0] <= (uintptr_t)frame ||
			frame[0] < floor)
			break;
		frame = (uintptr_t *)frame[0];
	}
	if (depth == 0)
		host_logf(HOST_LOG_WARN, "  no frames: the host was built without frame pointers");
}

/* the guest's thread ids, so that a mechanism which can reach them knows
which ones to reach */
#define MAXIMUM_THREADS 64

static pid_t guest_threads[MAXIMUM_THREADS];
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

static pid_t current_thread_id(void)
{
	u64 id = 0;

	if (R_FAILED(svcGetThreadId(&id, 0xFFFFFFFF)))
		return 0;
	return (pid_t)id;
}

void host_debug_thread_started(void)
{
	pid_t self = current_thread_id();
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (!guest_threads[index])
		{
			guest_threads[index] = self;
			break;
		}
	}
	pthread_mutex_unlock(&threads_lock);
}

void host_debug_thread_exited(void)
{
	pid_t self = current_thread_id();
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (guest_threads[index] == self)
			guest_threads[index] = 0;
	}
	pthread_mutex_unlock(&threads_lock);
}

void host_debug_start_sampler(const char *setting)
{
	unsigned int seconds = setting ? (unsigned int)atoi(setting) : 0;

	if (!seconds)
		return;
	host_logf(HOST_LOG_WARN,
		"debug.sample_seconds is %u, but the thread sampler needs a signal handler, "
		"which the Switch has no way to install; not starting it", seconds);
}
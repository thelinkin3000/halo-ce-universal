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

What the console does have is a profiler's way in: a process may pause one
of its own threads (svcSetThreadActivity) and read its registers while it is
paused (svcGetThreadContext3), with no debugger attached - the debug SVCs need
a debug handle, which a process cannot take on itself. The profiler below
(debug.profiler) is built on that.
*/

#include "host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

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

/* the guest's threads, by id and handle, for the profiler to reach; is_game
names the game thread */
#define MAXIMUM_THREADS 64

static struct
{
	pid_t id;
	Handle handle;
	int is_game;
} guest_threads[MAXIMUM_THREADS];
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

static pid_t current_thread_id(void)
{
	u64 id = 0;

	if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE)))
		return 0;
	return (pid_t)id;
}

void host_debug_thread_started(int is_game)
{
	pid_t self = current_thread_id();
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (!guest_threads[index].id)
		{
			guest_threads[index].id = self;
			guest_threads[index].handle = threadGetCurHandle();
			guest_threads[index].is_game = is_game;
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
		if (guest_threads[index].id == self)
			guest_threads[index].id = 0;
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

/* ------------------------------------------------------------------ profiler

config.toml's debug.profiler starts a thread that, debug.profile_hz (500)
times a second,
pauses each of the guest's threads in turn, reads where it is - its program
counter and the return addresses of its frame chain - and lets it go on. Each
distinct stack is counted, per thread, and every PROFILE_WINDOW_SECONDS the
counts go to a file of their own in the profile folder beside the game
(profile_NNN.txt), written by the profiler's thread, never the game's.
tools/switch_profile.py turns the addresses into functions.

A sample is taken whatever the thread is doing, waiting included, so a thread
stuck on the card or a lock shows as such. While a thread is paused the
profiler only reads: it takes no lock, allocates nothing and logs nothing,
since the paused thread may hold the lock it would need. The frame chain is
followed only inside the memory region the stack pointer is in, increasing,
so a corrupt chain ends the walk rather than faulting the profiler.

The host's addresses move from run to run; the file says where one of its
functions was (the anchor) so the tool can find the rest. The guest's image
is linked where it runs. */

#define PROFILE_DEPTH 24
#define PROFILE_ENTRIES 8192
#define PROFILE_WINDOW_SECONDS 20

struct profile_entry
{
	uint32_t count;
	uint8_t slot;
	uint8_t depth;
	uint64_t addresses[PROFILE_DEPTH];
};

static struct
{
	struct profile_entry *entries;
	uint32_t used;
	uint32_t dropped;
	uint32_t samples[MAXIMUM_THREADS];
	uint32_t failed[MAXIMUM_THREADS];
	pid_t ids[MAXIMUM_THREADS];
	int is_game[MAXIMUM_THREADS];
	unsigned hz;
	char image[64];
} profile;

static int profile_walk(const ThreadContext *context, uint64_t *addresses)
{
	MemoryInfo info;
	u32 page_info;
	uint64_t frame = context->fp, low, high;
	int depth = 0;

	addresses[depth++] = context->pc.x;
	if (R_FAILED(svcQueryMemory(&info, &page_info, context->sp)) || !(info.perm & Perm_R))
		return depth;
	low = info.addr;
	high = info.addr + info.size;
	while (depth < PROFILE_DEPTH && !(frame & 7) && frame >= low && frame + 16 <= high)
	{
		const uint64_t *record = (const uint64_t *)(uintptr_t)frame;
		uint64_t next = record[0], back = record[1];

		if (!back)
			break;
		addresses[depth++] = back;
		if (next <= frame)
			break;
		frame = next;
	}
	return depth;
}

static void profile_count(int slot, const uint64_t *addresses, int depth)
{
	uint64_t hash = 1469598103934665603ULL ^ (uint64_t)slot;
	uint32_t index, probe;
	int at;

	for (at = 0; at < depth; at++)
		hash = (hash ^ addresses[at]) * 1099511628211ULL;
	index = (uint32_t)(hash % PROFILE_ENTRIES);
	for (probe = 0; probe < PROFILE_ENTRIES; probe++)
	{
		struct profile_entry *entry = &profile.entries[(index + probe) % PROFILE_ENTRIES];

		if (!entry->count)
		{
			if (profile.used >= PROFILE_ENTRIES * 3 / 4)
				break;
			entry->count = 1;
			entry->slot = (uint8_t)slot;
			entry->depth = (uint8_t)depth;
			memcpy(entry->addresses, addresses, (size_t)depth * sizeof(*addresses));
			profile.used++;
			return;
		}
		if (entry->slot == slot && entry->depth == depth &&
			!memcmp(entry->addresses, addresses, (size_t)depth * sizeof(*addresses)))
		{
			entry->count++;
			return;
		}
	}
	profile.dropped++;
}

static void profile_write(unsigned window, double seconds)
{
	char path[PATH_MAX];
	FILE *file;
	uint32_t index;
	int slot, at;

	snprintf(path, sizeof(path), "%s/profile", host_executable_root());
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/profile/profile_%03u.txt", host_executable_root(), window);
	file = fopen(path, "w");
	if (!file)
	{
		host_logf(HOST_LOG_WARN, "profiler: cannot write %s", path);
		return;
	}
	fprintf(file, "halo profile 1\nwindow %u seconds %.1f hz %u\nimage %s\nanchor host_debug_start_profiler 0x%llx\n",
		window, seconds, profile.hz, profile.image, (unsigned long long)(uintptr_t)host_debug_start_profiler);
	fprintf(file, "dropped %u\n", (unsigned)profile.dropped);
	for (slot = 0; slot < MAXIMUM_THREADS; slot++)
	{
		if (profile.samples[slot] || profile.failed[slot])
			fprintf(file, "thread %d %s id %d samples %u failed %u\n", slot, profile.is_game[slot] ? "game" : "other",
				(int)profile.ids[slot], (unsigned)profile.samples[slot], (unsigned)profile.failed[slot]);
	}
	for (index = 0; index < PROFILE_ENTRIES; index++)
	{
		const struct profile_entry *entry = &profile.entries[index];

		if (!entry->count)
			continue;
		fprintf(file, "stack %u %d", (unsigned)entry->count, entry->slot);
		for (at = 0; at < entry->depth; at++)
			fprintf(file, " %llx", (unsigned long long)entry->addresses[at]);
		fputc('\n', file);
	}
	fclose(file);
	host_logf(HOST_LOG_INFO, "profiler: %s written (%u stacks)", path, (unsigned)profile.used);
}

static void *profile_thread(void *unused)
{
	u64 interval = 1000000000ULL / profile.hz;
	u64 window_start = armGetSystemTick();
	unsigned window = 0;
	int failed_said = 0;

	(void)unused;
	for (;;)
	{
		int slot;

		svcSleepThread((s64)interval);
		pthread_mutex_lock(&threads_lock);
		for (slot = 0; slot < MAXIMUM_THREADS; slot++)
		{
			ThreadContext context;
			uint64_t addresses[PROFILE_DEPTH];
			Handle handle = guest_threads[slot].handle;
			Result result;
			int depth = 0;

			if (!guest_threads[slot].id)
				continue;
			if (profile.ids[slot] != guest_threads[slot].id)
			{
				profile.ids[slot] = guest_threads[slot].id;
				profile.is_game[slot] = guest_threads[slot].is_game;
			}
			/* (paused: nothing below takes a lock or allocates) */
			result = svcSetThreadActivity(handle, ThreadActivity_Paused);
			if (R_SUCCEEDED(result))
			{
				result = svcGetThreadContext3(&context, handle);
				if (R_SUCCEEDED(result))
					depth = profile_walk(&context, addresses);
				svcSetThreadActivity(handle, ThreadActivity_Runnable);
			}
			if (!depth)
			{
				profile.failed[slot]++;
				if (!failed_said)
				{
					failed_said = 1;
					pthread_mutex_unlock(&threads_lock);
					host_logf(HOST_LOG_WARN, "profiler: a thread could not be sampled: 0x%x", (unsigned)result);
					pthread_mutex_lock(&threads_lock);
				}
				continue;
			}
			profile.samples[slot]++;
			profile_count(slot, addresses, depth);
		}
		pthread_mutex_unlock(&threads_lock);
		if (armTicksToNs(armGetSystemTick() - window_start) >= PROFILE_WINDOW_SECONDS * 1000000000ULL)
		{
			profile_write(window++, armTicksToNs(armGetSystemTick() - window_start) / 1e9);
			memset(profile.entries, 0, PROFILE_ENTRIES * sizeof(*profile.entries));
			memset(profile.samples, 0, sizeof(profile.samples));
			memset(profile.failed, 0, sizeof(profile.failed));
			profile.used = profile.dropped = 0;
			window_start = armGetSystemTick();
		}
	}
	return NULL;
}

void host_debug_start_profiler(unsigned hz, const char *image)
{
	pthread_t thread;

	if (!hz)
		return;
	if (hz > 2000)
		hz = 2000;
	profile.entries = calloc(PROFILE_ENTRIES, sizeof(*profile.entries));
	if (!profile.entries)
	{
		host_logf(HOST_LOG_WARN, "profiler: no memory for its table; not starting it");
		return;
	}
	profile.hz = hz;
	snprintf(profile.image, sizeof(profile.image), "%s", image);
	if (pthread_create(&thread, NULL, profile_thread, NULL) != 0)
	{
		host_logf(HOST_LOG_WARN, "profiler: cannot start its thread");
		return;
	}
	pthread_detach(thread);
	host_logf(HOST_LOG_WARN, "profiler: sampling the guest's threads %u times a second; a file every %d s in "
		"%s/profile (tools/switch_profile.py reads them)", hz, PROFILE_WINDOW_SECONDS, host_executable_root());
}

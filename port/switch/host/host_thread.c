/*
HOST_THREAD.C

Threads that run guest code.

Guest (ILP32) code keeps stack addresses in 32-bit registers, so every
thread that runs it needs its stack in guest memory. Every such thread is
created here, with its stack given to pthread_create: the stack pointer
never leaves the thread's own stack, which is also the one ART checks on
every call into Java (SDL). The host's main thread and SDL's audio callback
(host_sdl.c) hand their work to threads made by host_native_thread_create.
The guest's thread pointer (its musl struct pthread) is kept per thread in
host TLS.

Thread stacks are freed by a reaper thread once the thread has fully exited.

Every thread here would run on one core if left alone: libnx makes a
pthread on the process's default core, and the console's scheduler never
moves a thread off the cores its mask allows. So the first thread made, the
game's, keeps its core to itself, and each later one moves itself onto the
others (place_thread).
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <switch.h>

#define GUARD_SIZE 0x4000
/* the stack the C library gives a thread, for the host's part of it before it
moves onto the stack below 4 GB (host_native_thread_create) */
#define HOST_PART_STACK_SIZE (64 * 1024)

static __thread uint32_t guest_tp;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

/* ---------- stacks */

static void *stack_allocate(size_t size, void **mapping, size_t *mapping_size)
{
	size_t total = size + GUARD_SIZE;
	void *base = host_low_map(total, PROT_READ | PROT_WRITE);

	if (!base)
		return NULL;
	*mapping = base;
	*mapping_size = total;
	return (char *)base + GUARD_SIZE;
}

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)__builtin_frame_address(0);

	return sp < 0x100000000ULL;
}

/* ---------- calling into the guest */

typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		((guest_function)(uintptr_t)host_image.header->thread_attach)(0, 0, 0, 0);
	return ((guest_function)(uintptr_t)function)(a, b, c, d);
}

void host_run_guest_main(uint32_t boot)
{
	void (*entry)(uint32_t) = (void (*)(uint32_t))(uintptr_t)host_image.header->start;

	/* There is no crash handler on the Switch - libnx exposes no way to
	install a signal handler - so a guest that faults says nothing at all.
	These lines are the whole of what is known about it: the address it was
	about to be entered at, whether it is inside the image and inside its
	executable part, and where the stack and the boot structure are. A
	fault at the first instruction points at the entry or the stack; a
	fault later leaves the entry looking right. */
	/* The whole header, field by field. Reading only `start` showed it as
	0xffffffff while every field the loader used - the magic, the
	version, the import table, the names, the count - was correct, and
	those are the six words that sit before it. A fault at exactly one
	word is not what corruption usually looks like, and it could equally
	be a field that is filled in later, so the layout is printed in full
	rather than one value at a time. */
	{
		const struct halo_guest_header *header = host_image.header;

		host_logf(HOST_LOG_INFO, "guest header at %p", (void *)(uintptr_t)header);
		host_logf(HOST_LOG_INFO, "  magic %08x abi %d image_end %08x", header->magic, header->abi_version,
			header->image_end);
		host_logf(HOST_LOG_INFO, "  import table %08x names %08x count %08x (%u)", header->import_table,
			header->import_names, header->import_count, *(const uint32_t *)(uintptr_t)header->import_count);
		host_logf(HOST_LOG_INFO, "  start %08x thread_start %08x thread_attach %08x", header->start,
			header->thread_start, header->thread_attach);
		host_logf(HOST_LOG_INFO, "  init array %08x-%08x", header->init_array_start, header->init_array_end);
		host_logf(HOST_LOG_INFO, "  image %08x-%08x", host_image.base, host_image.end);
	}
	host_logf(HOST_LOG_INFO, "guest entry at %p, boot %08x", (void *)(uintptr_t)host_image.header->start,
		boot);
	host_logf(HOST_LOG_INFO, "  image %08x-%08x, guest thread pointer %p",
		host_image.base, host_image.end, (void *)(uintptr_t)host_get_tp());
	entry(boot);
	host_fatal("the guest returned from __guest_start");
}

/* ---------- cores

Every thread the game and the port make lands on the process's default core
(libnx's pthread_create asks for it, -2), and the kernel does not move a
thread to another core by itself. Everything shared one core: the game, the
Direct3D translation and Mesa's driver, and beside them internet play's
tunnel - which decrypts every packet the host sends and carries its
connections - its signalling, and the game's own helper threads. With a
lobby of eighteen players the game stuttered badly, while the console's
other cores had nothing to do.

So the game thread, the first one made here (host_main.c's game_main, which
becomes the guest's main thread), is held to the core it started on, and
every thread after it moves itself onto the other cores the process may use,
in turn. Each is allowed all of those, so the kernel can move it between
them, and none can take time from the game's core. */

static int game_core = -1;
static unsigned threads_placed;

static void place_thread(int is_game)
{
	u64 allowed = 0;
	u32 helpers;
	int core = (int)svcGetCurrentProcessorNumber();
	unsigned pick, index;
	Result result;

	if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !allowed)
		return;
	if (is_game)
	{
		game_core = core;
		result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
		host_logf(HOST_LOG_INFO, "the game thread keeps core %d to itself (the process may use cores %llx): 0x%x",
			core, (unsigned long long)allowed, (unsigned)result);
		return;
	}
	helpers = (u32)allowed & ~(game_core >= 0 ? 1u << game_core : 0u);
	if (!helpers)
		return;
	/* the next of the helper cores, in turn */
	pick = __atomic_fetch_add(&threads_placed, 1, __ATOMIC_RELAXED) % (unsigned)__builtin_popcount(helpers);
	for (index = 0; index < 32; index++)
	{
		if (!(helpers & (1u << index)))
			continue;
		if (!pick--)
			break;
	}
	result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, (s32)index, helpers);
	host_logf(HOST_LOG_INFO, "a thread starts on core %u (it may use cores %x): 0x%x", index, (unsigned)helpers,
		(unsigned)result);
}

/* place_thread, for threads that never run guest code: the shader cache's
compile thread (host_dk_shaders.c), which only writes files and its queue
and must not take the game thread's core. Threads that call into the guest
are made by host_native_thread_create, which places them itself. */
void host_thread_place_on_helper_core(void)
{
	place_thread(0);
}

/* ---------- guest threads */

struct thread_start
{
	void *(*function)(void *);
	void *argument;
	void *mapping;
	size_t mapping_size;
	/* the top of the stack below 4 GB the function runs on (guest_stack_call) */
	void *stack_top;
	/* the first thread made, the game's (place_thread) */
	int is_game;
};

void *guest_stack_call(void *stack, void *(*function)(void *), void *argument);

static int game_thread_made;

struct finished_thread
{
	struct finished_thread *next;
	pthread_t thread;
	void *mapping;
	size_t mapping_size;
};

static pthread_mutex_t reaper_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reaper_condition = PTHREAD_COND_INITIALIZER;
static struct finished_thread *finished_threads;
static int reaper_started;

static void *reaper(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct finished_thread *finished;

		pthread_mutex_lock(&reaper_lock);
		while (!finished_threads)
			pthread_cond_wait(&reaper_condition, &reaper_lock);
		finished = finished_threads;
		finished_threads = finished->next;
		pthread_mutex_unlock(&reaper_lock);
		pthread_join(finished->thread, NULL);
		host_low_unmap(finished->mapping, finished->mapping_size);
		free(finished);
	}
	return NULL;
}

static void *thread_main(void *context)
{
	struct thread_start start = *(struct thread_start *)context;
	struct finished_thread *finished;

	free(context);
	place_thread(start.is_game);
	host_debug_thread_started(start.is_game);
	/* on the stack below 4 GB the port mapped, and back (guest_stack.S) */
	guest_stack_call(start.stack_top, start.function, start.argument);
	host_debug_thread_exited();
	guest_tp = 0;

	finished = calloc(1, sizeof(*finished));
	if (!finished)
	{
		/* the stack mapping leaks; better than dereferencing nothing
		 * on a thread that is about to exit anyway */
		host_logf(HOST_LOG_ERROR, "a guest thread's stack could not be recorded for the reaper");
		return NULL;
	}
	finished->thread = pthread_self();
	finished->mapping = start.mapping;
	finished->mapping_size = start.mapping_size;
	pthread_mutex_lock(&reaper_lock);
	finished->next = finished_threads;
	finished_threads = finished;
	pthread_cond_signal(&reaper_condition);
	pthread_mutex_unlock(&reaper_lock);
	return NULL;
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	pthread_attr_t attributes;
	pthread_t thread;
	void *stack;
	int error;

	if (!start)
		return ENOMEM;
	pthread_mutex_lock(&reaper_lock);
	if (!reaper_started)
	{
		pthread_t reaper_thread;

		if (pthread_create(&reaper_thread, NULL, reaper, NULL) == 0)
		{
			pthread_detach(reaper_thread);
			reaper_started = 1;
		}
	}
	pthread_mutex_unlock(&reaper_lock);

	stack_size = (stack_size + 0xffff) & ~(size_t)0xffff;
	stack = stack_allocate(stack_size, &start->mapping, &start->mapping_size);
	if (!stack)
	{
		host_logf(HOST_LOG_ERROR, "the %zu byte thread stack could not be mapped below 4 GB", stack_size);
		free(start);
		return EAGAIN;
	}
	/* the guard page below the stack is a nicety: it turns a runaway thread
	 * into a fault rather than silent corruption of whatever is under it.
	 * On the Switch the console refuses to change the protection of a
	 * mapping made by svcMapMemory, so this is expected to fail here and
	 * is not worth stopping for. */
	if (mprotect(start->mapping, GUARD_SIZE, PROT_NONE) != 0)
		host_logf(HOST_LOG_INFO, "the stack's guard page was not applied (%s); the stack is at %p",
			strerror(errno), stack);
	/* Prove the stack is actually there.

	 * Everything above says the stack was mapped: the allocator logged the
	 * size, the split logged the guard and the range, and the guest was
	 * entered on it. None of that is evidence. The guest runs on this
	 * memory and a console crash report from the Switch says its stack
	 * pointer was 0x0FFFFAA0 with the faulting access at 0x0FFFFAB0 - a
	 * stack pointer just *below* the guard page at 0x10000000, in the part
	 * of the range that the split claimed to map.

	 * So: touch every page of the stack, write a pattern, read it back,
	 * and say so plainly if any page is not really there. A page that is
	 * not is a page the guest will fault on, and finding that out here
	 * costs a millisecond instead of a crash report. */
	{
		size_t offset;
		int bad = 0;
		volatile unsigned char *bytes = (volatile unsigned char *)stack;

		for (offset = 0; offset < stack_size; offset += 4096)
		{
			bytes[offset] = (unsigned char)(offset >> 12);
			if (bytes[offset] != (unsigned char)(offset >> 12))
			{
				if (bad < 8)
					host_logf(HOST_LOG_ERROR, "stack page %p (%zu bytes in) did not hold what was written to it",
						(void *)(stack + offset), offset);
				bad++;
			}
		}
		host_logf(bad ? HOST_LOG_ERROR : HOST_LOG_INFO,
			"stack check: %zu bytes at %p, %d of %zu pages unusable",
			stack_size, stack, bad, stack_size / 4096);
	}
	start->function = function;
	start->argument = argument;
	/* a stack grows down, from here */
	start->stack_top = (char *)stack + (stack_size & ~(size_t)15);
	/* (made only once one starts: main asks again at smaller sizes when the
	game thread's stack is refused) */
	start->is_game = !game_thread_made;
	/* The thread is made with a stack the C library chooses, and moves onto
	the one mapped here as it starts (thread_main). The library will not run
	a thread on a stack it is handed below 4 GB: on Horizon 21.2 it refused
	one, so the port made a plain thread and moved the game thread onto its
	stack by hand, while the guest's other threads ran on the library's own
	stacks, which on that firmware were low enough; on 22.5 it accepted the
	port's stack - aliased heap there - and ran the thread on a copy it mapped
	above 4 GB, where the guest cannot address it, while the original could no
	longer be touched. Every thread now moves onto its own stack the same way,
	on either. The library's stack only carries the host's part. */
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, HOST_PART_STACK_SIZE);
	error = pthread_create(&thread, &attributes, thread_main, start);
	pthread_attr_destroy(&attributes);
	if (error)
		host_logf(HOST_LOG_ERROR, "a thread for a %zu byte stack at %p could not be made: %s", stack_size, stack,
			strerror(error));
	if (error)
	{
		host_low_unmap(start->mapping, start->mapping_size);
		free(start);
	}
	else
		game_thread_made = 1;
	return error;
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size);
}

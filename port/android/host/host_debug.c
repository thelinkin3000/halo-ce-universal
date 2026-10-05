/*
HOST_DEBUG.C

A sampler for finding where the guest spends its time (or hangs) on a
device without root, where debuggerd cannot attach: with sample_seconds in
config.toml's [debug], every guest thread is interrupted that often and its program
counter, link register and frame chain are written to logcat. The addresses
symbolize against build/android/halo_guest.elf (llvm-symbolizer
--obj=build/android/halo_guest.elf 0x...).

A second mode profiles (debug.profile_hz in config.toml's [debug]): every guest
thread is interrupted that many times a second, the handler writes the
program counter, the link register and up to eight return addresses of the
frame chain into a preallocated ring (nothing in the handler allocates, locks
or logs), and a thread appends the ring to profile.bin in the data folder every
ten seconds, with each guest thread's CPU time (/proc/self/task/<tid>/schedstat)
once a second, the threads' names, and /proc/self/maps at the start of the file
so that addresses outside the guest image can be named by library.
tools/android_profile_report.py reads it (port/android/VULKAN.md, step 6).
*/

#include "host.h"

#include <errno.h>
#include <link.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAXIMUM_THREADS 64
#define SAMPLE_SIGNAL SIGURG

static pid_t guest_threads[MAXIMUM_THREADS];
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

void host_debug_thread_started(void)
{
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (!guest_threads[index])
		{
			guest_threads[index] = gettid();
			break;
		}
	}
	pthread_mutex_unlock(&threads_lock);
}

void host_debug_thread_exited(void)
{
	pid_t self = gettid();
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (guest_threads[index] == self)
			guest_threads[index] = 0;
	}
	pthread_mutex_unlock(&threads_lock);
}

/* Reads 16 bytes (a frame record) at an address of the guest without a lock and without faulting: the
kernel says no for an address that is not mapped. A signal handler may not take the memory lock
(host_low_owns does) that the thread it interrupted may hold. */
static int read_memory(uint64_t address, void *out, size_t size)
{
	struct iovec local = { out, size }, remote = { (void *)address, size };

	return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == (long)size;
}

/* ---- the profiling ring */

#define PROFILE_TAG 0x50524f46 /* in the sigqueue value of a profiling signal; the old sampler uses tgkill */
#define PROFILE_FRAMES 8

struct profile_sample
{
	uint32_t tid; /* written last: 0 while the entry is being filled or free */
	uint32_t reserved;
	uint64_t pc, lr;
	uint64_t frames[PROFILE_FRAMES];
};

static struct profile_sample *profile_ring;
static uint64_t profile_capacity; /* a power of two */
static uint64_t profile_head;
static volatile uint64_t profile_dropped;

static void profile_record(const struct sigcontext *registers)
{
	uint64_t index = __atomic_fetch_add(&profile_head, 1, __ATOMIC_RELAXED) & (profile_capacity - 1);
	struct profile_sample *sample = &profile_ring[index];
	uint64_t fp = registers->regs[29];
	int depth = 0;

	if (__atomic_load_n(&sample->tid, __ATOMIC_ACQUIRE))
	{
		/* the writer has not caught up: the ring is full */
		__atomic_fetch_add(&profile_dropped, 1, __ATOMIC_RELAXED);
		return;
	}
	sample->pc = registers->pc;
	sample->lr = registers->regs[30];
	/* in a system call (blocked, or only just back): the instruction before the pc is svc #0 */
	{
		uint32_t before = 0;

		sample->reserved = registers->pc >= 4 && read_memory(registers->pc - 4, &before, 4) && before == 0xd4000001u;
	}
	for (depth = 0; depth < PROFILE_FRAMES; depth++)
		sample->frames[depth] = 0;
	for (depth = 0; depth < PROFILE_FRAMES && fp && fp < 0x100000000ULL && !(fp & 7); depth++)
	{
		uint64_t frame[2];

		if (!read_memory(fp, frame, 16))
			break;
		sample->frames[depth] = frame[1];
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
	__atomic_store_n(&sample->tid, (uint32_t)gettid(), __ATOMIC_RELEASE);
}

static void sample_handler(int signal_number, siginfo_t *information, void *context)
{
	const ucontext_t *ucontext = context;
	const struct sigcontext *registers = (const struct sigcontext *)&ucontext->uc_mcontext;
	char line[512];
	int length, depth;
	uint64_t fp = registers->regs[29];

	(void)signal_number;
	if (information->si_code == SI_QUEUE && information->si_value.sival_int == PROFILE_TAG)
	{
		profile_record(registers);
		return;
	}
	length = snprintf(line, sizeof(line), "sample tid %d: pc %llx lr %llx", gettid(),
		(unsigned long long)registers->pc, (unsigned long long)registers->regs[30]);
	for (depth = 0; depth < 12 && fp && fp < 0x100000000ULL && !(fp & 7) && host_low_owns(fp, 16); depth++)
	{
		const uint64_t *frame = (const uint64_t *)fp;

		length += snprintf(line + length, sizeof(line) - length, " %llx", (unsigned long long)frame[1]);
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
	host_logf(HOST_LOG_INFO, "%s", line);
}

static void install_sample_handler(void)
{
	static int installed;
	struct sigaction action;

	if (installed)
		return;
	installed = 1;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = sample_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SAMPLE_SIGNAL, &action, NULL);
}

static void *sampler(void *context)
{
	unsigned int seconds = (unsigned int)(uintptr_t)context;

	for (;;)
	{
		pid_t threads[MAXIMUM_THREADS];
		int index;

		sleep(seconds);
		pthread_mutex_lock(&threads_lock);
		memcpy(threads, guest_threads, sizeof(threads));
		pthread_mutex_unlock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
		{
			if (threads[index])
				syscall(SYS_tgkill, getpid(), threads[index], SAMPLE_SIGNAL);
		}
	}
	return NULL;
}

void host_debug_start_sampler(const char *setting)
{
	pthread_t thread;
	unsigned int seconds = setting ? (unsigned int)atoi(setting) : 0;

	if (!seconds)
		return;
	install_sample_handler();
	if (pthread_create(&thread, NULL, sampler, (void *)(uintptr_t)seconds) == 0)
		pthread_detach(thread);
	host_logf(HOST_LOG_INFO, "sampling guest threads every %u s", seconds);
}

/* ---- the profiler's threads */

struct profile_file_header
{
	char magic[8]; /* "HPRF1" */
	uint32_t hz, sample_size, maps_size, reserved;
};

enum { PROFILE_BLOCK_SAMPLES = 1, PROFILE_BLOCK_CPU = 2, PROFILE_BLOCK_NAMES = 3 };

struct profile_block
{
	uint32_t type, count;
};

struct profile_cpu
{
	uint32_t tid, second;
	uint64_t nanoseconds;
};

struct profile_name
{
	uint32_t tid, reserved;
	char name[16];
};

static int profile_hz;
static char profile_path[640];

static void profile_write_block(FILE *file, uint32_t type, uint32_t count, const void *data, size_t size)
{
	struct profile_block block = { type, count };

	fwrite(&block, sizeof(block), 1, file);
	if (count)
		fwrite(data, size, count, file);
}

/* the guest threads' CPU time from schedstat (nanoseconds on the CPU, the first number) */
static uint32_t profile_cpu_times(const pid_t *threads, uint32_t second, struct profile_cpu *out)
{
	uint32_t index, count = 0;

	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		char path[96];
		unsigned long long nanoseconds = 0;
		FILE *file;

		if (!threads[index])
			continue;
		snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", threads[index]);
		file = fopen(path, "r");
		if (file)
		{
			if (fscanf(file, "%llu", &nanoseconds) == 1)
			{
				out[count].tid = (uint32_t)threads[index];
				out[count].second = second;
				out[count].nanoseconds = nanoseconds;
				count++;
			}
			fclose(file);
		}
	}
	return count;
}

static void profile_flush(FILE *file, uint64_t *tail, const pid_t *threads, struct profile_cpu *cpu, uint32_t cpu_count)
{
	uint64_t head = __atomic_load_n(&profile_head, __ATOMIC_ACQUIRE);
	struct profile_sample *batch;
	uint32_t count = 0, index;
	struct profile_name names[MAXIMUM_THREADS];
	uint32_t name_count = 0;

	if (head - *tail > profile_capacity)
		*tail = head - profile_capacity;
	batch = malloc(sizeof(*batch) * (size_t)(head - *tail ? head - *tail : 1));
	while (*tail < head)
	{
		struct profile_sample *sample = &profile_ring[*tail & (profile_capacity - 1)];

		if (!__atomic_load_n(&sample->tid, __ATOMIC_ACQUIRE))
			break; /* still being written: next time */
		batch[count++] = *sample;
		__atomic_store_n(&sample->tid, 0, __ATOMIC_RELEASE);
		(*tail)++;
	}
	profile_write_block(file, PROFILE_BLOCK_SAMPLES, count, batch, sizeof(*batch));
	free(batch);
	profile_write_block(file, PROFILE_BLOCK_CPU, cpu_count, cpu, sizeof(*cpu));
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		char path[96];
		FILE *comm;

		if (!threads[index])
			continue;
		memset(&names[name_count], 0, sizeof(names[name_count]));
		names[name_count].tid = (uint32_t)threads[index];
		snprintf(path, sizeof(path), "/proc/self/task/%d/comm", threads[index]);
		comm = fopen(path, "r");
		if (comm)
		{
			if (fgets(names[name_count].name, sizeof(names[name_count].name), comm))
				names[name_count].name[strcspn(names[name_count].name, "\n")] = 0;
			fclose(comm);
		}
		name_count++;
	}
	profile_write_block(file, PROFILE_BLOCK_NAMES, name_count, names, sizeof(names[0]));
	fflush(file);
	host_logf(HOST_LOG_INFO, "profile: %u samples written (%llu dropped so far)", count, (unsigned long long)profile_dropped);
}

/* the loaded objects with their base addresses ("#dl <base> <name>"), appended to the maps: a library
that is mapped from the APK shows in /proc/self/maps as the APK, whichever it is */
struct dl_text
{
	char *text;
	size_t size;
};

static int dl_collect(struct dl_phdr_info *info, size_t size, void *argument)
{
	struct dl_text *out = argument;
	char line[640];
	int length = snprintf(line, sizeof(line), "#dl %llx %s\n", (unsigned long long)info->dlpi_addr, info->dlpi_name ? info->dlpi_name : "");

	(void)size;
	out->text = realloc(out->text, out->size + (size_t)length);
	memcpy(out->text + out->size, line, (size_t)length);
	out->size += (size_t)length;
	return 0;
}

static void *profiler(void *unused)
{
	FILE *file = fopen(profile_path, "wb");
	struct profile_file_header header;
	struct profile_cpu cpu[10 * MAXIMUM_THREADS];
	uint32_t cpu_count = 0, second = 0;
	uint64_t tail = 0, next, period = 1000000000ull / (uint64_t)profile_hz, samples_this_second = 0;
	char *maps = NULL;
	size_t maps_size = 0;
	FILE *maps_file;

	(void)unused;
	if (!file)
	{
		host_logf(HOST_LOG_ERROR, "profile: cannot write %s: %s", profile_path, strerror(errno));
		return NULL;
	}
	maps_file = fopen("/proc/self/maps", "r");
	if (maps_file)
	{
		char buffer[4096];
		size_t got;

		while ((got = fread(buffer, 1, sizeof(buffer), maps_file)) > 0)
		{
			maps = realloc(maps, maps_size + got);
			memcpy(maps + maps_size, buffer, got);
			maps_size += got;
		}
		fclose(maps_file);
	}
	{
		struct dl_text objects = { NULL, 0 };

		dl_iterate_phdr(dl_collect, &objects);
		maps = realloc(maps, maps_size + objects.size);
		memcpy(maps + maps_size, objects.text, objects.size);
		maps_size += objects.size;
		free(objects.text);
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, "HPRF1", 5);
	header.hz = (uint32_t)profile_hz;
	header.sample_size = (uint32_t)sizeof(struct profile_sample);
	header.maps_size = (uint32_t)maps_size;
	fwrite(&header, sizeof(header), 1, file);
	fwrite(maps, 1, maps_size, file);
	free(maps);
	fflush(file);
	{
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		next = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
	}
	for (;;)
	{
		pid_t threads[MAXIMUM_THREADS];
		uint32_t index;
		struct timespec deadline;
		siginfo_t information;

		next += period;
		deadline.tv_sec = (time_t)(next / 1000000000ull);
		deadline.tv_nsec = (long)(next % 1000000000ull);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
		pthread_mutex_lock(&threads_lock);
		memcpy(threads, guest_threads, sizeof(threads));
		pthread_mutex_unlock(&threads_lock);
		memset(&information, 0, sizeof(information));
		information.si_signo = SAMPLE_SIGNAL;
		information.si_code = SI_QUEUE;
		information.si_pid = getpid();
		information.si_uid = getuid();
		information.si_value.sival_int = PROFILE_TAG;
		for (index = 0; index < MAXIMUM_THREADS; index++)
			if (threads[index])
				syscall(SYS_rt_tgsigqueueinfo, getpid(), threads[index], SAMPLE_SIGNAL, &information);
		samples_this_second++;
		if (samples_this_second >= (uint64_t)profile_hz)
		{
			samples_this_second = 0;
			second++;
			if (cpu_count + MAXIMUM_THREADS <= sizeof(cpu) / sizeof(cpu[0]))
				cpu_count += profile_cpu_times(threads, second, cpu + cpu_count);
			if (second % 10 == 0)
			{
				profile_flush(file, &tail, threads, cpu, cpu_count);
				cpu_count = 0;
			}
		}
	}
	return NULL;
}

void host_debug_start_profiler(int hz, const char *data_root)
{
	pthread_t thread;
	uint64_t capacity = 1;

	if (hz <= 0)
		return;
	profile_hz = hz;
	/* ten seconds of samples of up to 24 threads, and some more */
	while (capacity < (uint64_t)hz * 12 * 24 && capacity < (1ull << 20))
		capacity <<= 1;
	profile_ring = mmap(NULL, capacity * sizeof(struct profile_sample), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (profile_ring == MAP_FAILED)
	{
		profile_ring = NULL;
		host_logf(HOST_LOG_ERROR, "profile: cannot allocate the ring");
		return;
	}
	profile_capacity = capacity;
	snprintf(profile_path, sizeof(profile_path), "%s/profile.bin", data_root);
	install_sample_handler();
	if (pthread_create(&thread, NULL, profiler, NULL) == 0)
		pthread_detach(thread);
	host_logf(HOST_LOG_INFO, "profiling guest threads %d times a second into %s (ring of %llu samples)", hz, profile_path,
		(unsigned long long)capacity);
}

/*
HOST_MEMORY.C

Guest address space for the Android port.

Everything the guest touches must lie below 4 GB, but an Android process
shares that range with ART, whose heap spaces use compressed 32-bit
references and so also live there. The host therefore claims only what the
guest needs, when it needs it:

- the Xbox contiguous window at 0x80000000 and the image's own range, at
  start-up (both at the addresses the guest was built for, else wherever
  there is room: the guest is told where the window is, and the image's
  pointers are moved with it, host_loader.c);
- pools of address space for the guest's other mappings (malloc arenas,
  thread stacks), reserved in free gaps below 4 GB as they fill up.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages behind
the textures it caches, and the SIGSEGV handler here records the first
write to each. Other faults are reported (with guest-relative addresses) and
passed on to the previous handler. Under ARM translation (the x86 emulator)
page faults cannot be caught, and the tracking compares page contents
instead (host_watch_hash.c).
*/

#include "host.h"
#include "host_watch_hash.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#else
/* (the Linux arm64 build's host, port/linux/host: no Android properties,
each read as unset) */
#define PROP_VALUE_MAX 92

static int __system_property_get(const char *name, char *value)
{
	(void)name;
	value[0] = '\0';
	return 0;
}
#endif
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE 0x1000ULL
#define LOW_LIMIT 0x100000000ULL
#define LOW_START 0x01000000ULL
#define POOL_SIZE (256ULL * 1024 * 1024)
#define POOL_PAGES (POOL_SIZE / PAGE)
#define MAXIMUM_POOLS 12

struct pool
{
	uint64_t base;
	uint32_t free_pages;
	uint8_t used[POOL_PAGES]; /* 1 for each page handed out */
};

/* A pool is never freed, and its base never changes: pool_count is
published after its entry (with release ordering), so host_low_owns reads
both without the lock, from signal handlers too. */
static struct pool *pools[MAXIMUM_POOLS];
static int pool_count;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t window_base, window_end, window_size;
static uint64_t image_base, image_end;

static uint64_t round_up(uint64_t value)
{
	return (value + PAGE - 1) & ~(PAGE - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

/* ---------- moving the window */

/* The guest was built for the window at HALO_GUEST_WINDOW_BASE and works in
guest addresses throughout, so every address it hands the host through a
system call is in its own address space. When the window had to be placed
elsewhere those calls name the address the game expects, and the host has to
put them where the window actually is before touching the page tables, and
report results back in guest addresses. Where the window did not move, both
are the identity and nothing here changes. */

static int guest_window_address(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE &&
		address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t to_host(uint64_t address)
{
	if (window_base != HALO_GUEST_WINDOW_BASE && guest_window_address(address))
		return window_base + (address - HALO_GUEST_WINDOW_BASE);
	return address;
}

static uint64_t to_guest(uint64_t address)
{
	if (window_base != HALO_GUEST_WINDOW_BASE && guest_window_address(window_base + (address - window_base)))
		return HALO_GUEST_WINDOW_BASE + (address - window_base);
	return address;
}

/* ---------- reading /proc/self/maps and smaps */

/* the next line of a /proc file into line, without its newline; a line
longer than the buffer (a long path) is cut short there, and the rest of it
skipped, so that it is never taken for lines of its own. 0 at the end. */
static int read_proc_line(FILE *file, char *line, size_t size)
{
	size_t length;

	if (!fgets(line, (int)size, file))
		return 0;
	length = strlen(line);
	if (length && line[length - 1] == '\n')
	{
		line[length - 1] = '\0';
		return 1;
	}
	for (;;)
	{
		int c = fgetc(file);

		if (c == '\n' || c == EOF)
			break;
	}
	return 1;
}

/* ---------- reserving address space below 4 GB */

/* the lowest gap of at least size bytes at or above minimum, from
/proc/self/maps, starting on an alignment boundary; 0 if none. The window is
placed on one because the game turns a window offset into an address by
masking, which only holds while the window is a whole number of its own
size from the bottom. */
static uint64_t find_gap(uint64_t size, uint64_t minimum, uint64_t alignment)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	uint64_t previous_end = minimum;
	uint64_t result = 0;

	if (alignment > 1)
		minimum = round_up(minimum) & ~(alignment - 1);
	if (!maps)
		return 0;
	while (read_proc_line(maps, line, sizeof(line)))
	{
		unsigned long long start, end;

		if (sscanf(line, "%llx-%llx", &start, &end) != 2)
			continue;
		if (end <= previous_end)
			continue;
		if (start > previous_end && start - previous_end >= size)
			break;
		previous_end = end > previous_end ? end : previous_end;
		if (previous_end >= LOW_LIMIT)
			break;
	}
	fclose(maps);
	previous_end = round_up(previous_end);
	if (alignment > 1)
		previous_end = (previous_end + alignment - 1) & ~(alignment - 1);
	if (previous_end + size <= LOW_LIMIT)
		result = previous_end;
	return result;
}

/* reports whatever already occupies part of a range, so a refusal to use it
says what was in the way rather than only that something was */
static void log_conflicts(uint64_t address, uint64_t size)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];

	if (!maps)
		return;
	while (read_proc_line(maps, line, sizeof(line)))
	{
		unsigned long long start, end;

		if (sscanf(line, "%llx-%llx", &start, &end) != 2)
			continue;
		if (end > address && start < address + size)
		{
			line[strcspn(line, "\n")] = 0;
			host_logf(HOST_LOG_ERROR, "  in the way: %s", line);
		}
	}
	fclose(maps);
}

/* claims a fixed range for the guest, or reports the address is taken */
static int reserve(uint64_t address, uint64_t size)
{
	void *result = mmap((void *)address, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

	if (result == (void *)address)
		return 0;
	if (result != MAP_FAILED)
	{
		/* some kernels honour the "no replace" part by handing back an
		address elsewhere rather than by failing, so errno still says
		whatever the last call left behind and cannot be trusted here */
		munmap(result, size);
		errno = EEXIST;
	}
	return -1;
}

static int crosses_custom_edition(uint64_t address, uint64_t size);

static struct pool *pool_new(void)
{
	/* above the image first: ART allocates its own low-4 GB memory from
	the bottom up */
	uint64_t minimum = image_end ? image_end : LOW_START;
	struct pool *pool;
	int attempt;

	if (pool_count == MAXIMUM_POOLS)
	{
		host_logf(HOST_LOG_ERROR, "guest memory: all %d pools are in use", MAXIMUM_POOLS);
		return NULL;
	}
	for (attempt = 0; attempt < 64; attempt++)
	{
		uint64_t address = find_gap(POOL_SIZE, minimum, 1);

		if (!address && minimum != LOW_START)
		{
			minimum = LOW_START;
			address = find_gap(POOL_SIZE, minimum, 1);
		}
		if (!address)
			return NULL;
		/* (clear of Custom Edition's tag cache's own address, as the window
		and the image are: where the tag cache cannot be had there and goes
		elsewhere, an address in that range is always one its maps were
		written with, to be moved, port/linux/include/halo_port_window.h) */
		if (crosses_custom_edition(address, POOL_SIZE))
		{
			minimum = HALO_GUEST_CUSTOM_EDITION_END;
			continue;
		}
		if (reserve(address, POOL_SIZE) == 0)
		{
			pool = calloc(1, sizeof(*pool));
			if (!pool)
			{
				munmap((void *)(uintptr_t)address, POOL_SIZE);
				host_logf(HOST_LOG_ERROR, "guest memory: out of memory for a pool's page map");
				return NULL;
			}
			pool->base = address;
			pool->free_pages = POOL_PAGES;
			pools[pool_count] = pool;
			__atomic_store_n(&pool_count, pool_count + 1, __ATOMIC_RELEASE);
			host_logf(HOST_LOG_INFO, "guest memory pool %d at %08llx", pool_count - 1, (unsigned long long)address);
			return pool;
		}
		/* raced with another mapping; look further up */
		minimum = address + PAGE;
	}
	return NULL;
}

uint32_t host_memory_window_base(void)
{
	return (uint32_t)window_base;
}

uint32_t host_memory_window_size(void)
{
	return (uint32_t)window_size;
}

/* whether a range crosses Custom Edition's tag cache, which the guest
reserves at its fixed address (HALO_GUEST_CUSTOM_EDITION_BASE): the window
and a moved image are placed clear of it, so that Custom Edition maps can
run wherever the Java runtime leaves that range free */
static int crosses_custom_edition(uint64_t address, uint64_t size)
{
	return address < HALO_GUEST_CUSTOM_EDITION_END && address + size > HALO_GUEST_CUSTOM_EDITION_BASE;
}

#ifdef __ANDROID__
/* the window of size at the place the game data was linked for, else the
first free range that starts on a whole number of the alignment */
uint32_t host_memory_window_wanted;
uint32_t host_memory_image_wanted;

static uint64_t place_window(uint64_t size)
{
	uint64_t alignment = size > HALO_GUEST_WINDOW_ALIGNMENT ? size : HALO_GUEST_WINDOW_ALIGNMENT;
	uint64_t minimum = LOW_START;
	int attempt;

	/* (debug.window_base: tried first, at each size) */
	if (host_memory_window_wanted)
	{
		if (host_memory_window_wanted % alignment == 0 && !crosses_custom_edition(host_memory_window_wanted, size) &&
			reserve(host_memory_window_wanted, size) == 0)
			return host_memory_window_wanted;
		host_logf(HOST_LOG_INFO, "debug.window_base %08x will not take %u MB", host_memory_window_wanted,
			(unsigned int)(size / (1024 * 1024)));
	}
	if (reserve(HALO_GUEST_WINDOW_BASE, size) == 0)
		return HALO_GUEST_WINDOW_BASE;
	for (attempt = 0; attempt < 128; attempt++)
	{
		uint64_t candidate = find_gap(size, minimum, alignment);

		if (!candidate)
			break;
		if (crosses_custom_edition(candidate, size))
		{
			minimum = HALO_GUEST_CUSTOM_EDITION_END;
			continue;
		}
		/* a mapping in the way here is one ART has just made, and may be
		live, so it is never taken back: look further up */
		if (reserve(candidate, size) != 0)
		{
			minimum = candidate + PAGE;
			continue;
		}
		return candidate;
	}
	return 0;
}
#endif

/* the image is moved by whole numbers of this: more than a page, so its
sections keep the alignment they were linked with (guest.ld) */
#define IMAGE_ALIGNMENT 0x10000ULL

/* The image's linked range, held from JNI_OnLoad (host_memory_reserve_early),
when the game's own process (":game") has just started and its Java side has
allocated little, until host_memory_initialize claims the image's exact range
there. It is held, not required: the image goes elsewhere if it cannot be
(host_loader.c moves its pointers), as the window does. Large enough for
either game image (GL ES or Vulkan). */
#define EARLY_IMAGE_HOLD 0x03000000ULL

static uint64_t early_hold_base, early_hold_size;
/* host_memory_initialize found no room for the window or the image */
static int placement_failed;

void host_memory_reserve_early(void)
{
	if (reserve(HALO_GUEST_IMAGE_BASE, EARLY_IMAGE_HOLD) == 0)
	{
		early_hold_base = HALO_GUEST_IMAGE_BASE;
		early_hold_size = EARLY_IMAGE_HOLD;
		host_logf(HOST_LOG_INFO, "held the guest image's address %08llx at start-up",
			(unsigned long long)early_hold_base);
	}
	else
		host_logf(HOST_LOG_INFO, "the guest image's address %08llx is taken at start-up (%s); it will go elsewhere",
			(unsigned long long)HALO_GUEST_IMAGE_BASE, strerror(errno));
}

int host_memory_fixed_unavailable(void)
{
	return placement_failed;
}

/* the mappings below 4 GB, for a report of why the game's memory could not
be placed (logcat, and memory_map.txt in the data folder: host_main.c) */
void host_memory_report_low_mappings(FILE *file)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	char model[PROP_VALUE_MAX] = "", heap[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "";

	__system_property_get("ro.product.model", model);
	__system_property_get("dalvik.vm.heapsize", heap);
	__system_property_get("ro.build.version.release", release);
	if (file)
	{
		fprintf(file, "device: %s, Android %s, Java heap %s\n", model[0] ? model : "unknown",
			release[0] ? release : "unknown", heap[0] ? heap : "unknown");
		fprintf(file, "mappings below 4 GB:\n");
	}
	if (!maps)
		return;
	while (read_proc_line(maps, line, sizeof(line)))
	{
		unsigned long long lo, hi;

		if (sscanf(line, "%llx-%llx", &lo, &hi) != 2 || lo >= LOW_LIMIT)
			continue;
		host_logf(HOST_LOG_INFO, "low mapping: %s", line);
		if (file)
			fprintf(file, "%s\n", line);
	}
	fclose(maps);
}

int host_memory_initialize(uint32_t preferred_base, uint32_t size, uint32_t *base)
{
	uint64_t minimum = LOW_START;
	int attempt;
	int image_placed;

	/* the image first, where it was linked: the search below reads the
	mappings, so it has to be claimed before the window is placed or the
	search hands back the image's own address */
	image_base = preferred_base;
	image_end = preferred_base + round_up(size);
	/* (the hold from start-up gives way to the image's own range) */
	if (early_hold_size)
	{
		munmap((void *)(uintptr_t)early_hold_base, early_hold_size);
		early_hold_base = early_hold_size = 0;
	}
	image_placed = 0;
#ifdef __ANDROID__
	/* (debug.image_base: tried first, as if the linked address were taken) */
	if (host_memory_image_wanted)
	{
		if (host_memory_image_wanted % IMAGE_ALIGNMENT == 0 &&
			!crosses_custom_edition(host_memory_image_wanted, round_up(size)) &&
			reserve(host_memory_image_wanted, round_up(size)) == 0)
		{
			image_base = host_memory_image_wanted;
			image_end = image_base + round_up(size);
			image_placed = 1;
			host_logf(HOST_LOG_INFO, "debug.image_base: the guest image at %08llx", (unsigned long long)image_base);
		}
		else
			host_logf(HOST_LOG_INFO, "debug.image_base %08x will not take the image", host_memory_image_wanted);
	}
#endif
	if (!image_placed)
		image_placed = reserve(image_base, image_end - image_base) == 0;
#ifndef __ANDROID__
	/* (the desktop guests, 64-bit ARM Linux's and the x32 one, are built
	for the window and the image where they were linked, and are never
	moved: halo_android_abi.h) */
	if (!image_placed)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's address %08llx is taken (%s)", (unsigned long long)image_base,
			strerror(errno));
		log_conflicts(image_base, image_end - image_base);
		placement_failed = 1;
		return -1;
	}
	window_size = HALO_GUEST_WINDOW_SIZE;
	window_base = reserve(HALO_GUEST_WINDOW_BASE, window_size) == 0 ? HALO_GUEST_WINDOW_BASE : 0;
	if (!window_base)
	{
		host_logf(HOST_LOG_ERROR, "the Xbox memory window's range %08x-%08llx is taken (%s)", HALO_GUEST_WINDOW_BASE,
			(unsigned long long)HALO_GUEST_WINDOW_BASE + window_size, strerror(errno));
		log_conflicts(HALO_GUEST_WINDOW_BASE, window_size);
		placement_failed = 1;
		return -1;
	}
#else
	if (!image_placed)
	{
		/* the Java runtime got there first (its heap can start as low as
		32 MB and run past 1 GB): the image goes wherever there is room once
		the window has its place, and its pointers are moved with it */
		host_logf(HOST_LOG_INFO, "the guest image's address %08llx is taken (%s); putting it somewhere free",
			(unsigned long long)image_base, strerror(errno));
		log_conflicts(image_base, image_end - image_base);
		image_base = image_end = 0;
	}

	/* The window, as large as there is room for: each size is tried where
	the game and its data expect it, then anywhere free. The Java runtime
	usually holds 0x80000000; the game runs with the window elsewhere as
	long as the map data is moved with it, which the port does and is told
	about here. */
	for (window_size = HALO_GUEST_WINDOW_MAXIMUM; window_size >= HALO_GUEST_WINDOW_SIZE; window_size /= 2)
	{
		window_base = place_window(window_size);
		if (window_base)
			break;
		host_logf(HOST_LOG_INFO, "no free range of %u MB for the Xbox memory window",
			(unsigned int)(window_size / (1024 * 1024)));
	}
	if (!window_base)
	{
		host_logf(HOST_LOG_ERROR, "no free range of %u MB for the Xbox memory window",
			(unsigned int)(HALO_GUEST_WINDOW_SIZE / (1024 * 1024)));
		placement_failed = 1;
		return -1;
	}
#endif
	window_end = window_base + window_size;
	for (attempt = 0, minimum = LOW_START; attempt < 128 && !image_placed; attempt++)
	{
		uint64_t candidate = find_gap(round_up(size), minimum, IMAGE_ALIGNMENT);

		if (!candidate)
			break;
		if (crosses_custom_edition(candidate, round_up(size)))
		{
			minimum = HALO_GUEST_CUSTOM_EDITION_END;
			continue;
		}
		if (reserve(candidate, round_up(size)) != 0)
		{
			minimum = candidate + PAGE;
			continue;
		}
		image_base = candidate;
		image_end = candidate + round_up(size);
		image_placed = 1;
	}
	if (!image_placed)
	{
		host_logf(HOST_LOG_ERROR, "no free range of %u MB for the guest image",
			(unsigned int)(round_up(size) / (1024 * 1024)));
		placement_failed = 1;
		return -1;
	}
	*base = (uint32_t)image_base;
	host_logf(HOST_LOG_INFO, "Xbox memory window of %u MB at %08llx-%08llx, guest image at %08llx-%08llx",
		(unsigned int)(window_size / (1024 * 1024)),
		(unsigned long long)window_base, (unsigned long long)window_end,
		(unsigned long long)image_base, (unsigned long long)image_end);
	return 0;
}

/* ---------- page pools */

static void *pool_take(struct pool *pool, uint64_t pages)
{
	uint64_t run = 0, page;

	if (pool->free_pages < pages)
		return NULL;
	for (page = 0; page < POOL_PAGES; page++)
	{
		if (pool->used[page])
		{
			run = 0;
			continue;
		}
		if (++run == pages)
		{
			uint64_t first = page + 1 - pages;

			memset(&pool->used[first], 1, pages);
			pool->free_pages -= pages;
			return (void *)(pool->base + first * PAGE);
		}
	}
	return NULL;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t pages = round_up(size) / PAGE;
	void *address = NULL;
	int index;

	if (!pages || pages > POOL_PAGES)
		return NULL;
	pthread_mutex_lock(&memory_lock);
	for (index = 0; index < pool_count && !address; index++)
		address = pool_take(pools[index], pages);
	if (!address)
	{
		struct pool *pool = pool_new();

		if (pool)
			address = pool_take(pool, pages);
	}
	pthread_mutex_unlock(&memory_lock);
	if (!address)
		return NULL;
	if (mmap(address, pages * PAGE, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address)
	{
		host_low_unmap(address, pages * PAGE);
		return NULL;
	}
	return address;
}

static struct pool *pool_of(uint64_t address, uint64_t size)
{
	int count = __atomic_load_n(&pool_count, __ATOMIC_ACQUIRE);
	int index;

	for (index = 0; index < count; index++)
	{
		if (in_range(address, size, pools[index]->base, pools[index]->base + POOL_SIZE))
			return pools[index];
	}
	return NULL;
}

/* gives pages of a pool back, with the memory lock held: the memory goes,
the address space stays reserved */
static void pool_release(struct pool *pool, uint64_t start, uint64_t length)
{
	uint64_t first = (start - pool->base) / PAGE, count = length / PAGE, page;

	mmap((void *)(uintptr_t)start, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
	for (page = first; page < first + count; page++)
	{
		if (pool->used[page])
		{
			pool->used[page] = 0;
			pool->free_pages++;
		}
	}
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)address + size) - start;
	struct pool *pool;

	pthread_mutex_lock(&memory_lock);
	pool = pool_of(start, length);
	if (pool)
		pool_release(pool, start, length);
	pthread_mutex_unlock(&memory_lock);
}

int host_low_owns(uintptr_t address, size_t size)
{
	if (in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end))
		return 1;
	return pool_of(address, size) != NULL;
}

/* ---------- the guest's memory system calls

The guest maps, unmaps, reprotects and advises only memory that is its own:
the pools' pages, the Xbox window, the image's range, and the ranges it
mapped itself at addresses of its choosing (MAP_FIXED_NOREPLACE in free
address space). The rest of the low 4 GB is the host's (on Android, ART's
too). What each page is in use for is kept so that a "no replace" mapping
fails where something is mapped, as the kernel's does: a pool's used[], and
guest_pages for the window and the guest's own ranges. All under the memory
lock. */

/* a bit for each page below 4 GB outside the pools and the image that the
guest has mapped: in the window, and its own ranges */
static uint8_t guest_pages[LOW_LIMIT / PAGE / 8];

static int guest_page(uint64_t address)
{
	uint64_t page = address / PAGE;

	return (guest_pages[page >> 3] >> (page & 7)) & 1;
}

static void set_guest_pages(uint64_t address, uint64_t length, int value)
{
	uint64_t page;

	for (page = address / PAGE; page < (address + length) / PAGE; page++)
	{
		if (value)
			guest_pages[page >> 3] |= (uint8_t)(1 << (page & 7));
		else
			guest_pages[page >> 3] &= (uint8_t)~(1 << (page & 7));
	}
}

/* what a page below 4 GB is */
enum
{
	PAGE_HOST, /* the host's, or free address space */
	PAGE_POOL,
	PAGE_WINDOW,
	PAGE_IMAGE,
	PAGE_GUEST, /* a range the guest mapped itself */
};

static int page_kind(uint64_t address, struct pool **pool)
{
	*pool = NULL;
	if (in_range(address, PAGE, window_base, window_end))
		return PAGE_WINDOW;
	if (in_range(address, PAGE, image_base, image_end))
		return PAGE_IMAGE;
	*pool = pool_of(address, PAGE);
	if (*pool)
		return PAGE_POOL;
	return guest_page(address) ? PAGE_GUEST : PAGE_HOST;
}

/* the end of the run of pages from address (before end) of the same kind,
and pool, as the first */
static uint64_t run_end(uint64_t address, uint64_t end, int *kind, struct pool **pool)
{
	struct pool *next_pool;

	*kind = page_kind(address, pool);
	for (address += PAGE; address < end; address += PAGE)
	{
		if (page_kind(address, &next_pool) != *kind || next_pool != *pool)
			break;
	}
	return address;
}

/* whether any page of a run holds a mapping */
static int run_in_use(uint64_t address, uint64_t end, int kind, const struct pool *pool)
{
	for (; address < end; address += PAGE)
	{
		if (kind == PAGE_IMAGE || kind == PAGE_GUEST)
			return 1;
		if (kind == PAGE_POOL && pool->used[(address - pool->base) / PAGE])
			return 1;
		if (kind == PAGE_WINDOW && guest_page(address))
			return 1;
	}
	return 0;
}

/* whether every page of a range is the guest's (PAGE_IMAGE too, unless
image is 0), with the memory lock held */
static int guest_owns(uint64_t address, uint64_t end, int image)
{
	while (address < end)
	{
		struct pool *pool;
		int kind;

		address = run_end(address, end, &kind, &pool);
		if (kind == PAGE_HOST || (kind == PAGE_IMAGE && !image))
			return 0;
	}
	return 1;
}

/* the write tracking forgets the window's pages that were remapped or
reprotected */
static void forget_window_range(uint64_t address, uint64_t length)
{
	uint64_t start = address, end = address + length;

	if (start < window_base)
		start = window_base;
	if (end > window_end)
		end = window_end;
	if (start < end)
		host_memory_watch_forget((uint32_t)start, (uint32_t)(end - start));
}

static long guest_mmap_fixed(uint64_t address, uint64_t length, int protection, int flags, int fd, int64_t offset)
{
	uint64_t end = address + length, at;
	int replace = !(flags & MAP_FIXED_NOREPLACE);
	int host = 0, owned = 0, in_use = 0;
	void *result;
	long status;

	if (address & (PAGE - 1))
		return -EINVAL;
	if (end > LOW_LIMIT || end < address)
		return -ENOMEM;
	pthread_mutex_lock(&memory_lock);
	for (at = address; at < end;)
	{
		struct pool *pool;
		int kind;
		uint64_t next = run_end(at, end, &kind, &pool);

		if (kind == PAGE_HOST)
			host = 1;
		else
		{
			owned = 1;
			in_use |= run_in_use(at, next, kind, pool);
		}
		at = next;
	}
	if (!owned)
	{
		/* free address space, or the host's: a range of the guest's own
		where the kernel finds nothing mapped */
		if (replace)
		{
			status = -EINVAL;
			goto done;
		}
		result = mmap((void *)(uintptr_t)address, length, protection, flags, fd, offset);
		if (result == MAP_FAILED)
			status = -errno;
		else if (result != (void *)(uintptr_t)address)
		{
			/* (a kernel older than 4.17 takes MAP_FIXED_NOREPLACE as a
			hint) */
			munmap(result, length);
			status = -EEXIST;
		}
		else
		{
			set_guest_pages(address, length, 1);
			status = (long)address;
		}
		goto done;
	}
	if (host)
	{
		/* (partly the host's: never replaced, and not free either) */
		status = replace ? -EINVAL : -EEXIST;
		goto done;
	}
	if (!replace && in_use)
	{
		status = -EEXIST;
		goto done;
	}
	/* the guest's own memory: a reservation (or, with MAP_FIXED, a mapping)
	is replaced */
	result = mmap((void *)(uintptr_t)address, length, protection, (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED,
		fd, offset);
	if (result == MAP_FAILED)
	{
		status = -errno;
		goto done;
	}
	for (at = address; at < end;)
	{
		struct pool *pool;
		int kind;
		uint64_t next = run_end(at, end, &kind, &pool);
		uint64_t page;

		if (kind == PAGE_POOL)
		{
			/* (so that host_low_map does not hand them out again) */
			for (page = (at - pool->base) / PAGE; page < (next - pool->base) / PAGE; page++)
			{
				if (!pool->used[page])
				{
					pool->used[page] = 1;
					pool->free_pages--;
				}
			}
		}
		else if (kind == PAGE_WINDOW)
			set_guest_pages(at, next - at, 1);
		at = next;
	}
	forget_window_range(address, length);
	status = (long)address;
done:
	pthread_mutex_unlock(&memory_lock);
	return status;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	void *result;

	if (!length || length < size)
		return -EINVAL;
	if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))
	{
		/* (in guest addresses, and the result too: where the window is, if
		it moved) */
		long status = guest_mmap_fixed(to_host(address), length, protection, flags, fd, offset);

		return status < 0 ? status : (long)to_guest((uint64_t)status);
	}
	result = host_low_map(length, PROT_NONE);
	if (!result)
		return -ENOMEM;
	if (mmap(result, length, protection, flags | MAP_FIXED, fd, offset) != result)
	{
		int error = errno;

		host_low_unmap(result, length);
		return -error;
	}
	return (long)(uintptr_t)result;
}

/* pool pages go back to their pool, the window's to its reservation, and
the guest's own ranges are unmapped; nothing is done unless all of the
range is one of these (the image is never unmapped) */
long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size), end, at;
	long status = 0;

	/* (where the window is, if it moved) */
	address = to_host(address);
	end = address + length;

	if ((address & (PAGE - 1)) || !length || length < size)
		return -EINVAL;
	if (end > LOW_LIMIT || end < address)
		return -EINVAL;
	pthread_mutex_lock(&memory_lock);
	if (!guest_owns(address, end, 0))
	{
		status = -EINVAL;
		goto done;
	}
	for (at = address; at < end;)
	{
		struct pool *pool;
		int kind;
		uint64_t next = run_end(at, end, &kind, &pool);

		if (kind == PAGE_POOL)
			pool_release(pool, at, next - at);
		else if (kind == PAGE_WINDOW)
			mmap((void *)(uintptr_t)at, next - at, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
				-1, 0);
		else if (kind == PAGE_GUEST)
			munmap((void *)(uintptr_t)at, next - at);
		if (kind == PAGE_WINDOW || kind == PAGE_GUEST)
			set_guest_pages(at, next - at, 0);
		at = next;
	}
	forget_window_range(address, length);
done:
	pthread_mutex_unlock(&memory_lock);
	return status;
}

/* mprotect and madvise only on memory that is the guest's (above). The
write tracking forgets what they change in the Xbox window. (Host
addresses: the callers move a guest's address to where the window is
first.) */
static long guest_range(uint64_t address, uint64_t size, uint64_t *length)
{
	int owned;

	if (address & (PAGE - 1))
		return -EINVAL;
	*length = round_up(size);
	if (*length < size)
		return -ENOMEM;
	if (!*length)
		return 0;
	if (address + *length > LOW_LIMIT || address + *length < address)
		return -ENOMEM;
	pthread_mutex_lock(&memory_lock);
	owned = guest_owns(address, address + *length, 1);
	pthread_mutex_unlock(&memory_lock);
	return owned ? 1 : -ENOMEM;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	/* (where the window is, if it moved) */
	uint64_t host = to_host(address), length;
	long check = guest_range(host, size, &length);

	if (check <= 0)
		return check;
	if (mprotect((void *)(uintptr_t)host, length, protection))
		return -errno;
	forget_window_range(host, length);
	return 0;
}

long host_guest_madvise(uint64_t address, uint64_t size, int advice)
{
	/* (where the window is, if it moved) */
	uint64_t host = to_host(address), length;
	long check = guest_range(host, size, &length);

	if (check <= 0)
		return check;
	if (madvise((void *)(uintptr_t)host, length, advice))
		return -errno;
	forget_window_range(host, length);
	return 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_MAXIMUM / PAGE)

/* (each page: 0 not watched, 1 watched and read-only, 2 made writable by a
watched write. A fault on a page at 2 is a write another thread made as
this one was making the page writable, and is made again; one on a page at
0, freed memory or none of the watch's, is a crash) */
static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
static int watch_hashing;
static uint8_t page_hash_watched[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static uint32_t page_hashed_frame[WATCH_PAGE_COUNT];
static struct watch_hash page_hashes =
{
	.base = (const uint8_t *)(uintptr_t)HALO_GUEST_WINDOW_BASE,
	.page_count = WATCH_PAGE_COUNT,
	.watched = page_hash_watched,
	.hash = page_hash,
	.hashed_frame = page_hashed_frame,
	.generation = page_generation,
	.current_generation = &current_generation,
	.frame = 1,
};

/* The guest reaches this with addresses that are already in the window
wherever it was placed (platform_contiguous_base), so the tracking follows
the window the host actually reserved, not the one the game was built for. */
static int in_window(uint64_t address)
{
	return address >= window_base && address - window_base < window_size;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - window_base) / PAGE;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 2;
	mprotect((void *)(window_base + page * PAGE), PAGE, PROT_READ | PROT_WRITE);
}

static struct sigaction previous_segv, previous_bus, previous_ill;

/* the guest's code, the stack and the frame records at a crash, through the
log's sink (which neither locks nor uses stdio); the walk copies each record
without faulting (host_arch_read_memory), as the frame pointer may hold
anything at a crash */
static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	char line[256];
	uint64_t pc, sp, fp, lr;
	uint64_t returns[24];
	int count, index;

	host_arch_context(context, &pc, &sp, &fp, &lr);
	snprintf(line, sizeof(line), "signal %d at address %p: pc %016llx lr %016llx sp %016llx fp %016llx",
		signal_number, information->si_addr, (unsigned long long)pc, (unsigned long long)lr,
		(unsigned long long)sp, (unsigned long long)fp);
	host_log(HOST_LOG_ERROR, line);
	/* (addr2line wants the addresses the image was linked at, which is not
	where it ran if the host had to move it) */
	if (pc >= host_image.base && pc < host_image.end)
	{
		snprintf(line, sizeof(line), "  in the guest image: addr2line -e halo_guest.elf 0x%llx 0x%llx",
			(unsigned long long)(uint32_t)(pc - host_image.shift),
			(unsigned long long)(lr ? (uint32_t)(lr - host_image.shift) : 0));
		host_log(HOST_LOG_ERROR, line);
	}
	for (index = 0; host_arch_register_line(context, index, line, sizeof(line)); index++)
		host_log(HOST_LOG_ERROR, line);
	count = host_arch_walk_frames(fp, host_arch_read_memory, returns, (int)(sizeof(returns) / sizeof(returns[0])));
	for (index = 0; index < count; index++)
	{
		snprintf(line, sizeof(line), "  frame %2d: return %016llx", index, (unsigned long long)returns[index]);
		host_log(HOST_LOG_ERROR, line);
	}
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the previous
	(or default) handler */
}

/* (each handler keeps the interrupted code's errno: a write tracking fault
can come between any call and its errno check) */
static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	uint64_t address = (uint64_t)information->si_addr;
	int saved_errno = errno;

	if (watch_active && in_window(address))
	{
		uint64_t page = watch_page(address);

		if (page_protected[page] == 1)
		{
			mark_written(page);
			errno = saved_errno;
			return;
		}
		if (page_protected[page] == 2)
			return;
	}
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
	errno = saved_errno;
}

static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
	int saved_errno = errno;

	report_crash(signal_number, information, context);
	chain(&previous_bus, signal_number, information, context);
	errno = saved_errno;
}

static void ill_handler(int signal_number, siginfo_t *information, void *context)
{
	int saved_errno = errno;

	report_crash(signal_number, information, context);
	chain(&previous_ill, signal_number, information, context);
	errno = saved_errno;
}

void host_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_use_hashes(void)
{
	watch_hashing = 1;
}

/* memory_watch_protect: write-protects the pages of a range, or keeps their
hash under host_memory_watch_use_hashes; a size outside the guest window
is ignored */
void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
	{
		watch_hash_protect(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		if (page_protected[page] != 1)
		{
			page_protected[page] = 1;
			mprotect((void *)(window_base + page * PAGE), PAGE, PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

/* memory_watch_generation: the newest generation of the pages of a range.
Under hashing this hashes the pages again, once a frame. */
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!size || !in_window(address))
		return 0;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
		return watch_hash_generation(&page_hashes, (uint32_t)first, (uint32_t)last);
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

/* memory_watch_prepare_write: unprotects the pages the host itself is about
to write into. Nothing under hashing: the write shows in the next hash and
there is no protection to fault on. */
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || watch_hashing || !size)
		return;
	if (start + size <= window_base || start >= window_end)
		return;
	if (start < window_base)
		start = window_base;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page] == 1)
			mark_written(page);
	}
}

/* memory_watch_forget: the pages of a range were remapped or reprotected, so
treat them as written and unwatched */
void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
	{
		watch_hash_forget(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

/* memory_watch_begin_frame: pages may be hashed again; nothing under page
protection */
void host_memory_watch_begin_frame(void)
{
	if (watch_hashing)
		watch_hash_begin_frame(&page_hashes);
}

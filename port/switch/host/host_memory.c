/*
HOST_MEMORY.C

Guest address space for the Android port.

Everything the guest touches must lie below 4 GB, but an Android process
shares that range with ART, whose heap spaces use compressed 32-bit
references and so also live there. The host therefore claims only what the
guest needs, when it needs it:

- the Xbox contiguous window at 0x80000000 and the image's own range, at
  start-up (both at fixed addresses the guest was built for);
- pools of address space for the guest's other mappings (malloc arenas,
  thread stacks), reserved in free gaps below 4 GB as they fill up.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages behind
the textures it caches. (On the Switch this file's crash handler is
absent; see host_install_signal_handlers below.) The handler there records the first
write to each. Other faults are reported (with guest-relative addresses) and
passed on to the previous handler.
*/

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE 0x1000ULL
#define LOW_LIMIT 0x100000000ULL
/* The lowest address the port will place guest memory at.

Android starts at 16 MB. The Switch starts at 256 MB, which is the lowest
address the memory probe ever placed anything at, and therefore the lowest
this has any evidence for. Mapping below it has failed at both 16 MB and
64 MB with ENOMEM, and both of those attempts were below anything the probe
had touched; at 256 MB and above every mapping the probe made succeeded.

This cost three runs to establish, and the reason it was not established
first is worth recording: the probe was written to answer the questions the
port asked of it - can this address hold memory, can this memory execute,
do the two views alias - and not the one the port turned out to need, which
is where the usable address space begins. A probe should have walked upwards
from the bottom of the sub-4 GB space mapping a page at a time until one
refused, and reported that address. It would have answered the question
directly instead of by elimination.

The pools still have room: they grow from here to the image at 0x40000000,
which is 768 MB.
*/
#define LOW_START 0x10000000ULL
#define POOL_SIZE (256ULL * 1024 * 1024)
#define POOL_PAGES (POOL_SIZE / PAGE)
#define MAXIMUM_POOLS 12

/* Where the guest's memory has to be code memory (host_mman.c), a pool and
the window are made real a chunk at a time and kept: see commit, below */
#define CHUNK_SIZE (16ULL * 1024 * 1024)
#define POOL_CHUNKS (POOL_SIZE / CHUNK_SIZE)
#define WINDOW_CHUNKS (HALO_GUEST_WINDOW_SIZE / CHUNK_SIZE)

struct pool
{
	uint64_t base;
	uint32_t free_pages;
	uint8_t used[POOL_PAGES]; /* 1 for each page handed out */
	uint8_t committed[POOL_CHUNKS]; /* 1 for each chunk made real (commit) */
};

static struct pool *pools[MAXIMUM_POOLS];
static int pool_count;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t window_base, window_end;
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
	/* No translation on the way back. The guest is told the window's real
	 * base in its boot structure and works in real addresses: it asked to
	 * map at the moved base, was once handed HALO_GUEST_WINDOW_BASE back
	 * instead, and its allocator - which checks that a reservation returns
	 * exactly what was asked for - unmapped the window it had just made
	 * and reported "cannot reserve the Xbox contiguous memory window". */
	(void)window_base;
	return address;
}

/* ---------- reserving address space below 4 GB */

/* whether anything at all is already mapped in a range, which is what a
reservation has to avoid: it is asking the kernel, because a reservation
made by this port is invisible to it */
static int pool_mapped(uint64_t address, uint64_t size)
{
	uint64_t where = address;

	while (where < address + size && where < LOW_LIMIT)
	{
		MemoryInfo information = { 0 };
		u32 page_info = 0;

		if (R_FAILED(svcQueryMemory(&information, &page_info, where)))
			return 1; /* cannot tell: say it is taken, which is the safe answer */
		if (information.type != MemType_Unmapped)
			return 1;
		if (information.addr + information.size <= where)
			return 1;
		where = round_up(information.addr + information.size);
	}
	return 0;
}

/* Ranges claimed for the guest but not yet mapped.

On Linux a reservation is a PROT_NONE mapping, and the pages are handed out
later by mapping over it with MAP_FIXED. That does not work here, and the
first run of the game on a console is how it showed: the pool's 256 MB
reservation was mapped, and then host_low_map asked for 16 MB of it with
MAP_FIXED, and svcMapMemory answered ENOMEM because something was already
mapped at that address.

MAP_FIXED means "replace what is there", and the port's mmap only knows how
to map; making it replace would mean splitting a mapping in three, which for
a PROT_NONE reservation has nothing to copy and for anything else is a
correctness question worth more than this problem needs.

So a reservation is recorded instead of mapped. Nothing is mapped, there is
nothing for MAP_FIXED to collide with, and find_gap - which reads the
kernel's view of the address space, where a reservation is invisible - is
given the list below so that it does not hand the same address out twice.
*/
#define RESERVATION_LIMIT 64

struct reservation_record
{
	uint64_t base, size;
	/* whether this range was set aside for one specific large purpose -
	 * the Xbox contiguous window - rather than being a pool the guest's
	 * ordinary allocations come out of. Only a reserved purpose is offered
	 * to host_mman's allocator for a large request: a pool is 256 MB and
	 * would always win a search for the largest range, and handing the
	 * window to the guest's heap is worse than not sharing the table at
	 * all. */
	int for_a_purpose;
} reservations[RESERVATION_LIMIT];
static int reservation_count;

static int range_claimed(uint64_t address, uint64_t size);

/* Whether this port has set a range aside, for the guest image, for a
 * memory pool or for the Xbox contiguous window.
 *
 * The second allocator asks this before handing an address out. A
 * reservation is only a note held here - nothing is mapped - so without
 * this the other allocator cannot see one, and the guest's heap, which
 * grows through the ordinary search, was offered the window's address and
 * took it: a 128 MB heap at 0x11014000, after which the window had nowhere
 * left below 4 GB and the game reported no contiguous memory.
 */
/* The reservations themselves, with their sizes.
 *
 * A pool is 256 MB and there can be a dozen of them, so the reservations
 * can add up to more than the whole of the space below 4 GB - and a
 * reservation is invisible to the kernel, so the address search has to be
 * told about them or it keeps finding gaps that are already spoken for.
 */
void host_memory_log_reservations(void)
{
	int index;

	host_logf(HOST_LOG_INFO, "%d reservations:", reservation_count);
	for (index = 0; index < reservation_count; index++)
		host_logf(HOST_LOG_INFO, "  %012llx-%012llx  %llu MB%s",
			(unsigned long long)reservations[index].base,
			(unsigned long long)(reservations[index].base + reservations[index].size),
			(unsigned long long)(reservations[index].size / (1024 * 1024)),
			reservations[index].for_a_purpose ? "  (a window)" : "");
}

int host_memory_range_is_reserved(uint64_t address, uint64_t size)
{
	return range_claimed(address, size);
}

static int range_claimed(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < reservation_count; index++)
	{
		if (address < reservations[index].base + reservations[index].size &&
			reservations[index].base < address + size)
			return 1;
	}
	return 0;
}

/* The largest reservation, so that a caller mapping something big can be
 * given the address the port already set aside for it.
 *
 * Two allocators live in this port and they had no way to talk: this file
 * reserves the Xbox contiguous window at HALO_GUEST_WINDOW_BASE (0x80000000)
 * and hands its pools out from below it, while host_mman.c's find_free
 * searches the kernel's view of the address space on its own. It cannot see
 * a reservation here - there is no mapping behind one, that was the whole
 * reason reservations stopped being mappings - so it handed out an address
 * that happened to be free, the guest's window landed away from where the
 * port had told everything it would be, and the guest's heap then grew into
 * it and was refused.
 *
 * So the search is told. It asks for the biggest range reserved and prefers
 * it when one is large enough, which puts the window where the rest of the
 * port expects it and keeps the heap out of its way.
 */
uint64_t host_memory_reserved_region(uint64_t size)
{
	uint64_t best_size = 0;
	uint64_t best_base = 0;
	int index;

	for (index = 0; index < reservation_count; index++)
	{
		if (reservations[index].for_a_purpose && reservations[index].size >= size &&
			reservations[index].size > best_size)
		{
			best_size = reservations[index].size;
			best_base = reservations[index].base;
		}
	}
	return best_base;
}

/* The guest image's fixed address, kept from libnx from the start of main.
 *
 * reserve() tells libnx about the image's range when the image is loaded, and
 * that is too late: threads already exist by then - the game thread, which
 * falls back to a stack libnx places itself, and the reaper - and libnx puts
 * each thread's stack (128 KB and its TLS, 0x21000 bytes) at a random free
 * address in its stack region. Usually that is not inside the image's 18 MB.
 * One run it was, at 0x409ad000, and since the thread lives as long as the
 * game, waiting for the space to come back could never work. The earlier
 * 0x400af000-0x400d0000 conflict was the same size, and very likely the same
 * thing.
 *
 * Held for the whole run: libnx does not mind reservations that overlap, so
 * the image's own reservation simply lands on top of this one. */
#define IMAGE_HOLD_SIZE (32u * 1024 * 1024)

void host_memory_hold_image_range(void)
{
	VirtmemReservation *hold;

	virtmemLock();
	hold = virtmemAddReservation((void *)(uintptr_t)HALO_GUEST_IMAGE_BASE, IMAGE_HOLD_SIZE);
	virtmemUnlock();
	if (!hold)
		host_logf(HOST_LOG_ERROR, "libnx would not hold the guest image's range at %08x; a thread's stack may "
			"land in it", HALO_GUEST_IMAGE_BASE);
}

/* Where the kernel put this process's regions, said once at start.

The guest's memory has to be below 4 GB, and the port maps it there with
svcMapMemory, which the kernel allows only inside some of these regions. On
Horizon 21.2 that worked from 0x10000000; on 22.5 the same build was refused
there with 0xdc01 (an address outside the region allowed) at every size, with
nothing mapped in the way. Which region moved, and to where, is what decides
the fix, and the port had never said. */
void host_memory_log_regions(void)
{
	static const struct
	{
		const char *name;
		u32 address, size;
	} regions[] = {
		{ "address space", InfoType_AslrRegionAddress, InfoType_AslrRegionSize },
		{ "heap", InfoType_HeapRegionAddress, InfoType_HeapRegionSize },
		{ "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
		{ "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
	};
	unsigned index;

	for (index = 0; index < sizeof(regions) / sizeof(regions[0]); index++)
	{
		u64 address = 0, size = 0;

		if (R_FAILED(svcGetInfo(&address, regions[index].address, CUR_PROCESS_HANDLE, 0)) ||
			R_FAILED(svcGetInfo(&size, regions[index].size, CUR_PROCESS_HANDLE, 0)))
		{
			host_logf(HOST_LOG_INFO, "memory: the %s region is not reported", regions[index].name);
			continue;
		}
		host_logf(HOST_LOG_INFO, "memory: %-13s region %010llx-%010llx (%llu MB)", regions[index].name,
			(unsigned long long)address, (unsigned long long)(address + size),
			(unsigned long long)(size / (1024 * 1024)));
	}
}

static int reserve(uint64_t address, uint64_t size, int for_a_purpose)
{
	/* taken by anything already mapped, or by another reservation */
	if (range_claimed(address, size) || pool_mapped(address, size))
	{
		errno = EEXIST;
		return -1;
	}
	if (reservation_count >= RESERVATION_LIMIT)
	{
		errno = ENOMEM;
		return -1;
	}
	reservations[reservation_count].base = address;
	reservations[reservation_count].size = size;
	reservations[reservation_count].for_a_purpose = for_a_purpose;
	reservation_count++;
	/* Told to libnx as well. A reservation here is only this table: the
	 * range stays unmapped in the kernel until something is put there, and
	 * libnx places thread stacks and its own mappings at random free
	 * addresses. One run it put something at 0x269f1000, inside a window
	 * reserved at 0x20000000, the guest's 128 MB mapping of the window
	 * failed with 0xd401, and the game had no memory to start with. */
	virtmemLock();
	if (!virtmemAddReservation((void *)(uintptr_t)address, (size_t)size))
		host_logf(HOST_LOG_ERROR, "libnx would not reserve %p (%llu bytes); it may place its own mappings there",
			(void *)(uintptr_t)address, (unsigned long long)size);
	virtmemUnlock();
	return 0;
}

/* The lowest gap of at least size bytes at or above minimum, starting on an
alignment boundary; 0 if none. The window is placed on one because the game
turns a window offset into an address by masking, which only holds while the
window is a whole number of its own size from the bottom.

This used to read /proc/self/maps, and on the Switch it returns nothing at
all, because there is no /proc. That is why the first run of the game on a
console failed to start its game thread with EAGAIN: this found no gap, so
no pool below 4 GB could be created, so host_low_map returned NULL, so the
thread's stack could not be allocated.

It was not a surprise. port/switch/host/host_probe.c says in as many words
that there is no /proc/self/maps here, and says why the memory probe cannot
be built for this reason. The two just were not connected when they should
have been.

The console answers the same question directly. svcQueryMemory reports the
extent containing an address, and where nothing has been mapped that is the
whole run of free space, so walking upwards from a starting address gives
the gaps without needing a file to be open. That matters here, because
this runs before anything else in the port has.
*/
static uint64_t find_gap(uint64_t size, uint64_t minimum, uint64_t alignment)
{
	uint64_t address;

	if (alignment > 1)
		minimum = round_up(minimum) & ~(alignment - 1);
	address = minimum < LOW_START ? LOW_START : minimum;
	while (address + size <= LOW_LIMIT)
	{
		MemoryInfo information = { 0 };
		u32 page_info = 0;

		if (R_FAILED(svcQueryMemory(&information, &page_info, address)))
			return 0;
		if (information.type == MemType_Unmapped)
		{
			/* free from wherever we asked, if that lies inside the extent.
			 * A range this port has reserved is free as far as the kernel
			 * knows, because a reservation is a note to ourselves rather
			 * than a mapping, so it has to be excluded here. */
			uint64_t start = information.addr > address ? information.addr : address;

			if (alignment > 1)
				start = (start + alignment - 1) & ~(u64)(alignment - 1);
			while (start + size <= information.addr + information.size)
			{
				uint64_t move;

				if (alignment > 1 && (start & ((u64)alignment - 1)))
				{
					start = (start + alignment - 1) & ~(u64)(alignment - 1);
					continue;
				}
				if (!range_claimed(start, size))
					return start;
				/* step past whatever is already claimed at this point */
				move = start;
				for (int index = 0; index < reservation_count; index++)
				{
					if (reservations[index].base <= move &&
						move < reservations[index].base + reservations[index].size)
						move = reservations[index].base + reservations[index].size;
				}
				if (move <= start)
					break;
				start = move;
			}
		}
		/* occupied, or free but too small: step past the whole extent */
		if (information.addr + information.size <= address)
			return 0;
		address = round_up(information.addr + information.size);
	}
	return 0;
}

/* reports whatever already occupies part of a range, so a refusal to use it
says what was in the way rather than only that something was. It asked the
console rather than a file, for the same reason find_gap does: there is no
/proc here. */
static void log_conflicts(uint64_t address, uint64_t size)
{
	uint64_t where = address;

	while (where < address + size && where < LOW_LIMIT)
	{
		MemoryInfo information = { 0 };
		u32 page_info = 0;

		if (R_FAILED(svcQueryMemory(&information, &page_info, where)))
			return;
		if (information.type != MemType_Unmapped && information.addr < address + size &&
			information.addr + information.size > address)
			host_logf(HOST_LOG_ERROR, "  in the way: %016llx-%016llx type %02x perm %x attr %08x",
				(unsigned long long)information.addr,
				(unsigned long long)(information.addr + information.size), information.type,
				information.perm, information.attr);
		if (information.addr + information.size <= where)
			return;
		where = round_up(information.addr + information.size);
	}
}

/* claims a fixed range for the guest, or reports the address is taken */
static struct pool *pool_new(void)
{
	/* above the image first: ART allocates its own low-4 GB memory from
	the bottom up */
	uint64_t minimum = image_end ? image_end : LOW_START;
	struct pool *pool;
	int attempt;

	if (pool_count == MAXIMUM_POOLS)
		return NULL;
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
		if (reserve(address, POOL_SIZE, 0) == 0)
		{
			pool = calloc(1, sizeof(*pool));
			pool->base = address;
			pool->free_pages = POOL_PAGES;
			pools[pool_count++] = pool;
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

/* Where the guest image was mapped. host_mman.c's code-memory check only
 * covers the executable part of it, which is not where the guest was seen
 * faulting, so the image check in host_main.c needs the whole range. */
uint32_t host_memory_image_base(void)
{
	return (uint32_t)image_base;
}

int host_memory_initialize(uint32_t base, uint32_t size)
{
	uint64_t minimum = LOW_START;
	int attempt;

	/* the image first: it is at a fixed address, and the search below reads
	the mappings, so it has to be claimed before the window is placed or the
	search hands back the image's own address */
	image_base = base;
	image_end = base + round_up(size);
	/* Claiming the image's fixed address can lose a race with the console's
	 * own low-memory manager, which places mappings below 4 GB from the
	 * bottom up and does not know this program wants 0x40000000. Where the
	 * port's own pool lands changes that pressure, so the same build has
	 * loaded the image on one run and found 0x400af000-0x400d0000 sitting
	 * in the middle of it on the next.
	 *
	 * Nothing here can release that mapping - it was not made here, and the
	 * kernel will not unmap memory without the buffer it was given - so the
	 * answer is to ask again. Those mappings come and go as the manager
	 * reuses its space, so the reservation is retried for a few seconds
	 * before it is called a failure. Each attempt says what was in the way,
	 * so a conflict that is permanent looks different from one that clears. */
	for (attempt = 0; attempt < 20; attempt++)
	{
		if (reserve(image_base, image_end - image_base, 0) == 0)
			break;
		if (attempt == 0)
			host_logf(HOST_LOG_WARN,
				"the guest image's address is in use; waiting for the space to come back");
		else
			host_logf(HOST_LOG_INFO, "  still in use, waiting (attempt %d)", attempt + 1);
		log_conflicts(image_base, image_end - image_base);
		usleep(250000);
	}
	if (attempt == 20)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the guest image range at %08llx (%s)",
			(unsigned long long)image_base, strerror(errno));
		log_conflicts(image_base, image_end - image_base);
		return -1;
	}

	/* The window, where the game and its data expect it - but only if that
	 * address will actually hold it.
	 *
	 * It will not. 0x80000000 is reported free by svcQueryMemory and
	 * refused by svcMapMemory at every size from 16 MB to 128 MB, with an
	 * error that is not "out of memory": the console simply will not map
	 * there. Checking whether anything was already mapped - which is all
	 * reserve() does - cannot tell the difference, so the port believed
	 * the address was usable, told the guest it was, and the guest then
	 * refused its own window.
	 *
	 * So the address is tried, and handed straight back, before it is
	 * promised to anything. */
	if (host_can_map_at(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE))
	{
		if (reserve(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, 1) == 0)
			window_base = HALO_GUEST_WINDOW_BASE;
	}
	else
		host_logf(HOST_LOG_INFO,
			"the window's usual address %08llx will not take %u MB; looking elsewhere",
			(unsigned long long)HALO_GUEST_WINDOW_BASE,
			(unsigned int)(HALO_GUEST_WINDOW_SIZE / (1024 * 1024)));
	if (!window_base)
	{
		/* the runtime holds the address. The game can still run with the
		window elsewhere, as long as the map data is moved with it, which
		the port does and is told about here. */
		host_logf(HOST_LOG_INFO, "the window at %08llx is taken; putting it somewhere free",
			(unsigned long long)HALO_GUEST_WINDOW_BASE);
		/* Before searching for a gap, the address this build prefers is
		 * tried. The search takes the first free extent the kernel
		 * reports, which differs from run to run - the launcher's own
		 * mappings, anything ASLR'd below 4 GB, and how much of the low
		 * pool the port has already taken all move it - and the port's
		 * behaviour is sensitive to where things land. One fixed address
		 * is one less variable. */
		if (host_can_map_at(HALO_SWITCH_WINDOW_FALLBACK, HALO_GUEST_WINDOW_SIZE))
		{
			if (reserve(HALO_SWITCH_WINDOW_FALLBACK, HALO_GUEST_WINDOW_SIZE, 1) == 0)
			{
				window_base = HALO_SWITCH_WINDOW_FALLBACK;
				host_logf(HOST_LOG_INFO, "  the window is pinned to %08llx",
					(unsigned long long)window_base);
			}
		}
		else
			host_logf(HOST_LOG_WARN, "  %08llx will not take the window; searching",
				(unsigned long long)HALO_SWITCH_WINDOW_FALLBACK);
	}
	if (!window_base)
	{
		window_base = 0;
		for (attempt = 0; attempt < 128 && !window_base; attempt++)
		{
			uint64_t candidate = find_gap(HALO_GUEST_WINDOW_SIZE, minimum,
				HALO_GUEST_WINDOW_ALIGNMENT);

			if (!candidate)
				break;
			/* a mapping in the way here is one ART has just made, and may
			be live, so it is never taken back: look further up */
			if (reserve(candidate, HALO_GUEST_WINDOW_SIZE, 1) != 0)
			{
				minimum = candidate + PAGE;
				continue;
			}
			/* and it has to be somewhere the console will really map,
			for the same reason the usual address was refused */
			if (!host_can_map_at(candidate, HALO_GUEST_WINDOW_SIZE))
			{
				host_logf(HOST_LOG_INFO, "  %08llx will not take the window either",
					(unsigned long long)candidate);
				minimum = candidate + PAGE;
				continue;
			}
			window_base = candidate;
		}
		if (!window_base)
		{
			host_logf(HOST_LOG_ERROR, "no free range of %u MB for the Xbox memory window",
				(unsigned int)(HALO_GUEST_WINDOW_SIZE / (1024 * 1024)));
			return -1;
		}
	}
	window_end = window_base + HALO_GUEST_WINDOW_SIZE;
	/* Where the window moved, the range it was linked for is kept empty.
	 * The guest moves any value it reads out of the game data that lies
	 * there (halo_port_window.h), so memory of its own at those addresses
	 * - a later pool, which is searched for from the image upwards - would
	 * have its pointers moved out from under it. Nothing has to be mapped
	 * for that: the reservation alone keeps the searches away. */
	if (window_base != HALO_GUEST_WINDOW_BASE &&
		reserve(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, 0) != 0)
		host_logf(HOST_LOG_WARN, "could not keep the window's linked range %08llx free",
			(unsigned long long)HALO_GUEST_WINDOW_BASE);
	host_logf(HOST_LOG_INFO, "Xbox memory window at %08llx-%08llx, guest image at %08llx-%08llx",
		(unsigned long long)window_base, (unsigned long long)window_end,
		(unsigned long long)image_base, (unsigned long long)image_end);
	return 0;
}

static struct pool *pool_of(uint64_t address, uint64_t size);

/* ---------- committing the guest's memory in chunks

Where svcMapMemory will not map below 4 GB - Horizon 22.5 put the only region
it maps in at 0x7e73200000 (host_mman.c) - the guest's memory is heap aliased
there with svcMapProcessCodeMemory instead, and that changes how it is handed
out. Each such mapping is a call into the kernel to make and another to undo,
and one cannot be partly undone, while the guest maps and unmaps memory all
the time - its allocator, thread stacks, file reads - and frees parts of what
it mapped. A mapping per request would be thousands of kernel calls, each
partial unmap a copy.

So a pool's address space and the window are made real sixteen megabytes at a
time, the first time any of a chunk is wanted, and stay so. Inside them a
mapping is bookkeeping: its pages are cleared, or a file's bytes read in, and
unmapping them only marks them free. What that costs is that memory once used
is not given back until the game exits, which the game, whose memory grows to
a working size and stays there, barely notices; and mprotect inside a chunk
does nothing, which is no more than the console allows for svcMapMemory's
mappings either.

Where svcMapMemory does map below 4 GB, none of this is used. */
static int chunked = -1;
static pthread_mutex_t commit_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t window_committed[WINDOW_CHUNKS];

static int memory_is_chunked(void)
{
	if (chunked < 0)
		chunked = host_mman_low_code_mode();
	return chunked;
}

/* Where svcMapMemory does map the window, it is chunked only if the deko3d
renderer runs: its draws read the window through the GPU (host_dk.c), and
what the GPU has mapped cannot be unmapped (DEKO3D.md), so the window is
committed in chunks there as well and stays real. */
static int window_is_chunked(void)
{
	return memory_is_chunked() || host_renderer_deko3d;
}

/* whether the range - the window or a pool - is one of the chunked arenas */
static int range_is_chunked(uint64_t address, uint64_t length)
{
	if (in_range(address, length, window_base, window_end))
		return window_is_chunked();
	return memory_is_chunked();
}

/* makes [start, start + length) of the arena at base real, a chunk at a
time; each chunk of the window goes to host_dk.c as it lands (the deko3d
renderer GPU-maps it), pools' do not */
static int commit(uint64_t base, uint8_t *committed, uint64_t chunks, uint64_t start, uint64_t length, int window)
{
	uint64_t chunk = (start - base) / CHUNK_SIZE;
	uint64_t last = (start + length - 1 - base) / CHUNK_SIZE;
	int result = 0;

	pthread_mutex_lock(&commit_lock);
	for (; chunk <= last && chunk < chunks; chunk++)
	{
		void *address = (void *)(uintptr_t)(base + chunk * CHUNK_SIZE);

		if (committed[chunk])
			continue;
		if (mmap(address, CHUNK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
			address)
		{
			host_logf(HOST_LOG_ERROR, "guest memory: the %llu MB at %p could not be made real",
				(unsigned long long)(CHUNK_SIZE / (1024 * 1024)), address);
			result = -1;
			break;
		}
		committed[chunk] = 1;
		if (window)
			host_dk_window_chunk_committed(base + chunk * CHUNK_SIZE, CHUNK_SIZE);
	}
	pthread_mutex_unlock(&commit_lock);
	return result;
}

/* commits whichever arena holds the range: the window or a pool (the image is
the loader's, and mapped whole) */
static int commit_owned(uint64_t address, uint64_t length)
{
	struct pool *pool;

	if (in_range(address, length, window_base, window_end))
		return commit(window_base, window_committed, WINDOW_CHUNKS, address, length, 1);
	pthread_mutex_lock(&memory_lock);
	pool = pool_of(address, length);
	pthread_mutex_unlock(&memory_lock);
	return pool ? commit(pool->base, pool->committed, POOL_CHUNKS, address, length, 0) : 0;
}

/* a mapping's contents, in committed memory: zeros, and a file's bytes from
offset if it maps one - read as host_mman.c reads one, into a scratch buffer
under the card's lock */
static int fill(uint64_t address, uint64_t length, int fd, int64_t offset)
{
	static char scratch[16384];
	unsigned char *bytes = (unsigned char *)(uintptr_t)address;
	uint64_t done = 0;

	memset(bytes, 0, length);
	if (fd < 0)
		return 0;
	host_sd_lock();
	if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset)
	{
		host_sd_unlock();
		return -EINVAL;
	}
	while (done < length)
	{
		long got = read(fd, scratch, (unsigned)(length - done < sizeof(scratch) ? length - done : sizeof(scratch)));

		if (got <= 0)
			break;
		memcpy(bytes + done, scratch, (size_t)got);
		done += (uint64_t)got;
	}
	host_sd_unlock();
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
	if (memory_is_chunked())
	{
		/* made real a chunk at a time and handed out cleared (commit) */
		if (commit_owned((uint64_t)(uintptr_t)address, pages * PAGE) != 0)
		{
			host_low_unmap(address, pages * PAGE);
			return NULL;
		}
		memset(address, 0, pages * PAGE);
		return address;
	}
	if (mmap(address, pages * PAGE, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address)
	{
		host_low_unmap(address, pages * PAGE);
		return NULL;
	}
	return address;
}

static struct pool *pool_of(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < pool_count; index++)
	{
		if (in_range(address, size, pools[index]->base, pools[index]->base + POOL_SIZE))
			return pools[index];
	}
	return NULL;
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)address + size) - start;
	struct pool *pool;

	pthread_mutex_lock(&memory_lock);
	pool = pool_of(start, length);
	if (pool)
	{
		uint64_t first = (start - pool->base) / PAGE, count = length / PAGE, page;

		/* give the memory back; the pool's reservation keeps the address
		space (find_free skips it), so nothing has to stay mapped there.
		It used to be mapped over PROT_NONE instead, which here is a real
		mapping - a zeroed buffer and a record in host_mman.c's table - that
		every later allocation inside it had to split, until the table was
		full and the menus' map could not be loaded after a network game.
		Committed memory stays as it is, and is cleared when handed out. */
		if (!memory_is_chunked())
			munmap((void *)start, length);
		for (page = first; page < first + count; page++)
		{
			if (pool->used[page])
			{
				pool->used[page] = 0;
				pool->free_pages++;
			}
		}
	}
	pthread_mutex_unlock(&memory_lock);
}

int host_low_owns(uintptr_t address, size_t size)
{
	int result;

	if (in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end))
		return 1;
	pthread_mutex_lock(&memory_lock);
	result = pool_of(address, size) != NULL;
	pthread_mutex_unlock(&memory_lock);
	return result;
}

/* ---------- the guest's memory system calls */

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	uint64_t host = to_host(address);
	void *result;

	if (!length)
		return -EINVAL;
	/* the descriptor, if there is one, is the guest's own number for it */
	if (fd >= 0)
	{
		fd = host_guest_fd(fd);
		if (fd < 0)
			return -EBADF;
	}
	if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))
	{
		int fixed_flags = (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED;

		if (host + length > LOW_LIMIT)
			return -ENOMEM;
		/* inside a range the host reserved for the guest, a "no replace"
		request replaces the reservation */
		if (!host_low_owns(host, length))
		{
			if (!(flags & MAP_FIXED_NOREPLACE))
				return -EINVAL;
			fixed_flags = flags;
		}
		else if (range_is_chunked(host, length))
		{
			/* inside the guest's own arenas: their memory, made real */
			int error = commit_owned(host, length) != 0 ? -ENOMEM : fill(host, length, fd, offset);

			return error ? error : (long)to_guest(host);
		}
		result = mmap((void *)host, length, protection, fixed_flags, fd, offset);
		if (result == MAP_FAILED)
			return -errno;
		return (long)to_guest((uintptr_t)result);
	}
	result = host_low_map(length, PROT_NONE);
	if (!result)
		return -ENOMEM;
	if (memory_is_chunked())
	{
		/* already cleared and real (host_low_map); a file's bytes, if any */
		int error = fd >= 0 ? fill((uint64_t)(uintptr_t)result, length, fd, offset) : 0;

		if (error)
		{
			host_low_unmap(result, length);
			return error;
		}
		return (long)(uintptr_t)result;
	}
	if (mmap(result, length, protection, flags | MAP_FIXED, fd, offset) != result)
	{
		int error = errno;

		host_low_unmap(result, length);
		return -error;
	}
	return (long)(uintptr_t)result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size);
	uint64_t host = to_host(address);

	if (host + length > LOW_LIMIT)
		return -EINVAL;
	if (in_range(host, length, window_base, window_end))
	{
		/* committed memory stays as it is (commit); otherwise it is given
		back, the window's reservation keeping its addresses (host_low_unmap) */
		if (!window_is_chunked())
			munmap((void *)host, length);
		return 0;
	}
	if (in_range(host, length, image_base, image_end))
		return -EINVAL;
	if (host_low_owns(host, length))
	{
		host_low_unmap((void *)host, length);
		return 0;
	}
	return munmap((void *)host, length) ? -errno : 0;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t host = to_host(address);

	if (host + size > LOW_LIMIT)
		return -EINVAL;
	/* committed memory is read-write and stays so (commit) */
	if (range_is_chunked(host, size) && host_low_owns(host, size))
		return 0;
	return mprotect((void *)host, size, protection) ? -errno : 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;

/* The guest reaches this with addresses that are already in the window
wherever it was placed (platform_contiguous_base), so the tracking follows
the window the host actually reserved, not the one the game was built for. */
static int in_window(uint64_t address)
{
	return address >= window_base && address - window_base < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - window_base) / PAGE;
}

/* The console will not change the protection of guest memory (host_mman.c),
so nothing here faults: a page is "protected" while a texture made from it
is cached, and it is the guest that says when it writes one (texture locks,
file reads - port/linux/src/d3d8_resources.c, xbox_files.c). */
static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
}

/* ---------- crash reporting

The Android host installs SIGSEGV, SIGBUS and SIGILL handlers that log the
faulting address, the program counter, the frame pointer chain and the guest
image's address range, because on a phone that is the only way to find out
what went wrong. None of it is available here: libnx exposes no syscall for
installing a signal handler, and there is no equivalent, so a fault on the
Switch takes the process down and Atmosphere writes the report.

What replaces it is a note in the log of where the guest image was mapped,
written at the moment the loader finishes, so that an address range from a
crash dump can still be matched against the image offline. */
void host_install_signal_handlers(void)
{
	/* nothing to install: see above */
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 1;
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

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
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= window_base || start >= window_base + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < window_base)
		start = window_base;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

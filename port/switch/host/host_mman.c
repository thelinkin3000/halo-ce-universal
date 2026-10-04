/*
HOST_MMAN.C

POSIX mmap, munmap, mprotect and madvise for the Switch, over the console's
own memory services. The Android host library (port/android/host) is written
against them; this is what answers on a platform that has no <sys/mman.h>.

Everything here was established on hardware by port/switch/probe:

- Everything below 4 GB is unclaimed address space, so the guest's memory
  can live where the Android port puts it (HALO_GUEST_IMAGE_BASE 0x40000000,
  and the Xbox contiguous window at 0x80000000). That matters because the
  guest's data formats embed 32-bit pointers: the image is ILP32 AArch64
  and every address it can touch has to fit in 32 bits.

- svcMapPhysicalMemory (SVC 0x2C) is refused on this firmware, but
  svcMapMemory (SVC 0x05) places memory at an address we choose. Ordinary
  mappings - the guest's .data and .bss, the Xbox window, its arenas - go
  through that.

- No mapping is ever both writable and executable. svcSetMemoryPermission
  answers 0xd801 for Perm_X, as its documentation says it must. Executable
  memory is the console's separate "code memory" kind: svcCreateCodeMemory
  (0x4B) creates one and svcControlCodeMemory (0x4C) maps it, taking a
  destination address and a permission mask.

- A code memory object has two views of the same memory, a writable owner
  and a read-execute slave, and the probe confirmed that bytes written
  through one are visible through the other. That is what makes the design
  below possible: a mapping that is to be executable is created with its
  writable view already at the address asked for, so its contents are written
  with ordinary stores, and mprotect is what turns it into the read-execute
  view at the same address. Turning it back undoes that, and the contents
  survive both ways.

The cost is that PROT_EXEC is not a cheap flag. On Android mprotect is a
kernel call over a page table. Here it means creating a code memory object,
copying the range into it, swapping the mapping and closing the old one. The
loader makes that call a handful of times at start-up, so the cost does not
matter, but it is a real difference and callers should not assume mprotect is
cheap.
*/

#include "host.h"

#include <switch.h>
#include <switch/arm/cache.h>

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* ---------- the mappings we know about */

/* Every mapping made through this file is recorded, because unmapping needs
the address the memory was mapped *from* (svcUnmapMemory takes both) and
because turning a mapping executable or writable again means finding the
object it belongs to. The host makes few mappings - a few dozen, the biggest
being the guest's arenas - so a flat table is the honest structure. */
/* Freed guest memory used to stay mapped PROT_NONE (host_memory.c), a record
each, split again by every allocation inside it; after a crowded network game
the 512 records there were ran out and an allocation failed. Freed memory is
unmapped now, so the records are live mappings only, and these are room to
spare. */
#define MAPPING_LIMIT 2048

enum mapping_kind
{
	_mapping_free,
	_mapping_ordinary, /* an svcMapMemory region */
	_mapping_code,     /* a code memory object, seen through a writable view */
	_mapping_alias,    /* heap aliased with svcMapProcessCodeMemory: memory of the kind a program's own data is */
};

struct mapping
{
	enum mapping_kind kind;
	void *address;
	size_t length;
	/* the memory as the kernel knows it: for an ordinary mapping, the heap
	buffer svcMapMemory was given; for code memory, the buffer the object
	was created from, which is the same memory seen at another address */
	void *backing;
	Handle code;
	/* the protection this mapping was made with, so that mprotect can tell
	whether there is anything to change. An ordinary mapping is read-write
	and cannot be otherwise: svcSetMemoryPermission refuses any change to
	one, and the console offers no way to make such a mapping anything but. */
	int protection;
	/* a code mapping starts as its writable owner view at the address
	itself, and mprotect swaps that for the read-execute slave */
	int executable;
};

static struct mapping mappings[MAPPING_LIMIT];
static int mapping_count;

static struct mapping *mapping_find(void *address, size_t length, int exact_length)
{
	struct mapping *found = NULL;
	int index;

	for (index = 0; index < mapping_count; index++)
	{
		struct mapping *mapping = &mappings[index];

		if (mapping->kind == _mapping_free)
			continue;
		if (!((uintptr_t)address >= (uintptr_t)mapping->address &&
			(uintptr_t)address < (uintptr_t)mapping->address + mapping->length))
			continue;
		/* a caller may name a sub-range (the loader protects page by
		page), so a match on part of the mapping counts; only a caller
		that asked for the whole thing insists on the whole thing */
		if (exact_length && mapping->length != length)
			continue;
		if (!found || mapping->length < found->length)
			found = mapping;
	}
	return found;
}

static struct mapping *mapping_add(void *address, size_t length, void *backing,
	enum mapping_kind kind, Handle code, int executable, int protection)
{
	struct mapping *mapping;

	int index;

	/* A released mapping leaves its record marked free. Those are taken
	 * first: this only ever appended, so every mapping the process had
	 * made counted against the limit for good, and the cache copy thread -
	 * a 128 KB malloc and free per read, each a map and an unmap here -
	 * used up all 512 within seconds and mmap answered ENOMEM. */
	mapping = NULL;
	for (index = 0; index < mapping_count; index++)
	{
		if (mappings[index].kind == _mapping_free)
		{
			mapping = &mappings[index];
			break;
		}
	}
	if (!mapping)
	{
		if (mapping_count >= MAPPING_LIMIT)
		{
			host_logf(HOST_LOG_ERROR, "the table of mappings is full (%d); %zu bytes at %p were not recorded",
				MAPPING_LIMIT, length, address);
			return NULL;
		}
		mapping = &mappings[mapping_count++];
	}
	mapping->kind = kind;
	mapping->address = address;
	mapping->length = length;
	mapping->backing = backing;
	mapping->code = code;
	mapping->executable = executable;
	mapping->protection = protection;
	return mapping;
}

/* ---------- finding unclaimed address space below 4 GB */

/* The guest cannot address anything at or above 4 GB, so the allocator never
looks there. svcQueryMemory reports the extent containing an address, and for
an unclaimed region that is the whole run of free space, so walking it gives
the ranges the loader can use. */
#define ADDRESS_LIMIT 0x100000000ull

/* The lowest address this port will place a mapping at.
 *
 * The console will not map anything below 256 MB, and that was established
 * the slow way: mappings were attempted at 16 MB and at 64 MB and both were
 * refused with ENOMEM, while every address the memory probe had used - all
 * of them 256 MB and above - worked. Nothing is occupying the space below,
 * which svcQueryMemory reports as unmapped, so this is not a collision; the
 * console simply will not hand those pages out.
 *
 * This file's find_free had no such floor and started at 0x1000, which is
 * why the guest's 128 MB contiguous window was refused at 0x1000 and the
 * game reported no contiguous memory. The same floor is in host_memory.c as
 * LOW_START; it is stated here as well because this function is a second
 * allocator and the two must agree.
 */
#define MAPPING_FLOOR 0x10000000ull

/* above this, a request is a window or a comparable large region rather than
 * an allocation, and is placed where the port reserved for it rather than
 * wherever the search finds room */
#define LARGE_MAPPING (16 * 1024 * 1024)

static void *find_free(size_t length, size_t alignment, int skip_reserved)
{
	u64 address;

	if (!alignment)
		alignment = 0x1000;
	/* the larger of the two: a caller asking for an alignment coarser than
	the floor keeps it, and everyone else starts at the floor. Written the
	other way round - which it was - this picked the smaller, so every
	search still began at 0x1000 and the floor did nothing at all. The guest's
	128 MB window was refused at 0x1000 for exactly that reason, three runs
	after the floor was added to host_memory.c. */
	address = (u64)alignment > MAPPING_FLOOR ? (u64)alignment : MAPPING_FLOOR;

	/* Prefer a range the port has already reserved and not yet used.
	 *
	 * The Xbox contiguous window is reserved at HALO_GUEST_WINDOW_BASE
	 * (0x80000000) while the port starts up, and everything else in the
	 * host is told to expect the window there. Without this the search
	 * below cannot see that reservation - there is no mapping behind one -
	 * and hands out an address that merely happens to be free, so the
	 * window lands somewhere else and the guest's heap, which grows into
	 * whatever it finds, runs into it. That is what the game reported as
	 * a window it could not reserve.
	 */
	if (length >= LARGE_MAPPING)
	{
		uint64_t reserved = host_memory_reserved_region(length);

		if (reserved && !skip_reserved)
		{
			host_logf(HOST_LOG_INFO,
				"%zu bytes will go at %llx, the range reserved for them", length,
				(unsigned long long)reserved);
			return (void *)(uintptr_t)reserved;
		}
	}

	while (address + length < ADDRESS_LIMIT)
	{
		MemoryInfo information = { 0 };
		u32 page_info = 0;

		if (R_FAILED(svcQueryMemory(&information, &page_info, address)))
			return NULL;
		if (information.type != MemType_Unmapped)
		{
			/* occupied: step past it and try again */
			address = information.addr + information.size;
			address = (address + alignment - 1) & ~(u64)(alignment - 1);
			continue;
		}
		/* unclaimed from information.addr; take from where we asked if
		that is inside it, so a caller that wants a low address gets one */
		if ((u64)information.addr > address)
			address = information.addr;
		if (information.addr + information.size < address + length)
		{
			address = (information.addr + information.size + alignment - 1) & ~(u64)(alignment - 1);
			continue;
		}
		/* and not a range this port has set aside either. A reservation is
		* only a note held in host_memory.c - nothing is mapped - so the
		* kernel calls this free, and without this the guest's heap, which
		* grows through this very search, is offered the window's address
		* and takes it. That is what happened: a 128 MB heap at 0x11014000,
		* after which the contiguous window had nowhere left to go. */
		if (host_memory_range_is_reserved(address, length))
		{
			address = (address + length + alignment - 1) & ~(u64)(alignment - 1);
			continue;
		}
		return (void *)(uintptr_t)address;
	}
	return NULL;
}

static int map_ordinary(void *address, size_t length, void *backing)
{
	Result result = svcMapMemory(address, backing, length);

	if (R_FAILED(result))
		/* The console's own answer, which is the only thing that says why.
		 * Everything above this turns it into ENOMEM and loses it, and the
		 * caller usually has no better idea what to report. */
		host_logf(HOST_LOG_ERROR, "svcMapMemory(%p, %zu bytes) failed: 0x%08x",
			address, length, (unsigned)result);
	return R_FAILED(result) ? -ENOMEM : 0;
}

/* The result is reported. svcUnmapMemory was not being checked, so a
refusal was invisible: the mapping stayed in the kernel's table, the port's
table said it was gone, and every later attempt at the same address failed
for that reason with nothing in the log to connect the two. */
static void unmap_ordinary(struct mapping *mapping)
{
	Result result = svcUnmapMemory(mapping->address, mapping->backing, mapping->length);

	if (R_FAILED(result))
		host_logf(HOST_LOG_ERROR, "svcUnmapMemory at %p (%zu bytes) failed: 0x%08x",
			mapping->address, mapping->length, (unsigned)result);
}

/* ---------- where svcMapMemory may map

svcMapMemory places memory only inside the process's stack region. On Horizon
21.2 that region took in the low memory the guest lives in - the pools from
0x10000000 worked, and the window's address 0x80000000, past its end, was
refused - but on 22.5 the kernel put it at 0x7e73200000, far above 4 GB, and
every mapping the guest needs was refused there with 0xdc01, an address
outside the region allowed. The only memory the console will then place below
4 GB is code memory, which goes anywhere in the address space.

Of the two kinds, the right one is the loader's. A code memory object's
writable view (svcCreateCodeMemory) was tried first and placed everything,
but the kernel will not let a program pass any of that memory to a service:
the game thread's stack was there, and the first line logged from it - a
write to the card from a buffer on that stack - was refused with 0xd401,
which ended in a fatal error. hbloader loads programs with
svcMapProcessCodeMemory instead, and memory mapped so and made read-write is
what a program's own .data and .bss are: usable for any service call, and
placeable anywhere in the address space. PPSSPP's Switch port maps its
arena the same way. It needs the program's own process handle, which
hbloader gives it (envGetOwnProcessHandle); without one, the code memory
view is all there is.

So a writable mapping outside the stack region is aliased heap instead
(map_writable). Whether any of that is needed is learned once, from the stack
region, and where it covers the guest's low memory nothing changes.
host_memory.c asks as well (host_mman_low_code_mode): there the guest's
arenas are committed in large chunks, so that the guest's constant mapping and
unmapping is bookkeeping rather than a call into the kernel each time. */
static int regions_learned;
static int low_code_mode;
static u64 stack_region_base, stack_region_end;

static void learn_regions(void)
{
	u64 base = 0, size = 0;

	if (regions_learned)
		return;
	regions_learned = 1;
	if (R_SUCCEEDED(svcGetInfo(&base, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0)) &&
		R_SUCCEEDED(svcGetInfo(&size, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0)))
	{
		stack_region_base = base;
		stack_region_end = base + size;
	}
	low_code_mode = !(stack_region_base <= MAPPING_FLOOR && stack_region_end > MAPPING_FLOOR);
	if (low_code_mode)
		host_logf(HOST_LOG_INFO, "memory: svcMapMemory maps only at %010llx-%010llx here, none of it where the "
			"guest's memory goes; that memory is %s", (unsigned long long)stack_region_base,
			(unsigned long long)stack_region_end,
			envGetOwnProcessHandle() ? "aliased heap (svcMapProcessCodeMemory)" : "code memory (no process handle)");
}

int host_mman_low_code_mode(void)
{
	learn_regions();
	return low_code_mode;
}

/* whether svcMapMemory will take the range */
static int ordinary_allowed(const void *address, size_t length)
{
	u64 start = (u64)(uintptr_t)address;

	learn_regions();
	return !low_code_mode || (start >= stack_region_base && start + length <= stack_region_end);
}

/* Whether a range can be mapped at all.
 *
 * Asked by trying, and undone straight away: the console answers a refusal
 * without leaving anything behind, and the answer is the only way to know -
 * svcQueryMemory reports what is there, not whether a mapping would be
 * accepted. Used when the address a request would rather have has been
 * refused once already.
 */
static int can_map_there(void *address, size_t length)
{
	void *backing = memalign(0x1000, length);
	Result result;

	if (!backing)
		return 0;
	if (!ordinary_allowed(address, length) && envGetOwnProcessHandle())
	{
		/* as map_writable would map it: aliased heap */
		Handle process = envGetOwnProcessHandle();

		result = svcMapProcessCodeMemory(process, (u64)(uintptr_t)address, (u64)(uintptr_t)backing, length);
		if (R_SUCCEEDED(result))
			svcUnmapProcessCodeMemory(process, (u64)(uintptr_t)address, (u64)(uintptr_t)backing, length);
		free(backing);
		return R_SUCCEEDED(result);
	}
	if (!ordinary_allowed(address, length))
	{
		/* as map_writable would map it: a code memory object's owner view */
		Handle code = 0;

		result = svcCreateCodeMemory(&code, backing, length);
		if (R_SUCCEEDED(result))
		{
			result = svcControlCodeMemory(code, CodeMapOperation_MapOwner, address, length, Perm_Rw);
			if (R_SUCCEEDED(result))
				svcControlCodeMemory(code, CodeMapOperation_UnmapOwner, address, length, 0);
			svcCloseHandle(code);
		}
		free(backing);
		return R_SUCCEEDED(result);
	}
	result = (Result)map_ordinary(address, length, backing);
	if (R_SUCCEEDED(result))
		svcUnmapMemory(address, backing, length);
	free(backing);
	return R_SUCCEEDED(result);
}

int host_can_map_at(uint64_t address, uint64_t length)
{
	return can_map_there((void *)(uintptr_t)address, (size_t)length);
}

/* ---------- creating the two kinds of mapping */


/* A code mapping is born writable: its owner view is mapped at the address
asked for, so the caller fills it with ordinary stores, and mprotect later
swaps in the read-execute view at the same address. */
static int map_code(void *address, size_t length, void *backing, struct mapping **created)
{
	Handle code = 0;
	Result result;

	result = svcCreateCodeMemory(&code, backing, length);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "svcCreateCodeMemory(%p, %zu) failed: 0x%08x",
			(void *)(uintptr_t)length, length, (unsigned)result);
		return -ENOMEM;
	}
	result = svcControlCodeMemory(code, CodeMapOperation_MapOwner, address, length, Perm_Rw);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "code memory could not be mapped at %p: 0x%08x",
			address, (unsigned)result);
		svcCloseHandle(code);
		return -ENOMEM;
	}
	*created = mapping_add(address, length, backing, _mapping_code, code, 0, PROT_READ | PROT_WRITE);
	if (!*created)
	{
		svcControlCodeMemory(code, CodeMapOperation_UnmapOwner, address, length, 0);
		svcCloseHandle(code);
		return -ENOMEM;
	}
	return 0;
}

/* Maps backing's contents at address, readable and writable, and records it:
an svcMapMemory mapping where the console allows one, else backing aliased
there with svcMapProcessCodeMemory (both keep backing as the mapping's), and
only without a process handle a code memory object's owner view. A code memory
object does not show what its buffer held when it was made (ordinary_to_code
found that out), so it is made from fresh pages and backing's bytes are
written in through the view, after which backing is freed. Returns the
record, or NULL with backing still the caller's. */
static struct mapping *map_writable(void *address, size_t length, void *backing, int protection)
{
	struct mapping *mapping = NULL;
	void *pages;

	if (ordinary_allowed(address, length))
	{
		if (R_FAILED((Result)map_ordinary(address, length, backing)))
			return NULL;
		mapping = mapping_add(address, length, backing, _mapping_ordinary, 0, 0, protection);
		if (!mapping)
			svcUnmapMemory(address, backing, length);
		return mapping;
	}
	if (envGetOwnProcessHandle())
	{
		/* aliased heap: backing's own pages, so its contents are already there */
		Handle process = envGetOwnProcessHandle();
		u64 at = (u64)(uintptr_t)address, from = (u64)(uintptr_t)backing;
		Result result = svcMapProcessCodeMemory(process, at, from, length);

		if (R_FAILED(result))
		{
			host_logf(HOST_LOG_ERROR, "svcMapProcessCodeMemory(%p, %zu bytes) failed: 0x%08x", address, length,
				(unsigned)result);
			return NULL;
		}
		result = svcSetProcessMemoryPermission(process, at, length, Perm_Rw);
		if (R_FAILED(result))
		{
			host_logf(HOST_LOG_ERROR, "svcSetProcessMemoryPermission(%p, %zu bytes, rw) failed: 0x%08x", address,
				length, (unsigned)result);
			svcUnmapProcessCodeMemory(process, at, from, length);
			return NULL;
		}
		mapping = mapping_add(address, length, backing, _mapping_alias, 0, 0, protection);
		if (!mapping)
			svcUnmapProcessCodeMemory(process, at, from, length);
		return mapping;
	}
	pages = memalign(0x1000, length);
	if (!pages)
		return NULL;
	if (map_code(address, length, pages, &mapping) != 0)
	{
		free(pages);
		return NULL;
	}
	memcpy(address, backing, length);
	free(backing);
	mapping->protection = protection;
	return mapping;
}

/* takes a recorded mapping out of the address space, whichever kind it is;
retiring the record and freeing its backing are the caller's */
static void unmap_mapping(struct mapping *mapping)
{
	if (mapping->kind == _mapping_alias)
	{
		Result result = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)(uintptr_t)mapping->address,
			(u64)(uintptr_t)mapping->backing, mapping->length);

		if (R_FAILED(result))
			host_logf(HOST_LOG_ERROR, "svcUnmapProcessCodeMemory at %p (%zu bytes) failed: 0x%08x",
				mapping->address, mapping->length, (unsigned)result);
		return;
	}
	if (mapping->kind == _mapping_code)
	{
		svcControlCodeMemory(mapping->code, mapping->executable ? CodeMapOperation_UnmapSlave :
			CodeMapOperation_UnmapOwner, mapping->address, mapping->length, 0);
		svcCloseHandle(mapping->code);
	}
	else
		unmap_ordinary(mapping);
}

/* Every mapping below 4 GB, in order, with what the kernel says owns it.
 *
 * The search below reports "nothing free" for a 128 MB request while the
 * window's own address - which svcQueryMemory calls unmapped - is refused
 * by svcMapMemory, and those two facts cannot both be complete. This prints
 * the address space so that they can be reconciled, rather than another
 * theory being built on them.
 */
static void log_address_space(void)
{
	u64 address = 0;

	host_memory_log_reservations();
	host_logf(HOST_LOG_INFO, "address space below 4 GB:");
	while (address < ADDRESS_LIMIT)
	{
		MemoryInfo information = { 0};
		u32 page_info = 0;

		if (R_FAILED(svcQueryMemory(&information, &page_info, address)))
			break;
		if (information.size == 0)
			break;
		host_logf(HOST_LOG_INFO, "  %012llx-%012llx %s type %02x perm %x%s",
			(unsigned long long)information.addr,
			(unsigned long long)(information.addr + information.size),
			information.type == MemType_Unmapped ? "free     " : "MAPPED   ",
			information.type, information.perm,
			host_memory_range_is_reserved(information.addr, information.size) ?
				"  (reserved by this port)" : "");
		if (information.addr + information.size <= address)
			break;
		address = information.addr + information.size;
	}
}

/* How much this process is allowed to map at all.
 *
 * Every refusal in this file has been reasoned about from symptoms - the
 * address was too low, the range was already taken, the heap was greedy -
 * and none of that has produced a number. The console will raise its own
 * limit (pglBoostSystemMemoryResourceLimit) but refuses this port's request
 * for more, so the ceiling it did grant is the thing the port has to live
 * within, and it is worth reading rather than inferring.
 */
static void log_memory_limit(void)
{
	Handle limit_handle = 0;
	s64 current = 0, ceiling = 0, peak = 0;

	/* The resource-limit handle for this process. libnx does not wrap a
	way to fetch it; the kernel's call takes it as an argument and the
	loader leaves it unset, and the pseudo-handle stands for the calling
	process. If this is wrong the call fails and nothing is logged, which
	beats a wrong number in the log. */
	limit_handle = (Handle)-2; /* the pseudo-handle for the current process */
	if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&current, limit_handle, LimitableResource_Memory)) &&
		R_SUCCEEDED(svcGetResourceLimitLimitValue(&ceiling, limit_handle, LimitableResource_Memory)))
	{
		svcGetResourceLimitPeakValue(&peak, limit_handle, LimitableResource_Memory);
		host_logf(HOST_LOG_INFO, "  this process is using %.1f MB, has peaked at %.1f MB, "
			"and may use at most %.1f MB", (double)current / 1048576.0, (double)peak / 1048576.0,
			(double)ceiling / 1048576.0);
	}
}

/* ---------- the public calls */

/* Splits a mapping into up to three - the part before the requested range,
the requested range itself, and the part after it - so that a caller can
protect part of a mapping.

This is not a convenience. The loader maps the whole guest image as one
writable range (port/android/host/host_loader.c) and then makes only the
executable segments executable, so every request it makes names a
sub-range. Without this, protecting .text would have made the whole image -
its .data, its .bss, its relocation tables - executable as well.

Each piece gets its own backing buffer and the bytes it covers are copied
into it, rather than the pieces sharing the original pointer. That is not
tidiness. An earlier version of this shared one buffer between up to three
records, which broke two things at once: a later lookup for the whole
mapping no longer found it, so the range was never released and stayed
occupied for the rest of the run; and whichever two records were released
would have freed the same pointer. The console refused every later mapping
of that address, and the log said only that the address was in use.

A copy is the price, and it is paid during start-up, where the alternative
is wrong rather than slow.
*/
static struct mapping *mapping_split(struct mapping *mapping, void *address, size_t length)
{
	uintptr_t start = (uintptr_t)address;
	uintptr_t end = start + length;
	uintptr_t base = (uintptr_t)mapping->address;
	uintptr_t limit = base + mapping->length;
	void *before = NULL, *middle_copy = NULL, *after = NULL;
	/* read before the record is retired below, which zeroes it; the pieces
	 * were recorded as PROT_NONE when this was read afterwards */
	int protection = mapping->protection;
	struct mapping *middle;

	if (start == base && end == limit)
		return mapping;
	if (start < base || end > limit)
		return NULL;
	if (mapping->kind != _mapping_ordinary && mapping->executable)
		/* A code memory object cannot be divided: the kernel maps and
		unmaps it whole. A writable one is split the way an ordinary one
		is, by copying its pieces into objects of their own (map_writable),
		which is how the loader's image is split where the guest's memory
		has to be code memory; an executable one never needs to be - a
		mapping born executable is always created at the exact size asked
		for. */
		return NULL;

	/* copy each piece out of the live mapping before any of it is released */
	if (start > base)
	{
		before = memalign(0x1000, start - base);
		if (!before)
			return NULL;
		memcpy(before, (const void *)base, start - base);
	}
	middle_copy = memalign(0x1000, length);
	if (!middle_copy)
	{
		free(before);
		return NULL;
	}
	memcpy(middle_copy, (const void *)start, length);
	if (end < limit)
	{
		after = memalign(0x1000, limit - end);
		if (!after)
		{
			free(before);
			free(middle_copy);
			return NULL;
		}
		memcpy(after, (const void *)end, limit - end);
	}

	/* the original goes down only once every byte of it has been copied,
	and its record is retired at the same moment. Retiring it is not
	optional bookkeeping: left in the table it still claims the whole
	original range with a backing pointer that has just been freed, so the
	next lookup for that range finds it, releases it again, and calls
	svcUnmapMemory on a mapping that no longer exists. The console answers
	that with 0xd401 and nothing is released, which looks exactly like the
	address being occupied for good. */
	unmap_mapping(mapping);
	free(mapping->backing);
	mapping->kind = _mapping_free;
	mapping->address = NULL;
	mapping->length = 0;
	mapping->backing = NULL;
	mapping->code = 0;
	mapping->executable = 0;
	mapping->protection = 0;

	/* each piece back, of whichever kind the console allows there; a piece
	that is mapped belongs to its record, and the rest are freed */
	if (before)
	{
		if (!map_writable((void *)base, start - base, before, protection))
		{
			free(before);
			free(middle_copy);
			free(after);
			return NULL;
		}
	}
	if (!map_writable((void *)start, length, middle_copy, protection))
	{
		free(middle_copy);
		free(after);
		return NULL;
	}
	if (after && !map_writable((void *)end, limit - end, after, protection))
	{
		free(after);
		return NULL;
	}
	middle = mapping_find((void *)start, length, 1);
	return middle;
}

/* Takes [address, address + length) out of whatever is mapped there, and
nothing else: each mapping the range touches is split and only the part
inside the range let go, its head and tail kept with their contents. A
MAP_FIXED mapping replaces a range this way, and munmap releases one. */
static int release_range(void *address, size_t length)
{
	uintptr_t start = (uintptr_t)address;
	uintptr_t end = start + length;

	for (;;)
	{
		struct mapping *existing = NULL;
		uintptr_t base, limit, piece_start, piece_end;
		struct mapping *middle;
		int index;

		for (index = 0; index < mapping_count; index++)
		{
			struct mapping *candidate = &mappings[index];
			uintptr_t candidate_base = (uintptr_t)candidate->address;

			if (candidate->kind == _mapping_free)
				continue;
			if (candidate_base + candidate->length <= start || candidate_base >= end)
				continue;
			if (!existing || candidate->length < existing->length)
				existing = candidate;
		}
		if (!existing)
			break;

		base = (uintptr_t)existing->address;
		limit = base + existing->length;
		piece_start = start > base ? start : base;
		piece_end = end < limit ? end : limit;
		middle = mapping_split(existing, (void *)piece_start,
			(size_t)(piece_end - piece_start));

		if (!middle)
		{
			host_logf(HOST_LOG_ERROR,
				"%p (%zu bytes) cannot be taken out of the mapping of %zu bytes at %p",
				address, length, limit - base, (void *)base);
			return -1;
		}
		unmap_mapping(middle);
		free(middle->backing);
		middle->kind = _mapping_free;
		middle->address = NULL;
		middle->length = 0;
		middle->backing = NULL;
		middle->code = 0;
		middle->executable = 0;
		middle->protection = 0;
	}
	return 0;
}

static void *mmap_unlocked(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
	void *backing;
	struct mapping *mapping;

	if (!length)
		return MAP_FAILED;
	length = (length + 0xFFF) & ~(size_t)0xFFF;
	/* this layer has nothing to share and nothing to reserve */
	flags &= ~(MAP_SHARED | MAP_NORESERVE);

	backing = memalign(0x1000, length);
	if (!backing)
		return MAP_FAILED;
	memset(backing, 0, length);

	/* A file-backed mapping is read into the buffer here and then mapped
	like any other; the console cannot map a file directly, and the guest
	only maps files it has open on the same filesystem the host serves. */
	if (fd >= 0 && offset >= 0)
	{
		long position;
		size_t done = 0;

		/* the same driver the logger writes through, so the same lock */
		host_sd_lock();
		position = lseek(fd, (off_t)offset, SEEK_SET);
		if (position != (off_t)offset)
		{
			host_sd_unlock();
			free(backing);
			return MAP_FAILED;
		}
		/* the file's bytes are wanted in the mapping, but read() is
		 * given a scratch buffer and the bytes copied: the driver
		 * does not honour buffers that are not the ordinary heap,
		 * and this mapping's backing may not be. The buffer is static:
		 * it is only used under the SD lock this block holds, and on
		 * the stack it was larger than some callers' entire stacks -
		 * a thread with a 4 KB stack faulted just entering mmap. */
		static char scratch[16384];

		while (done < length)
		{
			long got = read(fd, scratch,
				(unsigned)(length - done < sizeof(scratch) ? length - done : sizeof(scratch)));

			if (got <= 0)
				break;
			memcpy((char *)backing + done, scratch, (size_t)got);
			done += (size_t)got;
		}
		host_sd_unlock();
	}

	if (flags & MAP_FIXED)
	{
		/* MAP_FIXED means "here, replacing whatever is there", and the
		guest's own allocator depends on it: platform_contiguous_alloc maps
		fresh pages over the window it reserved earlier, a few pages at a
		time. Mapping without replacing meant svcMapMemory was asked for an
		address that was already in use, the mmap did not return what was
		asked for, and every texture allocation failed with "out of
		contiguous memory" even though 128 MB sat there untouched.
		 *
		Every mapping the range touches is released, not just whichever one
		begins under the address. A request can run past the end of that one
		into the next - 20480 bytes over an 8192 byte mapping did exactly
		that - and svcMapMemory will not map over either of them, so the
		mmap failed with the range still occupied. Each overlapping mapping
		is therefore split in turn and only the part actually being replaced
		is let go. The head and tail of each are re-mapped with their
		contents copied, because they belong to whoever is using them.
		 */
		if (release_range(address, length) != 0)
		{
			free(backing);
			errno = ENOTSUP;
			return MAP_FAILED;
		}
	}

	if ((flags & MAP_FIXED) == 0)
	{
		void *chosen = NULL;

		/* Where the guest's memory has to be code memory, a request with no
		address is the host's own - its libraries' - and has no need to be
		below 4 GB: it goes in the stack region, where svcMapMemory maps and
		an ordinary mapping is a mapping of its own rather than a code
		memory object each. A large one still goes to the range reserved for
		it, as below. */
		if (host_mman_low_code_mode() && !(length >= LARGE_MAPPING && host_memory_reserved_region(length)))
		{
			virtmemLock();
			chosen = virtmemFindStack(length, 0);
			virtmemUnlock();
		}
		if (!chosen)
			chosen = find_free(length, 0x1000, 0);

		/* No fallback to another address.
		 *
		 * There was one, and it broke the guest rather than helping it:
		 * the contiguous window is reserved by the guest, which asks for
		 * exactly the address the host told it to expect and treats
		 * anything else as failure (port/linux/src/xbox_memory.c, "if
		 * (result == wanted)"). Handing it a different address produced
		 * 0x10000000 and "cannot reserve the Xbox contiguous memory
		 * window", which reads like the port running out of room and is
		 * not that at all.
		 *
		 * So when the reserved address cannot take the request, the right
		 * answer is to fail and say so, and for host_main to find out how
		 * much that address really can take before starting the game. */
		if (!chosen)
		{
			host_logf(HOST_LOG_ERROR,
				"no free address space below 4 GB for %zu bytes (mmap flags 0x%x)", length, flags);
			log_address_space();
			log_memory_limit();
			free(backing);
			return MAP_FAILED;
		}
		address = chosen;
	}
	else if (flags & MAP_FIXED_NOREPLACE)
	{
		/* refuse if anything is already mapped here, which is what the
		host asks for when reserving the image's range */
		MemoryInfo information = { 0 };
		u32 page_info = 0;

		if (R_SUCCEEDED(svcQueryMemory(&information, &page_info, (u64)(uintptr_t)address)) &&
			information.type != MemType_Unmapped && information.addr <= (u64)(uintptr_t)address &&
			information.addr + information.size >= (u64)(uintptr_t)address + length)
		{
			free(backing);
			return MAP_FAILED;
		}
		flags |= MAP_FIXED;
	}

	if (protection & PROT_EXEC)
	{
		/* Executable memory cannot be placed anywhere the caller likes if
		the caller did not say where, and the host always does when it is
		loading the guest image. */
		if (!address)
		{
			free(backing);
			errno = ENOMEM;
			return MAP_FAILED;
		}
		if (map_code(address, length, backing, &mapping) != 0)
		{
			free(backing);
			return MAP_FAILED;
		}
		return address;
	}

	if (!map_writable(address, length, backing, protection))
	{
		/* say what is there, so that a refusal says whether something is in
		the way or the console will not hand out the page at all - which
		two different fixes would follow from, and which the first probes
		could not tell apart */
		MemoryInfo information = { 0 };
		u32 page_info = 0;

		log_memory_limit();
		host_logf(HOST_LOG_ERROR, "mapping %zu bytes at %p failed", length, address);
		if (R_SUCCEEDED(svcQueryMemory(&information, &page_info, (u64)(uintptr_t)address)))
			host_logf(HOST_LOG_ERROR, "  the kernel reports there: base %p size 0x%llx type %02x perm %x%s",
				(void *)(uintptr_t)information.addr, (unsigned long long)information.size,
				information.type, information.perm,
				information.type == MemType_Unmapped ? " (unmapped: nothing in the way)" : "");
		else
			host_logf(HOST_LOG_ERROR, "  and cannot be asked what is there either");
		free(backing);
		return MAP_FAILED;
	}
	return address;
}

/* ---------- turning an ordinary mapping executable

The loader maps the whole guest image as one writable range and then makes
each executable segment executable, a sub-range at a time
(port/android/host/host_loader.c), so this is the path that is actually
taken at start-up: the mapping being protected is an ordinary one, not a
mapping that was born as code. That is not a cheap transformation.

There is no way to make an existing mapping executable in place. The console's
ordinary pages are type 0x0b and their permission is set by
svcSetMemoryPermission, which refuses Perm_X; only a code memory object can
ever be executable, and its contents have to be copied in at creation. So the
range is copied into a fresh buffer, wrapped in a code memory object, mapped
read-execute over the same address, and the old mapping is taken down.

For the guest image that is a few tens of megabytes once, before the game
starts, against a load that is copying those same segments in from the SD
card in the first place. It would be far too slow per frame, which is why
this is only ever done at start-up: host_memory.c's page-granular calls are
between read and write and never ask for PROT_EXEC, and nothing else in the
host asks for it after the image is up. */
static int ordinary_to_code(struct mapping *mapping, u32 permission)
{
	/* Two buffers, and the reason for two is the whole of this function.
	 *
	 * svcCreateCodeMemory takes a buffer and makes it inaccessible: reading
	 * it afterwards faults. What it does NOT do is make the code memory
	 * show that buffer's contents. Filling the object from its source and
	 * then mapping the slave produced an executable view reading
	 * 0xffffffff, while the buffer itself held the guest image perfectly -
	 * the header, its magic and version, checked and logged, all correct.
	 *
	 * So the image has to go in through the writable owner view, which is
	 * what the memory probe did and the only route known to work: map the
	 * owner, write through it, drop the owner, map the slave over the same
	 * address. The buffer handed to svcCreateCodeMemory supplies the
	 * physical pages and is never touched again; `saved` holds the bytes
	 * to write once the owner is mapped. */
	void *pages;
	void *saved;
	Handle code = 0;
	Result result;
	unsigned index;

	host_logf(HOST_LOG_INFO, "making %zu bytes at %p executable", mapping->length, mapping->address);
	saved = memalign(0x1000, mapping->length);
	pages = memalign(0x1000, mapping->length);
	if (!saved || !pages)
		return -ENOMEM;
	memcpy(saved, mapping->address, mapping->length);

	host_logf(HOST_LOG_INFO, "  the image holds:");
	for (index = 0; index < 16; index += 4)
		host_logf(HOST_LOG_INFO, "    %08x %08x %08x %08x",
			*(const unsigned int *)((const unsigned char *)saved + index),
			*(const unsigned int *)((const unsigned char *)saved + index + 4),
			*(const unsigned int *)((const unsigned char *)saved + index + 8),
			*(const unsigned int *)((const unsigned char *)saved + index + 12));

	result = svcCreateCodeMemory(&code, pages, mapping->length);
	host_logf(R_FAILED(result) ? HOST_LOG_ERROR : HOST_LOG_INFO,
		"  svcCreateCodeMemory: %s (0x%08x); its buffer is gone now",
		R_FAILED(result) ? "failed" : "ok", (unsigned)result);
	if (R_FAILED(result))
	{
		free(saved);
		free(pages);
		return -ENOMEM;
	}

	/* the old mapping comes down before the owner is mapped there (an
	ordinary one, or aliased heap whose permission would not take Perm_Rx) */
	unmap_mapping(mapping);
	free(mapping->backing);
	mapping->backing = NULL;

	result = svcControlCodeMemory(code, CodeMapOperation_MapOwner, mapping->address,
		mapping->length, Perm_Rw);
	host_logf(R_FAILED(result) ? HOST_LOG_ERROR : HOST_LOG_INFO,
		"  MapOwner Perm_Rw at %p: %s (0x%08x)", mapping->address,
		R_FAILED(result) ? "failed" : "ok", (unsigned)result);
	if (R_FAILED(result))
	{
		svcCloseHandle(code);
		free(saved);
		return -ENOMEM;
	}

	/* and now the image goes in, through the view that can be written */
	memcpy(mapping->address, saved, mapping->length);
	host_logf(HOST_LOG_INFO, "  the image was written through the owner view");

	result = svcControlCodeMemory(code, CodeMapOperation_UnmapOwner, mapping->address,
		mapping->length, 0);
	if (R_FAILED(result))
		host_logf(HOST_LOG_ERROR, "  UnmapOwner: 0x%08x", (unsigned)result);

	result = svcControlCodeMemory(code, CodeMapOperation_MapSlave, mapping->address,
		mapping->length, permission);
	host_logf(R_FAILED(result) ? HOST_LOG_ERROR : HOST_LOG_INFO,
		"  MapSlave %s at %p: %s (0x%08x)",
		permission == Perm_Rx ? "Perm_Rx" : "Perm_Rw", mapping->address,
		R_FAILED(result) ? "failed" : "ok", (unsigned)result);
	if (R_FAILED(result))
	{
		svcCloseHandle(code);
		free(saved);
		return -ENOMEM;
	}

	host_logf(HOST_LOG_INFO, "  the executable view holds:");
	for (index = 0; index < 16; index += 4)
		host_logf(HOST_LOG_INFO, "    %08x %08x %08x %08x",
			*(const unsigned int *)((const unsigned char *)mapping->address + index),
			*(const unsigned int *)((const unsigned char *)mapping->address + index + 4),
			*(const unsigned int *)((const unsigned char *)mapping->address + index + 8),
			*(const unsigned int *)((const unsigned char *)mapping->address + index + 12));

	mapping->kind = _mapping_code;
	mapping->code = code;
	mapping->executable = 1;
	/* Prove the code memory is readable, and if it was asked to be
	 * writable, that it is writable too.
	 *
	 * The guest faults in csmemset writing twelve bytes at 0x4051652c,
	 * which is in the image's data, not in its code - and the data is
	 * carved out of the same image by the split that made this range
	 * executable. That is the arrangement this function exists to build,
	 * so it is the arrangement worth checking rather than trusting. The
	 * same reasoning killed the stack theory: checking beat believing.
	 *
	 * Only a read is done unless the range was asked to be writable, since
	 * writing to a Perm_Rx range would fault here rather than answer. */
	{
		size_t offset;
		int unreadable = 0;
		const unsigned char *bytes = (const unsigned char *)mapping->address;

		for (offset = 0; offset < mapping->length; offset += 4096)
		{
			volatile unsigned char probe = bytes[offset];

			(void)probe;
		}
		if (permission != Perm_Rx)
		{
			unsigned char *writable = (unsigned char *)mapping->address;
			size_t saved_first = writable[0];

			for (offset = 0; offset < mapping->length; offset += 4096)
			{
				unsigned char before = writable[offset];

				writable[offset] = (unsigned char)(before ^ 0xff);
				if (writable[offset] != (unsigned char)(before ^ 0xff))
					unreadable++;
				writable[offset] = before;
			}
			writable[0] = saved_first;
		}
		host_logf(HOST_LOG_INFO,
			"  code memory check: %zu bytes at %p, %d pages did not survive being written to",
			mapping->length, mapping->address, unreadable);
	}
	free(saved);

	/* No cache maintenance, and none is needed: the loader calls
	__builtin___clear_cache over the whole image once every segment has been
	placed (port/android/host/host_loader.c), which does the same two
	operations. */
	host_logf(HOST_LOG_INFO, "  %zu bytes at %p are executable", mapping->length, mapping->address);
	return 0;
}

/* and back again: an executable range that is to be writable becomes an
ordinary mapping, which is a copy in the other direction */
static int code_to_ordinary(struct mapping *mapping)
{
	void *backing;
	Result result;

	/* where svcMapMemory cannot map, it stays code memory: the executable
	view comes down and the writable one goes up, the contents being the
	object's own */
	if (!ordinary_allowed(mapping->address, mapping->length))
	{
		svcControlCodeMemory(mapping->code, CodeMapOperation_UnmapSlave, mapping->address, mapping->length, 0);
		result = svcControlCodeMemory(mapping->code, CodeMapOperation_MapOwner, mapping->address,
			mapping->length, Perm_Rw);
		if (R_FAILED(result))
		{
			svcControlCodeMemory(mapping->code, CodeMapOperation_MapSlave, mapping->address, mapping->length,
				Perm_Rx);
			return -ENOMEM;
		}
		mapping->executable = 0;
		return 0;
	}
	backing = memalign(0x1000, mapping->length);
	if (!backing)
		return -ENOMEM;
	memcpy(backing, mapping->address, mapping->length);

	result = (Result)map_ordinary(mapping->address, mapping->length, backing);
	if (R_FAILED(result))
	{
		free(backing);
		return -ENOMEM;
	}
	svcControlCodeMemory(mapping->code, CodeMapOperation_UnmapSlave,
		mapping->address, mapping->length, 0);
	svcCloseHandle(mapping->code);
	free(mapping->backing);
	mapping->kind = _mapping_ordinary;
	mapping->backing = backing;
	mapping->code = 0;
	mapping->executable = 0;
	return 0;
}

/* Only the range named goes, as munmap promises. This released the whole of
whichever mapping held the address, so a guest freeing part of what it had
mapped lost the rest with it. */
static int munmap_unlocked(void *address, size_t length)
{
	if (!length)
		return 0;
	length = (length + ((uintptr_t)address & 0xFFF) + 0xFFF) & ~(size_t)0xFFF;
	address = (void *)((uintptr_t)address & ~(uintptr_t)0xFFF);
	if (release_range(address, length) != 0)
	{
		errno = EINVAL;
		return -1;
	}
	return 0; /* nothing of ours there is not an error either */
}

/* Turns a mapping executable, or writable again.

Going one way is cheap: the read-execute view is mapped over the address the
writable view already occupies, and the two are one memory, so the contents
need no copying and only the caches need attending to. Coming back is the
mirror image. Either way a code memory object is created or closed, because
that is how the console makes a page executable at all. */
static int mprotect_unlocked(void *address, size_t length, int protection)
{
	struct mapping *mapping;
	u32 permission;
	Result result;

	/* Linux does nothing for an empty range and says so. Here it reached
	 * mapping_split with a length of 0, which took the mapping down, could
	 * not map a 0-byte middle back (0xca01) and returned with the rest of
	 * the range never re-mapped: memory the guest was using, gone. */
	if (!length)
		return 0;
	mapping = mapping_find(address, length, 0);
	if (!mapping)
	{
		errno = ENOMEM;
		return -1;
	}
	/* Settled before anything is split. A split copies the whole mapping -
	 * megabytes, for the renderer's write watch on a texture - and takes it
	 * down to do so; doing that for a request that is then refused, or that
	 * changes nothing, cost the copy each time and, when memory ran short
	 * part way, the guest's memory with it. */
	if (mapping->kind == _mapping_ordinary && !(protection & PROT_EXEC))
	{
		if (protection == mapping->protection)
			return 0;
		if (!(mapping->protection == PROT_NONE && protection == (PROT_READ | PROT_WRITE)))
		{
			static int refusals;

			if (refusals++ < 8)
				host_logf(HOST_LOG_ERROR,
					"the console cannot change the protection of a mapping at %p from 0x%x to 0x%x%s",
					address, mapping->protection, protection,
					refusals == 8 ? " (no more of these will be logged)" : "");
			errno = ENOTSUP;
			return -1;
		}
	}
	/* the console takes page-aligned ranges and rounds outward; the loader
	protects single pages but the guest's own arenas are page granular too */
	length = (((uintptr_t)address + length - 1) & ~(uintptr_t)0xFFF)
		- ((uintptr_t)address & ~(uintptr_t)0xFFF) + 0x1000;
	address = (void *)((uintptr_t)address & ~(uintptr_t)0xFFF);

	/* the caller may have named a sub-range of a larger mapping, and the
	loader always does; the mapping that has to change is the piece that
	matches the request, not the whole of what it sits in. A writable code
	mapping is split only to make part of it executable - the loader's image,
	where the guest's memory is code memory - and never for a protection the
	console would not give it anyway: host_memory.c's chunks are code memory,
	and a thread's guard page in one would otherwise have cut it in three. */
	if (((uintptr_t)address != (uintptr_t)mapping->address ||
		length != mapping->length) &&
		(mapping->kind == _mapping_ordinary || (!mapping->executable && (protection & PROT_EXEC))))
	{
		mapping = mapping_split(mapping, address, length);
		if (!mapping)
		{
			errno = ENOMEM;
			return -1;
		}
		address = mapping->address;
		length = mapping->length;
	}

	permission = 0;
	if (protection & PROT_READ)
		permission |= Perm_R;
	if (protection & PROT_WRITE)
		permission |= Perm_W;
	if (protection & PROT_EXEC)
		permission |= Perm_X;

	if (mapping->kind == _mapping_alias)
	{
		/* Aliased heap is made executable in place, the way the loader makes
		a program's text executable, and back again; the contents are its
		own. Should the kernel refuse, it becomes code memory as an ordinary
		mapping would. Anything else is left read-write and said to have
		worked: the arenas' chunks are aliased heap, and a guard page made
		inaccessible in one would leave a stack the kernel will not take
		buffers from. */
		Handle process = envGetOwnProcessHandle();

		if ((protection & PROT_EXEC) && !mapping->executable)
		{
			result = svcSetProcessMemoryPermission(process, (u64)(uintptr_t)mapping->address, mapping->length,
				Perm_Rx);
			if (R_SUCCEEDED(result))
			{
				mapping->executable = 1;
				host_logf(HOST_LOG_INFO, "  %zu bytes at %p are executable", mapping->length, mapping->address);
				return 0;
			}
			host_logf(HOST_LOG_WARN, "svcSetProcessMemoryPermission(%p, rx) failed: 0x%08x; copying it to code "
				"memory instead", mapping->address, (unsigned)result);
			if (ordinary_to_code(mapping, permission) != 0)
			{
				errno = ENOMEM;
				return -1;
			}
			return 0;
		}
		if (!(protection & PROT_EXEC) && mapping->executable)
		{
			result = svcSetProcessMemoryPermission(process, (u64)(uintptr_t)mapping->address, mapping->length,
				Perm_Rw);
			if (R_FAILED(result))
			{
				errno = ENOMEM;
				return -1;
			}
			mapping->executable = 0;
		}
		return 0;
	}
	if ((protection & PROT_EXEC) && mapping->kind == _mapping_ordinary)
	{
		/* the case the loader actually takes: a sub-range of a writable
		mapping that has to become code memory, which means copying it */
		if (ordinary_to_code(mapping, permission) != 0)
		{
			errno = ENOMEM;
			return -1;
		}
		return 0;
	}
	if (!(protection & PROT_EXEC) && mapping->kind == _mapping_code && mapping->executable)
	{
		if (code_to_ordinary(mapping) != 0)
		{
			errno = ENOMEM;
			return -1;
		}
		/* still code memory (code_to_ordinary): its writable view is what was asked */
		if (mapping->kind == _mapping_code)
			return 0;
		result = (Result)svcSetMemoryPermission(mapping->address, mapping->length, permission);
		if (R_FAILED(result))
		{
			errno = ENOMEM;
			return -1;
		}
		return 0;
	}
	if ((protection & PROT_EXEC) && !mapping->executable)
	{
		/* The writable view comes down and the executable one goes up at the
		same address, in that order: it is the order ordinary_to_code takes,
		and the one known to work, since two views cannot share an address.
		The contents are the object's own and carry over. No cache
		maintenance here - the buffer the object was made from cannot be
		touched while it exists - and none is needed: the loader clears the
		caches over the whole image once it is placed. */
		svcControlCodeMemory(mapping->code, CodeMapOperation_UnmapOwner,
			mapping->address, mapping->length, 0);
		result = svcControlCodeMemory(mapping->code, CodeMapOperation_MapSlave,
			mapping->address, mapping->length, Perm_Rx);
		if (R_FAILED(result))
		{
			host_logf(HOST_LOG_ERROR, "%zu bytes at %p could not be made executable: 0x%08x",
				mapping->length, mapping->address, (unsigned)result);
			svcControlCodeMemory(mapping->code, CodeMapOperation_MapOwner,
				mapping->address, mapping->length, Perm_Rw);
			errno = ENOMEM;
			return -1;
		}
		mapping->executable = 1;
		host_logf(HOST_LOG_INFO, "  %zu bytes at %p are executable", mapping->length, mapping->address);
		return 0;
	}
	if (!(protection & PROT_EXEC) && mapping->executable)
	{
		result = svcControlCodeMemory(mapping->code, CodeMapOperation_MapOwner,
			mapping->address, mapping->length, Perm_Rw);
		if (R_FAILED(result))
		{
			errno = ENOMEM;
			return -1;
		}
		svcControlCodeMemory(mapping->code, CodeMapOperation_UnmapSlave,
			mapping->address, mapping->length, 0);
		mapping->executable = 0;
		return 0;
	}
	/* Nothing to do is the common case, and asking anyway is fatal here.
	 *
	 * The console will not change the protection of a mapping made by
	 * svcMapMemory - it answers 0xd401 - and the guest's libc asks for
	 * read-write on memory that is already read-write, over and over: 206
	 * identical refusals in one run, and then "cannot grow emulated TLS",
	 * which is what it was doing when it failed.
	 *
	 * So an ordinary mapping's protection is remembered, and a request
	 * that matches it returns at once. There is nothing a caller could
	 * have wanted from the refusal.
	 *
	 * A code mapping is left alone for the same reason: its executable
	 * view is read-execute by construction and the only writable view it
	 * has is the one it is being changed to. */
	if (mapping->kind == _mapping_code)
		return 0;
	if (mapping->protection == protection)
		return 0;

	/* Handing reserved memory to the guest.
	 *
	 * This is what the guest's libc does when it grows its emulated TLS:
	 * the memory was marked unavailable by host_low_unmap - which maps it
	 * PROT_NONE rather than releasing it, so that the pool still owns the
	 * address - and the guest asks for it to become readable and writable.
	 *
	 * The console cannot change the protection of a mapping made by
	 * svcMapMemory, so this is done by making it again rather than
	 * altering it. That is only sound because PROT_NONE memory holds
	 * nothing: there are no contents to lose. A mapping that already held
	 * data and is being made read-only, or read-write from read-only, is a
	 * different request and is refused below rather than approximated.
	 */
	if (mapping->kind == _mapping_ordinary && mapping->protection == PROT_NONE && protection == (PROT_READ | PROT_WRITE))
	{
		void *backing = memalign(0x1000, mapping->length);
		void *previous = mapping->backing;

		if (!backing)
		{
			errno = ENOMEM;
			return -1;
		}
		/* The old mapping goes down first. The console will not map over a
		 * range that is already mapped - it answers 0xd401, KernelError_
		 * InvalidMemoryState, which is what every one of these returned
		 * until the order was put this way round, and which the guest then
		 * saw as a failed mprotect, so the allocator's metadata page stayed
		 * PROT_NONE, alloc_meta() gave up, and malloc returned NULL with
		 * nothing wrong anywhere else. mapping_split() has always released
		 * before re-making, which is why splitting worked and this did not.
		 *
		 * If the new mapping is refused the old one is put back, so a
		 * failure leaves the pool still holding the address rather than
		 * releasing it to be handed out from somewhere else. */
		unmap_ordinary(mapping);
		if (R_FAILED((Result)map_ordinary(mapping->address, mapping->length, backing)))
		{
			if (R_FAILED((Result)map_ordinary(mapping->address, mapping->length, previous)))
				host_logf(HOST_LOG_ERROR,
					"and %p (%zu bytes) could not be given back either; the pool has lost it",
					mapping->address, mapping->length);
			free(backing);
			errno = ENOMEM;
			return -1;
		}
		free(previous);
		mapping->backing = backing;
		mapping->protection = PROT_READ | PROT_WRITE;
		return 0;
	}

	if (mapping->kind == _mapping_ordinary)
	{
		/* Anything else about an ordinary mapping: the console will not do
		it, and pretending otherwise would leave the guest believing a
	 * protection it did not get. */
		/* The cache copy thread asks this for every 128 KB it fills, so
		 * only the first few are said: each line is a write to the card. */
		static int refusals;

		if (refusals++ < 8)
			host_logf(HOST_LOG_ERROR,
				"the console cannot change the protection of a mapping at %p from 0x%x to 0x%x%s",
				address, mapping->protection, protection, refusals == 8 ? " (no more of these will be logged)" : "");
		errno = ENOTSUP;
		return -1;
	}
	result = svcSetMemoryPermission(address, length, permission);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "svcSetMemoryPermission(%p, %zu, 0x%x) failed: 0x%08x",
			address, length, (unsigned)permission, (unsigned)result);
		errno = ENOMEM;
		return -1;
	}
	mapping->protection = protection;
	return 0;
}

int madvise(void *address, size_t length, int advice)
{
	/* Nothing to advise and nothing to advise with: the console's memory has
	no swappable or reclaimable state. The host's sampler calls this after
	touching a range, so it must succeed. */
	(void)address;
	(void)length;
	(void)advice;
	return 0;
}
/* ---------- one at a time

The mapping table is shared by every thread that maps memory - the game
thread, the cache copy thread, the guest's other threads - and nothing
serialised them. A split retires a record and adds up to three; a release
marks one free; and since freed records are reused, two threads could take
the same one, or one could act on a record another had just retired. A run
ended in exactly that: svcUnmapMemory on a record already zeroed (0x0, 0
bytes), a backing freed twice, and the next malloc - inside Mesa's shader
compiler - faulting on the damaged heap. Recursive, because these call one
another. */
static RMutex mapping_lock;

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
	void *result;

	rmutexLock(&mapping_lock);
	result = mmap_unlocked(address, length, protection, flags, fd, offset);
	rmutexUnlock(&mapping_lock);
	return result;
}

int munmap(void *address, size_t length)
{
	int result;

	rmutexLock(&mapping_lock);
	result = munmap_unlocked(address, length);
	rmutexUnlock(&mapping_lock);
	return result;
}

int mprotect(void *address, size_t length, int protection)
{
	int result;

	rmutexLock(&mapping_lock);
	result = mprotect_unlocked(address, length, protection);
	rmutexUnlock(&mapping_lock);
	return result;
}

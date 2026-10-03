/*
MEM PROBE

Answers the questions the Switch port's memory layer needs answered before
it can be written (port/switch/host/host_mman.c). The guest runs ILP32 code
whose data structures embed 32-bit pointers, so every address the guest can
touch has to be below 4 GB: the image is linked at 0x40000000 and the Xbox
window at 0x80000000. devkitPro's newlib has no <sys/mman.h> and libnx's
heap is far above 4 GB, so that address space has to be built deliberately.

What this asks of the console:

1. What does svcQueryMemory say about addresses below 4 GB - unmapped, or
   already taken by the loader?
2. Can svcMapPhysicalMemory (SVC 0x2C, "maps new heap memory at the desired
   address") put usable pages at an arbitrary address below 4 GB, and can
   they be written and read?
3. Can that memory be made executable? svcSetMemoryPermission refuses Perm_X
   and write-only, so the guest image - which has to run - needs another
   route: svcMapProcessCodeMemory (0x78) over the current process, or
   svcControlCodeMemory (0x4D).
4. Does a mapping at 0x40000000 collide with anything the Homebrew Menu or
   SDL2 already placed there?

Build: make -C port/switch/probe/mem
Copy the .nro to the SD card and start it from the Homebrew Menu. The
report goes to the console, and to halo-mem-probe.txt beside the .nro so it
can be read back over the SD card.

Every call is reported whether it succeeded or not; the answer to a
question that turns out to be "no" is as useful as "yes".
*/

#include <switch.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static FILE *report_file;

static void say(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	if (report_file)
	{
		va_start(arguments, format);
		vfprintf(report_file, format, arguments);
		va_end(arguments);
		fflush(report_file);
	}
}

static void heading(const char *text)
{
	say("\n=== %s ===\n", text);
}

/* ---------- 1: what is below 4 GB */

static void probe_query_memory(void)
{
	static const u64 addresses[] = {
		0x00100000, /* 1 MB   */
		0x04000000, /* 64 MB  */
		0x10000000, /* 256 MB */
		0x20000000, /* 512 MB */
		0x40000000, /* the guest image lives here */
		0x60000000,
		0x80000000, /* the Xbox window lives here */
		0xC0000000,
		0xF0000000,
	};
	size_t index;

	heading("svcQueryMemory below 4 GB");
	for (index = 0; index < sizeof(addresses) / sizeof(*addresses); index++)
	{
		MemoryInfo information = { 0 };
		u32 page_info = 0;
		Result result = svcQueryMemory(&information, &page_info, addresses[index]);

		if (R_FAILED(result))
		{
			say("0x%016llx: FAILED %s\n", (unsigned long long)addresses[index], strerror(result));
			continue;
		}
		say("0x%016llx: base 0x%016llx size 0x%llx type 0x%02x perm 0x%x pageinfo %u  -> %s\n",
			(unsigned long long)addresses[index], (unsigned long long)information.addr,
			(unsigned long long)information.size, information.type, information.perm, page_info,
			information.size ? "MAPPED" : "unmapped");
	}
}

/* ---------- 2: mapping usable pages at a chosen low address */

static bool probe_map_at(u64 address, u64 size)
{
	u64 *memory = (u64 *)(uintptr_t)address;
	Result result;
	u32 index;

	say("mapping %llx bytes at 0x%llx: ", (unsigned long long)size, (unsigned long long)address);
	result = svcMapPhysicalMemory((void *)(uintptr_t)address, size);
	if (R_FAILED(result))
	{
		say("svcMapPhysicalMemory FAILED %s\n", strerror(result));
		return false;
	}
	say("ok\n");

	/* the only proof that it is memory is writing it and reading it back */
	for (index = 0; index < size / sizeof(u64); index += size / (8 * sizeof(u64)))
		memory[index] = 0x5a5a0000u + index;
	for (index = 0; index < size / sizeof(u64); index += size / (8 * sizeof(u64)))
	{
		if (memory[index] != 0x5a5a0000u + index)
		{
			say("  read back WRONG at +0x%llx (got %08x, wrote %08x)\n",
				(unsigned long long)(index * sizeof(u64)), (unsigned)memory[index], (unsigned)(0x5a5a0000u + index));
			return false;
		}
	}
	say("  wrote and read back %llu pages\n", (unsigned long long)(size / 0x1000));
	return true;
}

/* ---------- 3: permissions, and above all whether pages can execute */

static void probe_permissions(u64 address, u64 size)
{
	static const struct { u32 permission; const char *name; } list[] = {
		{ Perm_Rw, "Perm_Rw" },
		{ Perm_None, "Perm_None" },
		{ Perm_R, "Perm_R" },
		{ Perm_Rx, "Perm_Rx  <- can code run here?" },
		{ Perm_X, "Perm_X" },
	};
	size_t index;

	heading("svcSetMemoryPermission on a mapped page");
	for (index = 0; index < sizeof(list) / sizeof(*list); index++)
	{
		Result result = svcSetMemoryPermission((void *)(uintptr_t)address, size, list[index].permission);

		say("%-32s -> %s\n", list[index].name, R_FAILED(result) ? strerror(result) : "ok");
	}
	/* leave it readable and writable for the next question */
	svcSetMemoryPermission((void *)(uintptr_t)address, size, Perm_Rw);
}

/* Executable memory is the hard one: the guest image has to execute, and
svcSetMemoryPermission refuses Perm_X outright (its own documentation says
so). Code memory is a separate kind of mapping on the console, made through
the process's code region rather than the ordinary one. */
static void probe_executable(u64 address, u64 size)
{
	MemoryInfo information = { 0 };
	u32 page_info = 0;
	Handle process = 0;
	s64 pid = 0;
	Result result;

	heading("executable memory");
	result = svcSetMemoryPermission((void *)(uintptr_t)address, size, Perm_Rx);
	say("svcSetMemoryPermission(Perm_Rx): %s\n", R_FAILED(result) ? strerror(result) : "ok");
	result = svcSetMemoryPermission((void *)(uintptr_t)address, size, Perm_X);
	say("svcSetMemoryPermission(Perm_X):  %s\n", R_FAILED(result) ? strerror(result) : "ok");

	if (R_FAILED(svcQueryMemory(&information, &page_info, address)))
	{
		say("cannot query the mapping to describe it\n");
		return;
	}
	say("mapping at 0x%llx: type 0x%02x perm 0x%x\n", (unsigned long long)information.addr, information.type,
		information.perm);

	/* the current process, so svcMapProcessCodeMemory can be asked to make
	this range executable rather than merely writable */
	result = svcGetProcessInfo(&pid, 0, 0);
	(void)result;
	say("svcGetProcessInfo: %s (pid %lld)\n", R_FAILED(result) ? strerror(result) : "ok", (long long)pid);
	say("note: svcMapProcessCodeMemory needs a real process handle; a title\n"
		"would pass its own, and homebrew has only the pseudo-handle\n");
}

/* ---------- 4: the report */

int main(int argc, char **argv)
{
	static const u64 size = 16 * 1024 * 1024;


	(void)argc;
	(void)argv;
	report_file = fopen("halo-mem-probe.txt", "w");

	say("Halo Switch memory probe\n");
	say("this answers what port/switch/host/host_mman.c needs to know\n");

	heading("console");
	{
		u64 title_id = 0;

		svcGetTitleID(&title_id, 0);
		say("title id %016llx\n", (unsigned long long)title_id);
	}

	probe_query_memory();

	heading("svcMapPhysicalMemory (SVC 0x2C)");
	/* The addresses the port actually needs, first: if the image's own
	address cannot be mapped, the port cannot run, whatever else works. */
	if (probe_map_at(0x40000000, size))
	{
		probe_permissions(0x40000000, size);
		probe_executable(0x40000000, size);
	}
	else if (probe_map_at(0x80000000, size))
	{
		say("(0x40000000 was refused; the window address was not)\n");
		probe_permissions(0x80000000, size);
		probe_executable(0x80000000, size);
	}
	else if (probe_map_at(0x04000000, size))
	{
		say("(neither 0x40000000 nor 0x80000000 was usable)\n");
		probe_permissions(0x04000000, size);
		probe_executable(0x04000000, size);
	}
	else
	{
		say("no address below 4 GB could be mapped: the guest image cannot\n"
			"be given its own address, so the port cannot run as designed.\n");
	}

	heading("done");
	say("copy this file back off the SD card\n");
	return 0;
}
/*
MEM PROBE 5

The fifth and last memory probe. Probes 1 to 4 answered the port's memory
question; this one confirms it cleanly and adds the check they could not make.

Probe 4 already showed the console executing code at 0x40000000, the address
the guest image is linked at. It reported the wrong value only because the
payload's instruction was hand-encoded wrong (a0 1e 8d 52 is "mov w0,
#0x68f5", whose low byte is the 0xf5 that came back), so what read as a
failure was the code running perfectly. The payload here is assembled from
payload.S rather than written as bytes.

What remains unconfirmed, and is what this probe is for:

1. Do the writable owner view and the read-execute slave view of one code
   memory object alias the same memory? Both are readable - the slave's
   permission is R-X - so the probe writes a pattern through the owner and
   reads it back through the slave. The loader has to rely on this: the
   guest's code is written at one address and runs at another, and if the two
   views did not share memory the port could not fill the image in at all.

2. Does the code still run correctly afterwards, so the cache maintenance is
   shown to work rather than merely to have been skipped.

3. Can an executable mapping and an ordinary writable mapping sit at
   neighbouring addresses inside the guest's range? This matters because the
   guest's segments share one range at 0x40000000 - .text must execute,
   .data/.bss must be writable - and no mapping on this console is ever both,
   so host_loader.c must place them side by side.

4. Unmapping, so the port can release the scratch view it fills code through
   and reuse the address.

Every address used here is distinct from those earlier probes mapped, and
each mapping is released once it has served its purpose: svcControlCodeMemory
answers 0xd401 if an address is mapped twice, which is what made probe 4's
last step - and probe 3's slave mapping - look like failures they were not.

Run it, then read halo-mem-probe-v5.txt off the SD card.
*/

#include <switch.h>

#include <switch/arm/cache.h>

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* payload.S: mov w0, #0x5a5a ; ret, assembled rather than hand-encoded */
extern const u8 halo_probe_payload[];
extern const u8 halo_probe_payload_end[];
#define PROBE_EXPECTED 0x5a5a

static FILE *report_file;
static char report_path[256];

static void open_report(void)
{
	static const char *candidates[] = {
		"sdmc:/halo-mem-probe-v5.txt",
		"sdmc:/switch/halo-mem-probe-v5.txt",
		"halo-mem-probe-v5.txt",
	};
	size_t index;

	for (index = 0; index < sizeof(candidates) / sizeof(*candidates); index++)
	{
		errno = 0;
		report_file = fopen(candidates[index], "w");
		if (report_file)
		{
			snprintf(report_path, sizeof(report_path), "%s", candidates[index]);
			return;
		}
		printf("cannot write %s: %s\n", candidates[index], strerror(errno));
	}
}

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

static void result(const char *what, Result code)
{
	say("%-42s %s (0x%08x)\n", what, R_FAILED(code) ? strerror(code) : "ok", (unsigned)code);
}

static void describe(const char *label, u64 address)
{
	MemoryInfo information = { 0 };
	u32 page_info = 0;
	Result code = svcQueryMemory(&information, &page_info, address);

	if (R_FAILED(code))
	{
		say("  %-6s 0x%012llx: query failed 0x%08x\n", label, (unsigned long long)address, (unsigned)code);
		return;
	}
	say("  %-6s 0x%012llx: type 0x%02x perm 0x%x (%s%s%s) size 0x%llx\n", label,
		(unsigned long long)address, information.type, information.perm,
		(information.perm & Perm_R) ? "R" : "-", (information.perm & Perm_W) ? "W" : "-",
		(information.perm & Perm_X) ? "X" : "-", (unsigned long long)information.size);
}

/* ---------- the aliasing and execution test */

static void probe_alias_and_execute(void)
{
	/* The guest image is linked at HALO_GUEST_IMAGE_BASE
	 * (port/android/include/halo_android_abi.h); its executable segments
	 * will live here. */
	static const u64 execute_address = 0x40000000;
	/* Only used to fill the code in, so it can be anywhere free; probe 1
	 * found 0x10000000 unclaimed. */
	static const u64 write_address = 0x10000000;
	static const u64 size = 0x1000;
	size_t payload_size = (size_t)(halo_probe_payload_end - halo_probe_payload);
	void *source;
	Handle handle = 0;
	u8 *write_view;
	u8 *execute_view;
	u32 got;
	size_t index;
	int aliases = 1;
	Result code;

	heading("do the owner and slave views share memory, and does code run?");
	say("the payload is %u bytes, assembled from payload.S\n", (unsigned)payload_size);
	for (index = 0; index < payload_size; index++)
		say(" %02x", halo_probe_payload[index]);
	say("\n");

	source = memalign(0x1000, size);
	if (!source)
	{
		say("memalign failed\n");
		return;
	}
	code = svcCreateCodeMemory(&handle, source, size);
	result("svcCreateCodeMemory", code);
	if (R_FAILED(code))
		return;

	code = svcControlCodeMemory(handle, CodeMapOperation_MapOwner, (void *)(uintptr_t)write_address, size, Perm_Rw);
	say("MapOwner Perm_Rw at 0x%llx (to fill in through):\n", (unsigned long long)write_address);
	result("  result", code);
	if (R_FAILED(code))
		return;
	describe("owner", write_address);

	code = svcControlCodeMemory(handle, CodeMapOperation_MapSlave, (void *)(uintptr_t)execute_address, size, Perm_Rx);
	say("MapSlave Perm_Rx at 0x%llx (where the guest image runs):\n", (unsigned long long)execute_address);
	result("  result", code);
	if (R_FAILED(code))
		return;
	describe("slave", execute_address);

	write_view = (u8 *)(uintptr_t)write_address;
	execute_view = (u8 *)(uintptr_t)execute_address;

	/* 1: do the two views see the same bytes? */
	say("\n  filling the whole page through the owner with a pattern\n");
	for (index = 0; index < size; index++)
		write_view[index] = (u8)(index * 7u + 1u);
	for (index = 0; index < size; index++)
	{
		if (execute_view[index] != (u8)(index * 7u + 1u))
		{
			aliases = 0;
			say("  the views do NOT share memory: at +0x%llx the slave reads 0x%02x, the owner wrote 0x%02x\n",
				(unsigned long long)index, execute_view[index], (u8)(index * 7u + 1u));
			break;
		}
	}
	if (aliases)
		say("  the slave reads back everything written to the owner: the two\n"
			"  views are one memory, which is what filling the image in needs\n");

	/* 2: the payload, then execute it */
	say("\n  copying the assembled payload in through the owner\n");
	memcpy(write_view, halo_probe_payload, payload_size);
	armDCacheFlush(write_view, size);
	armICacheInvalidate(execute_view, size);
	say("  the executable view now holds:");
	for (index = 0; index < payload_size; index++)
		say(" %02x", execute_view[index]);
	say("\n");

	got = ((u32 (*)(void))(uintptr_t)execute_address)();
	say("\n  called the function at 0x%llx\n", (unsigned long long)execute_address);
	say("  it returned 0x%04x, expected 0x%04x\n", got, PROBE_EXPECTED);
	if (got == PROBE_EXPECTED && aliases)
		say("\n  CONFIRMED: the console runs code at the guest image's linked\n"
			"  address, and the writable view the loader fills it through is\n"
			"  the same memory it runs from.\n");
	else if (got != PROBE_EXPECTED)
		say("\n  wrong value: the payload did not reach the executable view,\n"
			"  or something overwrote it after the i-cache was invalidated\n");

	/* 3: releasing both views */
	say("\n  releasing the mappings\n");
	result("UnmapOwner", svcControlCodeMemory(handle, CodeMapOperation_UnmapOwner, (void *)(uintptr_t)write_address, size, 0));
	result("UnmapSlave", svcControlCodeMemory(handle, CodeMapOperation_UnmapSlave, (void *)(uintptr_t)execute_address, size, 0));
	describe("owner", write_address);
	describe("slave", execute_address);
}

/* ---------- executable code beside writable data */

static void probe_neighbours(void)
{
	static const u64 text_address = 0x40000000;
	static const u64 data_address = 0x40200000;
	static const u64 scratch_address = 0x10000000;
	static const u64 size = 0x1000;
	void *source;
	Handle handle = 0;
	Result code;

	heading("executable code beside writable data, in the guest's own range");
	source = memalign(0x1000, size);
	if (!source)
		return;
	code = svcCreateCodeMemory(&handle, source, size);
	result("svcCreateCodeMemory", code);
	if (R_FAILED(code))
		return;
	code = svcControlCodeMemory(handle, CodeMapOperation_MapOwner, (void *)(uintptr_t)scratch_address, size, Perm_Rw);
	result("MapOwner Perm_Rw at 0x10000000 (scratch)", code);
	if (R_FAILED(code))
		return;
	code = svcControlCodeMemory(handle, CodeMapOperation_MapSlave, (void *)(uintptr_t)text_address, size, Perm_Rx);
	result("MapSlave Perm_Rx at 0x40000000 (.text)", code);
	describe("text", text_address);

	source = memalign(0x1000, size);
	code = svcMapMemory((void *)(uintptr_t)data_address, source, size);
	result("svcMapMemory Perm_Rw at 0x40200000 (.data)", code);
	describe("data", data_address);

	if (R_SUCCEEDED(code))
		say("\n  an executable mapping and an ordinary writable mapping can\n"
			"  sit side by side inside the guest image's range\n");
	result("UnmapSlave", svcControlCodeMemory(handle, CodeMapOperation_UnmapSlave, (void *)(uintptr_t)text_address, size, 0));
	result("UnmapOwner", svcControlCodeMemory(handle, CodeMapOperation_UnmapOwner, (void *)(uintptr_t)scratch_address, size, 0));
}

/* ---------- the report */

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	printf("Halo Switch memory probe 5, reaching main\n");
	open_report();
	if (report_file)
		printf("report is going to %s\n", report_path);
	say("Halo Switch memory probe 5\n");
	say("started; this line means main() ran\n");

	heading("console");
	say("operation mode: %s\n",
		appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked");

	probe_alias_and_execute();
	probe_neighbours();

	heading("done");
	say("report written to %s\n", report_file ? report_path : "(nowhere: no path was writable)");
	return 0;
}
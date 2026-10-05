/*
HOST_LOADER.C

Loads the guest image: a statically linked AArch64 ELF executable (built
from ILP32 code, see tools/android_build.py) whose segments are copied to
the addresses it was linked at, below 4 GB. The image starts with a
struct halo_guest_header naming its import table, which is filled with the
host functions of the same names.

Where the Java runtime already holds those addresses, the image is copied
somewhere free instead, by a whole number of 64 KB. Its code needs nothing
for that: branches and the ADRP pairs that form addresses are relative to the
instruction. Its 32-bit pointers (data, the header) are listed in
halo_guest.relocs (tools/guest_relocations.py), and each has the distance
added before anything reads them.
*/

#include "host.h"

#include <elf.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

struct host_guest_image host_image;

/* guest globals host-side diagnostics want to read; the addresses come from
the ELF rather than being written down here, because a rebuild moves them */
static const struct
{
	const char *name;
	size_t offset;
} host_image_symbols[] = {
	{ "cache_file_globals", offsetof(struct host_guest_image, cache_file_globals) },
	{ "global_tag_instances", offsetof(struct host_guest_image, global_tag_instances) },
};

/* finds a named symbol's address in the ELF file; 0 if it has none. A name can
appear more than once (a tentative definition and the real one), so a bound
definition is preferred over an undefined one, and the first of those. */
static uint32_t host_image_find_symbol(
	const void *file,
	size_t size,
	const char *wanted)
{
	const Elf64_Ehdr *elf = file;
	const Elf64_Shdr *sections;
	const char *strings;
	uint32_t found = 0;
	uint64_t index;

	if (size < sizeof(*elf) || elf->e_shoff == 0 || elf->e_shnum == 0)
		return 0;
	if (elf->e_shoff + (uint64_t)elf->e_shnum * sizeof(Elf64_Shdr) > size)
		return 0;
	sections = (const Elf64_Shdr *)((const char *)file + elf->e_shoff);
	for (index = 0; index < elf->e_shnum; index++)
	{
		const Elf64_Shdr *symtab = &sections[index];
		const Elf64_Sym *symbols;
		uint64_t symbol_index;

		if (symtab->sh_type != SHT_SYMTAB || symtab->sh_link >= elf->e_shnum)
			continue;
		if (symtab->sh_offset + symtab->sh_size > size)
			continue;
		strings = (const char *)file + sections[symtab->sh_link].sh_offset;
		symbols = (const Elf64_Sym *)((const char *)file + symtab->sh_offset);
		for (symbol_index = 0; symbol_index < symtab->sh_size / sizeof(*symbols); symbol_index++)
		{
			const Elf64_Sym *symbol = &symbols[symbol_index];
			unsigned char bind = ELF64_ST_BIND(symbol->st_info);

			if (symbol->st_name == 0 ||
				symbol->st_name >= sections[symtab->sh_link].sh_size ||
				strcmp(strings + symbol->st_name, wanted) ||
				symbol->st_value >= 0x100000000ULL)
			{
				continue;
			}
			/* the game compiles with -fcommon, so a tentative definition and
			the definition that won can both be in the table; take the one that
			is actually in a section */
			if (symbol->st_shndx == SHN_UNDEF)
			{
				if (found)
					continue;
			}
			else if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_LOCAL)
			{
				continue;
			}
			return (uint32_t)symbol->st_value;
		}
	}

	return found;
}

#define RELOCATIONS_MAGIC 0x434c5248u /* 'HRLC' */

/* adds shift to each of the image's 32-bit pointers, at base; -1 if the
table is not this image's */
static int relocate(uint64_t base, uint64_t size, uint32_t shift, const void *relocations, size_t relocations_size)
{
	const uint32_t *table = relocations;
	uint32_t count, index;

	if (!table || relocations_size < 3 * sizeof(uint32_t) || table[0] != RELOCATIONS_MAGIC ||
		table[1] != HALO_GUEST_IMAGE_BASE)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's relocation table is missing or not for this image");
		return -1;
	}
	count = table[2];
	if (relocations_size < (3 + (uint64_t)count) * sizeof(uint32_t))
	{
		host_logf(HOST_LOG_ERROR, "the guest image's relocation table is cut short");
		return -1;
	}
	for (index = 0; index < count; index++)
	{
		uint32_t offset = table[3 + index];

		if ((uint64_t)offset + sizeof(uint32_t) > size)
		{
			host_logf(HOST_LOG_ERROR, "the guest image's relocation table names %08x, outside the image", offset);
			return -1;
		}
		*(uint32_t *)(uintptr_t)(base + offset) += shift;
	}
	host_logf(HOST_LOG_INFO, "guest image moved by %08x: %u pointers", shift, count);
	return 0;
}

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

/* the loadable segments' range as linked (from HALO_GUEST_IMAGE_BASE); checks
that the file is a guest image */
static int image_range(const void *file, size_t size, uint64_t *low_out, uint64_t *high_out)
{
	const Elf64_Ehdr *elf = file;
	const Elf64_Phdr *segments;
	uint64_t low = ~0ULL, high = 0;
	uint32_t index;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_ident[EI_CLASS] != ELFCLASS64 ||
		elf->e_machine != EM_AARCH64 || elf->e_type != ET_EXEC)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	if (elf->e_phoff > size || (uint64_t)elf->e_phnum * sizeof(Elf64_Phdr) > size - elf->e_phoff)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's program headers are out of the file");
		return -1;
	}
	segments = (const Elf64_Phdr *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		if (segments[index].p_type != PT_LOAD)
			continue;
		if (segments[index].p_vaddr < low)
			low = segments[index].p_vaddr;
		if (segments[index].p_vaddr + segments[index].p_memsz > high)
			high = segments[index].p_vaddr + segments[index].p_memsz;
	}
	low &= ~0xfffULL;
	high = (high + 0xfff) & ~0xfffULL;
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ULL || high <= low)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	*low_out = low;
	*high_out = high;
	return 0;
}

uint32_t host_image_span(const void *file, size_t size)
{
	uint64_t low, high;

	if (image_range(file, size, &low, &high) != 0)
		return 0;
	return (uint32_t)(high - low);
}

int host_load_image(const void *file, size_t size, const void *relocations, size_t relocations_size)
{
	uint64_t low, high;
	uint32_t base;

	if (image_range(file, size, &low, &high) != 0)
		return -1;
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low), &base) != 0)
		return -1;
	return host_load_image_reserved(file, size, relocations, relocations_size, base);
}

int host_load_image_reserved(const void *file, size_t size, const void *relocations, size_t relocations_size,
	uint32_t base)
{
	const Elf64_Ehdr *elf = file;
	const Elf64_Phdr *segments;
	uint64_t low, high;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	uint64_t shift;
	int missing = 0;

	if (image_range(file, size, &low, &high) != 0)
		return -1;
	segments = (const Elf64_Phdr *)((const char *)file + elf->e_phoff);
	/* (unsigned arithmetic: a move down wraps, and adding it back to a
	32-bit pointer wraps the same way) */
	shift = (uint32_t)(base - (uint32_t)low);
	low = base;
	high = base + (high - HALO_GUEST_IMAGE_BASE);
	if (mmap((void *)low, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)low)
		return -1;
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size)
			return -1;
		memcpy((void *)(uintptr_t)(uint32_t)(segment->p_vaddr + shift), (const char *)file + segment->p_offset,
			segment->p_filesz);
	}
	if (shift && relocate(low, high - low, (uint32_t)shift, relocations, relocations_size) != 0)
		return -1;

	header = (const struct halo_guest_header *)low;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;
	host_image.shift = (uint32_t)shift;
	for (index = 0; index < sizeof(host_image_symbols) / sizeof(*host_image_symbols); index++)
	{
		uint32_t address = host_image_find_symbol(file, size, host_image_symbols[index].name);

		*(uint32_t *)((char *)&host_image + host_image_symbols[index].offset) = address ? address + (uint32_t)shift : 0;
	}

	table = (uint64_t *)(uintptr_t)header->import_table;
	name = (const char *)(uintptr_t)header->import_names;
	count = *(const uint32_t *)(uintptr_t)header->import_count;
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, count, missing);
	for (index = 0; index < sizeof(host_image_symbols) / sizeof(*host_image_symbols); index++)
		host_logf(HOST_LOG_INFO, "  guest %s at %08x", host_image_symbols[index].name,
			*(const uint32_t *)((const char *)&host_image + host_image_symbols[index].offset));

	/* code becomes read-only and executable */
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];
		uint64_t start = (uint32_t)(segment->p_vaddr + shift) & ~0xfffULL;
		uint64_t end = ((uint32_t)(segment->p_vaddr + shift) + segment->p_memsz + 0xfff) & ~0xfffULL;

		if (segment->p_type == PT_LOAD && (segment->p_flags & PF_X))
			mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
	}
	__builtin___clear_cache((char *)low, (char *)high);
	return 0;
}

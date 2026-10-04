/*
MAIN.CPP

The deko3d probe: answers, on the console, the questions the deko3d renderer
(port/switch/DEKO3D.md, phase 0) depends on, and writes the answers to
sdmc:/deko3d_probe.txt.

1. Whether SDL2's input and audio keep working when SDL opens no window and
   deko3d owns the display instead.
2. How long UAM's compiler takes on the console, for a small shader and for
   ones the size the game's translated shaders are.
3. Whether the GPU can read memory mapped the way the game's memory window
   is (port/switch/host/host_mman.c): heap aliased with svcMapMemory or with
   svcMapProcessCodeMemory. For each kind, a deko3d memory block is made on
   the alias and on the heap behind it, before and after aliasing, a
   triangle is drawn from vertices written through the alias, and the pixel
   it drew is read back - then the vertices are changed through the alias
   and drawn again, to see that the GPU sees CPU writes after a cache flush.
4. If the deko3d image has dumped the game's shaders (DEKO3D.md, phase 4),
   how long UAM takes on each of them, and on all of them together.

Each step is logged before it runs, so a crash leaves the step that crashed
as the last line. A case can be skipped by naming it on a line of
sdmc:/deko3d_probe_skip.txt.

At the end the screen is green if every memory case that ran passed, red if
one failed; press A (or wait 20 seconds) to exit, which also tests input.
*/

#include <switch.h>
#include <deko3d.h>
#include <SDL.h>

#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern "C" int probe_compile(int fragment, const char *glsl, const char *path);

#define LOG_PATH "sdmc:/deko3d_probe.txt"
#define SKIP_PATH "sdmc:/deko3d_probe_skip.txt"
#define SHADER_DIRECTORY "sdmc:/deko3d_probe"
#define CORPUS_DIRECTORY "sdmc:/halo_dk_shaders"

/* ---------- logging */

static FILE *log_file;

static void say(const char *format, ...)
{
	va_list arguments;

	if (!log_file)
		return;
	va_start(arguments, format);
	vfprintf(log_file, format, arguments);
	va_end(arguments);
	fputc('\n', log_file);
	fflush(log_file);
}

static double milliseconds_since(u64 start)
{
	return (double)armTicksToNs(armGetSystemTick() - start) / 1000000.0;
}

static char skip_list[1024];

static bool skipped(const char *name)
{
	const char *line = skip_list;
	size_t length = strlen(name);

	while (*line)
	{
		if (!strncmp(line, name, length) && (line[length] == '\n' || line[length] == '\r' || !line[length]))
			return true;
		line = strchr(line, '\n');
		if (!line)
			break;
		line++;
	}
	return false;
}

/* ---------- the device */

static DkDevice device;
static DkQueue queue;
static DkMemBlock command_memory;
static DkCmdBuf command_buffer;
static DkMemBlock code_memory;
static uint32_t code_used;

#define COMMAND_MEMORY_SIZE (1024 * 1024)
#define CODE_MEMORY_SIZE (1024 * 1024)

static void debug_callback(void *user, const char *context, DkResult result, const char *message)
{
	(void)user;
	say("  deko3d: %s: result %d: %s", context ? context : "?", (int)result, message ? message : "");
}

static DkMemBlock memory_block(uint32_t size, uint32_t flags, void *storage)
{
	DkMemBlockMaker maker;

	dkMemBlockMakerDefaults(&maker, device, (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
	maker.flags = flags;
	maker.storage = storage;
	return dkMemBlockCreate(&maker);
}

static bool device_create(void)
{
	DkDeviceMaker device_maker;
	DkQueueMaker queue_maker;
	DkCmdBufMaker command_maker;

	dkDeviceMakerDefaults(&device_maker);
	device_maker.cbDebug = debug_callback;
	/* Direct3D's conventions, which the renderer will use */
	device_maker.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
	device = dkDeviceCreate(&device_maker);
	if (!device)
		return false;
	dkQueueMakerDefaults(&queue_maker, device);
	queue_maker.flags = DkQueueFlags_Graphics;
	queue = dkQueueCreate(&queue_maker);
	command_memory = memory_block(COMMAND_MEMORY_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, NULL);
	code_memory = memory_block(CODE_MEMORY_SIZE,
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, NULL);
	if (!queue || !command_memory || !code_memory)
		return false;
	dkCmdBufMakerDefaults(&command_maker, device);
	command_buffer = dkCmdBufCreate(&command_maker);
	return command_buffer != NULL;
}

/* starts recording into the whole of the command memory */
static void commands_begin(void)
{
	dkCmdBufClear(command_buffer);
	dkCmdBufAddMemory(command_buffer, command_memory, 0, COMMAND_MEMORY_SIZE);
}

static void commands_run(void)
{
	dkQueueSubmitCommands(queue, dkCmdBufFinishList(command_buffer));
	dkQueueWaitIdle(queue);
}

/* ---------- shaders */

struct dksh_header
{
	uint32_t magic, header_size, control_size, code_size, programs_offset, program_count;
};

static bool shader_load(DkShader *shader, const char *path)
{
	FILE *file = fopen(path, "rb");
	struct dksh_header header;
	unsigned char *contents;
	long size;
	DkShaderMaker maker;

	if (!file)
	{
		say("  cannot open %s", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	contents = (unsigned char *)malloc((size_t)size);
	if (!contents || fread(contents, 1, (size_t)size, file) != (size_t)size)
	{
		fclose(file);
		free(contents);
		return false;
	}
	fclose(file);
	memcpy(&header, contents, sizeof(header));
	if (header.magic != 0x48534B44 || header.control_size + header.code_size > (uint32_t)size)
	{
		say("  %s is not a DKSH file", path);
		free(contents);
		return false;
	}
	code_used = (code_used + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
	if (code_used + header.code_size > CODE_MEMORY_SIZE - DK_SHADER_CODE_UNUSABLE_SIZE)
	{
		say("  no room for %s in the code memory", path);
		free(contents);
		return false;
	}
	memcpy((unsigned char *)dkMemBlockGetCpuAddr(code_memory) + code_used, contents + header.control_size,
		header.code_size);
	dkShaderMakerDefaults(&maker, code_memory, code_used);
	maker.control = contents;
	dkShaderInitialize(shader, &maker);
	code_used += header.code_size;
	free(contents);
	return dkShaderIsValid(shader);
}

static bool compile_timed(int fragment, const char *glsl, const char *name, double *milliseconds)
{
	char path[256];
	u64 start;
	int ok;

	snprintf(path, sizeof(path), SHADER_DIRECTORY "/%s.dksh", name);
	start = armGetSystemTick();
	ok = probe_compile(fragment, glsl, path);
	*milliseconds = milliseconds_since(start);
	say("compile %-20s %s in %.1f ms", name, ok ? "ok" : "FAILED", *milliseconds);
	return ok != 0;
}

static const char triangle_vertex[] =
	"#version 460\n"
	"layout(location = 0) in vec3 position;\n"
	"layout(location = 1) in vec4 color;\n"
	"layout(location = 0) out vec4 out_color;\n"
	"void main() { gl_Position = vec4(position, 1.0); out_color = color; }\n";

static const char triangle_fragment[] =
	"#version 460\n"
	"layout(location = 0) in vec4 in_color;\n"
	"layout(location = 0) out vec4 out_color;\n"
	"void main() { out_color = in_color; }\n";

/* the size of the game's: 192 constants, a program of some forty
instructions, and a pixel shader with four textures and eight combiner
stages. Their inputs are seeded by "seed" so that each compile is a
different shader. */
static void large_vertex(char *out, size_t size, int seed)
{
	size_t used = (size_t)snprintf(out, size,
		"#version 460\n"
		"layout(std140, binding = 0) uniform vertex_constants { vec4 c[192]; };\n"
		"layout(location = 0) in vec4 v0;\nlayout(location = 1) in vec4 v1;\nlayout(location = 2) in vec4 v2;\n"
		"layout(location = 3) in vec4 v3;\n"
		"layout(location = 0) out vec4 d0;\nlayout(location = 1) out vec4 d1;\n"
		"layout(location = 2) out vec4 t0;\nlayout(location = 3) out vec4 t1;\n"
		"layout(location = 4) out vec4 t2;\nlayout(location = 5) out vec4 t3;\n"
		"void main() {\n vec4 r0 = v0, r1 = v1, r2 = v2, r3 = v3;\n");
	int index;

	for (index = 0; index < 40 && used < size; index++)
	{
		int a = (index * 7 + seed) % 192, b = (index * 13 + seed * 3) % 192;

		used += (size_t)snprintf(out + used, size - used,
			index % 3 == 0 ? " r%d = vec4(dot(r%d, c[%d]), dot(r%d, c[%d]), dot(r%d, c[%d]), dot(r%d, c[%d]));\n" :
			index % 3 == 1 ? " r%d = r%d * c[%d] + r%d * c[%d] + r%d * c[%d] + r%d * c[%d];\n" :
			" r%d = max(r%d, c[%d]) + min(r%d, c[%d]) + r%d * c[%d] - r%d * c[%d];\n",
			index % 4, index % 4, a, (index + 1) % 4, b, (index + 2) % 4, (a + 1) % 192, (index + 3) % 4,
			(b + 1) % 192);
	}
	snprintf(out + used, size - used,
		" gl_Position = r0; d0 = r1; d1 = r2; t0 = r3; t1 = r0 * c[3]; t2 = r1 * c[4]; t3 = r2 * c[5];\n}\n");
}

static void large_fragment(char *out, size_t size, int seed)
{
	size_t used = (size_t)snprintf(out, size,
		"#version 460\n"
		"layout(std140, binding = 0) uniform pixel_constants { vec4 c0[8]; vec4 c1[8]; vec4 final_c0; vec4 final_c1; vec4 fog; };\n"
		"layout(binding = 0) uniform sampler2D tex0;\nlayout(binding = 1) uniform sampler2D tex1;\n"
		"layout(binding = 2) uniform samplerCube tex2;\nlayout(binding = 3) uniform sampler2D tex3;\n"
		"layout(location = 0) in vec4 d0;\nlayout(location = 1) in vec4 d1;\n"
		"layout(location = 2) in vec4 t0;\nlayout(location = 3) in vec4 t1;\n"
		"layout(location = 4) in vec4 t2;\nlayout(location = 5) in vec4 t3;\n"
		"layout(location = 0) out vec4 out_color;\n"
		"void main() {\n"
		" vec4 s0 = texture(tex0, t0.xy), s1 = texture(tex1, t1.xy), s2 = texture(tex2, t2.xyz), s3 = texture(tex3, t3.xy);\n"
		" vec4 r0 = d0, r1 = d1;\n");
	int stage;

	for (stage = 0; stage < 8 && used < size; stage++)
	{
		const char *a = (stage + seed) % 3 == 0 ? "s0" : (stage + seed) % 3 == 1 ? "s1" : "s2";
		const char *b = (stage + seed) % 2 ? "s3" : "r1";

		used += (size_t)snprintf(out + used, size - used,
			" { vec4 ab = %s * c0[%d], cd = %s * c1[%d];\n"
			"   float dot_ab = clamp(dot(%s.rgb * 2.0 - 1.0, r0.rgb * 2.0 - 1.0), 0.0, 1.0);\n"
			"   r0 = vec4(mix(ab.rgb + cd.rgb, vec3(dot_ab), %s), ab.a * cd.a);\n"
			"   r1 = clamp(r1 * ab + r0 * cd - 0.5, 0.0, 1.0); }\n",
			a, stage, b, stage, a, (stage + seed) % 2 ? "0.0" : "1.0");
	}
	snprintf(out + used, size - used,
		" vec3 color = mix(fog.rgb, r0.rgb * final_c0.rgb + r1.rgb * final_c1.rgb, clamp(d1.a, 0.0, 1.0));\n"
		" out_color = vec4(color, r0.a);\n}\n");
}

/* ---------- drawing a triangle into a small image and reading it back */

#define TARGET_SIZE 64

static DkMemBlock image_memory, readback_memory;
static DkImage target;
static DkShader triangle_shaders[2];

static bool target_create(void)
{
	DkImageLayoutMaker maker;
	DkImageLayout layout;

	dkImageLayoutMakerDefaults(&maker, device);
	maker.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = TARGET_SIZE;
	maker.dimensions[1] = TARGET_SIZE;
	dkImageLayoutInitialize(&layout, &maker);
	image_memory = memory_block((uint32_t)dkImageLayoutGetSize(&layout) + dkImageLayoutGetAlignment(&layout),
		DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, NULL);
	readback_memory = memory_block(TARGET_SIZE * TARGET_SIZE * 4, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	if (!image_memory || !readback_memory)
		return false;
	dkImageInitialize(&target, &layout, image_memory, 0);
	return true;
}

struct vertex
{
	float position[3];
	uint8_t color[4];
};

static void vertices_write(struct vertex *vertices, const uint8_t color[4])
{
	/* one triangle that covers the whole target */
	static const float positions[3][3] = { { -1.0f, -1.0f, 0.5f }, { 3.0f, -1.0f, 0.5f }, { -1.0f, 3.0f, 0.5f } };
	int index;

	for (index = 0; index < 3; index++)
	{
		memcpy(vertices[index].position, positions[index], sizeof(positions[index]));
		memcpy(vertices[index].color, color, 4);
	}
}

/* draws the triangle from vertices at gpu_address and returns the pixel at
the target's center */
static uint32_t draw_and_read(DkGpuAddr gpu_address)
{
	DkImageView view;
	DkViewport viewport = { 0.0f, 0.0f, (float)TARGET_SIZE, (float)TARGET_SIZE, 0.0f, 1.0f };
	DkScissor scissor = { 0, 0, TARGET_SIZE, TARGET_SIZE };
	DkRasterizerState rasterizer;
	DkColorState color;
	DkColorWriteState color_write;
	DkDepthStencilState depth_stencil;
	DkVtxAttribState attributes[2];
	DkVtxBufferState buffer = { sizeof(struct vertex), 0 };
	DkImageRect rectangle = { 0, 0, 0, TARGET_SIZE, TARGET_SIZE, 1 };
	DkCopyBuf copy = { dkMemBlockGetGpuAddr(readback_memory), 0, 0 };
	DkShader const *shaders[2] = { &triangle_shaders[0], &triangle_shaders[1] };
	const DkImageView *views[1] = { &view };
	const uint8_t *pixels;

	memset(attributes, 0, sizeof(attributes));
	attributes[0].bufferId = 0;
	attributes[0].offset = 0;
	attributes[0].size = DkVtxAttribSize_3x32;
	attributes[0].type = DkVtxAttribType_Float;
	attributes[1].bufferId = 0;
	attributes[1].offset = 12;
	attributes[1].size = DkVtxAttribSize_4x8;
	attributes[1].type = DkVtxAttribType_Unorm;
	dkRasterizerStateDefaults(&rasterizer);
	rasterizer.cullMode = DkFace_None;
	dkColorStateDefaults(&color);
	dkColorWriteStateDefaults(&color_write);
	dkDepthStencilStateDefaults(&depth_stencil);
	depth_stencil.depthTestEnable = false;
	depth_stencil.depthWriteEnable = false;
	dkImageViewDefaults(&view, &target);

	commands_begin();
	dkCmdBufBindRenderTargets(command_buffer, views, 1, NULL);
	dkCmdBufSetViewports(command_buffer, 0, &viewport, 1);
	dkCmdBufSetScissors(command_buffer, 0, &scissor, 1);
	dkCmdBufClearColorFloat(command_buffer, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
	dkCmdBufBindShaders(command_buffer, DkStageFlag_GraphicsMask, shaders, 2);
	dkCmdBufBindRasterizerState(command_buffer, &rasterizer);
	dkCmdBufBindColorState(command_buffer, &color);
	dkCmdBufBindColorWriteState(command_buffer, &color_write);
	dkCmdBufBindDepthStencilState(command_buffer, &depth_stencil);
	dkCmdBufBindVtxAttribState(command_buffer, attributes, 2);
	dkCmdBufBindVtxBufferState(command_buffer, &buffer, 1);
	dkCmdBufBindVtxBuffer(command_buffer, 0, gpu_address, 3 * sizeof(struct vertex));
	dkCmdBufDraw(command_buffer, DkPrimitive_Triangles, 3, 1, 0, 0);
	dkCmdBufBarrier(command_buffer, DkBarrier_Fragments, 0);
	dkCmdBufCopyImageToBuffer(command_buffer, &view, &rectangle, &copy, 0);
	commands_run();

	pixels = (const uint8_t *)dkMemBlockGetCpuAddr(readback_memory) +
		((TARGET_SIZE / 2) * TARGET_SIZE + TARGET_SIZE / 2) * 4;
	return (uint32_t)pixels[0] | (uint32_t)pixels[1] << 8 | (uint32_t)pixels[2] << 16 | (uint32_t)pixels[3] << 24;
}

static bool pixel_matches(uint32_t pixel, const uint8_t color[4])
{
	int channel;

	for (channel = 0; channel < 4; channel++)
	{
		int got = (int)((pixel >> (8 * channel)) & 0xff);

		if (abs(got - (int)color[channel]) > 1)
			return false;
	}
	return true;
}

/* ---------- the memory cases */

#define CASE_SIZE (64 * 1024)

static int cases_run, cases_passed;

/* block: a memory block over the case's memory; cpu: where the CPU writes
the vertices (the alias, if there is one) */
static bool case_draw(const char *name, DkMemBlock block, void *cpu)
{
	static const uint8_t first[4] = { 0x20, 0xc0, 0x40, 0xff };
	static const uint8_t second[4] = { 0xc0, 0x20, 0x80, 0xff };
	uint32_t pixel;
	bool ok_first, ok_second;

	cases_run++;
	say("  writing the vertices through %p", cpu);
	vertices_write((struct vertex *)cpu, first);
	armDCacheFlush(cpu, CASE_SIZE);
	say("  drawing from GPU address %010llx", (unsigned long long)dkMemBlockGetGpuAddr(block));
	pixel = draw_and_read(dkMemBlockGetGpuAddr(block));
	ok_first = pixel_matches(pixel, first);
	say("  first draw: pixel %08x, %s", (unsigned)pixel, ok_first ? "as written" : "WRONG");

	vertices_write((struct vertex *)cpu, second);
	armDCacheFlush(cpu, CASE_SIZE);
	pixel = draw_and_read(dkMemBlockGetGpuAddr(block));
	ok_second = pixel_matches(pixel, second);
	say("  after rewriting the vertices: pixel %08x, %s", (unsigned)pixel, ok_second ? "as rewritten" : "WRONG");
	if (ok_first && ok_second)
		cases_passed++;
	say("%s: %s", name, ok_first && ok_second ? "PASS" : "FAIL");
	return ok_first && ok_second;
}

enum alias_kind
{
	_alias_none,
	_alias_map_memory, /* svcMapMemory, into the stack region */
	_alias_code        /* svcMapProcessCodeMemory, made read-write */
};

static void *alias_create(enum alias_kind kind, void *backing)
{
	void *address = NULL;
	Result result;

	virtmemLock();
	if (kind == _alias_map_memory)
	{
		address = virtmemFindStack(CASE_SIZE, 0x4000);
		result = address ? svcMapMemory(address, backing, CASE_SIZE) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
	}
	else
	{
		Handle process = envGetOwnProcessHandle();

		address = virtmemFindCodeMemory(CASE_SIZE, 0x4000);
		result = address && process ?
			svcMapProcessCodeMemory(process, (u64)(uintptr_t)address, (u64)(uintptr_t)backing, CASE_SIZE) :
			MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		if (R_SUCCEEDED(result))
		{
			result = svcSetProcessMemoryPermission(process, (u64)(uintptr_t)address, CASE_SIZE, Perm_Rw);
			if (R_FAILED(result))
				svcUnmapProcessCodeMemory(process, (u64)(uintptr_t)address, (u64)(uintptr_t)backing, CASE_SIZE);
		}
	}
	virtmemUnlock();
	if (R_FAILED(result))
	{
		say("  the alias could not be made: 0x%08x", (unsigned)result);
		return NULL;
	}
	say("  heap at %p aliased at %p", backing, address);
	return address;
}

static void alias_destroy(enum alias_kind kind, void *alias, void *backing)
{
	Result result;

	if (kind == _alias_map_memory)
		result = svcUnmapMemory(alias, backing, CASE_SIZE);
	else
		result = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)(uintptr_t)alias, (u64)(uintptr_t)backing,
			CASE_SIZE);
	if (R_FAILED(result))
		say("  the alias could not be undone: 0x%08x", (unsigned)result);
}

/* deko3d aborts the program when it cannot make a memory block, release
build or debug, so whether the console will take the memory is asked first,
directly, in the two steps dkMemBlockCreate takes: an nvmap object on the
memory, then a mapping of it into a GPU address space (a private one here;
deko3d's own is not exposed) */
static NvAddressSpace test_address_space;
static bool test_address_space_ready;

static DkMemBlock case_memory_block(void *storage, uint32_t flags)
{
	NvMap map;
	iova_t address = 0;
	Result result = nvMapCreate(&map, storage, CASE_SIZE, 0x1000, NvKind_Pitch, true);

	if (R_FAILED(result))
	{
		say("  nvmap refuses %p: 0x%08x (module %u, description %u)", storage, (unsigned)result,
			(unsigned)R_MODULE(result), (unsigned)R_DESCRIPTION(result));
		return NULL;
	}
	say("  nvmap takes %p", storage);
	if (!test_address_space_ready)
	{
		say("  no test address space; not asking deko3d, which would abort");
		nvMapClose(&map);
		return NULL;
	}
	result = nvAddressSpaceMap(&test_address_space, nvMapGetHandle(&map), true, NvKind_Pitch, &address);
	if (R_FAILED(result))
	{
		say("  the GPU address space refuses it: 0x%08x (module %u, description %u)", (unsigned)result,
			(unsigned)R_MODULE(result), (unsigned)R_DESCRIPTION(result));
		nvMapClose(&map);
		return NULL;
	}
	say("  the GPU address space takes it");
	nvAddressSpaceUnmap(&test_address_space, address);
	nvMapClose(&map);
	return memory_block(CASE_SIZE, flags, storage);
}

/* on_alias: the memory block is made on the alias, else on the heap behind
it; gpu_first: the block is made before the alias (on the heap) */
static bool memory_case(const char *name, enum alias_kind kind, bool on_alias, bool gpu_first)
{
	void *backing, *alias = NULL;
	DkMemBlock block = NULL;
	uint32_t flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;

	if (skipped(name))
	{
		say("%s: skipped (%s)", name, SKIP_PATH);
		return false;
	}
	say("%s:", name);
	if (kind == _alias_code && !envGetOwnProcessHandle())
	{
		say("%s: skipped (no process handle; launched without hbloader's?)", name);
		return false;
	}
	backing = memalign(0x1000, CASE_SIZE);
	if (!backing)
	{
		say("%s: FAIL (no memory)", name);
		return false;
	}
	memset(backing, 0, CASE_SIZE);
	armDCacheFlush(backing, CASE_SIZE);
	if (gpu_first)
	{
		say("  making the memory block on the heap");
		block = case_memory_block(backing, flags);
		if (!block)
		{
			cases_run++;
			say("%s: FAIL (no memory block)", name);
			free(backing);
			return false;
		}
	}
	if (kind != _alias_none)
	{
		alias = alias_create(kind, backing);
		if (!alias)
		{
			cases_run++;
			say("%s: FAIL (no alias)", name);
			if (block)
				dkMemBlockDestroy(block);
			free(backing);
			return false;
		}
	}
	if (!gpu_first)
	{
		say("  making the memory block on the %s", on_alias ? "alias" : "heap behind it");
		block = case_memory_block(on_alias && alias ? alias : backing, flags);
		if (!block)
		{
			cases_run++;
			say("%s: FAIL (no memory block)", name);
			if (alias)
				alias_destroy(kind, alias, backing);
			free(backing);
			return false;
		}
	}
	bool passed = case_draw(name, block, alias ? alias : backing);
	dkMemBlockDestroy(block);
	if (alias)
		alias_destroy(kind, alias, backing);
	free(backing);
	return passed;
}

/* ---------- SDL: input and audio without a window */

static bool sdl_ready, audio_ready;
static SDL_GameController *controller;
static double tone_phase;

static void tone_callback(void *user, Uint8 *stream, int length)
{
	int16_t *samples = (int16_t *)stream;
	int index;

	(void)user;
	for (index = 0; index < length / 4; index++)
	{
		int16_t value = (int16_t)(sin(tone_phase) * 2000.0);

		samples[index * 2] = samples[index * 2 + 1] = value;
		tone_phase += 2.0 * M_PI * 440.0 / 48000.0;
	}
}

static void sdl_start(void)
{
	SDL_AudioSpec wanted, got;
	SDL_AudioDeviceID audio;

	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
	{
		say("SDL: SDL_Init without video failed: %s", SDL_GetError());
		return;
	}
	sdl_ready = true;
	say("SDL: initialised without video; %d joysticks", SDL_NumJoysticks());
	if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0))
	{
		controller = SDL_GameControllerOpen(0);
		say("SDL: controller 0 %s (%s)", controller ? "opened" : "not opened",
			controller ? SDL_GameControllerName(controller) : SDL_GetError());
	}
	memset(&wanted, 0, sizeof(wanted));
	wanted.freq = 48000;
	wanted.format = AUDIO_S16SYS;
	wanted.channels = 2;
	wanted.samples = 1024;
	wanted.callback = tone_callback;
	audio = SDL_OpenAudioDevice(NULL, 0, &wanted, &got, 0);
	if (!audio)
	{
		say("SDL: no audio device: %s", SDL_GetError());
		return;
	}
	audio_ready = true;
	say("SDL: audio open, %d Hz, %d channels; a quiet tone plays for a second", got.freq, got.channels);
	SDL_PauseAudioDevice(audio, 0);
	SDL_Delay(1000);
	SDL_PauseAudioDevice(audio, 1);
}

/* ---------- the swapchain, and the last screen */

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

static void present_until_a(bool all_passed)
{
	DkImageLayoutMaker maker;
	DkImageLayout layout;
	DkMemBlock memory;
	DkImage images[2];
	DkImage const *image_pointers[2] = { &images[0], &images[1] };
	DkSwapchainMaker swapchain_maker;
	DkSwapchain swapchain;
	uint32_t image_size;
	u64 start = armGetSystemTick();
	unsigned long frames = 0, events = 0;
	bool pressed = false;
	int index;
	DkViewport viewport = { 0.0f, 0.0f, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT, 0.0f, 1.0f };
	DkScissor scissor = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };

	dkImageLayoutMakerDefaults(&maker, device);
	maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = SCREEN_WIDTH;
	maker.dimensions[1] = SCREEN_HEIGHT;
	dkImageLayoutInitialize(&layout, &maker);
	image_size = (uint32_t)((dkImageLayoutGetSize(&layout) + dkImageLayoutGetAlignment(&layout) - 1) &
		~(uint64_t)(dkImageLayoutGetAlignment(&layout) - 1));
	memory = memory_block(2 * image_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, NULL);
	if (!memory)
	{
		say("swapchain: no memory for its images");
		return;
	}
	for (index = 0; index < 2; index++)
		dkImageInitialize(&images[index], &layout, memory, (uint32_t)index * image_size);
	dkSwapchainMakerDefaults(&swapchain_maker, device, nwindowGetDefault(), image_pointers, 2);
	swapchain = dkSwapchainCreate(&swapchain_maker);
	if (!swapchain)
	{
		say("swapchain: could not be made on the default window");
		return;
	}
	say("swapchain: made on the default window; showing %s until A is pressed or 20 seconds pass",
		all_passed ? "green" : "red");

	while (appletMainLoop() && !pressed && milliseconds_since(start) < 20000.0)
	{
		float pulse = 0.6f + 0.4f * (float)sin(milliseconds_since(start) / 300.0);
		int slot = dkQueueAcquireImage(queue, swapchain);
		DkImageView view;
		const DkImageView *views[1] = { &view };
		SDL_Event event;

		dkImageViewDefaults(&view, &images[slot]);
		commands_begin();
		dkCmdBufBindRenderTargets(command_buffer, views, 1, NULL);
		/* the queue keeps its state between command lists: without these the
		clear is cut to the last scissor set (the 64x64 test target's) */
		dkCmdBufSetViewports(command_buffer, 0, &viewport, 1);
		dkCmdBufSetScissors(command_buffer, 0, &scissor, 1);
		dkCmdBufClearColorFloat(command_buffer, 0, DkColorMask_RGBA, all_passed ? 0.0f : pulse,
			all_passed ? pulse : 0.0f, 0.0f, 1.0f);
		dkQueueSubmitCommands(queue, dkCmdBufFinishList(command_buffer));
		dkQueuePresentImage(queue, swapchain, slot);
		dkQueueWaitIdle(queue);
		frames++;
		if (frames == 1 || frames % 300 == 0)
			say("swapchain: %lu frames presented", frames);

		while (sdl_ready && SDL_PollEvent(&event))
		{
			events++;
			if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_JOYBUTTONDOWN)
				say("SDL input: %s button %d down", event.type == SDL_CONTROLLERBUTTONDOWN ? "controller" : "joystick",
					event.type == SDL_CONTROLLERBUTTONDOWN ? (int)event.cbutton.button : (int)event.jbutton.button);
			if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_A) ||
				(event.type == SDL_JOYBUTTONDOWN && event.jbutton.button == 0))
				pressed = true;
		}
	}
	say("swapchain: the loop ended (%s); %lu frames presented in %.1f s", pressed ? "A pressed" :
		milliseconds_since(start) >= 20000.0 ? "20 seconds passed" : "the applet asked to exit", frames,
		milliseconds_since(start) / 1000.0);
	say("SDL input: %lu events; A %s", events, pressed ? "pressed - input works" : "not seen");
	dkQueueWaitIdle(queue);
	dkSwapchainDestroy(swapchain);
	dkMemBlockDestroy(memory);
}

/* ---------- the game's dumped shaders, timed

4. If the deko3d image has dumped the game's shaders (DEKO3D.md, phase 4:
debug.gpu_dump_shaders = sdmc:/halo_dk_shaders), compile each with UAM and
log the time, then per stage the count, the total, the average and the
slowest ten. The total is what compiling every known key costs a console
that has none cached; the slowest are what a key first met in play costs
while its draws are skipped. */

#define SLOWEST_KEPT 10

struct slowest_one
{
	double milliseconds;
	char name[48];
};

static void slowest_note(struct slowest_one *slowest, double milliseconds, const char *name)
{
	int index, place = SLOWEST_KEPT;

	for (index = 0; index < SLOWEST_KEPT; index++)
	{
		if (milliseconds > slowest[index].milliseconds)
		{
			place = index;
			break;
		}
	}
	if (place == SLOWEST_KEPT)
		return;
	for (index = SLOWEST_KEPT - 1; index > place; index--)
		slowest[index] = slowest[index - 1];
	slowest[place].milliseconds = milliseconds;
	snprintf(slowest[place].name, sizeof(slowest[place].name), "%s", name);
}

static int name_compare(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static bool has_suffix(const char *name, const char *suffix)
{
	size_t name_length = strlen(name), suffix_length = strlen(suffix);

	return name_length > suffix_length && !strcmp(name + name_length - suffix_length, suffix);
}

/* the corpus as a sorted list of file names, or 0 (the caller says why) */
static size_t corpus_list(char (*names_out)[256])
{
	DIR *directory = opendir(CORPUS_DIRECTORY);
	struct dirent *entry;
	size_t count = 0;

	if (directory)
	{
		while ((entry = readdir(directory)) != NULL)
		{
			if (!has_suffix(entry->d_name, ".vert") && !has_suffix(entry->d_name, ".frag"))
				continue;
			snprintf(names_out[count], 256, "%s", entry->d_name);
			count++;
		}
		closedir(directory);
		qsort(names_out, count, sizeof(*names_out), name_compare);
	}
	return count;
}

/* ---------- the corpus on several threads

DEKO3D.md, phase 5, step 1: whether UAM's compiler, its locks made real and
its context made per thread (uam.patch), compiles the same bytes on two and
three threads as on one. Each run writes its DKSH files to its own folder and
every one is compared byte for byte with the one-thread run's. */

#define CORPUS_MAXIMUM 1024

static char corpus_names[CORPUS_MAXIMUM][256];
static size_t corpus_count;

struct corpus_share
{
	/* the folder this run's .dksh files go to */
	char directory[288];
	pthread_mutex_t lock;
	size_t next;
	unsigned long compiled, failed;
	/* the run's thread count and number, and how many workers have
	started (each takes the next of the process's cores) */
	unsigned long threads, run, started;
};

/* A thread libnx makes runs on the process's default core, and the
console's scheduler never moves it off the cores its mask allows
(port/switch/host/host_thread.c), so workers left alone would take turns on
one core: no faster than one thread, and racing only where one is pre-empted
mid-compile. Each worker moves itself onto the next of the cores the process
may use, and says where it runs, so the log shows the compiles were truly
side by side. */
static void worker_place(struct corpus_share *share)
{
	u64 allowed = 0;
	unsigned long mine, pick;
	int core = -1, index;
	Result result = 0;

	pthread_mutex_lock(&share->lock);
	mine = share->started++;
	pthread_mutex_unlock(&share->lock);
	if (R_SUCCEEDED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && allowed)
	{
		pick = mine % (unsigned long)__builtin_popcountll(allowed);
		for (index = 0; index < 64; index++)
		{
			if (!(allowed & (1ULL << index)))
				continue;
			if (!pick--)
			{
				core = index;
				break;
			}
		}
		if (core >= 0)
			result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
	}
	pthread_mutex_lock(&share->lock);
	say("corpus on %lu threads (run %lu): worker %lu runs on core %d (asked for core %d of mask %llx: 0x%x)",
		share->threads, share->run, mine, (int)svcGetCurrentProcessorNumber(), core, (unsigned long long)allowed,
		(unsigned)result);
	pthread_mutex_unlock(&share->lock);
}

static void *corpus_worker(void *context)
{
	struct corpus_share *share = (struct corpus_share *)context;

	worker_place(share);
	for (;;)
	{
		size_t index;
		char path[320], out_path[352];
		FILE *file;
		char *source;
		long size;
		int ok, fragment;

		pthread_mutex_lock(&share->lock);
		index = share->next;
		share->next = index < corpus_count ? index + 1 : index;
		pthread_mutex_unlock(&share->lock);
		if (index >= corpus_count)
			break;
		fragment = has_suffix(corpus_names[index], ".frag") ? 1 : 0;
		snprintf(path, sizeof(path), "%s/%s", CORPUS_DIRECTORY, corpus_names[index]);
		file = fopen(path, "rb");
		if (!file)
		{
			pthread_mutex_lock(&share->lock);
			share->failed++;
			pthread_mutex_unlock(&share->lock);
			continue;
		}
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		source = (char *)malloc((size_t)size + 1);
		if (!source || fread(source, 1, (size_t)size, file) != (size_t)size)
		{
			fclose(file);
			free(source);
			pthread_mutex_lock(&share->lock);
			share->failed++;
			pthread_mutex_unlock(&share->lock);
			continue;
		}
		fclose(file);
		source[size] = 0;
		snprintf(out_path, sizeof(out_path), "%s/%s.dksh", share->directory, corpus_names[index]);
		ok = probe_compile(fragment, source, out_path);
		free(source);
		pthread_mutex_lock(&share->lock);
		if (ok)
			share->compiled++;
		else
			share->failed++;
		pthread_mutex_unlock(&share->lock);
	}
	return NULL;
}

/* whether two files are the same bytes */
static bool file_matches(const char *one, const char *other)
{
	FILE *a = fopen(one, "rb"), *b = fopen(other, "rb");
	unsigned char buffer_one[4096], buffer_other[4096];
	size_t read_one, read_other;
	bool same = a && b;

	if (!a || !b)
	{
		if (a)
			fclose(a);
		if (b)
			fclose(b);
		return false;
	}
	while (same)
	{
		read_one = fread(buffer_one, 1, sizeof(buffer_one), a);
		read_other = fread(buffer_other, 1, sizeof(buffer_other), b);
		if (read_one != read_other || memcmp(buffer_one, buffer_other, read_one))
			same = false;
		if (read_one < sizeof(buffer_one))
			break;
	}
	fclose(a);
	fclose(b);
	return same;
}

/* the whole corpus on `threads` threads, its files compared with the
one-thread run's in SHADER_DIRECTORY */
static void corpus_threads(unsigned long threads, unsigned long run)
{
	struct corpus_share share;
	pthread_t thread_ids[3];
	pthread_attr_t attributes;
	char name[32];
	u64 start = armGetSystemTick();
	unsigned long index, different = 0, missing = 0;
	unsigned long thread_index;

	snprintf(name, sizeof(name), "t%lu_r%lu", threads, run);
	snprintf(share.directory, sizeof(share.directory), "%s/%s", SHADER_DIRECTORY, name);
	mkdir(share.directory, 0777);
	share.next = 0;
	share.compiled = share.failed = 0;
	share.threads = threads;
	share.run = run;
	share.started = 0;
	pthread_mutex_init(&share.lock, NULL);
	pthread_attr_init(&attributes);
	/* Mesa's compiler recurses; two megabytes a thread */
	pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
	for (thread_index = 0; thread_index < threads; thread_index++)
		pthread_create(&thread_ids[thread_index], &attributes, corpus_worker, &share);
	pthread_attr_destroy(&attributes);
	for (thread_index = 0; thread_index < threads; thread_index++)
		pthread_join(thread_ids[thread_index], NULL);
	pthread_mutex_destroy(&share.lock);
	for (index = 0; index < corpus_count; index++)
	{
		char one[352], other[384];

		snprintf(one, sizeof(one), "%s/%s.dksh", SHADER_DIRECTORY, corpus_names[index]);
		snprintf(other, sizeof(other), "%s/%s.dksh", share.directory, corpus_names[index]);
		if (!file_matches(one, other))
		{
			FILE *test = fopen(other, "rb");

			if (test)
				fclose(test);
			if (test)
				different++;
			else
				missing++;
		}
	}
	say("corpus on %lu threads (run %lu): %lu compiled, %lu failed, %lu different, %lu missing, %.1f ms",
		threads, run, share.compiled, share.failed, different, missing, milliseconds_since(start));
}

static void corpus_compile(void)
{
	double totals[2] = { 0.0, 0.0 };
	unsigned long counts[2] = { 0, 0 }, failures[2] = { 0, 0 };
	struct slowest_one slowest[2][SLOWEST_KEPT];
	size_t index;
	int stage;

	corpus_count = corpus_list(corpus_names);
	if (!corpus_count)
	{
		say("corpus: no %s (the deko3d image has not dumped the game's shaders); skipping", CORPUS_DIRECTORY);
		return;
	}
	say("corpus: compiling the %u shaders in %s (the probe's own thread is on core %d)", (unsigned)corpus_count,
		CORPUS_DIRECTORY, (int)svcGetCurrentProcessorNumber());
	memset(slowest, 0, sizeof(slowest));
	for (index = 0; index < corpus_count; index++)
	{
		char path[320], out_path[320];
		FILE *file;
		char *source;
		long size;
		u64 start;
		double milliseconds;
		int ok;

		stage = has_suffix(corpus_names[index], ".frag") ? 1 : 0;
		snprintf(path, sizeof(path), "%s/%s", CORPUS_DIRECTORY, corpus_names[index]);
		file = fopen(path, "rb");
		if (!file)
		{
			say("corpus %-48s cannot be opened", corpus_names[index]);
			failures[stage]++;
			continue;
		}
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		source = (char *)malloc((size_t)size + 1);
		if (!source || fread(source, 1, (size_t)size, file) != (size_t)size)
		{
			say("corpus %-48s cannot be read", corpus_names[index]);
			fclose(file);
			free(source);
			failures[stage]++;
			continue;
		}
		fclose(file);
		source[size] = 0;
		snprintf(out_path, sizeof(out_path), SHADER_DIRECTORY "/%s.dksh", corpus_names[index]);
		start = armGetSystemTick();
		ok = probe_compile(stage, source, out_path);
		milliseconds = milliseconds_since(start);
		free(source);
		say("corpus %-48s %s in %.1f ms", corpus_names[index], ok ? "ok" : "FAILED", milliseconds);
		if (ok)
		{
			counts[stage]++;
			totals[stage] += milliseconds;
			slowest_note(slowest[stage], milliseconds, corpus_names[index]);
		}
		else
		{
			failures[stage]++;
		}
	}
	for (stage = 0; stage < 2; stage++)
	{
		int place;

		say("corpus %s shaders: %lu compiled, %lu failed; total %.1f ms, average %.1f ms",
			stage ? "fragment" : "vertex", counts[stage], failures[stage], totals[stage],
			counts[stage] ? totals[stage] / (double)counts[stage] : 0.0);
		for (place = 0; place < SLOWEST_KEPT && slowest[stage][place].milliseconds > 0.0; place++)
			say("corpus %s slowest %2d: %-44s %.1f ms", stage ? "fragment" : "vertex", place + 1,
				slowest[stage][place].name, slowest[stage][place].milliseconds);
	}
	say("corpus: both stages together: %.1f ms", totals[0] + totals[1]);
}

/* ---------- the probe */

int main(int argc, char **argv)
{
	double milliseconds, total;
	int index;
	FILE *skip;
	static char source[2][32768];

	(void)argc;
	(void)argv;
	log_file = fopen(LOG_PATH, "w");
	mkdir(SHADER_DIRECTORY, 0777);
	skip = fopen(SKIP_PATH, "r");
	if (skip)
	{
		skip_list[fread(skip_list, 1, sizeof(skip_list) - 1, skip)] = 0;
		fclose(skip);
	}
	say("deko3d probe");
	say("process handle: %s", envGetOwnProcessHandle() ? "yes" : "no");

	/* 1. SDL first, as the game starts it: whatever it takes, deko3d needs
	the window after it */
	sdl_start();

	say("device: creating");
	if (!device_create())
	{
		say("device: FAILED");
		fclose(log_file);
		return 1;
	}
	say("device: ready");
	nvMapInit();
	{
		Result result = nvAddressSpaceCreate(&test_address_space, 0x10000);

		test_address_space_ready = R_SUCCEEDED(result);
		if (!test_address_space_ready)
			say("test address space: cannot be made: 0x%08x", (unsigned)result);
	}

	/* 2. compiling */
	if (!compile_timed(0, triangle_vertex, "triangle_vs", &milliseconds) ||
		!compile_timed(1, triangle_fragment, "triangle_fs", &milliseconds))
	{
		say("the triangle's shaders did not compile; stopping");
		fclose(log_file);
		return 1;
	}
	for (total = 0.0, index = 0; index < 5; index++)
	{
		char name[32];

		large_vertex(source[0], sizeof(source[0]), index);
		snprintf(name, sizeof(name), "large_vs_%d", index);
		compile_timed(0, source[0], name, &milliseconds);
		total += milliseconds;
	}
	say("large vertex shaders: %.1f ms each on average", total / 5.0);
	for (total = 0.0, index = 0; index < 5; index++)
	{
		char name[32];

		large_fragment(source[1], sizeof(source[1]), index);
		snprintf(name, sizeof(name), "large_fs_%d", index);
		compile_timed(1, source[1], name, &milliseconds);
		total += milliseconds;
	}
	say("large pixel shaders: %.1f ms each on average", total / 5.0);
	{
		u64 start = armGetSystemTick();
		DkShader shader;

		shader_load(&shader, SHADER_DIRECTORY "/large_fs_0.dksh");
		say("loading a compiled shader back from the card: %.2f ms", milliseconds_since(start));
	}

	if (!shader_load(&triangle_shaders[0], SHADER_DIRECTORY "/triangle_vs.dksh") ||
		!shader_load(&triangle_shaders[1], SHADER_DIRECTORY "/triangle_fs.dksh") || !target_create())
	{
		say("the triangle could not be set up; stopping");
		fclose(log_file);
		return 1;
	}

	/* 3. the memory cases, the safest first */
	/* the heap behind an alias, and an alias of memory already given to the
	GPU, are refused by the console (a mapped page cannot be both): those
	cases answer whether, and are expected to fail */
	{
		bool heap = memory_case("heap", _alias_none, false, false);
		bool map_memory, code;

		memory_case("map_memory/heap", _alias_map_memory, false, false);
		map_memory = memory_case("map_memory/alias", _alias_map_memory, true, false);
		memory_case("map_memory/gpu_first", _alias_map_memory, false, true);
		memory_case("code/heap", _alias_code, false, false);
		code = memory_case("code/alias", _alias_code, true, false);
		memory_case("code/gpu_first", _alias_code, false, true);
		say("memory: %d of %d cases passed; the GPU reads the window's kind of memory through its alias: %s",
			cases_passed, cases_run, heap && map_memory && code ? "yes" : "NO");
		corpus_compile();
		/* the one-thread run above wrote its DKSH files; the multi-thread
		runs compare theirs against them (three runs each, DEKO3D.md phase
		5 step 1) */
		{
			unsigned long threads, run;

			for (threads = 2; threads <= 3; threads++)
				for (run = 1; run <= 3; run++)
					corpus_threads(threads, run);
		}
		present_until_a(heap && map_memory && code);
	}

	dkQueueWaitIdle(queue);
	dkCmdBufDestroy(command_buffer);
	dkMemBlockDestroy(readback_memory);
	dkMemBlockDestroy(image_memory);
	dkMemBlockDestroy(code_memory);
	dkMemBlockDestroy(command_memory);
	dkQueueDestroy(queue);
	if (test_address_space_ready)
		nvAddressSpaceClose(&test_address_space);
	nvMapExit();
	dkDeviceDestroy(device);
	if (controller)
		SDL_GameControllerClose(controller);
	if (sdl_ready)
		SDL_Quit();
	say("done");
	fclose(log_file);
	return 0;
}

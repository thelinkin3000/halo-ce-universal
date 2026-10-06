/*
HOST.H

Internals of the Android port's host library (libmain.so). See
port/android/README.md for the overall design and
port/android/include/halo_android_abi.h for the guest contract.
*/

#ifndef __HALO_ANDROID_HOST_H
#define __HALO_ANDROID_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

/* ---------- logging (logcat tag "halo") */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* guest services also used inside the host (host_main.c) */
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. The host reserves the
Xbox window and the image's range at start-up, and hands out pages for
everything else (the guest's malloc arenas, thread stacks, anonymous
mappings) from pools of address space it reserves below 4 GB on demand. */

/* where the window was placed, for the guest's boot structure */
uint32_t host_memory_window_base(void);

/* claims the image's range (at preferred_base if it is free, else wherever
there is room: *base says where) and the Xbox window */
int host_memory_initialize(uint32_t preferred_base, uint32_t image_size, uint32_t *base);
/* starts the thread that reports the window's own contents (host_probe.c) */
void host_probe_start(void);
/* 1 if a fault on this thread, inside the window, is the probe's to
handle, and has been sent back to it */
int host_probe_skip_fault(uintptr_t address);
/* page-granular allocations below 4 GB; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* 1 if [address, address + size) was handed out by host_low_map or is one of
the fixed ranges */
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect/madvise/mremap (host_syscall.c) */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
	/* how far it was loaded from where it was linked (HALO_GUEST_IMAGE_BASE):
	0, unless the Java runtime held that address */
	uint32_t shift;
	/* addresses of named guest globals, found in the ELF's symbol table when
	the image was read, so that host-side diagnostics do not carry addresses
	that a rebuild would move (0 for a name the build does not have) */
	uint32_t cache_file_globals, global_tag_instances;
};

extern struct host_guest_image host_image;

/* the size of the address range an ELF file's loadable segments span, from
HALO_GUEST_IMAGE_BASE; 0 (after logging why) if it is not a guest image */
uint32_t host_image_span(const void *elf, size_t size);
/* maps the image from the ELF file in memory, at the address it was linked
at if it can, else elsewhere with its pointers moved by the relocation table
(tools/guest_relocations.py); returns 0 on success. The range is reserved here
(host_memory_initialize) ... */
int host_load_image(const void *elf, size_t size, const void *relocations, size_t relocations_size);
/* ... or, in the second form, before: the caller has called
host_memory_initialize for a range at least host_image_span(elf, size) long,
which it placed at base */
int host_load_image_reserved(const void *elf, size_t size, const void *relocations, size_t relocations_size,
	uint32_t base);

/* ---------- entering guest code (host_thread.c) */

/* calls the guest function at address with up to four 32-bit arguments on
this thread, which must have been made by host_native_thread_create (giving
the thread a guest struct pthread first if it has none); returns the
guest's w0 */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) with its stack in guest
memory, so that it can call guest code; the stack is freed after it exits.
Returns 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on the calling thread (one made by
host_native_thread_create); does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

/* ---------- debugging (host_debug.c) */

void host_debug_thread_started(void);
void host_debug_thread_exited(void);
/* config.toml's debug.sample_seconds: seconds between samples of the guest
threads, as text */
void host_debug_start_sampler(const char *setting);
/* config.toml's debug.profile_hz: samples of every guest thread a second, into
profile.bin in data_root; 0 does nothing */
void host_debug_start_profiler(int hz, const char *data_root);

/* ---------- the Vulkan probe (host_vk_probe.c; port/android/VULKAN.md, phase 0) */

/* runs the steps named in steps ("all", or a comma list of caps, memory,
compile, pipelines, draw, present) instead of the game, writes vk_probe.txt in
data_root and ends the app */
void host_vk_probe_run(const char *steps, const char *data_root, const char *vk_driver) __attribute__((noreturn));

/* ---------- the renderer (host_main.c, host_vk.c; port/android/VULKAN.md)

config.toml's display.renderer: "gl" (the default) runs halo_guest.elf, whose
renderer is OpenGL ES; "vulkan" runs halo_guest_vk.elf, whose renderer is
Vulkan, when Vulkan comes up on this device (host_vk_startup). Under Vulkan the
host makes no GL context and no EGL surface: the guest's SDL window is real
(Android lets one API own a window, and phase 2 makes its Vulkan surface on it)
but its GL context is a stand-in. Set once, before the guest starts. */
extern int host_renderer_vulkan;

/* config.toml's debug.gpu_stats: under Vulkan, host_gl.c reports the GL calls the
shared files still make (host_gl_resolve) every 10 seconds */
extern int host_gl_statistics;
/* once per frame, under Vulkan (host_gl.c): the report of the GL calls still made */
void host_gl_frame(void);

/* brings Vulkan up as far as an instance and a physical device and decides
whether the Vulkan image can run: the driver display.vk_driver names (host_vk_driver.c),
an instance (with the validation layer when validation is set and the app
carries it), and a physical device with a graphics queue, VK_KHR_swapchain and
dynamic rendering. Returns 1 and keeps them for the backend, or returns 0 after
destroying what it made. line is the text of the log line "renderer: ...", for
either outcome. Not thread safe; called once, by the thread that starts the
game, after the image's range is reserved and SDL's video is up */
int host_vk_startup(const char *vk_driver, int validation, char *line, size_t size);
/* the Vulkan renderer's commands from the guest (port/android/guest/vk_commands.h), run during the call;
commands is a guest address. host_vk_retired is the highest submission number the GPU has finished */
void host_vk_submit(uint32_t commands, uint32_t size);
uint32_t host_vk_retired(void);
/* the shader service (host_vk_shaders.c): see port/android/VULKAN.md, phase 5. find returns a handle (1 or more) of the
shader's module, or 0 and writes a VK_SHADER_STATUS_* to the guest address status_out; compile queues GLSL (a guest
address, copied during the call) for the compile thread. A 64-bit hash travels in one register */
uint32_t host_vk_shader_find(uint32_t stage, uint64_t hash, uint32_t status_out);
void host_vk_shader_compile(uint32_t stage, uint64_t hash, uint32_t glsl, uint32_t glsl_size);
/* whether the device reads a Vulkan format as a vertex attribute (host_vk_render.c), asked once for each by the guest */
uint32_t host_vk_format_supported(uint32_t format);
/* whether BC1 to BC3 images can be sampled with linear filtering and written by a copy (the guest sends them as they are if so,
and decodes them to BGRA if not) */
uint32_t host_vk_bc_supported(void);
/* the latest count of the game's visibility test slot, in the game's pixels (host_vk_visibility.c); never waits */
uint32_t host_vk_visibility(uint32_t index);
/* the name of the Vulkan driver archive in use (meta.json's), written into the guest's buffer at out of size bytes; an empty string
for the phone's own driver (host_vk_driver.c). Returns its length */
uint32_t host_vk_driver_name(uint32_t out, uint32_t size);
/* the game's exit: what the backend keeps on the device is written */
void host_vk_exit(void);
/* errors the validation layer has reported so far */
unsigned host_vk_validation_errors(void);
/* set once the backend presents frames, after which its swapchain paces them,
not the stand-in window's swap (phase 2) */
extern int host_vk_presenting;
/* config.toml's debug.vk_present_marker (host_vk_present.c) */
extern int host_vk_present_marker;
/* debug.vk_self_test: the backend's self-tests, run once at the device's creation (host_vk_render.c) */
extern int host_vk_self_test;
/* the game's window, the last made under Vulkan: an SDL_Window (host_sdl.c) */
extern void *host_vk_window;

/* ---------- import table (host_imports.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

void *host_gl_resolve(const char *name);

#endif

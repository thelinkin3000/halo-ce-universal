/*
HOST.H

Internals of the host that runs the ILP32 guest image: the Android port's
library (libmain.so), and the Linux executable of port/linux/host, which
shares every file here but host_main.c and host_touch.c. See
port/android/README.md for the overall design and
port/android/include/halo_android_abi.h for the guest contract.
*/

#ifndef __HALO_ANDROID_HOST_H
#define __HALO_ANDROID_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_android_abi.h"

/* ---------- logging and termination (host_common.c)

Each host's main sends the lines somewhere (host_log_set_sink): logcat (tag
"halo") on Android, the standard error stream on Linux. */

/* Android's log priorities (android/log.h) */
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
#define HOST_LOG_FATAL 7

/* writes one line (without its newline); called from signal handlers too, so
it must not take locks or use stdio */
typedef void (*host_log_sink)(int priority, const char *line);
void host_log_set_sink(host_log_sink sink);

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* (also the guest's, host_imports.list) */
void host_log(int priority, const char *text);
/* guest services also used inside the host (host_main.c) */
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- starting the guest (host_common.c) */

/* the guest's environment, which each host's main fills */
#define HOST_ENVIRONMENT_MAXIMUM 128

struct host_environment
{
	char *entries[HOST_ENVIRONMENT_MAXIMUM];
	int count;
};

/* sets name=value, replacing an entry of that name; an entry that does not
fit is logged and dropped */
void host_environment_set(struct host_environment *environment, const char *name, const char *value);
/* the same for an entry "name=value" (one of this process's environ) */
void host_environment_add(struct host_environment *environment, const char *entry);
/* POSIX TZ (the value) for the current local offset: the guest's musl has
no zone database */
void host_time_zone(char *buffer, size_t size);
/* copies argv (the program's name) and the environment into guest memory,
for __guest_start */
uint32_t host_make_boot(const struct host_environment *environment, const char *program);

/* the debugging settings of config.toml at path that the host reads itself:
debug.sample_seconds (the sampler, host_debug.c; or HALO_SAMPLE in the
host's environment) and debug.memory_watch (false: page hashes,
host_memory_watch_use_hashes). hashes_reason, if not NULL, says why the host
must use page hashes whatever the setting. */
void host_apply_debug_settings(const char *path, const char *hashes_reason);

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. The host reserves the
Xbox window and the image's range at start-up, and hands out pages for
everything else (the guest's malloc arenas, thread stacks, anonymous
mappings) from pools of address space it reserves below 4 GB on demand. */

/* where the window was placed, for the guest's boot structure */
uint32_t host_memory_window_base(void);
uint32_t host_memory_window_size(void);

/* where the window is to go (debug.window_base), if it will go there; 0
for the usual placement */
extern uint32_t host_memory_window_wanted;
/* and the image (debug.image_base): tried before its linked address, so the
image's shift can be tried on any device; 0 for the usual placement */
extern uint32_t host_memory_image_wanted;

/* claims the image's range (at preferred_base if it is free, else wherever
there is room: *base says where) and the Xbox window */
int host_memory_initialize(uint32_t preferred_base, uint32_t image_size, uint32_t *base);
/* holds the image's linked range as early as the process allows: from
JNI_OnLoad, before the Java side of the game's process allocates much
(host_main.c) */
void host_memory_reserve_early(void);
/* 1 if no room was found for the window or the image (host_memory_initialize) */
int host_memory_fixed_unavailable(void);
/* lists the mappings below 4 GB in logcat and, if file is not NULL, in it */
void host_memory_report_low_mappings(FILE *file);
/* Makes the write tracking compare page contents instead of catching page
faults (host_watch_hash.h), for when the app cannot receive its own
SIGSEGV (ARM translation). Call before the guest starts. */
void host_memory_watch_use_hashes(void);
/* starts the thread that reports the window's own contents (host_probe.c) */
void host_probe_start(void);
/* 1 if a fault on this thread, inside the window, is the probe's to
handle, and has been sent back to it */
int host_probe_skip_fault(uintptr_t address);
/* page-granular allocations below 4 GB; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* 1 if [address, address + size) lies in a pool (host_low_map's) or one of
the fixed ranges. Not whether it can be read: those hold reserved and
PROT_NONE pages too (host_arch_read_memory tells that). */
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect/madvise (host_syscall.c): only on the
pools, the fixed ranges and the ranges the guest mapped itself; -errno */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
long host_guest_madvise(uint64_t address, uint64_t size, int advice);
/* memory_watch_forget (also the guest's): the pages were remapped or
reprotected */
void host_memory_watch_forget(uint32_t address, uint32_t size);

/* the SIGSEGV, SIGBUS and SIGILL handlers: the write tracking's faults, and
a report of any other (host_memory.c); the main thread calls it first */
void host_install_signal_handlers(void);
/* gives the calling thread a stack of its own for those handlers, so that a
stack overflow can be reported (host_thread.c does it for its threads); 0
on success */
int host_signal_stack_install(void);
void host_signal_stack_remove(void);

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

/* The image's class and machine for this host's guest: a 64-bit AArch64
ELF file (arm64_32 code) on AArch64, a 32-bit x86-64 one (x32) on x86-64. */
#if defined(__aarch64__)
#define HOST_IMAGE_CLASS 2 /* ELFCLASS64 */
#define HOST_IMAGE_MACHINE 183 /* EM_AARCH64 */
#elif defined(__x86_64__)
#define HOST_IMAGE_CLASS 1 /* ELFCLASS32 */
#define HOST_IMAGE_MACHINE 62 /* EM_X86_64 */
#else
#error "no guest image for this architecture"
#endif

#define HOST_IMAGE_SEGMENTS_MAXIMUM 16

/* a loadable segment, in either ELF class */
struct host_image_segment
{
	uint64_t offset, address, file_size, memory_size;
	uint32_t flags;
};

struct host_image_layout
{
	uint64_t low, high; /* the page-aligned range the segments take */
	struct host_image_segment segments[HOST_IMAGE_SEGMENTS_MAXIMUM];
	uint32_t segment_count;
};

/* checks an ELF file before anything is mapped: its class and machine, a
static executable (no PT_DYNAMIC or PT_INTERP), and its loadable segments
inside the file and inside the image's reserved range
(HALO_GUEST_IMAGE_BASE, HALO_GUEST_IMAGE_RESERVE); fills layout. 0, or -1
with the reason logged. */
int host_image_validate(const void *file, size_t size, int elf_class, int machine, struct host_image_layout *layout);
/* checks the header's import table, its names and count, and its entry
points against the image, whose bytes from base to end are at memory (the
image itself once loaded); 0 with the import count, or -1 with the reason
logged */
int host_image_validate_imports(const uint8_t *memory, uint32_t base, uint32_t end,
	const struct host_image_layout *layout, uint32_t *count);

/* ---------- the processor's registers (host_arch.c) */

/* the program counter, stack pointer, frame pointer and link register of a
signal's ucontext_t; *lr is 0 where calls keep the return address on the
stack (x86-64) */
void host_arch_context(const void *ucontext, uint64_t *pc, uint64_t *sp, uint64_t *fp, uint64_t *lr);
/* the line'th line of the register dump of a ucontext_t into buffer; 0 past
the last line */
int host_arch_register_line(const void *ucontext, int line, char *buffer, size_t size);
/* copies size bytes at address into buffer, never faulting: 0 if any of
them cannot be read (unmapped, PROT_NONE). Async-signal-safe (it may change
errno). */
int host_arch_read_memory(uintptr_t address, void *buffer, size_t size);
/* follows the frame records from fp (both guests, and the host, keep the
caller's frame pointer then the return address, 8 bytes each), collecting
up to maximum return addresses; read copies a record (host_arch_read_memory,
or a test's), and the walk stops where it fails. Returns how many. */
int host_arch_walk_frames(uint64_t fp, int (*read)(uintptr_t address, void *buffer, size_t size),
	uint64_t *returns, int maximum);

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
/* a frame shown, `nanoseconds` after the last (the Vulkan renderer's present: host_vk_profile.c), for the profile's
slow frames; nothing unless the profiler runs */
void host_debug_profile_frame(uint64_t nanoseconds);
/* the run's profile folder, for the Vulkan renderer's gpu.txt (host_vk_profile.c); empty when no profile is
recorded (debug.profile_run: host_main.c) */
extern char host_vk_profile_folder[512];
extern int host_vk_profile_split;
/* debug.original_multisample: images of samples rather than VK_EXT_multisampled_render_to_single_sampled */
extern int host_vk_original_multisample;

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
shared files still make (host_gl_resolve) every 10 seconds, and host_vk_render.c
its counts every 60 frames */
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
/* the Vulkan renderer's commands from the guest (port/android/guest/vk_commands.h): run on the render thread
(debug.vk_render_thread) once the call has returned, or else during it; commands is a guest address, a stream the guest
writes again only VK_COMMAND_STREAMS - 1 hand-overs later. host_vk_retired is the highest submission number the GPU has
finished */
void host_vk_submit(uint32_t commands, uint32_t size);
uint32_t host_vk_retired(void);
/* the game's geometry at [address, address + size) (guest), written once as a map loads, copied now into a buffer the
GPU reads: its id (draws name it with VK_DATA_STATIC), or 0 if there is none (the draws put the bytes as before).
VK_COMMAND_STATIC_FREE lets it go (host_vk_data.c) */
uint32_t host_vk_static_make(uint32_t address, uint32_t size);
/* the shader service (host_vk_shaders.c): see port/android/VULKAN.md, phase 5. find returns a handle (1 or more) of the
shader's module, or 0 and writes a VK_SHADER_STATUS_* to the guest address status_out; compile queues GLSL (a guest
address, copied during the call) for the compile thread. A 64-bit hash travels in one register */
uint32_t host_vk_shader_find(uint32_t stage, uint64_t hash, uint32_t status_out);
void host_vk_shader_compile(uint32_t stage, uint64_t hash, uint32_t glsl, uint32_t glsl_size);
/* a queued shader compiled now, by the draw that needs it, or one a compile thread is making waited for, while the frame
has spent less than a frame's time on it: its handle, or 0 (the draw is left out until the threads make it) */
uint32_t host_vk_shader_make(uint32_t stage, uint64_t hash);
/* whether the device reads a Vulkan format as a vertex attribute (host_vk_render.c), asked once for each by the guest */
uint32_t host_vk_format_supported(uint32_t format);
/* whether BC1 to BC3 images can be sampled with linear filtering and written by a copy (the guest sends them as they are if so,
and decodes them to BGRA if not) */
uint32_t host_vk_bc_supported(void);
/* the most samples a pixel, up to samples, that the device draws the game's targets with (display.anti_aliasing's
multisampling); 1 if it cannot multisample them */
uint32_t host_vk_samples_supported(uint32_t samples);
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
/* debug.vk_render_thread: the guest's commands are recorded and presented on a thread of their own (host_vk_render.c) */
extern int host_vk_render_thread;
/* debug.vk_self_test: the backend's self-tests, run once at the device's creation (host_vk_render.c) */
extern int host_vk_self_test;
/* the game's window, the last made under Vulkan: an SDL_Window (host_sdl.c) */
extern void *host_vk_window;

/* ---------- import table (generated by tools/android_imports.py into the
build's host/host_import_table.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

/* SDL objects are 64-bit pointers, which the guest cannot hold: it gets
small handles into a table instead (host_sdl.c; port/linux/host's
host_desktop.c uses it too) */
enum host_sdl_handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
	_handle_renderer,
};

/* the object's handle (the same one again for an object that has one), or
0 for NULL or a full table */
uint32_t host_sdl_handle_new(int type, void *object);
/* the object of a handle of that type, or NULL */
void *host_sdl_handle_get(uint32_t handle, int type);
void host_sdl_handle_free(uint32_t handle);

/* SDL_ShowMessageBox's question and the player's answer (the guest's
struct message_box, port/android/guest/runtime/guest_sdl.c): 32-bit members
only, and one pointer to it, so that every argument of the call has a
register on x86-64 (tools/check_host_imports.py); title, message and
button_texts are guest addresses, the texts one after another, each NUL
terminated, in button_texts_size bytes */
#define HOST_MESSAGE_BOX_BUTTONS 8

struct host_message_box
{
	uint32_t flags;
	uint32_t title;
	uint32_t message;
	int32_t button_count;
	int32_t button_flags[HOST_MESSAGE_BOX_BUTTONS];
	int32_t button_ids[HOST_MESSAGE_BOX_BUTTONS];
	uint32_t button_texts;
	uint32_t button_texts_size;
	int32_t chosen;
};

typedef char host_message_box_size_check[sizeof(struct host_message_box) == 92 ? 1 : -1];

/* host_https_get's buffers' sizes and its answer (host_https.c), the guest's
layout (port/android/guest/runtime/guest_host.h) */
struct host_https_sizes
{
	int headers_size;
	int body_capacity;
	int error_size;
	/* the answer: its status, and how much of the body came */
	int status;
	int body_size;
};
typedef char host_https_sizes_size_check[sizeof(struct host_https_sizes) == 20 ? 1 : -1];
int host_https_get(const char *url, char *headers, char *body, char *error, struct host_https_sizes *sizes);

void *host_gl_resolve(const char *name);

#endif

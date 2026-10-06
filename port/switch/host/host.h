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

/* ---------- logging (logcat tag "halo")

The priorities are the NDK's android/log.h values, because the guest is
told what they are (port/android/guest/runtime/guest_host.h) and passes them
to host_logf; on the Switch they only choose the letter the log line starts
with, but keeping the numbers means the shared files need no changes. */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* the NDK's ANDROID_LOG_FATAL, which the Android host gets from
<android/log.h>; here it is spelled out so host_main.c needs no change */
#define HOST_LOG_FATAL 7
/* guest services also used inside the host (host_main.c) */
void host_exit(int code) __attribute__((noreturn));
/* puts back the clocks host_sdl2.c raised for the game */
void host_restore_clocks(void);
int host_errno(void);

/* One lock around every entry into the SD card driver. The logger already
 * holds its lock across each line's write and fsync; the guest's file calls
 * reach the same driver, which is not known to be thread-safe, so they take
 * the same lock rather than a second one - a second lock would still let a
 * guest read meet the logger's fsync inside the driver. Nothing that holds
 * this lock may log. */
void host_sd_lock(void);
void host_sd_unlock(void);

/* the host descriptor behind a guest one, or -1 (host_syscall.c) */
int host_guest_fd(int guest_fd);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. The host reserves the
Xbox window and the image's range at start-up, and hands out pages for
everything else (the guest's malloc arenas, thread stacks, anonymous
mappings) from pools of address space it reserves below 4 GB on demand. */

/* where the window was placed, for the guest's boot structure */
uint32_t host_memory_window_base(void);
uint32_t host_memory_image_base(void);

/* The base of the largest range this port has reserved and not yet used,
 * when one is at least `size`; 0 if none is. host_mman's allocator asks
 * for this so that a large mapping - the Xbox contiguous window - lands
 * where the rest of the port expects it rather than wherever its own search
 * happened to land. See host_memory.c. */
uint64_t host_memory_reserved_region(uint64_t size);

/* Keeps libnx's own mappings (thread stacks) out of the guest image's fixed
 * address. Called first thing in main, before any thread exists. */
void host_memory_hold_image_range(void);
/* logs where the kernel put the process's address space, heap, alias and
 * stack regions (host_memory.c) */
void host_memory_log_regions(void);
/* whether the guest's memory below 4 GB has to be code memory, svcMapMemory
 * mapping nowhere there (host_mman.c) */
int host_mman_low_code_mode(void);

/* Whether this port has set [address, address+size) aside, for the image, a
 * pool or the window. host_mman's allocator asks this before handing an
 * address out, so that the guest's heap does not grow into a range the rest
 * of the port is relying on. See host_memory.c. */
int host_memory_range_is_reserved(uint64_t address, uint64_t size);

/* Whether this console will actually map a range at that address. Reports
 * a range as usable only if a mapping was tried and accepted: asking the
 * kernel whether anything is there is not enough, because the console
 * refuses some addresses outright - 0x80000000, where the port wanted the
 * Xbox contiguous window, is reported free and cannot be mapped at any
 * size. The mapping made to find out is handed straight back. */
int host_can_map_at(uint64_t address, uint64_t length);

/* Lists the ranges this port has set aside, for the log. */
void host_memory_log_reservations(void);
/* Lists every extent the console can see, including the ones this port did
 * not make - libraries, heaps, the loader - so that an address in a crash
 * report can be attributed rather than guessed at. */

int host_memory_initialize(uint32_t image_base, uint32_t image_size);
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
	/* addresses of named guest globals, found in the ELF's symbol table when
	the image was read, so that host-side diagnostics do not carry addresses
	that a rebuild would move (0 for a name the build does not have) */
	uint32_t cache_file_globals, global_tag_instances;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c) */

/* Runs the guest on a stack below 4 GB (guest_stack.S). One way: the old
stack pointer is not saved and the call does not return, because the guest
ends the process and this is only used by the game thread. */
void guest_stack_enter(void *stack, void (*function)(void *), void *argument) __attribute__((noreturn));

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
/* moves the calling thread onto a core the game thread does not keep, for
threads that never run guest code (host_dk_shaders.c's compile thread);
logs "a thread starts on core N" when it does */
void host_thread_place_on_helper_core(void);

/* ---------- the renderer (host_main.c)

The game image, halo_guest.elf, draws with deko3d (port/switch/DEKO3D.md),
and this is always set. The host makes no EGL surface under it: the guest's
SDL window and GL context are stand-ins, so the display is left for deko3d's
swapchain. (The OpenGL-over-Mesa paths it skips are what is left of the
OpenGL image the Switch no longer builds.) */
extern int host_renderer_deko3d;

/* host_dk.c: runs a frame's commands from the deko3d renderer's guest half
(port/switch/guest/dk_commands.h); commands is a guest address */
void host_dk_submit(uint32_t commands, uint32_t size);
/* set once deko3d has presented a frame, after which its swapchain paces
the frames, not the stand-in window's swap */
extern int host_dk_presenting;
/* the highest submission (one a host_dk_submit) the GPU has finished; the
deko3d guest half's IsBusy and its locks wait against it (DEKO3D.md,
phase 3) */
uint32_t host_dk_retired(void);
/* called as each 16 MB chunk of the window is committed (host_memory.c);
under the deko3d renderer the chunk gets a deko3d memory block (phase 3) */
void host_dk_window_chunk_committed(uint64_t address, uint64_t size);
/* the deko3d device, made on the game thread (the only thread that touches
deko3d), for the shader cache's code memory (host_dk_shaders.c). It starts
the backend if the guest has not yet handed a frame over. (deko3d.h's
DkDevice, an opaque struct tag_DkDevice *; spelled out so this header needs
no deko3d.h.) */
struct tag_DkDevice *host_dk_device(void);

/* host_dk_compiler.cpp (UAM's compiler; DEKO3D.md, phase 5, step 2) or
host_dk_compiler_stub.c, when this build has no UAM: compiles GLSL into a
DKSH file at dksh_path (written complete: .tmp then renamed, by the caller).
Returns 0 on failure. Called on the compile thread only, and locked inside,
so nothing else can ever be compiling at the same time. */
int host_dk_compile_glsl(int fragment, const char *glsl, const char *dksh_path);

/* host_dk_shaders.c (DEKO3D.md, phase 5, step 3): the shader cache, called
by the guest through imports. stage: 0 vertex, 1 pixel. A shader is known by
a 64-bit hash of its key; the host never sees keys, only hashes and GLSL. */
/* a handle (1 or more) if the shader is in GPU code memory; else 0, and
*state (when state_out is a guest address) says which: 0 unknown, 1 queued
or compiling. Loads a shader whose file is on the card (~2 ms), on the game
thread, the only thread that touches deko3d. */
uint32_t host_dk_shader_find(uint32_t stage, uint64_t hash, uint32_t state_out);
/* queues the shader to be compiled in the background and written to the
card. glsl is a guest address, copied into host memory. Already queued: the
priority is raised if the new one is higher, and the GLSL is ignored (the
old copy compiles the same). Already on the card or compiled: nothing. */
void host_dk_shader_compile(uint32_t stage, uint64_t hash, uint32_t glsl, uint32_t glsl_size,
	uint32_t priority);
/* what the host has of a shader, without loading it: 0 nothing, 1 queued
or being compiled, 2 on the card, 3 loaded (the guest's startup pass) */
uint32_t host_dk_shader_known(uint32_t stage, uint64_t hash);
/* the DkShader (as a const void *) a handle of find's names, or NULL: for the
backend's draws, on the game thread */
const void *host_dk_shader(uint32_t handle);

/* ---------- debugging (host_debug.c) */

void host_debug_thread_started(int is_game);
void host_debug_thread_exited(void);
/* config.toml's debug.sample_seconds: seconds between samples of the guest
threads, as text */
void host_debug_start_sampler(const char *setting);
/* config.toml's debug.profiler (and debug.profile_hz): samples a second of the guest's threads,
counted and written to the profile folder (0 none); image names the guest
image running, for tools/switch_profile.py */
void host_debug_start_profiler(unsigned hz, const char *image);

/* The host's own stack, at the moment it decides it cannot continue. Called
 * from host_fatal and anywhere else that has a failure worth attributing. */
void host_backtrace(const char *reason);


/* ---------- HTTPS (host_https.c) */

/* the longest link followed or handed back (GitHub's signed download links
are some 900 characters) */
#define HOST_HTTPS_MAXIMUM_URL 2048

/* takes a piece of a body as it arrives (total is its whole length, or -1 if
the server did not say); returns 0 to stop */
typedef int (*host_https_body)(void *context, const void *data, size_t size, long long total);

/* GET url (https://...). A 200's body goes to body, in pieces; redirects are
followed if follow_redirects, and the last Location seen is copied to location
either way. Returns the last status, or -1 with error said. */
int host_https_get(const char *url, int follow_redirects, host_https_body body, void *context, char *location,
	size_t location_size, char *error, size_t error_size);

/* ---------- the updater (host_update.c) */

/* the folder the port's files are in: sdmc:/switch/halo (host_main.c) */
const char *host_executable_root(void);
/* the data root, the same folder by default: config.toml and the game's
maps are there, and the deko3d renderer's shader cache under it
(host_dk_shaders.c) */
const char *host_data_root(void);

/* before the game, with the screens up (host_ui_open): offers a newer release
of the port if there is one, and installs it if the player agrees, for the
next start; returns in every case, the session going on as it was. pad is a
PadState. */
void host_update_offer(void *pad);
/* first thing at start, before the game image is loaded: puts in place the
image an update left waiting (halo_guest.elf.new), if there is one */
void host_update_finish(void);

/* ---------- import table (host_imports.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

void *host_gl_resolve(const char *name);

#endif

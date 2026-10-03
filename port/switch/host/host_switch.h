/*
HOST_SWITCH.H

What the two halves of the SDL bridge share (port/switch/host): the
events host_sdl2.c reads from devkitPro's SDL2, in a form that
host_sdl3_events.c writes into the guest's SDL3 event. They are separate
files because the SDL2 and SDL3 headers cannot be included together.

The Knulli port this came from also declared its profiler and its OpenGL
call timer here (host_profile.c, host_gl_timing.c); neither is ported yet,
so those declarations are gone and the frames' statistics read the
Switch's own counters instead.
*/

#ifndef __HALO_SWITCH_HOST_H
#define __HALO_SWITCH_HOST_H

#include <stdint.h>

enum host_event_kind
{
	_host_event_quit,
	_host_event_key,
	_host_event_gamepad_added,
	_host_event_gamepad_removed,
	_host_event_focus_gained,
	_host_event_focus_lost,
};

struct host_event
{
	int kind;
	uint64_t timestamp_ns;
	uint32_t which;         /* joystick instance (gamepads), keyboard (keys) */
	int32_t scancode;       /* the same values in SDL2 and SDL3 */
	int32_t keycode;
	uint16_t modifiers;
	uint8_t down;
	uint8_t repeat;
};

/* writes the SDL3 SDL_Event (128 bytes) for event */
void host_event_to_sdl3(const struct host_event *event, void *sdl3_event);

/* SDL3's numbering of the values the guest passes (host_sdl3_events.c):
the attribute an SDL3 SDL_GLAttr names (enum host_gl_attribute; the
profile and flag values are the same in both versions), and the SDL3
SDL_GamepadType of an enum host_gamepad_kind */
int host_sdl3_gl_attribute(int sdl3_attribute);
int host_sdl3_gamepad_type(int kind);

enum host_gamepad_kind
{
	_host_gamepad_xbox360,
	_host_gamepad_xboxone,
	_host_gamepad_ps3,
	_host_gamepad_ps4,
	_host_gamepad_ps5,
	_host_gamepad_switch_pro,
};

/* the attributes host_sdl3_gl_attribute names */
enum host_gl_attribute
{
	_host_gl_unsupported = -1,
	_host_gl_red_size,
	_host_gl_green_size,
	_host_gl_blue_size,
	_host_gl_alpha_size,
	_host_gl_buffer_size,
	_host_gl_doublebuffer,
	_host_gl_depth_size,
	_host_gl_stencil_size,
	_host_gl_multisamplebuffers,
	_host_gl_multisamplesamples,
	_host_gl_context_major_version,
	_host_gl_context_minor_version,
	_host_gl_context_flags,
	_host_gl_context_profile_mask,
};

/* the generic timer: its count, and its counts a second. The physical count,
as libnx's armGetSystemTick reads it: the console does not let a program read
the virtual one (cntvct_el0), and reading it faults */
static inline uint64_t host_ticks(void)
{
	uint64_t value;

	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value) :: "memory");
	return value;
}

static inline uint64_t host_tick_frequency(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

#endif

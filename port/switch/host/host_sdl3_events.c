/*
HOST_SDL3_EVENTS.C

The SDL3 side of the Switch port's SDL bridge (host_sdl2.c): the guest was
built against SDL3's headers, so the events it reads are SDL3 SDL_Events
and the values it passes are SDL3's. Only the headers are used here; no
SDL3 library is linked.
*/

#include "host_switch.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_video.h>
#include <string.h>

_Static_assert(sizeof(SDL_Event) == 128, "the guest's SDL_Event is 128 bytes");

void host_event_to_sdl3(const struct host_event *event, void *sdl3_event)
{
	SDL_Event result;

	memset(&result, 0, sizeof(result));
	switch (event->kind)
	{
	case _host_event_quit:
		result.quit.type = SDL_EVENT_QUIT;
		result.quit.timestamp = event->timestamp_ns;
		break;
	case _host_event_key:
		result.key.type = event->down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		result.key.timestamp = event->timestamp_ns;
		result.key.which = event->which;
		result.key.scancode = (SDL_Scancode)event->scancode;
		result.key.key = (SDL_Keycode)event->keycode;
		result.key.mod = (SDL_Keymod)event->modifiers;
		result.key.down = event->down != 0;
		result.key.repeat = event->repeat != 0;
		break;
	case _host_event_gamepad_added:
	case _host_event_gamepad_removed:
		result.gdevice.type = event->kind == _host_event_gamepad_added ? SDL_EVENT_GAMEPAD_ADDED :
			SDL_EVENT_GAMEPAD_REMOVED;
		result.gdevice.timestamp = event->timestamp_ns;
		result.gdevice.which = (SDL_JoystickID)event->which;
		break;
	case _host_event_focus_gained:
	case _host_event_focus_lost:
		result.window.type = event->kind == _host_event_focus_gained ? SDL_EVENT_WINDOW_FOCUS_GAINED :
			SDL_EVENT_WINDOW_FOCUS_LOST;
		result.window.timestamp = event->timestamp_ns;
		break;
	}
	memcpy(sdl3_event, &result, sizeof(result));
}

int host_sdl3_gl_attribute(int sdl3_attribute)
{
	switch ((SDL_GLAttr)sdl3_attribute)
	{
	case SDL_GL_RED_SIZE: return _host_gl_red_size;
	case SDL_GL_GREEN_SIZE: return _host_gl_green_size;
	case SDL_GL_BLUE_SIZE: return _host_gl_blue_size;
	case SDL_GL_ALPHA_SIZE: return _host_gl_alpha_size;
	case SDL_GL_BUFFER_SIZE: return _host_gl_buffer_size;
	case SDL_GL_DOUBLEBUFFER: return _host_gl_doublebuffer;
	case SDL_GL_DEPTH_SIZE: return _host_gl_depth_size;
	case SDL_GL_STENCIL_SIZE: return _host_gl_stencil_size;
	case SDL_GL_MULTISAMPLEBUFFERS: return _host_gl_multisamplebuffers;
	case SDL_GL_MULTISAMPLESAMPLES: return _host_gl_multisamplesamples;
	case SDL_GL_CONTEXT_MAJOR_VERSION: return _host_gl_context_major_version;
	case SDL_GL_CONTEXT_MINOR_VERSION: return _host_gl_context_minor_version;
	case SDL_GL_CONTEXT_FLAGS: return _host_gl_context_flags;
	case SDL_GL_CONTEXT_PROFILE_MASK: return _host_gl_context_profile_mask;
	default: return _host_gl_unsupported;
	}
}

int host_sdl3_gamepad_type(int kind)
{
	switch (kind)
	{
	case _host_gamepad_xboxone: return SDL_GAMEPAD_TYPE_XBOXONE;
	case _host_gamepad_ps3: return SDL_GAMEPAD_TYPE_PS3;
	case _host_gamepad_ps4: return SDL_GAMEPAD_TYPE_PS4;
	case _host_gamepad_ps5: return SDL_GAMEPAD_TYPE_PS5;
	case _host_gamepad_switch_pro: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
	/* the handheld's own controls, which SDL2 knows only by their mapping */
	default: return SDL_GAMEPAD_TYPE_XBOX360;
	}
}

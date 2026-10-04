/*
HOST_DK_COMPILER_STUB.C

host_dk_compile_glsl for a build with no UAM (tools/switch_build.py links
this instead of host_dk_compiler.cpp when meson, bison, flex or mako is
missing): it fails, and says so once, so the deko3d renderer still runs with
the shaders already on the card and compiles nothing. The OpenGL renderer
never asks for a shader and never depends on UAM.
*/

#include "host.h"

int host_dk_compile_glsl(int fragment, const char *glsl, const char *dksh_path)
{
	static int said;

	(void)fragment;
	(void)glsl;
	(void)dksh_path;
	if (!said)
	{
		said = 1;
		host_logf(HOST_LOG_ERROR, "no shader compiler in this build (it needs meson, bison, flex and mako); the "
			"deko3d renderer will use only the shaders already on the card");
	}
	return 0;
}

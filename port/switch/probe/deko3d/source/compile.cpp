/*
COMPILE.CPP

UAM's compiler, run on the console: GLSL in, a DKSH file out. Built with
UAM's own include paths (see the Makefile), so that main.cpp needs none of
them.
*/

#include "compiler_iface.h"

extern "C" int probe_compile(int fragment, const char *glsl, const char *path)
{
	DekoCompiler compiler{fragment ? pipeline_stage_fragment : pipeline_stage_vertex};

	if (!compiler.CompileGlsl(glsl))
		return 0;
	compiler.OutputDksh(path);
	return 1;
}

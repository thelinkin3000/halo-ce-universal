/*
HOST_DK_COMPILER.CPP

UAM's compiler, linked into the host (DEKO3D.md, phase 5, step 2): GLSL in,
a DKSH file out, for the shader cache (host_dk_shaders.c). This is the one
file compiled with UAM's own include paths and defines (as the probe's
compile.cpp is), so nothing else in the host sees UAM at all; the build
renames every symbol this object and UAM's library define and keeps only
host_dk_compile_glsl, which is what lets UAM sit beside Mesa (both are
Mesa's GLSL compiler; the probe's Makefile found the clash and the fix).

The lock is not for the compile thread - there is one, and it is the only
caller - but for certainty: nothing else may ever be inside UAM at the same
time, and the lock costs nothing next to a compile.

The front end is initialised once and kept for the program's life
(uam.patch, phase 5, step 1: releasing it made every compile rebuild Mesa's
built-in functions, which was most of a compile's time).

The DKSH is written through WriteDksh into a file opened under the card's
one lock (host_sd_lock, host.h): the compile itself is CPU with no card in
it, and holding that lock across it put the game's own file calls behind a
tenth of a second at a time.

C++ because UAM is.
*/

#include "compiler_iface.h"

#include <pthread.h>
#include <stdio.h>

/* host_main.c's card lock (host.h), spelled out so this file needs none of
the host's headers: only UAM's own paths and defines compile it */
extern "C" void host_sd_lock(void);
extern "C" void host_sd_unlock(void);

extern "C" int host_dk_compile_glsl(int fragment, const char *glsl, const char *dksh_path)
{
	static pthread_mutex_t compile_lock = PTHREAD_MUTEX_INITIALIZER;
	int ok = 0;

	pthread_mutex_lock(&compile_lock);
	{
		DekoCompiler compiler{fragment ? pipeline_stage_fragment : pipeline_stage_vertex};

		if (compiler.CompileGlsl(glsl))
		{
			host_sd_lock();
			FILE *file = fopen(dksh_path, "wb");
			if (file)
			{
				/* written only if every byte was: a card that filled
				part way leaves a short file, which the caller deletes
				rather than putting in place */
				compiler.WriteDksh(file);
				ok = !ferror(file);
				if (fclose(file) != 0)
					ok = 0;
			}
			host_sd_unlock();
		}
	}
	pthread_mutex_unlock(&compile_lock);
	return ok;
}

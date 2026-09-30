/*
PHYSICAL_MEMORY_MAP.H

header included in hcex build.
*/

#ifndef __PHYSICAL_MEMORY_MAP_H
#define __PHYSICAL_MEMORY_MAP_H
#pragma once

/* ---------- constants */

/* port: the tag cache's size, which the loader checks a map's tag data and
structure bsps against (cache_files.c): the Xbox's 22 MB, or the native
builds' larger one (halo_port_capacity.h) */
#define TAG_CACHE_SIZE HALO_PORT_TAG_CACHE_SIZE

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/PHYSICAL_MEMORY_MAP.C */

void physical_memory_allocate(void);
void physical_memory_verify(void);
void physical_memory_free(void);

void *physical_memory_get_game_state_base_address(void);
void *physical_memory_get_tag_cache_base_address(void);
void *physical_memory_get_texture_cache_base_address(void);
void *physical_memory_get_sound_cache_base_address(void);

/* ---------- globals */

/* ---------- public code */

#endif // __PHYSICAL_MEMORY_MAP_H

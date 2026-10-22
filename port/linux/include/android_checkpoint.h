/*
ANDROID_CHECKPOINT.H

port: Android's checkpoint file protocol (source/saved games/game_state.c).
The guest handles requests on its game thread, never from a JNI callback.
*/
#ifndef __ANDROID_CHECKPOINT_H
#define __ANDROID_CHECKPOINT_H

#ifdef HALO_ANDROID
void android_checkpoint_boot_prepare(void);
void android_checkpoint_after_new_map(void);
void android_checkpoint_poll(unsigned long milliseconds);
/* The renderer that was actually linked/loaded, including Vulkan fallback. */
int android_checkpoint_renderer(void);
/* Copies the last checkpoint without reverting or changing the live state. */
int android_checkpoint_snapshot(void *image, long size);
#endif

#endif

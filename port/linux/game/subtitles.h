/* Language-neutral subtitle core. No game assets or translations. */
#ifndef HALO_SUBTITLES_H
#define HALO_SUBTITLES_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct subtitles subtitles;
typedef void (*subtitles_draw_fn)(void *user, const uint16_t *utf16);
subtitles *subtitles_create(void);
void subtitles_destroy(subtitles *s);
/* Transactional UTF-8 TSV load. Returns 1 on success, 0 on invalid data/OOM.
 * Copies the input; caller retains ownership. Successful load resets voices.
 * Rows: tag TAB pitch TAB permutation TAB begin_ms TAB end_ms TAB text.
 * Empty input unloads captions. Limits: 8 MiB, 20,000 cues, 1-hour offsets. */
int subtitles_load(subtitles *s, const char *data, size_t bytes);
void subtitles_reset(subtitles *s);
/* Use the selected sound's actual tag/pitch/permutation AFTER sound creation.
 * voice_id must identify that sound instance. now_ms is game time, not wall
 * time. Larger priority wins. Call reset on scenario/load/time discontinuity. */
void subtitles_sound_start(subtitles *s, int32_t voice_id, const char *tag,
                          uint16_t pitch, uint16_t permutation,
                          uint32_t now_ms, int priority);
void subtitles_sound_stop(subtitles *s, int32_t voice_id);
/* Call once per rendered game frame. visible=0 suppresses drawing in menus.
 * Callback receives borrowed UTF-16 valid until reload/destroy. Draw it
 * synchronously using the port's own font, bounds, wrapping and draw state.
 * Uses four voice slots and displays one cue, by priority then recency. */
void subtitles_render(subtitles *s, uint32_t now_ms, int visible,
                      subtitles_draw_fn draw, void *user);
#ifdef __cplusplus
}
#endif
#endif

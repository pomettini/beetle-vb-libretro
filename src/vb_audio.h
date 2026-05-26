#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct PlaydateAPI;

/* Register the audio source with the Playdate sound system. Call once at init. */
void vb_audio_init(struct PlaydateAPI *playdate);

/*
 * Push the samples produced by vb_run_frame() into the audio ring buffer.
 * Call once per frame after vb_run_frame().
 */
void vb_audio_push(void);

#ifdef __cplusplus
}
#endif

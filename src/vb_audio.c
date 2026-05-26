/*
 * vb_audio.c — Bridge the Blip_Buffer output to the Playdate audio system.
 *
 * The Playdate audio system pulls samples via a registered AudioSourceFunction
 * callback.  We decouple the VB frame rate (50 Hz, ~882 samples/frame) from
 * the callback by using a small power-of-two ring buffer.
 *
 * vb_sound_buf layout: interleaved stereo [L0, R0, L1, R1, ...]
 * with vb_sound_samples stereo pairs produced per frame.
 */

#include <string.h>
#include "pd_api.h"
#include "vb_core.h"
#include "vb_audio.h"

#define RING_LEN  4096   /* must be a power of 2, ≥ ~2× max samples/frame */
#define RING_MASK (RING_LEN - 1)

static int16_t ring_l[RING_LEN];
static int16_t ring_r[RING_LEN];
static volatile int ring_write = 0; /* written by main thread */
static volatile int ring_read  = 0; /* written by audio callback */

static int audio_callback(void *context, int16_t *left, int16_t *right, int len)
{
   (void)context;

   int avail = (ring_write - ring_read) & RING_MASK;

   for (int i = 0; i < len; i++)
   {
      if (avail > 0)
      {
         int rd     = ring_read & RING_MASK;
         left[i]    = ring_l[rd];
         right[i]   = ring_r[rd];
         ring_read  = (ring_read + 1) & RING_MASK;
         avail--;
      }
      else
      {
         left[i] = right[i] = 0; /* underrun: silence */
      }
   }
   return 1; /* keep source alive */
}

void vb_audio_init(PlaydateAPI *playdate)
{
   memset(ring_l, 0, sizeof(ring_l));
   memset(ring_r, 0, sizeof(ring_r));
   ring_write = 0;
   ring_read  = 0;

   playdate->sound->addSource(audio_callback, NULL, 0);
}

void vb_audio_push(void)
{
   /* vb_sound_buf: interleaved [L0, R0, L1, R1, ...] */
   int n = vb_sound_samples;
   for (int i = 0; i < n; i++)
   {
      int wr = ring_write & RING_MASK;
      ring_l[wr] = vb_sound_buf[i * 2];
      ring_r[wr] = vb_sound_buf[i * 2 + 1];
      ring_write = (ring_write + 1) & RING_MASK;
   }
}

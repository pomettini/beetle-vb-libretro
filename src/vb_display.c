/*
 * vb_display.c — Convert the VB 8-bpp grayscale framebuffer to the Playdate 1-bit LCD.
 *
 * The VIP produces a 384×224 8bpp grayscale image (WANT_8BPP). We apply 4×4
 * Bayer ordered dithering to simulate four shades on the 1-bit display.
 *
 * The 384×224 image is centred in the 400×240 display: X_OFFSET=8, Y_OFFSET=8.
 *
 * Playdate framebuffer: 1=white, 0=black, MSB=leftmost, stride=LCD_ROWSIZE (52).
 *
 * Because X_OFFSET=8 (one byte), VB pixels 0..383 land exactly on bytes 1..48
 * of each Playdate row. We process 8 VB pixels → 1 output byte per inner
 * iteration, eliminating per-pixel bit manipulation.
 */

#include <string.h>
#include "pd_api.h"
#include "vb_core.h"
#include "vb_display.h"

/* Bayer 4×4 thresholds pre-scaled to 0-255 (bayer4[r][c] * 17) */
static const uint8_t bayer4s[4][4] = {
   {   0, 136,  34, 170 },
   { 204,  68, 238, 102 },
   {  51, 187,  17, 153 },
   { 255, 119, 221,  85 }
};

#define X_OFFSET ((LCD_COLUMNS - VB_SCREEN_WIDTH)  / 2)   /* 8 */
#define Y_OFFSET ((LCD_ROWS    - VB_SCREEN_HEIGHT) / 2)   /* 8 */

void vb_render_frame(uint8_t *pd_fb)
{
   memset(pd_fb, 0xFF, (size_t)LCD_ROWSIZE * LCD_ROWS);

   for (int vy = 0; vy < VB_SCREEN_HEIGHT; vy++)
   {
      const uint8_t *src = vb_framebuffer + (size_t)vy * VB_SCREEN_WIDTH;
      /* X_OFFSET=8 → VB pixel 0 is byte 1 of the Playdate row */
      uint8_t *dst = pd_fb + (size_t)(vy + Y_OFFSET) * LCD_ROWSIZE + 1;
      const uint8_t *t = bayer4s[vy & 3];

      for (int vx = 0; vx < VB_SCREEN_WIDTH; vx += 8, src += 8, dst++)
      {
         uint8_t b = 0;
         if (src[0] >= t[0]) b  = 0x80;
         if (src[1] >= t[1]) b |= 0x40;
         if (src[2] >= t[2]) b |= 0x20;
         if (src[3] >= t[3]) b |= 0x10;
         if (src[4] >= t[0]) b |= 0x08;
         if (src[5] >= t[1]) b |= 0x04;
         if (src[6] >= t[2]) b |= 0x02;
         if (src[7] >= t[3]) b |= 0x01;
         *dst = b;
      }
   }
}

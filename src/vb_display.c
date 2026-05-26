/*
 * vb_display.c — Convert the VB 32-bpp ARGB framebuffer to the Playdate 1-bit LCD.
 *
 * The VIP produces a 384×224 ARGB32 image in anaglyph mode (left eye = white,
 * right eye = black), giving pixel values in four luminance levels:
 *   level 0 → 0x000000 (black)
 *   level 1 → dim gray
 *   level 2 → bright gray
 *   level 3 → 0xFFFFFF (white)
 *
 * We extract the green channel as luminance and apply 4×4 Bayer ordered
 * dithering to simulate these shades on the 1-bit display.
 *
 * The 384×224 image is centred in the 400×240 display:
 *   x_offset = 8, y_offset = 8
 *
 * Playdate framebuffer format (from pd_api.h):
 *   1 = white, 0 = black
 *   MSB = leftmost pixel in the byte
 *   Row stride = LCD_ROWSIZE bytes (52 for 400-pixel wide display)
 */

#include <string.h>
#include "pd_api.h"
#include "vb_core.h"
#include "vb_display.h"

/* Bayer 4×4 threshold matrix, range 0–15 (multiply by 17 for 0–255) */
static const uint8_t bayer4[4][4] = {
   {  0,  8,  2, 10 },
   { 12,  4, 14,  6 },
   {  3, 11,  1,  9 },
   { 15,  7, 13,  5 }
};

#define X_OFFSET ((LCD_COLUMNS - VB_SCREEN_WIDTH)  / 2)   /* 8 */
#define Y_OFFSET ((LCD_ROWS    - VB_SCREEN_HEIGHT) / 2)   /* 8 */

void vb_render_frame(uint8_t *pd_fb)
{
   /* Clear entire display to white (1 = white on Playdate) */
   memset(pd_fb, 0xFF, (size_t)LCD_ROWSIZE * LCD_ROWS);

   for (int vy = 0; vy < VB_SCREEN_HEIGHT; vy++)
   {
      const uint32_t *src_row = vb_framebuffer + vy * VB_SCREEN_WIDTH;
      int py = vy + Y_OFFSET;
      uint8_t *dst_row = pd_fb + py * LCD_ROWSIZE;

      for (int vx = 0; vx < VB_SCREEN_WIDTH; vx++)
      {
         /* Extract green channel as luminance (bits 8–15 of ARGB32) */
         uint8_t luma = (uint8_t)((src_row[vx] >> 8) & 0xFF);

         /* Bayer dither: threshold in 0–255 range */
         uint8_t threshold = (uint8_t)(bayer4[vy & 3][vx & 3] * 17);

         if (luma < threshold)
         {
            /* Black pixel: clear the bit */
            int px = vx + X_OFFSET;
            dst_row[px >> 3] &= (uint8_t)(~(0x80 >> (px & 7)));
         }
         /* else: white pixel — already set by memset */
      }
   }
}

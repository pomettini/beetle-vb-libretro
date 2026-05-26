#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert vb_framebuffer (384x224 ARGB32) to Playdate 1-bit LCD buffer.
 * Centres the image in the 400x240 display with 8px borders.
 * Uses 4x4 Bayer ordered dithering to simulate 4 gray shades.
 *
 * pd_fb must point to the buffer returned by playdate->graphics->getFrame().
 * Caller must call playdate->graphics->markUpdatedRows(0, LCD_ROWS-1) after.
 */
void vb_render_frame(uint8_t *pd_fb);

#ifdef __cplusplus
}
#endif

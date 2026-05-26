#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Virtual Boy screen dimensions (anaglyph / single-eye mode) */
#define VB_SCREEN_WIDTH  384
#define VB_SCREEN_HEIGHT 224

#ifdef __cplusplus
extern "C" {
#endif

/* 32-bpp ARGB framebuffer filled by vb_run_frame() */
extern uint32_t vb_framebuffer[VB_SCREEN_WIDTH * VB_SCREEN_HEIGHT];

/*
 * Interleaved stereo audio produced by vb_run_frame().
 * Layout: [L0, R0, L1, R1, ...], vb_sound_samples pairs total.
 * Blip_Buffer_read_samples() writes with stride 2.
 */
extern int16_t  vb_sound_buf[0x10000];
extern int      vb_sound_samples;

/*
 * Controller state written by vb_input.c before each vb_run_frame() call.
 * Bit layout mirrors libretro map[] order:
 *   0=A  1=B  2=R  3=L  4=RDU  5=RDR  6=LDR  7=LDL
 *   8=LDD  9=LDU  10=Start  11=Select  12=RDL  13=RDD
 */
extern uint16_t vb_input_buf;

bool vb_load_rom_data(const uint8_t *data, uint32_t size);
void vb_run_frame(void);
void vb_destroy(void);

/* Debug: set a log function so vb_core can emit serial checkpoints */
void vb_set_log(void (*fn)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif

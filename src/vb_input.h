#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read Playdate button state and crank, update vb_input_buf.
 * Call once per frame before vb_run_frame().
 */
void vb_update_input(uint32_t buttons, float crank_change);

#ifdef __cplusplus
}
#endif

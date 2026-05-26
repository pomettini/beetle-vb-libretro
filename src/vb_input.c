/*
 * vb_input.c — Map Playdate buttons + crank to the VB controller bit-field.
 *
 * vb_input_buf bit layout (matches libretro map[] order in the original code):
 *   bit  0 = A
 *   bit  1 = B
 *   bit  2 = R trigger
 *   bit  3 = L trigger
 *   bit  4 = Right D-pad Up
 *   bit  5 = Right D-pad Right
 *   bit  6 = Left D-pad Right
 *   bit  7 = Left D-pad Left
 *   bit  8 = Left D-pad Down
 *   bit  9 = Left D-pad Up
 *   bit 10 = Start
 *   bit 11 = Select
 *   bit 12 = Right D-pad Left
 *   bit 13 = Right D-pad Down
 */

#include "pd_api.h"
#include "vb_core.h"
#include "vb_input.h"

/* Crank threshold in degrees per frame to register a directional input */
#define CRANK_THRESHOLD 5.0f

void vb_update_input(uint32_t buttons, float crank_change)
{
   uint16_t buf = 0;

   /* Left D-pad → VB left D-pad */
   if (buttons & kButtonLeft)  buf |= (1 << 7);
   if (buttons & kButtonRight) buf |= (1 << 6);
   if (buttons & kButtonUp)    buf |= (1 << 9);
   if (buttons & kButtonDown)  buf |= (1 << 8);

   /* Face buttons */
   if (buttons & kButtonA) buf |= (1 << 0);
   if (buttons & kButtonB) buf |= (1 << 1);

   /* Crank → VB right D-pad left / right */
   if (crank_change >  CRANK_THRESHOLD) buf |= (1 << 5);  /* Right D-pad Right */
   if (crank_change < -CRANK_THRESHOLD) buf |= (1 << 12); /* Right D-pad Left  */

   vb_input_buf = buf;
}

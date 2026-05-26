/*
 * No-op stubs for the cheat/memory-patcher system.
 * mempatcher.cpp (the real implementation) uses std::vector which requires
 * libstdc++ — too heavy for the embedded Playdate target. Cheats are not
 * needed for a first-boot port; add a real implementation later if desired.
 */

#include <stdint.h>
#include <stdbool.h>
#include "mednafen/mempatcher.h"

bool MDFNMP_Init(uint32_t ps, uint32_t numpages)   { (void)ps; (void)numpages; return true; }
void MDFNMP_AddRAM(uint32_t size, uint32_t address, uint8_t *RAM) { (void)size; (void)address; (void)RAM; }
void MDFNMP_Kill(void)                              {}
void MDFNMP_InstallReadPatches(void)                {}
void MDFNMP_RemoveReadPatches(void)                 {}
void MDFNMP_ApplyPeriodicCheats(void)               {}

void MDFN_LoadGameCheats(void *override)            { (void)override; }
void MDFN_FlushGameCheats(int nosave)               { (void)nosave; }

/*
 * vb_core.cpp — Virtual Boy emulator core for Playdate.
 *
 * Derived from libretro.cpp (Mednafen/beetle-vb).  All libretro callbacks and
 * libretro-specific code have been removed; the Playdate-facing API is defined
 * in vb_core.h.
 *
 * Sections kept verbatim or near-verbatim from the original:
 *   RecalcIntLevel, VBIRQ_Assert, HWCTRL_Read/Write, MemRead/Write 8/16,
 *   FixNonEvents, EventReset, CalcNextTS, RebaseTS, VB_SetEvent, EventHandler,
 *   ForceEventUpdates, VB_Power, Load (→ vb_load_rom_data), Emulate (→ vb_run_frame),
 *   CloseGame (→ vb_destroy), StateAction, VB_ExitLoop.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef INLINE
#  define INLINE inline
#endif

#include "mednafen/mempatcher.h"
#include "mednafen/git.h"
#include "mednafen/state_helpers.h"
#include "mednafen/masmem.h"
#include "mednafen/settings.h"
#include "mednafen/vb/vb.h"
#include "mednafen/vb/timer.h"
#include "mednafen/vb/vsu.h"
#include "mednafen/vb/vip.h"
#include "mednafen/vb/input.h"
#include "mednafen/hw_cpu/v810/v810_cpu.h"

#include "vb_core.h"

/* Forward declarations for stubs in mempatcher_stub.c */
extern "C" {
   void MDFN_LoadGameCheats(void *override);
   void MDFN_FlushGameCheats(int nosave);
}

/* ── Debug log ─────────────────────────────────────────────────────────────── */

static void (*vb_log_fn)(const char *fmt, ...) = NULL;

void vb_set_log(void (*fn)(const char *fmt, ...))
{
   vb_log_fn = fn;
}

#define VB_LOG(...) do { if (vb_log_fn) vb_log_fn(__VA_ARGS__); } while(0)

/* ── Global state ──────────────────────────────────────────────────────────── */

V810 *VB_V810 = NULL;

static uint8  *WRAM  = NULL;
static uint8  *GPRAM = NULL;
static uint32  GPRAM_Mask;
static uint8  *GPROM = NULL;
static uint32  GPROM_Mask;

static uint32 VSU_CycleFix;
static uint8  WCR;

static int32 next_vip_ts, next_timer_ts, next_input_ts;
static uint32 IRQ_Asserted;

static Blip_Buffer sbuf[2];

/* ── Public state (accessed from main.c, vb_display.c, vb_audio.c) ────────── */

uint8_t  vb_framebuffer[VB_SCREEN_WIDTH * VB_SCREEN_HEIGHT];
int16_t  vb_sound_buf[0x10000];
int      vb_sound_samples = 0;
uint16_t vb_input_buf     = 0;

static uint8_t vb_low_battery = 0;
static uint32_t vb_frame_count = 0;
static uint32_t dbg_vip_calls = 0;

bool vb_frame_rendered = false;

/* Render 1 in every VB_RENDER_EVERY_N frames; skip the rest for speed */
#define VB_RENDER_EVERY_N 8
static int vb_render_skip_counter = 0;

/* ── Memory access profiling ────────────────────────────────────────────────── */
static uint32_t vip_reads_window = 0; /* VIP reads since last EventHandler call */
static bool     vb_idle_mode    = false;

static uint32_t dbg_rd_vip   = 0;  /* space 0: VIP registers/DRAM */
static uint32_t dbg_rd_wram  = 0;  /* space 5: WRAM */
static uint32_t dbg_rd_gprom = 0;  /* space 7: cart ROM */
static uint32_t dbg_rd_other = 0;  /* VSU, HWCTRL, GPRAM */
static uint32_t dbg_wr_vip   = 0;
static uint32_t dbg_wr_wram  = 0;
static uint32_t dbg_wr_other = 0;

static struct MDFN_Surface surf;

/* ── IRQ helpers ────────────────────────────────────────────────────────────── */

static INLINE void RecalcIntLevel(void)
{
   int i, ilevel = -1;
   for (i = 4; i >= 0; i--)
   {
      if (IRQ_Asserted & (1 << i))
      {
         ilevel = i;
         break;
      }
   }
   VB_V810->SetInt(ilevel);
}

extern "C" void VBIRQ_Assert(int source, bool assert_)
{
   assert(source >= 0 && source <= 4);
   IRQ_Asserted &= ~(1 << source);
   if (assert_)
      IRQ_Asserted |= (1 << source);
   RecalcIntLevel();
}

/* ── Hardware register access ─────────────────────────────────────────────── */

static uint8 HWCTRL_Read(v810_timestamp_t &timestamp, uint32 A)
{
   if (A & 0x3)
      return 0;

   switch (A & 0xFF)
   {
      case 0x18:
      case 0x1C:
      case 0x20:
         return TIMER_Read(timestamp, A);
      case 0x24:
         return WCR | 0xFC;
      case 0x10:
      case 0x14:
      case 0x28:
         return VBINPUT_Read(timestamp, A);
   }
   return 0;
}

static void HWCTRL_Write(v810_timestamp_t &timestamp, uint32 A, uint8 V)
{
   if (A & 0x3)
      return;

   switch (A & 0xFF)
   {
      case 0x18:
      case 0x1C:
      case 0x20:
         TIMER_Write(timestamp, A, V);
         break;
      case 0x24:
         WCR = V & 0x3;
         break;
      case 0x10:
      case 0x14:
      case 0x28:
         VBINPUT_Write(timestamp, A, V);
         break;
   }
}

/* ── Memory bus callbacks ───────────────────────────────────────────────────── */

uint8 MDFN_FASTCALL MemRead8(v810_timestamp_t &timestamp, uint32 A)
{
   A &= (1 << 27) - 1;
   switch (A >> 24)
   {
      case 0: dbg_rd_vip++;   return VIP_Read8(timestamp, A);
      case 2: dbg_rd_other++; return HWCTRL_Read(timestamp, A);
      case 1: case 3: case 4: dbg_rd_other++; break;
      case 5: dbg_rd_wram++;  return WRAM[A & 0xFFFF];
      case 6: dbg_rd_other++; if (GPRAM) return GPRAM[A & GPRAM_Mask]; break;
      case 7: dbg_rd_gprom++; return GPROM[A & GPROM_Mask];
   }
   return 0;
}

uint16 MDFN_FASTCALL MemRead16(v810_timestamp_t &timestamp, uint32 A)
{
   A &= (1 << 27) - 1;
   switch (A >> 24)
   {
      case 0: dbg_rd_vip++; vip_reads_window++; return VIP_Read16(timestamp, A);
      case 2: dbg_rd_other++; return HWCTRL_Read(timestamp, A);
      case 1: case 3: case 4: dbg_rd_other++; break;
      case 5: dbg_rd_wram++;  return LoadU16_LE((uint16 *)&WRAM[A & 0xFFFF]);
      case 6: dbg_rd_other++; if (GPRAM) return LoadU16_LE((uint16 *)&GPRAM[A & GPRAM_Mask]); break;
      case 7: dbg_rd_gprom++; return LoadU16_LE((uint16 *)&GPROM[A & GPROM_Mask]);
   }
   return 0;
}

void MDFN_FASTCALL MemWrite8(v810_timestamp_t &timestamp, uint32 A, uint8 V)
{
   A &= (1 << 27) - 1;
   switch (A >> 24)
   {
      case 0: dbg_wr_vip++;   VIP_Write8(timestamp, A, V);  break;
      case 1: dbg_wr_other++; VSU_Write((timestamp + VSU_CycleFix) >> 2, A, V); break;
      case 2: dbg_wr_other++; HWCTRL_Write(timestamp, A, V); break;
      case 5: dbg_wr_wram++;  WRAM[A & 0xFFFF] = V; break;
      case 6: dbg_wr_other++; if (GPRAM) GPRAM[A & GPRAM_Mask] = V; break;
      case 3: case 4: case 7: dbg_wr_other++; break;
   }
}

void MDFN_FASTCALL MemWrite16(v810_timestamp_t &timestamp, uint32 A, uint16 V)
{
   A &= (1 << 27) - 1;
   switch (A >> 24)
   {
      case 0: dbg_wr_vip++;   VIP_Write16(timestamp, A, V); break;
      case 1: dbg_wr_other++; VSU_Write((timestamp + VSU_CycleFix) >> 2, A, V); break;
      case 2: dbg_wr_other++; HWCTRL_Write(timestamp, A, V); break;
      case 5: dbg_wr_wram++;  StoreU16_LE((uint16 *)&WRAM[A & 0xFFFF], V); break;
      case 6: dbg_wr_other++; if (GPRAM) StoreU16_LE((uint16 *)&GPRAM[A & GPRAM_Mask], V); break;
      case 3: case 4: case 7: dbg_wr_other++; break;
   }
}

/* ── Event scheduling ────────────────────────────────────────────────────────── */

static void FixNonEvents(void)
{
   if (next_vip_ts   & 0x40000000) next_vip_ts   = VB_EVENT_NONONO;
   if (next_timer_ts & 0x40000000) next_timer_ts = VB_EVENT_NONONO;
   if (next_input_ts & 0x40000000) next_input_ts = VB_EVENT_NONONO;
}

static void EventReset(void)
{
   next_vip_ts   = VB_EVENT_NONONO;
   next_timer_ts = VB_EVENT_NONONO;
   next_input_ts = VB_EVENT_NONONO;
}

static INLINE int32 CalcNextTS(void)
{
   int32 next = next_vip_ts;
   if (next > next_timer_ts) next = next_timer_ts;
   if (next > next_input_ts) next = next_input_ts;
   return next;
}

static void RebaseTS(const v810_timestamp_t timestamp)
{
   assert(next_vip_ts   > timestamp);
   assert(next_timer_ts > timestamp);
   assert(next_input_ts > timestamp);
   next_vip_ts   -= timestamp;
   next_timer_ts -= timestamp;
   next_input_ts -= timestamp;
}

extern "C" void VB_SetEvent(const int type, const v810_timestamp_t next_timestamp)
{
   if      (type == VB_EVENT_VIP)   next_vip_ts   = next_timestamp;
   else if (type == VB_EVENT_TIMER) next_timer_ts = next_timestamp;
   else if (type == VB_EVENT_INPUT) next_input_ts = next_timestamp;

   if (next_timestamp < VB_V810->GetEventNT())
      VB_V810->SetEventNT(next_timestamp);
}

/* One VB frame = 20 MHz / 50 Hz = 400 000 cycles. Allow 2× as a safety budget. */
#define VB_FRAME_BUDGET 800000

static int32 MDFN_FASTCALL EventHandler(const v810_timestamp_t timestamp)
{
   if (timestamp >= next_vip_ts) {
      next_vip_ts = VIP_Update(timestamp);
      dbg_vip_calls++;
   }
   if (timestamp >= next_timer_ts) next_timer_ts = TIMER_Update(timestamp);
   if (timestamp >= next_input_ts) next_input_ts = VBINPUT_Update(timestamp);

   if (timestamp >= VB_FRAME_BUDGET)
      VB_ExitLoop();

   /* Idle skip: if the game is busy-waiting on VIP (≥10 VIP reads per 259-cycle
      window) and no IRQ is pending, inject HALT so the inner instruction loop is
      skipped entirely — timestamp_rl jumps straight to next_event_ts each outer
      iteration instead of executing hundreds of no-op polling instructions. */
   uint32_t reads = vip_reads_window;
   vip_reads_window = 0;
   if (IRQ_Asserted != 0)
      vb_idle_mode = false;
   else if (reads >= 10)
      vb_idle_mode = true;
   VB_V810->SetIdleHalt(vb_idle_mode);

   return CalcNextTS();
}

static void ForceEventUpdates(const v810_timestamp_t timestamp)
{
   next_vip_ts   = VIP_Update(timestamp);
   next_timer_ts = TIMER_Update(timestamp);
   next_input_ts = VBINPUT_Update(timestamp);
   VB_V810->SetEventNT(CalcNextTS());
}

/* ── Power / reset ────────────────────────────────────────────────────────────── */

static void VB_Power(void)
{
   memset(WRAM, 0, 65536);
   VIP_Power();
   VSU_Power();
   TIMER_Power();
   VBINPUT_Power();
   EventReset();
   IRQ_Asserted = 0;
   RecalcIntLevel();
   VB_V810->Reset();
   VSU_CycleFix = 0;
   WCR = 0;
   ForceEventUpdates(0);
}

extern "C" void VB_ExitLoop(void)
{
   VB_V810->Exit();
}

/* ── Helpers ──────────────────────────────────────────────────────────────────── */

static INLINE uint32 round_up_pow2(uint32 v)
{
   v--;
   v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
   v |= v >> 8;  v |= v >> 16;
   v++;
   v += (v == 0);
   return v;
}

/* ── Public API ──────────────────────────────────────────────────────────────── */

bool vb_load_rom_data(const uint8_t *data, uint32_t size)
{
   uint32_t *Map_Addresses;
   uint32_t  map_size = 0;
   uint64_t  A, sub_A;

   VB_LOG("[VB] build " __DATE__ " " __TIME__);
   VB_LOG("[VB] load_rom_data: size=%u", (unsigned)size);

   if (size != round_up_pow2(size)) return false;
   if (size < 256)                  return false;
   if (size > (1 << 24))           return false;

   /* ── Configure display settings for grayscale output ── */
   setting_vb_lcolor        = 0xFFFFFF;
   setting_vb_rcolor        = 0x000000;
   setting_vb_default_color = 0xFFFFFF;
   setting_vb_3dmode        = VB3DMODE_ANAGLYPH;
   setting_vb_cpu_emulation = 0; /* V810_EMU_MODE_FAST */

   VB_LOG("[VB] new V810");
   VB_V810 = new V810();
   VB_LOG("[VB] V810::Init FAST");
   VB_V810->Init((V810_Emu_Mode)setting_vb_cpu_emulation, true);
   VB_LOG("[VB] V810::Init done");

   VB_V810->SetMemReadHandlers (MemRead8,  MemRead16,  NULL);
   VB_V810->SetMemWriteHandlers(MemWrite8, MemWrite16, NULL);
   VB_V810->SetIOReadHandlers  (MemRead8,  MemRead16,  NULL);
   VB_V810->SetIOWriteHandlers (MemWrite8, MemWrite16, NULL);

   for (int i = 0; i < 256; i++)
   {
      VB_V810->SetMemReadBus32 (i, false);
      VB_V810->SetMemWriteBus32(i, false);
   }

   Map_Addresses = (uint32_t *)malloc(8192 * 4);
   if (!Map_Addresses) return false;

   VB_LOG("[VB] SetFastMap WRAM");
   for (A = 0; A < 1ULL << 32; A += (1 << 27))
      for (sub_A = 5ULL << 24; sub_A < (6ULL << 24); sub_A += 65536)
         Map_Addresses[map_size++] = (uint32_t)(A + sub_A);
   WRAM = VB_V810->SetFastMap(Map_Addresses, 65536, map_size, "WRAM");
   VB_LOG("[VB] WRAM=%p", WRAM);

   VB_LOG("[VB] SetFastMap ROM mask=%u", (unsigned)((size < 65536) ? (65536 - 1) : (size - 1)));
   GPROM_Mask = (size < 65536) ? (65536 - 1) : (size - 1);
   map_size = 0;
   for (A = 0; A < 1ULL << 32; A += (1 << 27))
      for (sub_A = 7ULL << 24; sub_A < (8ULL << 24); sub_A += GPROM_Mask + 1)
         Map_Addresses[map_size++] = (uint32_t)(A + sub_A);
   GPROM = VB_V810->SetFastMap(Map_Addresses, GPROM_Mask + 1, map_size, "Cart ROM");
   VB_LOG("[VB] GPROM=%p", GPROM);

   for (uint64_t i = 0; i < 65536; i += size)
      memcpy(GPROM + i, data, size);

   VB_LOG("[VB] SetFastMap RAM");
   GPRAM_Mask = 0xFFFF;
   map_size = 0;
   for (A = 0; A < 1ULL << 32; A += (1 << 27))
      for (sub_A = 6ULL << 24; sub_A < (7ULL << 24); sub_A += GPRAM_Mask + 1)
         Map_Addresses[map_size++] = (uint32_t)(A + sub_A);
   GPRAM = VB_V810->SetFastMap(Map_Addresses, GPRAM_Mask + 1, map_size, "Cart RAM");
   VB_LOG("[VB] GPRAM=%p", GPRAM);

   free(Map_Addresses);
   memset(GPRAM, 0, GPRAM_Mask + 1);

   VB_LOG("[VB] VIP_Init");
   VIP_Init();
   VB_LOG("[VB] VSU_Init");
   VSU_Init(&sbuf[0], &sbuf[1]);
   VB_LOG("[VB] VBINPUT_Init");
   VBINPUT_Init();

   VIP_Set3DMode(VB3DMODE_ANAGLYPH, false, 1, 0);
   VIP_SetAnaglyphColors(0xFFFFFF, 0x000000);
   VIP_SetDefaultColor(0xFFFFFF);
   VIP_SetParallaxDisable(false);
   VIP_SetInstantDisplayHack(true);
   VIP_SetAllowDrawSkip(true);
   VBINPUT_SetInstantReadHack(true);

   surf.pixels8     = vb_framebuffer;
   surf.w           = VB_SCREEN_WIDTH;
   surf.h           = VB_SCREEN_HEIGHT;
   surf.pitchinpix  = VB_SCREEN_WIDTH;
   surf.pitch32     = VB_SCREEN_WIDTH;
   surf.format.bpp        = 8;
   surf.format.colorspace = MDFN_COLORSPACE_RGB;
   surf.format.Rshift     = 0;
   surf.format.Gshift     = 0;
   surf.format.Bshift     = 0;
   surf.format.Ashift     = 0;

   VBINPUT_SetInput(0, "gamepad", &vb_input_buf);
   VBINPUT_SetInput(1, "gamepad", &vb_low_battery);

   for (int y = 0; y < 2; y++)
   {
      Blip_Buffer_set_sample_rate(&sbuf[y], 44100, 50);
      Blip_Buffer_set_clock_rate (&sbuf[y], (long)(VB_MASTER_CLOCK / 4));
      Blip_Buffer_bass_freq      (&sbuf[y], 20);
   }

   MDFN_LoadGameCheats(NULL);
   MDFNMP_Init(32768, ((uint64_t)1 << 27) / 32768);
   MDFNMP_AddRAM(65536, 5 << 24, WRAM);
   if ((GPRAM_Mask + 1) >= 32768)
      MDFNMP_AddRAM(GPRAM_Mask + 1, 6 << 24, GPRAM);
   MDFNMP_InstallReadPatches();

   VB_LOG("[VB] VB_Power");
   vb_frame_count = 0;
   VB_Power();
   VB_LOG("[VB] load done");
   return true;
}

void vb_run_frame(void)
{
   v810_timestamp_t v810_timestamp;
   EmulateSpecStruct spec;

   if (vb_frame_count == 0)
      VB_LOG("[VB] run_frame #0 next_event_ts=%d", (int)VB_V810->GetEventNT());

   MDFNMP_ApplyPeriodicCheats();
   VBINPUT_Frame();

   bool do_render = (vb_render_skip_counter == 0);
   vb_render_skip_counter = (vb_render_skip_counter + 1) % VB_RENDER_EVERY_N;
   vb_frame_rendered = do_render;

   spec.surface            = &surf;
   spec.VideoFormatChanged = (vb_frame_count == 0);
   spec.skip               = !do_render;
   spec.DisplayRect.x      = 0;
   spec.DisplayRect.y      = 0;
   spec.DisplayRect.w      = 0;
   spec.DisplayRect.h      = 0;
   spec.SoundBufMaxSize    = (int32)(sizeof(vb_sound_buf) / sizeof(int16_t)) / 2;
   spec.SoundBufSize       = 0;

   vip_reads_window = 0;
   vb_idle_mode = false;
   VB_V810->SetIdleHalt(false);

   dbg_vip_calls = 0;
   dbg_rd_vip = dbg_rd_wram = dbg_rd_gprom = dbg_rd_other = 0;
   dbg_wr_vip = dbg_wr_wram = dbg_wr_other = 0;
   VIP_StartFrame(&spec);

   v810_timestamp = VB_V810->Run(EventHandler);

   if (vb_frame_count % 300 == 0)
      VB_LOG("[VB] frame %u skip=%d rd:vip=%u wram=%u rom=%u oth=%u wr:vip=%u wram=%u oth=%u",
             (unsigned)vb_frame_count, (int)!do_render,
             dbg_rd_vip, dbg_rd_wram, dbg_rd_gprom, dbg_rd_other,
             dbg_wr_vip, dbg_wr_wram, dbg_wr_other);

   FixNonEvents();
   ForceEventUpdates(v810_timestamp);

   VSU_EndFrame((v810_timestamp + VSU_CycleFix) >> 2);

   for (int y = 0; y < 2; y++)
   {
      Blip_Buffer_end_frame  (&sbuf[y], (v810_timestamp + VSU_CycleFix) >> 2);
      vb_sound_samples = Blip_Buffer_read_samples(&sbuf[y], vb_sound_buf + y,
                                                   spec.SoundBufMaxSize);
   }

   VSU_CycleFix = (v810_timestamp + VSU_CycleFix) & 3;
   vb_frame_count++;

   TIMER_ResetTS();
   VBINPUT_ResetTS();
   VIP_ResetTS();
   RebaseTS(v810_timestamp);
   VB_V810->ResetTS(0);
}

void vb_destroy(void)
{
   MDFN_FlushGameCheats(0);
   MDFNMP_Kill();

   if (VB_V810)
   {
      VB_V810->Kill();
      delete VB_V810;
      VB_V810 = NULL;
   }
}

/* ── Save state (required by mednafen subsystems) ───────────────────────────── */

extern "C" int StateAction(StateMem *sm, int load, int data_only)
{
   const v810_timestamp_t timestamp = VB_V810->v810_timestamp;
   int ret = 1;

   SFORMAT StateRegs[] =
   {
      SFARRAY  (WRAM,  65536),
      SFARRAY  (GPRAM, GPRAM_Mask ? (GPRAM_Mask + 1) : 0),
      SFVARN   (WCR,           "WCR"),
      SFVARN   (IRQ_Asserted,  "IRQ_Asserted"),
      SFVARN   (VSU_CycleFix,  "VSU_CycleFix"),
      SFEND
   };

   ret &= MDFNSS_StateAction(sm, load, data_only, StateRegs, "MAIN", false);
   ret &= VB_V810->StateAction(sm, load, data_only);
   ret &= VSU_StateAction  (sm, load, data_only);
   ret &= TIMER_StateAction(sm, load, data_only);
   ret &= VBINPUT_StateAction(sm, load, data_only);
   ret &= VIP_StateAction  (sm, load, data_only);

   if (load)
      ForceEventUpdates(timestamp);

   return ret;
}

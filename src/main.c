/*
 * main.c — Playdate entry point for the Virtual Boy emulator.
 *
 * Responsibilities:
 *   - Initialise the audio system.
 *   - Load the ROM from the Playdate data folder ("rom.vb").
 *   - Register the 50 Hz update callback.
 *   - Each frame: poll input → run emulation → render → push audio.
 */

#include <stdlib.h>
#include <string.h>

#include "pd_api.h"

#include "vb_core.h"
#include "vb_display.h"
#include "vb_input.h"
#include "vb_audio.h"

static PlaydateAPI *pd;
static bool rom_loaded = false;

/* ── Stack high-water mark via FreeRTOS 0xA5 fill ────────────────────────── */
#ifdef TARGET_PLAYDATE
/* SP captured at eventHandler entry — a fixed reference point in the stack. */
static uint32_t sc_entry_sp;

/* Scan DOWNWARD from sc_entry_sp for the first 0xA5 byte (FreeRTOS pre-fill).
   Returns bytes used below the eventHandler entry level (our code + init). */
static uint32_t stack_hwm(void)
{
   const uint8_t *p   = (const uint8_t *)sc_entry_sp;
   const uint8_t *lim = (const uint8_t *)0x20000200U; /* stop 512B above DTCM base */
   while (p > lim && *p != 0xA5u) p--;
   return sc_entry_sp - (uint32_t)p;
}

/* One-time DTCM scan: find the longest contiguous 0xA5 run.
   Start = deepest frame ever reached.  Length = remaining stack margin. */
static void stack_scan_dtcm(void)
{
   const uint8_t *d = (const uint8_t *)0x20000000U;
   uint32_t bs = 0, bl = 0, cs = 0, cl = 0;
   for (uint32_t i = 0; i < 65536; i++) {
      if (d[i] == 0xA5u) { if (!cl) cs = i; cl++; }
      else               { if (cl > bl) { bl = cl; bs = cs; } cl = 0; }
   }
   if (cl > bl) { bl = cl; bs = cs; }
   pd->system->logToConsole("[VB] stk scan: %u bytes free, deepest=0x%08x",
       bl, 0x20000000u + bs);
}
#endif

/* ── ROM loading ─────────────────────────────────────────────────────────── */

static bool load_rom(const char *path)
{
   SDFile *f = pd->file->open(path, kFileRead | kFileReadData);
   if (!f)
      return false;

   pd->file->seek(f, 0, SEEK_END);
   int size = pd->file->tell(f);
   pd->file->seek(f, 0, SEEK_SET);

   if (size <= 0)
   {
      pd->file->close(f);
      return false;
   }

   uint8_t *data = (uint8_t *)malloc((size_t)size);
   if (!data)
   {
      pd->system->logToConsole("[VB] Out of memory for ROM (%d bytes)", size);
      pd->file->close(f);
      return false;
   }

   pd->file->read(f, data, size);
   pd->file->close(f);

   bool ok = vb_load_rom_data(data, (uint32_t)size);
   free(data);

   if (ok)
      pd->system->logToConsole("[VB] Loaded: %s (%d bytes)", path, size);
   return ok;
}

/* Scan the data directory for the first *.vb file and load it. */
static char found_rom_buf[64];
static bool found_rom = false;

static void find_vb_file(const char *path, void *userdata)
{
   (void)userdata;
   if (found_rom) return;
   int len = 0;
   while (path[len]) len++;
   if (len > 3 && path[len-3] == '.' &&
       (path[len-2] == 'v' || path[len-2] == 'V') &&
       (path[len-1] == 'b' || path[len-1] == 'B'))
   {
      /* Copy into stable buffer — listfiles may reuse the path pointer */
      int i = 0;
      while (path[i] && i < (int)sizeof(found_rom_buf) - 1)
      {
         found_rom_buf[i] = path[i];
         i++;
      }
      found_rom_buf[i] = '\0';
      found_rom = true;
   }
}

static bool find_and_load_rom(void)
{
   found_rom = false;
   pd->file->listfiles(".", find_vb_file, NULL, 0);
   if (!found_rom)
   {
      pd->system->logToConsole("[VB] No .vb ROM found in data folder");
      return false;
   }
   return load_rom(found_rom_buf);
}

/* ── Update callback (called ~50 fps by Playdate runtime) ────────────────── */

static int update(void *userdata)
{
   (void)userdata;
   static int frame_count = 0;

   if (!rom_loaded)
   {
      pd->graphics->clear(kColorWhite);
      pd->graphics->drawText("No ROM found!", 13, kASCIIEncoding, 20, 112);
      pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
      return 1;
   }

   PDButtons current;
   pd->system->getButtonState(&current, NULL, NULL);
   float crank_change = pd->system->getCrankChange();

   vb_update_input((uint32_t)current, crank_change);

   uint32_t t0 = pd->system->getCurrentTimeMilliseconds();
   vb_run_frame();
   uint32_t t1 = pd->system->getCurrentTimeMilliseconds();

   uint32_t t2 = t1;
   if (vb_frame_rendered)
   {
      uint8_t *fb = pd->graphics->getFrame();
      vb_render_frame(fb);
      t2 = pd->system->getCurrentTimeMilliseconds();
      pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
   }

   vb_audio_push();

   if (frame_count % 300 == 0)
   {
#ifdef TARGET_PLAYDATE
      if (frame_count == 0) stack_scan_dtcm(); /* one-time DTCM margin scan */
      pd->system->logToConsole("[VB] frame %d: emu=%ums disp=%ums stk=%u",
          frame_count, t1-t0, t2-t1, (unsigned)stack_hwm());
#else
      pd->system->logToConsole("[VB] frame %d: emu=%ums disp=%ums", frame_count, t1-t0, t2-t1);
#endif
   }
   frame_count++;

   pd->system->drawFPS(0, 0);

   return 1;
}

/* ── Event handler ───────────────────────────────────────────────────────── */

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
#ifdef TARGET_PLAYDATE
   /* Capture entry SP once — the shallowest in-game depth ≈ stack alloc top. */
   {
      uint32_t _sp;
      __asm volatile ("mov %0, sp" : "=r"(_sp));
      if (!sc_entry_sp) sc_entry_sp = _sp;
   }
#endif
   (void)arg;

   switch (event)
   {
      case kEventInit:
         pd = playdate;
         pd->display->setRefreshRate(50);

         {
            uint32_t t0 = pd->system->getCurrentTimeMilliseconds();
            volatile uint32_t x = 0;
            for (uint32_t i = 0; i < 1000000; i++) x += i;
            uint32_t t1 = pd->system->getCurrentTimeMilliseconds();
            pd->system->logToConsole("[VB] CPU bench: 1M iters=%dms (168MHz~12ms 48MHz~42ms)", (int)(t1-t0));
         }

         vb_audio_init(pd);
         vb_set_log(pd->system->logToConsole);

         rom_loaded = find_and_load_rom();
         if (!rom_loaded)
            pd->system->logToConsole("[VB] Failed to load any .vb ROM");

#ifdef TARGET_PLAYDATE
         pd->system->logToConsole("[VB] stack ref sp=%p size=%d",
             (void*)sc_entry_sp, __STACK_SIZE);
#endif
         pd->system->setUpdateCallback(update, NULL);
         break;

      case kEventTerminate:
         vb_destroy();
         break;

      default:
         break;
   }

   return 0;
}

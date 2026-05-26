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

/* ── ROM loading ─────────────────────────────────────────────────────────── */

static bool load_rom(const char *path)
{
   SDFile *f = pd->file->open(path, kFileRead | kFileReadData);
   if (!f)
   {
      pd->system->logToConsole("[VB] Cannot open %s", path);
      return false;
   }

   pd->file->seek(f, 0, SEEK_END);
   int size = pd->file->tell(f);
   pd->file->seek(f, 0, SEEK_SET);

   if (size <= 0)
   {
      pd->system->logToConsole("[VB] ROM file is empty");
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

   pd->system->logToConsole("[VB] Loaded ROM: %d bytes", size);

   bool ok = vb_load_rom_data(data, (uint32_t)size);
   free(data);

   if (!ok)
      pd->system->logToConsole("[VB] vb_load_rom_data() failed");
   else
      pd->system->logToConsole("[VB] ROM initialised OK");

   return ok;
}

/* ── Update callback (called ~50 fps by Playdate runtime) ────────────────── */

static int update(void *userdata)
{
   (void)userdata;

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
   vb_run_frame();

   uint8_t *fb = pd->graphics->getFrame();
   vb_render_frame(fb);
   pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);

   vb_audio_push();

   pd->system->drawFPS(0, 0);

   return 1;
}

/* ── Event handler ───────────────────────────────────────────────────────── */

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
   (void)arg;

   switch (event)
   {
      case kEventInit:
         pd = playdate;
         pd->display->setRefreshRate(50);

         vb_audio_init(pd);

         rom_loaded = load_rom("rom.vb");
         if (!rom_loaded)
            pd->system->logToConsole("[VB] Failed to load rom.vb");

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

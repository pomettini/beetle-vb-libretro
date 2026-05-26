# beetle-vb Playdate Port — Notes

## Goal

Port the beetle-vb Virtual Boy emulator (Mednafen-based) to the Panic Playdate handheld.
This eliminates the libretro layer entirely and targets the Playdate C SDK directly.

---

## Hardware Comparison

| Feature       | Virtual Boy           | Playdate              |
|---------------|-----------------------|-----------------------|
| CPU           | NEC V810 @ 20 MHz     | ARM Cortex-M7 @ 180 MHz |
| RAM           | 64 KB WRAM            | 16 MB                 |
| Display       | 384×224, 4-shade mono | 400×240, 1-bit mono   |
| Audio         | 6-ch PSG stereo       | Stereo headphone jack |
| Input         | 2× D-pad, A/B, L/R, Start/Select | D-pad, A/B, crank |

The clock ratio is ~9:1 in favour of the host — emulation should be feasible but tight.

---

## Source Structure

```
beetle-vb-libretro/
├── src/                    ← New Playdate-specific code
│   ├── main.c              ← Playdate eventHandler + update loop
│   ├── vb_core.h / .cpp    ← VB emulator init/run/destroy (replaces libretro.cpp)
│   ├── vb_display.h / .c   ← Convert VIP 32-bpp ARGB output → 1-bit Playdate LCD
│   ├── vb_input.h / .c     ← Map Playdate buttons → VB controller bits
│   ├── vb_audio.h / .c     ← Bridge Blip_Buffer → Playdate audio callback
│   ├── mempatcher_stub.c   ← No-op cheat/patch stubs (avoids <vector> on device)
│   └── cxx_support.cpp     ← Minimal C++ runtime (new/delete, __cxa_pure_virtual)
├── mednafen/               ← Emulation core (unchanged)
│   ├── vb/                 ← VIP, VSU, Timer, Input hardware
│   ├── hw_cpu/v810/        ← V810 CPU + FPU
│   └── sound/              ← Blip_Buffer audio synthesis
├── libretro-common/        ← Kept only for utility headers (boolean.h, strl.h, etc.)
│   └── include/            ← Used by mednafen core via #include <boolean.h>
├── Source/                 ← Playdate bundle output directory
│   └── pdxinfo             ← Game metadata for PDC
├── Makefile                ← Playdate SDK build (C + C++)
└── NOTES.md                ← This file
```

---

## What Was Removed

| Removed                        | Reason                                     |
|--------------------------------|--------------------------------------------|
| `libretro.cpp`                 | Replaced by `src/vb_core.cpp` + `src/main.c` |
| `libretro_core_options.h`      | Libretro settings UI, not needed           |
| `libretro_core_options_intl.h` | Libretro i18n, not needed                  |
| `jni/`                         | Android NDK build, not needed              |
| `Makefile` (original)          | libretro multi-platform build              |
| `Makefile.common`              | libretro multi-platform build              |
| `link.T`                       | Libretro linker version file               |
| `mednafen/mempatcher.cpp`      | Uses `<vector>`, replaced by no-op stub    |

The `libretro-common/include/` directory is kept because several mednafen files use
`#include <boolean.h>`, `#include <retro_inline.h>`, and `#include <compat/strl.h>`.
These are generic portability headers with no libretro-specific logic.

---

## Architecture of the Port

```
main.c
  ├── Loads ROM from Playdate filesystem ("rom.vb" in game data folder)
  ├── Calls vb_load_rom_data() to initialise the emulator
  ├── Registers Playdate update callback (50 fps)
  └── Each frame:
       ├── vb_update_input()    — read Playdate buttons → vb_input_buf
       ├── vb_run_frame()       — execute one VB frame, fills vb_framebuffer + vb_sound_buf
       ├── vb_render_frame()    — convert vb_framebuffer (32-bpp) → Playdate 1-bit LCD
       └── vb_audio_push()      — push vb_sound_buf samples into audio ring buffer

Audio callback (async, called by Playdate audio system):
  └── Dequeues from ring buffer → fills left/right PCM buffers
```

---

## Display Pipeline

The VIP renders a 384×224 ARGB32 framebuffer in **anaglyph mode** with:
- Left eye colour = `0xFFFFFF` (white)
- Right eye colour = `0x000000` (black, invisible)

This produces a grayscale image where pixel intensity = left-eye brightness (4 levels: 0, ~85, ~170, 255).

Conversion to Playdate 1-bit:
1. Extract green channel (0–255) as luminance.
2. Apply **4×4 Bayer ordered dithering** to simulate the 4 gray shades.
3. Centre the 384×224 image in the 400×240 display (8 px left/right, 8 px top/bottom border).
4. Write to `playdate->graphics->getFrame()` buffer (MSB = leftmost pixel, 1 = white).

---

## Input Mapping

| Playdate        | VB Controller         | Bit in vb_input_buf |
|-----------------|-----------------------|---------------------|
| D-pad Left      | Left D-pad Left       | 7                   |
| D-pad Right     | Left D-pad Right      | 6                   |
| D-pad Up        | Left D-pad Up         | 9                   |
| D-pad Down      | Left D-pad Down       | 8                   |
| A               | A                     | 0                   |
| B               | B                     | 1                   |
| Crank CW        | Right D-pad Right     | 5                   |
| Crank CCW       | Right D-pad Left      | 12                  |

Start (bit 10), Select (bit 11), L/R triggers (bits 2/3), and remaining right D-pad directions
are not mapped in this initial version. Refinement planned after first boot.

---

## Audio

- VB audio: 6-channel PSG through Blip_Buffer, outputs ~882 stereo samples per frame at 44100 Hz.
- `Blip_Buffer_read_samples` writes with stride 2 (interleaved stereo): `[L0, R0, L1, R1, ...]`.
- A small ring buffer decouples the VB frame rate from the Playdate audio callback.
- The Playdate audio callback is registered via `playdate->sound->addSource()`.

---

## Build Instructions

```sh
# Prerequisites:
#   - Playdate SDK installed at ~/Developer/PlaydateSDK
#   - arm-none-eabi toolchain in PATH (for device build)
#   - Place your Virtual Boy ROM as Source/rom.vb

make device    # build for real Playdate hardware
make simulator # build for Playdate Simulator (macOS)
make all       # build both + package into VirtualBoy.pdx
```

---

## Key Settings (Hardcoded)

| Setting                  | Value                  | Reason                          |
|--------------------------|------------------------|---------------------------------|
| `vb.3dmode`              | `VB3DMODE_ANAGLYPH`    | Single-image output             |
| `vb.anaglyph.lcolor`     | `0xFFFFFF`             | White left eye → grayscale      |
| `vb.anaglyph.rcolor`     | `0x000000`             | Black right eye → hidden        |
| `vb.cpu_emulation`       | `V810_EMU_MODE_FAST`   | Performance                     |
| `vb.instant_display_hack`| `true`                 | Performance                     |
| `vb.input.instant_read`  | `true`                 | Performance                     |
| `vb.allow_draw_skip`     | `false`                | Always render frames            |

---

## Known Issues / Limitations (v0.1)

- No audio initially (may be added after display/input stabilise).
- Right D-pad, L/R triggers, Start/Select not yet mapped.
- No save states (state.c is compiled but not wired to Playdate file API).
- ROM must be named `rom.vb` and placed in the Playdate data folder.
- Performance unknown until first test on hardware.

---

## Plan of Attack

### Phase 1 — Build (done)
1. Remove libretro layer (`libretro.cpp`, `jni/`, `Makefile.common`, etc.)
2. Create Playdate skeleton: `Makefile`, `Source/pdxinfo`
3. Create `src/vb_core.cpp` — port `Load()`, `Emulate()`, `CloseGame()` from libretro.cpp
4. Create `src/main.c` — Playdate `eventHandler` + `update` callback
5. Create `src/vb_display.c` — VIP 32-bpp ARGB → 1-bit Playdate LCD (Bayer dither)
6. Create `src/vb_input.c` — Playdate buttons/crank → VB controller bits
7. Create `src/vb_audio.c` — Blip_Buffer → Playdate audio via ring buffer
8. Create stubs: `mempatcher_stub.c` (no `<vector>`), `cxx_support.cpp` (no libstdc++)
9. Fix all compile and link errors until `make all` produces `VirtualBoy.pdx`

### Phase 2 — First Boot (current)
10. Test in Playdate Simulator — confirm ROM loads and first frame renders
11. Fix any runtime crashes (assert failures, null pointers, uninitialized state)
12. Verify display output: something visible on screen, even if wrong
13. Verify input: buttons move something in game
14. Verify audio: samples reach the ring buffer without crashes

### Phase 3 — Playability
15. Map remaining inputs (Start, Select, L/R triggers, right D-pad)
16. Tune frame pacing (VB runs at 50.27 Hz, Playdate callback at 50 Hz)
17. Measure real-time performance on device hardware
18. If too slow: drop dithering → simple threshold, skip alternate frames if needed
19. If audio lags: tune ring buffer size or reduce audio quality

### Phase 4 — Polish
20. Add a menu screen (ROM selector or settings)
21. Wire save-state to Playdate file API (optional)
22. Add more complete input mapping (Start/Select combos)
23. Profile and optimise the hot path (VIP render, V810 interpreter loop)
24. **Rewrite `vb_core.cpp` and `cxx_support.cpp` to pure C** — eliminate all C++ from the codebase so the entire port is C99/C11

---

## Optimisation Ideas (Post-First-Boot)

1. Skip dithering — use threshold-only 1-bit conversion for speed.
2. Skip VIP rendering for non-display frames (audio-only frames).
3. Profile with Playdate hardware profiler.
4. Reduce audio ring buffer latency.
5. Try `V810_EMU_MODE_ACCURATE` to compare accuracy vs. speed.

---

## Discovery Log

### 2026-05-26

- Confirmed Playdate SDK 3.0.6 at `~/Developer/PlaydateSDK`.
- `libretro.cpp` contains `Load()`, `Emulate()`, `CloseGame()` — all ported to `src/vb_core.cpp`.
- `Blip_Buffer_read_samples` always writes with stride 2 → creates interleaved stereo in one pass.
- `mempatcher.cpp` uses `std::vector` → removed to avoid libstdc++ dependency on device.
- VB frame rate: 50.27 Hz.  Master clock: 20 MHz.
- VIP surface dimensions: 384×224 for anaglyph mode (no need for the libretro 1024×448 buffer).
- `settings.c` already simplified (no libretro variables) — just set globals before calling VIP init.
- The `libretro-common/include/` headers are needed by mednafen core files (`boolean.h`, `retro_inline.h`, `compat/strl.h`). Keeping them in place.
- `Blip_Buffer.h` uses `INLINE` without pulling in the definition — fixed by adding `#ifndef INLINE / #include <retro_inline.h>` guard to the header itself.
- `mempatcher.h` lacked `extern "C"` — C++ translation units saw mangled symbol names; stubs compiled as C provided unmangled names → linker failure. Fixed by adding `#ifdef __cplusplus extern "C" { #endif` guard.
- `AudioSourceFunction` signature is `int(void*, int16_t*, int16_t*, int)` — no `SoundSource*` first arg; `vb_audio.c` callback was wrong.
- `pdc` copies unknown file types by default (no `-k` flag); `.vb` ROMs are included in the PDX automatically.
- Simulator crashed on first run because `update()` called `vb_run_frame()` even when ROM loading failed (VB_V810 was NULL). Fixed with `rom_loaded` guard.
- Newlib syscall stubs (`_exit`, `_write`, etc.) must be provided manually for the ARM bare-metal target — added `src/syscalls_stub.c`.

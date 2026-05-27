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

## Compile Flags (TARGET_PLAYDATE)

| Flag | Default | Effect |
|------|---------|--------|
| `VB_SCANLINES` | `0` | `1` = black-gap scanlines (retro CRT look), `2` = duplicate lines (same density). Halves rendering work but has minimal real-world perf impact since V810 CPU dominates. |
| `VB_V810_FAST_ONLY` | `1` | Strips `Run_Accurate` (5,744 dead bytes) from the binary. We always init in `V810_EMU_MODE_FAST`, so `Run_Accurate` is never called. With this flag the interpreter fits in the 16KB I-cache (11,183 bytes vs 18,267). |
| `VB_NO_RENDER` | off | Diagnostic: makes `VIP_DrawBlock` return immediately. Used to isolate V810 CPU cost from VIP rendering cost. |

---

## Performance Architecture

### Frame timing budget

- VB runs at 50 Hz → 20ms per emulated frame.
- `VB_RENDER_EVERY_N = 8`: full VIP rendering runs only on every 8th frame (≈ every 160ms wall-clock).
- On the 7 non-render frames, `DrawBlock` is skipped entirely (`skip && InstantDisplayHack && AllowDrawSkip`).
- Log format: `[VB] frame N: emu=Xms disp=Yms` — `emu` covers one emulated frame including V810 CPU + any VIP work that frame triggered.

### Measured baselines (Mario Clash, menu→demo→menu)

| Build | Light frames | Heavy frames (gameplay) |
|-------|-------------|------------------------|
| Full rendering (after all VIP opts) | 20–35 ms | 87–127 ms |
| VB_NO_RENDER (zero VIP work) | 14–29 ms | 74–106 ms |
| **Delta (VIP cost)** | ~6 ms | **~13–30 ms** |

**Key finding:** VIP rendering contributes only 13–30ms to a frame that already takes 74–106ms from V810 alone. The V810 CPU interpreter is the dominant bottleneck (~80% of frame time on heavy scenes).

### Why the V810 is slow — SDRAM bottleneck

The Playdate's memory map:
- **DTCM** `0x20000000–0x2000FFFF` (64 KB) — zero-wait-state, used by the stack.
- **SDRAM** `0x60000000+` (16 MB external) — ~100–200 ARM cycles per cache miss.

All game data lives in SDRAM:
- WRAM `0x60270220` (64 KB)
- ROM `0x60280630` (1 MB)
- GPRAM `0x60380a40`

The V810 interpreter uses **SetFastMap** for ROM and WRAM (direct pointer dereference, no callback). Every instruction fetch reads 2 bytes from the ROM pointer → SDRAM. The D-cache (16 KB) helps, but during heavy gameplay the V810 instruction working set (complex AI for 10+ enemies) can exceed 16 KB, causing D-cache thrash and effectively throttling the interpreter to ~20–27% of real-time on the worst frames.

CPU benchmark (`main.c`): `1M iterations of x+=i = 44ms` — consistent with the CPU running at 168 MHz (7–8 ARM cycles per loop iteration). The CPU is NOT throttled; the SDRAM latency is the fundamental limit.

---

## VIP Rendering Pipeline (TARGET_PLAYDATE)

### Original Mednafen path (removed for Playdate)
```
VIP_DrawBlock → scratch buffer (2bpp column-major) → CopyFBColumnToTarget (768-column pass)
```

### Playdate path
```
VIP_DrawBlock (8bpp row-major scratch) → direct memcpy to surface (row-by-row)
```

Key changes in `mednafen/vb/vip.c`:
1. **WANT_8BPP surface** (`-DWANT_8BPP=1`): VIP renders into an 8bpp surface. `BrightCLUT[0][src]` maps the 2-bit VB brightness level to a palette index in one table lookup.
2. **Direct surface write**: after each `VIP_DrawBlock`, the 8-line scratch buffer is immediately copied to `surface->pixels8` row by row (sequential, cache-friendly). The pack-to-FB and `CopyFBColumnToTarget` passes are skipped entirely for `TARGET_PLAYDATE`.
3. **Right eye disabled**: `Anaglyph_Colors[1] = 0` causes all `lron[1]` checks to be false, eliminating all right-eye work inside `DrawBG`/`DrawOBJ`/`DrawAffine` and inside `VIP_DrawBlock`'s own world loop.
4. **`InstantDisplayHack`**: batches all column-copy work into one pass at XPEND time. Already enabled; the `TARGET_PLAYDATE` path skips the column loop body entirely.

### DrawingBuffers
`DrawingBuffers[2][512 * 8]` — 8 KB stack allocation in `VIP_Update`. Only `DrawingBuffers[0]` (left eye) is used for `TARGET_PLAYDATE`. The right-eye buffer `DrawingBuffers[1]` is allocated but never written.

---

## V810 Interpreter Optimisations

### ITCM / DTCM copy (hot callbacks)

The five V810 bus callbacks (`MemRead8`, `MemRead16`, `MemWrite8`, `MemWrite16`, `EventHandler`) are placed in `.itcm.v810` section. At the start of each call to `vb_run_frame()`, they are copied to a 1 KB stack buffer (`stk_buf[1024]`) in `vb_run_frame()`'s own stack frame (which lives in DTCM).

Why the stack frame is safe: the callee frames grow **below** `stk_buf`; they cannot overwrite it. The stack-frame allocation guarantees the copy stays live for the entire duration of `VB_V810->Run()`.

`-mlong-calls` on `vb_core.cpp`: forces all outgoing calls to use `LDR rN, [pc, #offset]; BLX rN` (absolute literal pool) instead of PC-relative `B.W`. Required so the copied code can call external functions from its new (stack) address.

Section size: `__itcm_v810_end - __itcm_v810_start = 0x2A0` (672 bytes including 32-byte alignment fill). `EventHandler` is a static function — not exported as a linker symbol — confirmed present at offset `0x1E4` in the section via `arm-none-eabi-objdump -d --section=.itcm.v810`.

### Removing Run_Accurate (VB_V810_FAST_ONLY)

`v810_cpu.cpp` compiles two full copies of the interpreter loop via `v810_oploop.inc`:
- `Run_Fast` (5,060 bytes) — the path always used (`V810_EMU_MODE_FAST`).
- `Run_Accurate` (5,744 bytes) — dead code, never called.

Before: total `.text` = **18,267 bytes** (overflows the 16 KB I-cache).  
After (`VB_V810_FAST_ONLY=1`): total `.text` = **11,183 bytes** (fits in I-cache).

### -Os + -falign-loops=32 for v810_cpu.cpp

Compiled with `-Os` (size-optimise) and `-falign-loops=32` (align inner loops to I-cache line boundaries). This minimises the interpreter's I-cache footprint and ensures the tight dispatch loop doesn't straddle cache lines.

---

## Diagnostic Tools

### World-count logging

`VIP_DrawBlock` in `vip_draw.inc` logs the number of processed worlds once every 50 renders on `block_no == 0`:
```
[VIP] worlds=N
```
Correlation with frame time (Mario Clash):
- `worlds=0–2` → menu / title → 20–35 ms frames
- `worlds=10` → mid-complexity scene → 32–35 ms frames  
- `worlds=29` → heavy gameplay (10+ enemies) → 97–126 ms frames

`worlds=29` means world indices 31 down to 3 were all rendered; world 2 had the END bit set.

The logging uses `VIP_LOG(...)` which wraps `vb_log_fn` (defined externally from `vb_core.cpp`). The extern is declared at the top of `vip_draw.inc` to avoid a missing-symbol linker error since `vip.c` has no access to the VB_LOG macro.

### VB_NO_RENDER flag

`#if defined(VB_NO_RENDER) && VB_NO_RENDER` at the top of `VIP_DrawBlock` makes it return immediately. Used to measure pure V810 CPU cost, isolating it from VIP rendering. Result confirmed V810 dominates at 74–106 ms on heavy frames.

---

## Experiments That Did Not Help

### VIP scanlines (VB_SCANLINES=1)

Hypothesis: halving the inner `y` loop in `VIP_DrawBlock` (step 2 instead of 1) would halve DrawBG/DrawOBJ calls and give ~2× VIP speedup.

Result: negligible improvement on heavy frames. Why: VIP accounts for only 13–30 ms per frame (confirmed by VB_NO_RENDER test). Halving 20 ms saves ~10 ms against a 100 ms frame — not enough to be noticeable. Additionally, text in Mario Clash becomes unreadable with black-gap scanlines. **Reverted to `VB_SCANLINES=0`.**

Side effect: `VIP_LOG` macro and `extern vb_log_fn` for `vip_draw.inc` were added during this work and are still in place for the world-count diagnostic.

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

### 2026-05-27 — Performance Profiling & Optimisation

- **Display pipeline rewritten for WANT_8BPP**: eliminated pack-to-FB (column-major 2bpp) and `CopyFBColumnToTarget` (768-column pass). Now writes directly from `DrawingBuffers[0]` to `surface->pixels8` row by row inside `VIP_Update`.
- **Right eye fully disabled at VIP level**: `Anaglyph_Colors[1] = 0` causes all right-eye world/sprite rendering to be skipped inside `vip_draw.inc`.
- **ITCM callback copy**: `MemRead8/16`, `MemWrite8/16`, `EventHandler` copied to 1 KB stack buffer in DTCM at start of each `vb_run_frame()`. Dispatcher uses `TO_STK()` macro for address relocation. Stack-frame safety guarantee: callee frames grow below the buffer and cannot corrupt it.
- **First ITCM buffer too large**: `stk_buf[2048]` caused stack overflow (section is only 672 bytes). Reduced to `stk_buf[1024]`.
- **EventHandler is a static function**: does not appear in the linker map's global symbol table. Confirmed present at offset `0x1E4` in `.itcm.v810` via `objdump`. `TO_STK(EventHandler)` arithmetic is correct.
- **VB_RENDER_EVERY_N=8**: render only 1 in 8 emulated frames. Light (non-render) frames take ~12ms. Render frames were 87–127ms on heavy scenes.
- **World-count diagnostic added**: `[VIP] worlds=N` logged from `vip_draw.inc`. Discovered `worlds=29` (nearly all 32 worlds active) correlates exactly with the 97–126ms heavy frames in Mario Clash.
- **Scanlines experiment (VB_SCANLINES=1)**: halved the inner y-loop step in `VIP_DrawBlock`. No meaningful improvement (VIP is only ~20% of frame time). Text became unreadable. Reverted.
- **VB_NO_RENDER diagnostic**: `VIP_DrawBlock` returns immediately. Result: heavy frames still 74–106ms — **V810 CPU is the real bottleneck**, not VIP rendering. VIP contributes only 13–30ms per frame.
- **CPU benchmark analysis**: `1M loop iters = 44ms` ≈ 168MHz (7–8 cycles/iter) — CPU is not throttled. The effective V810 execution rate is limited by SDRAM D-cache misses on ROM instruction fetches via SetFastMap.
- **Run_Accurate stripped (VB_V810_FAST_ONLY)**: `Run_Accurate` (5,744 bytes) is dead code — `Init FAST` is always used. Removing it brings total interpreter text from 18,267 → 11,183 bytes, under the 16KB I-cache limit.
- **VB_V810_FAST_ONLY result (Mario Clash)**: heavy frames improved from 87–127ms → 44–115ms. Best frames nearly halved. Worst-case outliers still ~115ms. Confirms I-cache was a real factor. Remaining variance is D-cache pressure on ROM instruction reads via SetFastMap.
- **Wario Land cross-check**: switched test ROM to warioland.vb (2MB). worlds=12–16 during gameplay, yet heavy frames are 36–175ms — comparable to or worse than Mario Clash despite half the world count. Proves world count is not the bottleneck; V810 CPU game logic + larger ROM (more D-cache pressure) dominates. Frame 900 spike of 412ms is level loading. **The bottleneck is identical across games: SDRAM-backed V810 ROM instruction fetches.**
- **Stack canary: three failed iterations before working design**.
  1. First attempt filled stack with `0xDEAD` → FreeRTOS stack overflow method 2 checks its own `0xA5` pattern at context-switch time; our fill overwrote it, triggering a false-positive "stack overflow in task gameTask" crash.
  2. Second attempt (no fill, scan upward from `sc_top - __STACK_SIZE`) → `sc_top` captured after ROM loading is already mid-stack (`0x20009b90`); subtracting 61,800 underflows below DTCM base → BusFault on frame 0.
  3. Third attempt (scan downward from `sc_entry_sp`, one-time DTCM-wide scan for 0xA5 run) → **working**.

- **Stack canary results (Wario Land, Wario level 2 demo)**:
  - `stk scan: 7736 bytes free, deepest=0x20000060`
  - DTCM layout: BSS at `0x20000000–0x2000005F` (96 bytes — all large buffers go to `.bss.DRAM` → SDRAM via SDK loader), then stack fills the rest to `0x2000FFFF`.
  - Deepest frame ever reached: `0x20001DF8` — occurred during init (VB_Power → V810::Init), NOT during gameplay.
  - Gameplay frames: `stk=45–1012` bytes below `sc_entry_sp` (`0x20009b90`). Very shallow during the update loop.
  - RTOS overhead: ~32 KB between `sc_entry_sp` and deepest init frame; ~26 KB above `sc_entry_sp` for the RTOS task loop itself. Total RTOS + init usage: ~54 KB of the 61,800-byte stack.

- **WRAM-in-DTCM ruled out**:
  - DTCM = 64 KB. Stack needs ~54 KB for the RTOS task loop + init. Remaining margin: only 7,736 bytes.
  - WRAM = 64 KB. Even after aggressive stack shrinking there is no room for a full 64 KB static array in DTCM. The plan is not feasible within the hardware constraints.
  - The D-cache pressure problem (V810 ROM instruction reads vs. WRAM reads competing for 16 KB D-cache) **cannot be solved via DTCM placement of WRAM**.

- **Root cause of "Run loop stalled >10 seconds" crash identified and fixed**:
  - `VB_EVENT_NONONO = 0x7fffffff`. When VIP display is off, timer is disabled, and input `ReadCounter = 0`, all three event sources return NONONO.
  - `CalcNextTS()` returns `0x7fffffff` → `VB_V810->SetEventNT(0x7fffffff)` → V810 runs **2.1 billion emulated cycles** without calling EventHandler.
  - `VB_FRAME_BUDGET` (800,000 cycles) is checked INSIDE EventHandler — which is never called — so the budget check never fires. The Playdate's 10-second watchdog kills the task.
  - **Fix**: clamp EventHandler's return value to `VB_FRAME_BUDGET`. `return (next_ts > VB_FRAME_BUDGET) ? VB_FRAME_BUDGET : next_ts;` ensures EventHandler is called at most every 800,000 cycles regardless of event state. Normal gameplay is unaffected (VIP fires at ~400,000 cycles, well below the budget).
  - **Confirmed**: game ran past frame 4200 without stalling. The trigger was a level-loading scene where VIP display goes dark, confirmed by visible loading on device.

- **Early-exit dispatch + `__builtin_prefetch` (PLD) added to MemRead8/16 / MemWrite8/16**:
  - MemRead16: fast path checks `A >> 24 == 7` (ROM) first with `__builtin_expect(..., 1)`, then issues `__builtin_prefetch(&GPROM[(A+32) & GPROM_Mask], 0, 0)` before the load. Fallback goes to `switch` for other address regions.
  - MemWrite8/16: fast path for `A >> 24 == 5` (WRAM) first, fallback to switch.
  - MemRead8: fast path for ROM (no prefetch — only 1 byte, not an instruction fetch).
  - ITCM section size after these changes: 672 → 676 bytes (well under 1024-byte stk_buf).
  - **Result (Wario Land, level 2 demo, worlds=12–16)**: frames 41–159ms. Essentially unchanged from pre-prefetch baseline. PLD does not meaningfully hide SDRAM latency here because the V810 instruction access pattern is too branchy — by the time the prefetched line arrives in D-cache, the program counter has typically branched elsewhere. The early-exit dispatch saves one `switch` comparison on the common ROM path but the gain is within measurement noise.

- **VB_FRAME_BUDGET halved (800 000 → 400 000) — null result**:
  - Hypothesis: halving the budget halves V810 work per Playdate tick, halving frame times.
  - Result: frame times essentially unchanged (131ms → 131ms for worlds=16). The game was already completing 1 VB frame (~400 000 cycles) per Playdate tick even with the 800 000 budget. The extra 400 000 cycles were idle time (V810 halted via SetIdleHalt while waiting for VIP IRQ). The 800 000 budget was doing 2 game frames per tick only during idle-free scenes, which were not the bottleneck.
  - Budget reduced to 400 000 retained (now emulates exactly 1 VB frame per Playdate tick — cleaner 1:1 ratio).

- **`__attribute__((optimize("O2")))` on Run_Fast (hot interpreter loop)**:
  - Approach: keep whole file at `-Os` to stay under 16 KB I-cache, but apply per-function `-O2` to `Run_Fast` for better register allocation (fewer spills to SDRAM on the inner loop).
  - Code sizes: Run_Fast at -Os = 5,060 bytes; at per-function -O2 = 7,108 bytes. Total interpreter: 11,183 → 13,157 bytes (still under 16 KB I-cache limit).
  - Result: frame 600 (worlds=14): 48ms → 41ms (-15%). All other frames within noise. Heavy frames (worlds=16) still 100–144ms — no change.
  - Interpretation: -O2 register allocation helps slightly on moderate scenes where register spill was occasional. On worst-case scenes the bottleneck is purely SDRAM latency on ROM instruction fetches; no amount of register allocation affects that.
  - Change retained (free improvement on moderate frames, no downside).

- **Performance ceiling assessment (2026-05-27)**:
  - All straightforward micro-optimisations exhausted: ITCM copy, VB_V810_FAST_ONLY, early-exit dispatch, PLD prefetch, right-eye disable, VIP scanline skip (reverted), VB_DISABLE_AUDIO, VB_RENDER_EVERY_N=8, half-budget (null), -O2 Run_Fast (marginal).
  - The V810 interpreter already uses computed-goto dispatch (`goto *op_goto_table[opcode]`) — no switch overhead to eliminate.
  - Heavy frames (worlds=16, Wario Land level 2) still take 100–144ms vs 20ms budget — 5–7× over.
  - Measured cost: ~55 ARM cycles per V810 cycle on heavy scenes. At 168 MHz and 400 K V810 cycles/frame: 22 M ARM cycles = ~131ms. SDRAM miss penalty (100–200 ARM cycles) dominates; ~1 miss per 2–3 V810 instructions.
  - **Remaining approaches with meaningful upside (all multi-day projects)**:
    1. **JIT / basic-block compiler**: generate ARM Thumb-2 from V810 basic blocks, execute natively. Eliminates instruction fetch latency entirely. Realistic gain: 5–10×. Months of work.
    2. **ARM assembly inner loop**: hand-write fetch/decode/dispatch in Thumb-2 assembly. Better scheduling around load latency, software pipelining. Realistic gain: 1.5–2×. Weeks of work.
    3. **Dynamic emulation frame skip**: skip entire VB frames when behind wall clock. Does not reduce per-frame time but keeps game advancing at the cost of dropped input frames.
  - Conclusion: consistent playable framerates on all scenes are not achievable without a JIT or assembly rewrite. Light/moderate scenes (worlds ≤ 12) at 41–60ms could be made playable with aggressive frame skipping and a 30fps Playdate target.

### 2026-05-28 — V810 Basic-Block JIT Compiler

**Design**: V810 basic blocks translated once to ARM Thumb-2, cached in a 128 KB SDRAM code buffer. Generated code runs via the I-cache (separate 16 KB from the D-cache). A hot game loop (50–200 instructions ≈ 200–800 halfwords) fits in the I-cache → near-100% hit rate, eliminating the SDRAM D-cache miss that drives the interpreter cost.

**New files**:
- `src/v810_jit.h` — public API: `JitBlock`, `JitResult`, `JitBlockFn`, `JitLookupFn`, `jit_init`, `jit_flush`, `jit_lookup`, plus C-ABI memory callback declarations.
- `src/v810_jit.c` — Thumb-2 code generator (~700 lines).

**Integration**:
- `v810_cpu.h`: added `void *(*jit_lookup_fn)(uint32)` private member; `SetJITLookup()` public setter; `GetPREGPtr()`/`GetSREGPtr()` accessors.
- `v810_cpu.cpp`: `#include "v810_jit.h"`; `RB_CPUHOOK(n)` redefined to dispatch JIT blocks — if `!IPendingCache && jit_lookup_fn`, call `jit_lookup_fn(pc)`, execute the block, update `timestamp_rl` + PC, `continue`. Constructor initialises `jit_lookup_fn = NULL`.
- `vb_core.cpp`: C-ABI wrappers `jit_mem_r8s/r16s/w8/w16` bridge JIT-generated calls to existing `MemRead8/16` and `MemWrite8/16`. `vb_load_rom_data()` calls `jit_init(GPROM, GPROM_Mask)` then `VB_V810->SetJITLookup(jit_lookup)`.
- `Makefile`: `src/v810_jit.c` added to `C_SRC`.

**ARM register conventions inside each block**:
- `r8` = P_REG base, `r7` = S_REG base, `r6` = V810 timestamp, `r5` = next_event_ts.
- `r0–r3`, `r9`, `r10` = scratch. PUSH/POP `{r4–r8, lr/pc}` at block entry/exit.
- Return: `JitResult` in `r0:r1` (next_pc, timestamp) per ARM AAPCS.

**Thumb-2 encoding notes** (verified against ARM ARM):
- `MOV.W Rd, Rm, LSR #N`: hw1=0xEA4F, hw2=(imm3<<12)|(Rd<<8)|(imm2<<6)|(01<<4)|Rm where imm3=N>>2, imm2=N&3. A common mistake: placing imm3 in imm2 position gives shift-by-0 (bug in shift=1 case: 0x0110 not 0x0150).
- `PUSH {r4–r8, lr}` = 0xE92D, 0x41F0 (not 0xE82D — check the Rlist bit for r8 = bit 8 = 0x0100, so mask = 0x01F0 + lr bit 14 = 0x4000 → 0x41F0).
- BL to absolute address: encode as S/I1/I2/J1/J2 with J1=~(I1^S), J2=~(I2^S).
- `ADD.W r6, r6, #N` (ADDCLOCK): hw1=0xF106, hw2=0x0600|N. Encoding is T3 (modified immediate), not T4.

**PSW flag model**:
- After ARM ADDS: `MRS r2, APSR; LSR.W r2, r2, #28` → bits[3:0]=[N,Z,C,V]; `ROR.W r2, r2, #2` → [C,V,N,Z] = V810 [CY,OV,S,Z].
- After ARM SUBS: same sequence plus `EOR r2, r2, #8` to invert CY (ARM carry = !borrow).
- Logic ops: mask=0x3 (S,Z only; CY and OV always 0 for OR/AND/XOR/NOT).

**Supported V810 instructions** (translated inline):
- ALU reg-reg: MOV ADD SUB CMP SHL SHR SAR OR AND XOR NOT
- ALU imm5: MOV_I ADD_I CMP_I SHL_I SHR_I SAR_I SETF
- ALU imm16: MOVEA ADDI ORI ANDI XORI MOVHI
- Branches: all 16 conditions (BV/BL/BE/BNH/BN/BR/BLT/BLE/BNV/BNC/BNE/BH/BP/NOP/BGE/BGT), JR, JAL, JMP
- Memory: LD_B LD_H LD_W ST_B ST_H ST_W (via C-ABI callbacks)
- EI (ends block; interpreter handles interrupt re-enable)

**Branch condition encoding trap**: branches use op7=0x40–0x4F (single table entries, not doubled). Two conditions share each op6 value (e.g., BV op7=0x40 and BL op7=0x41 both have op6=0x20). Condition code = `op7 - 0x40`, NOT `op6 - 0x20`.

**Block cache**: direct-mapped, 1024 slots, keyed by `(pc>>1) & 0x3FF`. Only translates ROM-space code (upper byte = 7). On overflow flushes all and restarts from position 0.

**Build result**: clean build, both ARM device and simulator targets.

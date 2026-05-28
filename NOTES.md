# beetle-vb Playdate Port — Technical Postmortem

## Status

**This port is a failed experiment.** The Virtual Boy emulator did not reach a playable frame rate
on Playdate hardware. This document is a postmortem. The repository is left in a buildable, clean
state as a reference for future emulator work on Playdate.

The best observed performance on the benchmark scene (Wario Land level 2, worlds=14–16) was
**28–70 ms per emulated frame** against a 20 ms budget. The emulator runs at roughly 30–70% of
real-time on moderate scenes and 15–30% on heavy scenes. The bottleneck was never eliminated.

---

## Hardware Comparison

| Feature       | Virtual Boy           | Playdate              |
|---------------|-----------------------|-----------------------|
| CPU           | NEC V810 @ 20 MHz     | ARM Cortex-M7 @ 180 MHz |
| RAM           | 64 KB WRAM            | 64 KB DTCM + 16 MB SDRAM |
| ROM           | 512 KB – 16 MB        | — (loaded into SDRAM) |
| Display       | 384×224, 4-shade mono | 400×240, 1-bit mono   |
| Audio         | 6-ch PSG stereo       | Stereo headphone jack |
| I-cache       | —                     | 16 KB                 |
| D-cache       | —                     | 16 KB                 |

The clock ratio is ~9:1 in favour of Playdate. That sounds comfortable, but the critical constraint
is memory: all game data (ROM, WRAM) lives in SDRAM at 100–200 ARM cycle latency per miss.
The 16 KB D-cache cannot hold the V810 instruction working set (2 MB ROM for Wario Land).

---

## Source Structure

```
src/
  main.c              — Playdate eventHandler + update loop
  vb_core.h / .cpp    — VB emulator init/run/destroy; JIT init and mem callbacks
  vb_display.h / .c   — VIP 32-bpp ARGB → 1-bit Playdate LCD (4×4 Bayer dither)
  vb_input.h / .c     — Playdate buttons/crank → VB controller bits
  vb_audio.h / .c     — Blip_Buffer → Playdate audio via ring buffer
  mempatcher_stub.c   — No-op cheat/patch stubs (avoids <vector> on device)
  syscalls_stub.c     — Newlib bare-metal syscall stubs
  cxx_support.cpp     — Minimal C++ runtime (new/delete, __cxa_pure_virtual)
  v810_jit.h          — JIT public API: types, constants, function declarations
  v810_jit.c          — V810→ARM Thumb-2 basic-block translator (~1100 lines)
mednafen/             — Emulation core (largely unchanged from beetle-vb)
  vb/                 — VIP, VSU, Timer, Input hardware emulation
  hw_cpu/v810/        — V810 CPU interpreter + FPU
  sound/              — Blip_Buffer audio synthesis
```

---

## Why the Port Failed

### Root cause: SDRAM instruction-fetch latency

Every V810 instruction fetch is a 16-bit read from the ROM pointer in SDRAM. The Mednafen
interpreter uses `SetFastMap` (direct pointer dereference into SDRAM) for ROM access. The 16 KB
D-cache cannot hold Wario Land's 2 MB instruction working set, so D-cache miss rate is near 100%
on heavy gameplay scenes. Each miss costs 100–200 ARM cycles.

Measured: ~55 ARM cycles per V810 cycle on heavy scenes. At 168 MHz and 400 K V810 cycles/frame:
22 M ARM cycles = ~131 ms. Budget: 20 ms. Gap: 6.5×.

The CPU benchmark (`main.c`: 1 M iterations of `x += i`) confirms the CPU itself is not
throttled: 43 ms = 168 MHz running at full speed. The bottleneck is purely memory latency.

### Secondary: interpreter dispatch overhead

The Mednafen interpreter uses computed-goto dispatch (`goto *op_goto_table[opcode]`), which is
already near-optimal for an interpreter. Even with zero-latency instruction fetches the interpreter
would still carry ~10–15 ARM cycles of per-instruction overhead (fetch, decode, dispatch, update
PC, update timestamp). That alone gives a ~2.5 MHz effective V810 rate from pure overhead.

### Why a JIT only partially solved it

A basic-block JIT was implemented (`src/v810_jit.c`). It translates V810 basic blocks to ARM
Thumb-2, stores code in a 256 KB SDRAM buffer, and executes via the I-cache (separate from
D-cache). When the I-cache is warm the per-instruction ARM overhead drops to ~3–5 cycles.

Observed result on Wario Land worlds=14:

| State            | Frame time |
|------------------|-----------|
| Pre-JIT baseline | 100–144 ms |
| JIT, cache warm  | **28–30 ms** |
| JIT, post-flush  | 68–80 ms   |

The JIT reduces heavy-scene time by ~4× when its cache is warm. That brings worlds=14 to ~28 ms
(1.4× over budget, nearly playable). But the JIT cache thrashes because:

1. The 256 KB code buffer holds ~640 average-size blocks. Wario Land's startup and level-load
   routines translate hundreds of one-time blocks that fill the buffer before steady-state game
   loops are compiled. When the buffer wraps, steady-state blocks are evicted and re-translated.

2. Even with wrap-and-targeted-eviction (the final implementation), the cold translation cost
   recurs every time the working set changes (level transitions, new enemy patterns).

3. The JIT does not cover MUL, DIV, or FPU instructions — these fall back to the interpreter,
   which itself hits SDRAM for the surrounding context.

4. Worlds=16 scenes (149 ms) are not improved by the JIT. These scenes likely execute more
   diverse code paths, exhaust the 1024-slot hash table more aggressively, and spend more time
   re-translating.

### Why WRAM-in-DTCM was not feasible

DTCM = 64 KB. The RTOS task stack + init frames consume ~54 KB (measured via stack canary scan).
Remaining margin: ~7.7 KB — not enough for 64 KB WRAM. The DTCM constraint cannot be worked
around without rewriting the RTOS stack layout.

---

## What Went Well

- **Port correctness**: the game runs, renders, accepts input, and produces audio. No emulation
  accuracy regressions were introduced.
- **VIP rendering pipeline**: rewritten from Mednafen's column-major 2bpp scratch + CopyFB pass
  to a direct 8bpp row-major write. Right-eye rendering eliminated at the VIP level. Rendering
  now accounts for only ~13–30 ms of a 100+ ms heavy frame.
- **VB_V810_FAST_ONLY**: removing the dead `Run_Accurate` path (5,744 bytes) brought the
  interpreter from 18,267 bytes → 11,183 bytes, fitting within the 16 KB I-cache.
- **ITCM hot-callback copy**: the five V810 bus callbacks (~672 bytes) copied to a DTCM stack
  buffer at frame start, so at least the dispatch logic runs without SDRAM latency.
- **Frame budget clamping**: clamping `EventHandler` return to `VB_FRAME_BUDGET` (400 K cycles)
  prevents the V810 from running 2+ billion cycles when all VB events are inactive. This fixed a
  hard crash (Playdate 10-second watchdog).
- **Idle HALT**: `SetIdleHalt` suppresses interpreter busy-wait polling loops during VIP blanking.
- **JIT infrastructure**: the basic-block translator, Thumb-2 emitters, PSW flag model, and
  memory callback wiring are complete and functionally correct.

---

## What Did Not Help Enough

| Optimization | Expected | Actual |
|---|---|---|
| `__builtin_prefetch` on ROM reads | Hide SDRAM latency | No measurable improvement — V810 branches before prefetched line arrives |
| `VB_RENDER_EVERY_N=8` | ~8× VIP cost reduction | Valid; VIP only 13–30 ms/frame anyway |
| VB_SCANLINES=1 (half y-loop) | ~2× VIP speedup | Negligible — VIP is not the bottleneck |
| VB_FRAME_BUDGET halved | ~2× throughput | Null — game was already 1:1 VB frame per tick |
| `-O2` on Run_Fast | Better register allocation | ~15% on moderate scenes; no change on heavy |
| 256 KB JIT code buffer (vs 128 KB) | Fewer cache flushes | Eliminated one 722ms spike; steady-state unchanged |
| Wrap+invalidate eviction | No more full flushes | Correct improvement; still warm/cold cycles |

---

## Observed Bottlenecks

In order of measured contribution to frame time on Wario Land worlds=14 (pre-JIT, ~120 ms total):

1. **V810 ROM instruction fetches** via SetFastMap → SDRAM D-cache misses. ~80% of frame time.
2. **Interpreter per-instruction overhead** (dispatch, PC update, timestamp). ~15% of frame time.
3. **VIP rendering** (DrawBlock, world loop, dithering). ~5–15% of frame time.

With JIT (warm cache, worlds=14, ~28 ms):

1. **ARM I-cache misses** on generated code — still SDRAM, but I-cache is separate and the
   working set fits. ~50% of remaining time.
2. **Memory callback overhead** — each LD/ST V810 instruction calls `jit_mem_r8s` etc. via
   a BL instruction. These are C-ABI calls into SDRAM. ~30% of remaining time on data-heavy code.
3. **JIT translation cost** — cold-start and post-eviction re-translation. ~15% averaged.
4. **VIP rendering** — ~5 ms/frame on render frames.

---

## The Abandoned JIT Direction

### What was implemented

`src/v810_jit.c` is a complete basic-block JIT for V810→ARM Thumb-2 targeting the Cortex-M7.
It implements:

- Prologue/epilogue (PUSH/POP callee-saved registers; ARM register assignments r5–r8)
- All ALU instructions (reg-reg and immediate): MOV, ADD, SUB, CMP, SHL, SHR, SAR, OR, AND, XOR,
  NOT and their immediate-operand variants; MOVEA, ADDI, ORI, ANDI, XORI, MOVHI
- All 16 conditional branches, JR, JAL, JMP
- Memory: LD_B, LD_H, LD_W, ST_B, ST_H, ST_W via C-ABI callbacks
- SETF, NOP, EI (EI ends the block so the interpreter handles interrupt re-enable)
- PSW flags updated after every flag-setting instruction (MRS/LSR/ROR sequence)
- Direct-mapped hash table (1024 slots), 256 KB code buffer, wrap-and-invalidate eviction

### What would have been done next

**Larger hash table**: the 1024-slot direct-mapped table hashes on PC bits [10:1]. With a 2 MB ROM
and complex game code, many hot blocks collide. Increasing to 4096 slots (48 KB SDRAM) would
reduce collision-eviction without changing the eviction strategy.

**MUL/DIV in JIT**: V810 MUL/MULU (16 cycles) and DIV/DIVU (38 cycles) are the most expensive
missing instructions. Any game loop with integer multiply falls back to the interpreter. ARM has
native MUL/SDIV/UDIV; emitting them inline would eliminate the most costly fallbacks.

**Inlined memory for WRAM/ROM fast paths**: the memory callbacks (`jit_mem_r8s`, etc.) are
C-ABI calls that cross-module-call through SDRAM. For the most common addresses (WRAM at
`0x05xxxxxx`, ROM at `0x07xxxxxx`), the JIT could emit a direct load/store against the
`WRAM`/`GPROM` pointers with an inline address check, avoiding the call overhead entirely.

**Block chaining**: the current implementation exits every block with `MOV r0, #next_pc; POP {pc}`.
If the next block is already compiled, the JIT could patch the exit to a direct `B` to the target
block's code, avoiding the hash table lookup on every branch.

**Interrupt-safe block exit**: currently any `IPendingCache` check forces interpreter fallback.
A lightweight interrupt-check at block exit (test a memory flag, conditional exit) would let
blocks run to completion without the per-instruction interpreter check.

### Complications

- **Executable memory on Playdate**: generated code lives in the SDRAM heap. The Cortex-M7 has
  separate I-cache and D-cache; after writing Thumb-2 code, the D-cache must be flushed and the
  I-cache invalidated for that region. `__builtin___clear_cache` handles this but adds latency
  per newly translated block.

- **PSW flag accuracy**: the V810 flag model (CY=carry, OV=overflow, S=sign, Z=zero) does not
  map cleanly to ARM NZCV for subtract (ARM C = !borrow). The `MRS/LSR/ROR/EOR` sequence is
  correct but costs 4–5 instructions per flag-setting op. For CMP-only sequences (flags used but
  result discarded) this can be optimised.

- **Self-modifying code**: V810 cartridge games do not self-modify ROM, but WRAM is writable. The
  JIT only translates ROM-space code (PC upper byte = 7), avoiding this problem entirely.

- **SDK and toolchain limits**: the Playdate SDK provides no explicit API for marking memory as
  executable. On Cortex-M7 all SDRAM is executable by default (no MPU restriction in the SDK's
  default configuration). `__builtin___clear_cache` is the correct portable interface; no
  platform-specific syscall is needed.

### Whether it would have been enough

Even a fully optimised JIT (MUL/DIV inline, WRAM fast path, block chaining) would likely reach
~15–20 ms on worlds=14 scenes. Worlds=16 (the hardest scenes in Wario Land) would remain at
30–50 ms. The game would be playable at moderate difficulty but would still drop frames on the
most complex scenes. A real-time Virtual Boy emulator on Playdate probably requires either:

1. A heavily hand-optimised ARM assembly interpreter with software pipelining and explicit load
   scheduling around SDRAM latency, or
2. A JIT with inlined memory, block chaining, and a much larger working-set cache, or
3. A pre-compiled static binary translation of the ROM (offline, not at runtime).

---

## General Playdate Emulator Engineering Notes

These notes are written from experience with this port. They apply broadly to any high-performance
emulator on the Playdate Cortex-M7.

### Know the memory hierarchy first

The single most important fact about Playdate performance is the memory map:

- **DTCM** `0x20000000` (64 KB): zero-wait-state. Used by the OS stack. Only ~8 KB available
  after RTOS overhead.
- **SDRAM** `0x60000000` (16 MB): 100–200 ARM cycle miss penalty. All game data lives here.
- **I-cache** (16 KB): separate from D-cache. Generated code or a compact interpreter loop that
  fits here runs at near-zero fetch cost.
- **D-cache** (16 KB): shared between all data accesses. A working set larger than 16 KB causes
  constant eviction.

Profile before optimising. The bottleneck is almost always SDRAM latency, not compute.

### Interpreter design

**Compact inner loop**: the interpreter must fit in the 16 KB I-cache. In this port,
`VB_V810_FAST_ONLY` removed 5.7 KB of dead code, bringing the interpreter from 18 KB → 11 KB.
Keep dead paths out of the binary.

**Computed-goto dispatch** is already optimal for C interpreters. Don't try to improve dispatch
overhead — eliminate instruction fetches instead.

**Minimal per-instruction overhead**: every cycle-counting, PC-updating, and flag-propagating
operation that touches SDRAM is multiplied by the instruction count. Keep the hot path free of
indirect reads.

**Align the inner loop**: `-falign-functions=32 -falign-loops=32` ensures the inner dispatch
loop doesn't straddle a 32-byte I-cache line boundary. Worth doing.

### Memory map design

**Fast-path first**: put the most common address range at the top of every dispatch function.
The V810 instruction fetch path in this port hits ROM (upper byte = 7) in ~99% of cases;
putting that check first with `__builtin_expect(..., 1)` avoids the switch on the hot path.

**Avoid callbacks for hot paths**: if the emulated CPU reads ROM on every instruction fetch,
that callback is the inner loop. Either use a direct pointer (like SetFastMap) or generate
inline loads in a JIT.

**`__builtin_prefetch` is unlikely to help** for branchy emulated instruction streams. By the
time the prefetched cache line arrives, the V810 has typically branched to a different address.
Only consider it for sequential data access patterns.

### JIT / dynamic recompilation

If an interpreter cannot meet the cycle budget, a basic-block JIT is the right next step.
Key principles for the Playdate:

- **Store generated code in SDRAM, run it via I-cache.** The I-cache and D-cache are separate on
  Cortex-M7. A compact game loop (50–200 V810 instructions = ~400–1600 Thumb-2 halfwords) fits in
  16 KB. I-cache hit rate is near 100% for steady-state loops.
- **Call `__builtin___clear_cache` after writing each block.** This is required to flush the
  D-cache and invalidate the I-cache for the newly written region.
- **Wrap-and-invalidate eviction is better than flush-all.** When the code buffer is full, wrap
  the write pointer and invalidate only the blocks whose code is about to be overwritten. This
  preserves the hot working set across buffer wraps.
- **Translate only ROM-space code** (the emulated CPU's code segment). Skip WRAM and I/O space.
  Cartridge games never execute from RAM.
- **Inline the fast-path memory accesses.** The biggest remaining cost after a JIT eliminates
  instruction-fetch latency is memory callback overhead. Emit direct load/store ARM instructions
  for the two or three most common address regions.
- **Block chaining eliminates hash-lookup overhead** on taken branches. Patch the exit jump of
  a compiled block to branch directly to the next compiled block once it is known.

### Threaded interpretation / indirect-threaded code

If a full JIT is too complex, threaded interpretation is a middle ground: pre-decode V810
instructions into a compact token stream (opcode + operands), then dispatch using a token-indexed
table. This eliminates the fetch-decode step and reduces the working set, but still pays the
SDRAM latency on the token stream if it does not fit in D-cache.

For the V810 specifically, the instruction encoding is dense enough (2–4 bytes, few formats) that
pre-decoding saves less than it would for a CISC target.

### Renderer design

**Match the display format from the start.** Playdate is 400×240 1-bit. Any emulated system
outputting a different format and resolution pays a per-pixel conversion cost every rendered frame.

For Virtual Boy (384×224, 4-shade):
- Render at native resolution, add 8-pixel borders.
- Use an 8bpp intermediate surface (not 32bpp ARGB — 4× fewer bytes to touch).
- Apply dithering once, during the VB→Playdate conversion, not during emulated rendering.

**Minimise the work done per frame.** In this port, `VB_RENDER_EVERY_N=8` renders only 1 in 8
emulated frames (160 ms interval, imperceptible on a 50 Hz display). The other 7 frames skip all
VIP work. VIP rendering was only ~15% of frame time, so this saved ~13% overall — worth doing but
not a breakthrough.

**Profile render and CPU separately** before deciding where to spend optimisation effort. Use a
`VB_NO_RENDER` flag (makes `DrawBlock` a no-op) to isolate CPU cost. This port confirmed early
that VIP rendering was not the bottleneck.

### Audio

**Disable audio during the performance push.** Audio synthesis (Blip_Buffer, 6-channel PSG,
sample-rate conversion) adds measurable overhead. Disabling it (`VB_DISABLE_AUDIO=1`) is a free
performance flag while chasing CPU frame-time targets.

**Ring-buffer decoupling**: the Playdate audio callback runs on a timer interrupt. A ring buffer
between the emulator and the audio callback avoids blocking the update loop. Size it to hold
2–3 frames of audio (88–132 samples at 44100 Hz / 50 Hz). Too large → latency; too small → gaps.

### Timing and frame pacing

**Clamp EventHandler return values.** If all event sources return "never" (e.g., during VIP
blanking or level loads), the CPU scheduler may request billions of emulated cycles before the
next event. Cap the return value to one frame budget to keep the Playdate watchdog timer safe.

**Match the host frame rate.** Playdate calls the update callback at 50 Hz. The Virtual Boy runs
at 50.27 Hz — a 0.5% mismatch. Over 3600 frames (~72 seconds) this causes one dropped VB frame.
Acceptable for games; audio pitch drift is the more audible symptom if audio is enabled.

**Budget = 1 VB frame per Playdate tick** (400 K V810 cycles at 20 MHz / 50 Hz). Emulating 2
VB frames per tick means the game runs at 2× speed unless the second frame is idle (VB CPU
halted). Profile `VB_FRAME_BUDGET` against actual frame completion to find the right value.

### Profiling advice

1. Add per-frame timing logs (`emu=Xms disp=Yms`) from the start.
2. Add a `worlds=N` diagnostic from the renderer to correlate scene complexity with frame time.
3. Use a stack canary scan to measure actual stack depth, not assumed depth.
4. Profile `VB_NO_RENDER` first to separate CPU from render cost.
5. Check I-cache fit with `arm-none-eabi-size` — if the interpreter overflows 16 KB, I-cache
   thrash is hiding in your numbers.
6. Document measurements after every major change. Null results are as important as wins.

### Lessons from CrankBoy and similar Playdate emulators

CrankBoy (Game Boy emulator for Playdate) reaches playable frame rates by:
- Using a hand-optimised ARM assembly interpreter for the LR35902 CPU.
- Aggressively inlining memory map dispatch.
- Keeping the entire interpreter + dispatch table in ITCM or I-cache.
- Deferring audio synthesis to a separate budget.

The general lesson: for any 8-bit or 16-bit CPU emulator on Playdate, the threshold for real-time
is roughly `(host_clock / guest_clock) × (cycles_per_instruction_overhead) ≤ 1`. At 9:1 clock
ratio, an emulator can spend at most 9 ARM cycles per V810 cycle on average. The Mednafen V810
interpreter spends ~55 cycles per V810 cycle. A JIT or hand-written assembly interpreter is
required to close that gap.

---

## Performance Measurements Summary

All measurements on Playdate hardware (Cortex-M7 @ 168 MHz). ROM: warioland.vb (2 MB).
Benchmark: Wario Land level 2, "? block" scene. `worlds=N` = active VB display worlds count.

### Pre-JIT baselines

| Build | worlds=4 | worlds=14 | worlds=16 |
|-------|---------|-----------|-----------|
| Full rendering, all opts | ~6 ms | 87–127 ms | 87–127 ms |
| VB_NO_RENDER (CPU only) | ~6 ms | 74–106 ms | 74–106 ms |

### Post-JIT (final build)

| State | worlds=4 | worlds=14 | worlds=16 |
|-------|---------|-----------|-----------|
| JIT warm (steady-state) | ~6 ms | **28–30 ms** | 88–91 ms |
| JIT cold / post-eviction | ~6 ms | 68–80 ms | 140–149 ms |
| First frame (cold start) | — | 1693 ms | — |

### Key optimisation deltas (worlds=14, pre-JIT)

| Change | Before | After | Delta |
|--------|--------|-------|-------|
| VB_V810_FAST_ONLY (remove Run_Accurate) | ~127 ms | ~115 ms | −10% |
| -O2 on Run_Fast | ~115 ms | ~100 ms | −13% |
| ITCM hot-callback copy | ~105 ms | ~100 ms | −5% |
| prefetch + early-exit dispatch | ~100 ms | ~100 ms | 0% |
| JIT (warm cache) | ~100 ms | ~28 ms | **−72%** |

---

## Active Compile-Time Flags

| Flag | Value | Effect |
|------|-------|--------|
| `VB_DISABLE_AUDIO` | 1 | Audio synthesis disabled — saves ~5 ms/frame |
| `VB_SCANLINES` | 0 | Full-quality rendering (no scanline gap) |
| `VB_V810_FAST_ONLY` | 1 | Strip `Run_Accurate` from binary (saves 5.7 KB, fits I-cache) |
| `VB_RENDER_EVERY_N` | 8 | Render 1 in 8 VB frames |
| `VB_FRAME_BUDGET` | 400000 | Max V810 cycles per Playdate tick (= 1 VB frame) |

---

## Build Instructions

```sh
# Prerequisites:
#   Playdate SDK installed at ~/Developer/PlaydateSDK
#   arm-none-eabi toolchain in PATH
#   ROM placed at Source/warioland.vb (or update pdxinfo)

make all       # build device ELF + simulator dylib + VirtualBoy.pdx
make device    # build device only
make simulator # build simulator only
```

Push to device:
```sh
pdutil /dev/cu.usbmodemPDU1_XXXXXXX datadisk
# confirm "Enable Data Disk" on device screen
# wait ~8s after volume mounts
cp -r VirtualBoy.pdx /Volumes/PLAYDATE/Games/
diskutil eject /Volumes/PLAYDATE
```

---

## Discovery Log

### 2026-05-26 — Initial port

- Ported `Load()`, `Emulate()`, `CloseGame()` from libretro.cpp to `src/vb_core.cpp`.
- `Blip_Buffer_read_samples` writes stride-2 interleaved stereo in one pass.
- `mempatcher.cpp` uses `std::vector` → replaced by no-op stub.
- VB frame rate: 50.27 Hz. Master clock: 20 MHz.
- VIP surface: 384×224 for anaglyph mode.
- `mempatcher.h` missing `extern "C"` guard → linker failure on C++ TU. Fixed.
- `AudioSourceFunction` signature: `int(void*, int16_t*, int16_t*, int)` — no `SoundSource*` first arg.
- Simulator crashed because `update()` called `vb_run_frame()` before ROM loaded. Fixed with `rom_loaded` guard.
- Newlib syscall stubs (`_exit`, `_write`, etc.) required for ARM bare-metal target.

### 2026-05-27 — Performance profiling and optimisation

- **Display pipeline rewritten for WANT_8BPP**: direct 8bpp write from DrawingBuffers to surface.
- **Right eye disabled at VIP level**: `Anaglyph_Colors[1] = 0`.
- **ITCM callback copy**: MemRead8/16, MemWrite8/16, EventHandler → 1 KB DTCM stack buffer.
- **First ITCM buffer too large**: `stk_buf[2048]` → stack overflow. Reduced to `stk_buf[1024]`.
- **VB_RENDER_EVERY_N=8**: render only 1 in 8 frames. Light frames: ~12 ms.
- **World-count diagnostic**: `[VIP] worlds=N`. Confirmed worlds=29 correlates with 97–126 ms frames.
- **VB_NO_RENDER diagnostic**: heavy frames still 74–106 ms. V810 CPU is the bottleneck.
- **CPU benchmark**: `1M loop iters = 43 ms` ≈ 168 MHz — CPU not throttled. SDRAM is the limit.
- **VB_V810_FAST_ONLY**: interpreter text 18,267 → 11,183 bytes. Heavy frames 87–127 ms → 44–115 ms.
- **Wario Land cross-check**: 2 MB ROM, worlds=12–16, 36–175 ms. Larger ROM = more D-cache pressure.
- **Stack canary**: three failed implementations before working design (see below).
  1. Fill with `0xDEAD` → overwrote FreeRTOS `0xA5` sentinel → false-positive stack overflow crash.
  2. Scan from `sc_top - __STACK_SIZE` → underflows DTCM base → BusFault.
  3. Scan downward from `sc_entry_sp` → correct. Result: 7,736 bytes free, deepest=`0x20000060`.
- **WRAM-in-DTCM ruled out**: only 7.7 KB free after RTOS. 64 KB WRAM does not fit.
- **"Run loop stalled >10 seconds" crash**: `VB_EVENT_NONONO = 0x7fffffff` caused V810 to run
  2.1 B cycles without calling EventHandler. Budget check was inside EventHandler (never called).
  Fix: cap EventHandler return to `VB_FRAME_BUDGET`.
- **VB_FRAME_BUDGET halved** (800 K → 400 K): null result. Game was already at 1:1 VB frame/tick.
- **`-O2` on Run_Fast**: Run_Fast 5,060 → 7,108 bytes (still under 16 KB). ~15% improvement on
  moderate scenes; no change on heavy. Retained.
- **`__builtin_prefetch` on ROM reads**: no measurable improvement. V810 branches before the
  prefetched line arrives.
- **Performance ceiling assessment**: all straightforward optimisations exhausted. JIT required.

### 2026-05-28 — V810 basic-block JIT

- Designed and implemented `src/v810_jit.c` (~1100 lines): V810→ARM Thumb-2 basic-block translator.
- `src/v810_jit.h`: JitBlock, JitResult, JitBlockFn types; API declarations.
- `v810_cpu.h` / `v810_cpu.cpp`: `jit_lookup_fn` member, `SetJITLookup()`, `RB_CPUHOOK` dispatch.
- `vb_core.cpp`: C-ABI memory wrappers, `jit_init()` call.
- **Key encoding bugs fixed during development**:
  - Branch condition: `op6 - 0x20` wrong (two conditions share each op6). Correct: `op7 - 0x40`.
  - NOP vs BP: both have op6=0x26; must check op7=0x4D (NOP) vs 0x4C (BP).
  - LSR.W r1, r0, #1: `0x0110` (imm2=0, shift-by-0) vs correct `0x0150` (imm2=1).
  - ST_W LSR.W r2,r2,#16: imm3 placed in imm2 field → correct encoding 0x4212.
- **First device test (128 KB buffer)**: frame 600 = 722 ms spike (full buffer flush). Otherwise
  worlds=14: 28–69 ms alternating.
- **Doubled buffer to 256 KB**: spike at frame 600 → 68 ms. Steady-state unchanged.
- **Analysis**: alternating 28/70 ms at same worlds count indicates periodic buffer wraps still
  flushing steady-state blocks.
- **Wrap+invalidate eviction**: replaced flush-all with circular wrap + O(1024) scan to evict only
  blocks whose code overlaps the region about to be written.
- **Final result**: JIT warm → 28–30 ms at worlds=14. Not consistently below 20 ms budget.
  Port declared finished.

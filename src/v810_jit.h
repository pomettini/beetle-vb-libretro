/*
 * v810_jit.h — V810 basic-block JIT for ARM Cortex-M7 (Thumb-2).
 *
 * ARM register assignments inside every generated block:
 *   r8 = P_REG base    (callee-saved)
 *   r7 = S_REG base    (callee-saved)
 *   r6 = v810 timestamp (callee-saved, updated by ADDCLOCK)
 *   r5 = next_event_ts (callee-saved)
 *   r0-r4, r9, r10     = scratch per instruction
 *
 * Block function prototype (via JitBlockFn):
 *   JitResult block(uint32_t *p_reg, uint32_t *s_reg,
 *                   uint32_t ts, uint32_t next_event_ts);
 * Returns next V810 PC in r0, updated timestamp in r1
 * (struct JitResult is returned in r0:r1 per ARM AAPCS).
 */

#ifndef V810_JIT_H
#define V810_JIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sizing ──────────────────────────────────────────────────────────── */
#define JIT_HTAB_BITS     10              /* 2^10 = 1024 direct-mapped slots */
#define JIT_HTAB_SIZE     (1 << JIT_HTAB_BITS)
#define JIT_CODE_WORDS    (128 * 1024)    /* 256 KB code buffer (uint16_t units) */
#define JIT_MAX_BLOCK_HW  512             /* max halfwords emitted per block */
#define JIT_MAX_OPS       48              /* max V810 instructions per block */

/* ── Core types ──────────────────────────────────────────────────────── */

/* Cached translated block */
typedef struct {
    uint32_t  vb_pc;       /* V810 PC this block starts at (0 = empty slot) */
    uintptr_t code_thumb;  /* Thumb function pointer (LSB=1) */
    uint32_t  hw_count;    /* halfwords emitted (for statistics) */
} JitBlock;

/* Return value of a JIT block function.
 * Returned in r0:r1 per ARM AAPCS for a 2×uint32 struct. */
typedef struct {
    uint32_t next_pc;    /* Next V810 PC to execute */
    uint32_t timestamp;  /* Updated V810 timestamp */
} JitResult;

/* Block function pointer */
typedef JitResult (*JitBlockFn)(uint32_t *p_reg, uint32_t *s_reg,
                                uint32_t ts, uint32_t next_event_ts);

/* Lookup function stored in V810 class */
typedef JitBlock *(*JitLookupFn)(uint32_t vb_pc);

/* ── Public API ──────────────────────────────────────────────────────── */

/* One-time init; rom/rom_mask are used for translation. */
void jit_init(const uint8_t *rom, uint32_t rom_mask);

/* Invalidate all cached blocks (called on cache overflow or reset). */
void jit_flush(void);

/* Look up a block for vb_pc; translate on miss.
 * Returns NULL if translation is rejected (interpreter will run instead). */
JitBlock *jit_lookup(uint32_t vb_pc);

/* ── C-ABI memory callbacks (defined in vb_core.cpp) ────────────────── */
/* Called from JIT-generated code: r0=ts, r1=addr (reads) or r2=val (writes) */
uint32_t jit_mem_r8s (uint32_t ts, uint32_t addr);   /* sign-ext byte  */
uint32_t jit_mem_r16s(uint32_t ts, uint32_t addr);   /* sign-ext short */
void     jit_mem_w8  (uint32_t ts, uint32_t addr, uint32_t val);
void     jit_mem_w16 (uint32_t ts, uint32_t addr, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* V810_JIT_H */

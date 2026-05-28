/*
 * v810_jit.c — V810 basic-block JIT compiler targeting ARM Thumb-2.
 *
 * Strategy
 * --------
 * Every V810 basic block (instructions up to the first branch or
 * JIT_MAX_OPS instructions) is translated once to native Thumb-2 code
 * and cached in jit_code_buf.  Future trips through the same PC skip
 * the interpreter's fetch-decode-dispatch loop entirely, eliminating the
 * SDRAM D-cache miss (~100-200 ARM cycles) that dominates interpreter cost.
 *
 * Generated code lives in jit_code_buf (SDRAM), which the ARM I-cache
 * serves directly — the working set of a tight game loop is far smaller
 * than 16 KB, so I-cache hit rate is near 100 %.
 *
 * Supported instructions (translated inline)
 * -------------------------------------------
 * ALU reg-reg: MOV ADD SUB CMP SHL SHR SAR OR AND XOR NOT
 * ALU imm5:    MOV_I ADD_I CMP_I SHL_I SHR_I SAR_I
 * ALU imm16:   MOVEA ADDI ORI ANDI XORI MOVHI
 * Branches:    all 16 conditions + JR JAL JMP
 * Memory:      LD_B LD_H LD_W ST_B ST_H ST_W (via C callbacks)
 * System:      NOP EI DI SETF
 *
 * All other instructions (MUL DIV FPU BSTR IN OUT etc.) cause early block
 * termination; the interpreter handles them normally.
 *
 * PSW flag model
 * --------------
 * After every flag-modifying instruction, S_REG[PSW] bits [3:0] are updated:
 *   bit3 = CY (carry/borrow)
 *   bit2 = OV (signed overflow)
 *   bit1 = S  (sign / negative)
 *   bit0 = Z  (zero)
 *
 * The ARM APSR bits (read via MRS) are [N,Z,C,V] in bits [31:28].
 * After LSR #28 → bits [3:0] = [N,Z,C,V].
 * After ROR #2  → bits [3:0] = [C,V,N,Z] = V810 [CY,OV,S,Z] for ADD.
 * For SUB/CMP an extra EOR #8 inverts CY (ARM carry is !borrow).
 */

#include "v810_jit.h"
#include <string.h>

/* ── Globals ─────────────────────────────────────────────────────────── */

/* Code buffer — lives in .bss (SDRAM).  Aligned to 4 bytes so the
   first Thumb-2 instruction is naturally aligned. */
static uint16_t jit_code_buf[JIT_CODE_WORDS] __attribute__((aligned(4)));
static uint32_t jit_code_pos;   /* next free halfword index */

/* Direct-mapped block hash table keyed by (vb_pc >> 1) & (JIT_HTAB_SIZE-1) */
static JitBlock  jit_table[JIT_HTAB_SIZE];

/* ROM pointer and mask, set by jit_init() */
static const uint8_t *jit_rom;
static uint32_t       jit_rom_mask;

/* ── Helpers: ROM access ─────────────────────────────────────────────── */

static uint16_t rom_read16(uint32_t pc)
{
    uint32_t off = pc & jit_rom_mask;
    return (uint16_t)(jit_rom[off] | ((uint16_t)jit_rom[off + 1] << 8));
}

/* ── Sign-extension helpers ──────────────────────────────────────────── */

static int32_t sx5 (uint32_t v) { return (int32_t)(v << 27) >> 27; }
static int32_t sx9 (uint32_t v) { return (int32_t)(v << 23) >> 23; }
static int32_t sx16(uint32_t v) { return (int32_t)(int16_t)(uint16_t)v; }
static int32_t sx26(uint32_t v) { return (int32_t)(v <<  6) >>  6; }

/* ── Emitter ─────────────────────────────────────────────────────────── */

static void e16(uint16_t hw)
{
    jit_code_buf[jit_code_pos++] = hw;
}
static void e32(uint16_t hw1, uint16_t hw2)
{
    jit_code_buf[jit_code_pos++] = hw1;
    jit_code_buf[jit_code_pos++] = hw2;
}

/* Current write position in bytes from start of buffer (for BL offsets) */
static uint8_t *ep_bytes(void)
{
    return (uint8_t *)&jit_code_buf[jit_code_pos];
}

/* ── Thumb-2 encoding helpers ────────────────────────────────────────── */

/* LDR.W rt, [r8, #(ri*4)] — load P_REG[ri] */
static void emit_ldr_preg(int rt, int ri)
{
    if (ri == 0) {
        /* MOVW Rd, #0 */
        e32(0xF240, (uint16_t)(rt << 8));
        return;
    }
    e32(0xF8D8, (uint16_t)((rt << 12) | (ri * 4)));
}

/* STR.W rt, [r8, #(ri*4)] — store to P_REG[ri] (nop for ri==0) */
static void emit_str_preg(int rt, int ri)
{
    if (ri == 0) return;
    e32(0xF8C8, (uint16_t)((rt << 12) | (ri * 4)));
}

/* LDR.W rt, [r7, #off] — load S_REG word at byte offset off */
static void emit_ldr_sreg(int rt, int off)
{
    e32(0xF8D7, (uint16_t)((rt << 12) | off));
}

/* STR.W rt, [r7, #off] */
static void emit_str_sreg(int rt, int off)
{
    e32(0xF8C7, (uint16_t)((rt << 12) | off));
}

/* MOVW Rd, #imm16 */
static void emit_movw(int rd, uint32_t val)
{
    uint32_t imm16 = val & 0xFFFF;
    uint32_t imm4  = (imm16 >> 12) & 0xF;
    uint32_t i     = (imm16 >> 11) & 1;
    uint32_t imm3  = (imm16 >> 8) & 7;
    uint32_t imm8  = imm16 & 0xFF;
    e32((uint16_t)(0xF240 | (i << 10) | imm4),
        (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

/* MOVT Rd, #(val >> 16) */
static void emit_movt(int rd, uint32_t val)
{
    uint32_t imm16 = (val >> 16) & 0xFFFF;
    uint32_t imm4  = (imm16 >> 12) & 0xF;
    uint32_t i     = (imm16 >> 11) & 1;
    uint32_t imm3  = (imm16 >> 8) & 7;
    uint32_t imm8  = imm16 & 0xFF;
    e32((uint16_t)(0xF2C0 | (i << 10) | imm4),
        (uint16_t)((imm3 << 12) | (rd << 8) | imm8));
}

/* Load full 32-bit constant into Rd (MOVW + optional MOVT) */
static void emit_mov32(int rd, uint32_t val)
{
    emit_movw(rd, val);
    if (val & 0xFFFF0000u)
        emit_movt(rd, val);
}

/* MOV.W rd, rn (no flags) */
static void emit_mov_reg(int rd, int rn)
{
    e32(0xEA4F, (uint16_t)((rd << 8) | rn));
}

/* ADD.W r6, r6, #n  (ADDCLOCK, n < 256) */
static void emit_addclock(int n)
{
    e32(0xF106, (uint16_t)(0x0600 | (uint8_t)n));
}

/* BL to absolute address (must be within ±16 MB of code buffer) */
static void emit_bl(const void *target)
{
    /* PC at this instruction = ep_bytes() + 4 (ARM pipeline) */
    uint8_t  *pc  = ep_bytes() + 4;
    int32_t   off = (int32_t)((uintptr_t)target - (uintptr_t)pc);
    uint32_t  o   = (uint32_t)off;
    uint32_t  S   = (o >> 31) & 1;
    uint32_t  I1  = (o >> 23) & 1;
    uint32_t  I2  = (o >> 22) & 1;
    uint32_t  imm10 = (o >> 12) & 0x3FF;
    uint32_t  imm11 = (o >> 1)  & 0x7FF;
    uint32_t  J1  = (~(I1 ^ S)) & 1;
    uint32_t  J2  = (~(I2 ^ S)) & 1;
    e16((uint16_t)(0xF000 | (S << 10) | imm10));
    e16((uint16_t)(0xD000 | (J1 << 13) | (J2 << 11) | imm11));
}

/*
 * Emit PSW update after an ARM flag-setting instruction.
 * Uses r1 and r2 as scratch; r0 must still hold the result (for logic ops).
 *
 * mode:
 *   0 = ADD/logic (use ARM flags as-is for all 4 bits)
 *   1 = SUB/CMP   (invert CY bit since ARM C = !borrow)
 *   2 = SHR/SAR   (ARM C=last shifted out but OV always 0)
 *   3 = logic ops only S,Z — CY and OV forced 0
 *
 * flag_mask passed to AND to keep only wanted bits:
 *   mode 0 ADD : mask=0xF  (all 4)
 *   mode 1 SUB : mask=0xF  (all 4, then CY inverted)
 *   mode 2 SHR : mask=0xB  (CY,S,Z; OV=0)
 *   mode 3 logic: mask=0x3 (S,Z; CY=0, OV=0)
 */
static void emit_psw_flags(int is_sub, uint8_t mask)
{
    /* MRS r2, APSR */
    e32(0xF3EF, 0x8200);
    /* LSR.W r2, r2, #28 → bits[3:0] = [N,Z,C,V] */
    e32(0xEA4F, 0x7212);
    /* ROR.W r2, r2, #2  → bits[3:0] = [C,V,N,Z] = V810 [CY,OV,S,Z] */
    e32(0xEA4F, 0x02B2);
    if (is_sub) {
        /* EOR.W r2, r2, #8 — invert CY for subtraction */
        e32(0xF082, 0x0208);
    }
    /* AND.W r2, r2, #mask — enforce OV/CY=0 for logic/shift */
    e32(0xF002, (uint16_t)(0x0200 | mask));
    /* LDR.W r1, [r7, #PSW_offset=20] */
    emit_ldr_sreg(1, 20);
    /* BIC.W r1, r1, #0xF — clear lower 4 PSW bits */
    e32(0xF021, 0x010F);
    /* ORR.W r1, r1, r2 */
    e32(0xEA41, 0x0102);
    /* STR.W r1, [r7, #20] */
    emit_str_sreg(1, 20);
}

/* Emit block prologue */
static void emit_prologue(void)
{
    /* PUSH {r4-r8, lr} */
    e32(0xE92D, 0x41F0);
    /* MOV r8,r0; r7,r1; r6,r2; r5,r3 */
    emit_mov_reg(8, 0);
    emit_mov_reg(7, 1);
    emit_mov_reg(6, 2);
    emit_mov_reg(5, 3);
}

/* Emit block epilogue: r0 must already hold next_pc */
static void emit_epilogue(void)
{
    /* MOV r1, r6 (return timestamp in r1) */
    emit_mov_reg(1, 6);
    /* POP {r4-r8, pc} */
    e32(0xE8BD, 0x81F0);
}

/* Emit: r0 = pc; epilogue */
static void emit_exit(uint32_t pc)
{
    emit_mov32(0, pc);
    emit_epilogue();
}

/*
 * Emit a conditional-branch block exit.
 * Tests the V810 condition code against S_REG[PSW], then exits
 * with taken_pc (ADDCLOCK 3) or not_taken_pc (ADDCLOCK 1).
 *
 * Uses r0, r1, r2 as scratch.
 * Enters with r6=timestamp, r7=S_REG base.
 */
static void emit_cond_branch(uint32_t cond, uint32_t taken, uint32_t not_taken)
{
    /* Load PSW into r0 */
    emit_ldr_sreg(0, 20);

    /*
     * For each condition we emit a short test sequence that leaves
     * r1 = non-zero if condition is TRUE (branch taken), 0 if false.
     * Then CBZ/CBNZ or a TST + BEQ selects the path.
     *
     * We use the following Thumb-2 TST T2 (32-bit, modified immediate):
     *   TST r0, #imm:  hw1 = 0xF010, hw2 = 0x0F00 | imm
     * and 16-bit conditional branches BEQ/BNE with forward offset.
     */

    /* forward_branch_idx: position of the conditional branch hw that
       we will patch once we know the offset to .not_taken */
    uint32_t fwd_idx;

    switch (cond) {
    /*  Simple single-bit conditions ──────────────────────────────── */
    case 0:  /* V  = OV  (bit2) */
        e32(0xF010, 0x0F04); /* TST r0, #4 */
        fwd_idx = jit_code_pos;
        e16(0xD000);         /* BEQ .not_taken (placeholder) */
        break;
    case 1:  /* L/C = CY  (bit3) */
        e32(0xF010, 0x0F08); /* TST r0, #8 */
        fwd_idx = jit_code_pos;
        e16(0xD000);
        break;
    case 2:  /* E/Z = Z   (bit0) */
        e32(0xF010, 0x0F01); /* TST r0, #1 */
        fwd_idx = jit_code_pos;
        e16(0xD000);
        break;
    case 4:  /* N/S = S   (bit1) */
        e32(0xF010, 0x0F02); /* TST r0, #2 */
        fwd_idx = jit_code_pos;
        e16(0xD000);
        break;
    case 8:  /* NV = !OV */
        e32(0xF010, 0x0F04); /* TST r0, #4 */
        fwd_idx = jit_code_pos;
        e16(0xD100);         /* BNE .not_taken (OV set → not NV → skip) */
        break;
    case 9:  /* NL/NC = !CY */
        e32(0xF010, 0x0F08); /* TST r0, #8 */
        fwd_idx = jit_code_pos;
        e16(0xD100);
        break;
    case 10: /* NE/NZ = !Z */
        e32(0xF010, 0x0F01); /* TST r0, #1 */
        fwd_idx = jit_code_pos;
        e16(0xD100);
        break;
    case 12: /* P/NS = !S */
        e32(0xF010, 0x0F02); /* TST r0, #2 */
        fwd_idx = jit_code_pos;
        e16(0xD100);
        break;

    /*  Two-bit conditions ─────────────────────────────────────────── */
    case 3:  /* NH = Z||CY: taken if bit0||bit3 set */
        /* AND r1, r0, #9; TST r1, r1 */
        e32(0xF000, 0x0109); /* AND.W r1, r0, #9 */
        e32(0xEA10, 0x0F01); /* TST.W r1, r1 (like ANDS r15, r1, r1) */
        fwd_idx = jit_code_pos;
        e16(0xD000);         /* BEQ .not_taken (nothing set) */
        break;
    case 11: /* H = !(Z||CY): taken if neither bit0 nor bit3 set */
        e32(0xF000, 0x0109); /* AND.W r1, r0, #9 */
        e32(0xEA10, 0x0F01); /* TST.W r1, r1 */
        fwd_idx = jit_code_pos;
        e16(0xD100);         /* BNE .not_taken (any bit set → not H) */
        break;

    /*  XOR conditions ─────────────────────────────────────────────── */
    case 6:  /* LT = S^OV: taken when (PSW_S>>1)^(PSW_OV>>2) ≠ 0 */
        /* LSR.W r1, r0, #1: imm3=0,imm2=1,type=01,Rd=1,Rm=0 → 0x0150 */
        e32(0xEA4F, 0x0150); /* LSR.W r1, r0, #1 (S bit → r1[0]) */
        /* LSR.W r2, r0, #2: imm3=0,imm2=2,type=01,Rd=2,Rm=0 → 0x0290 */
        e32(0xEA4F, 0x0290); /* LSR.W r2, r0, #2 (OV bit → r2[0]) */
        e32(0xEA81, 0x0102); /* EOR.W r1, r1, r2 */
        e32(0xF011, 0x0F01); /* TST.W r1, #1 */
        fwd_idx = jit_code_pos;
        e16(0xD000);         /* BEQ .not_taken (S==OV → not LT) */
        break;
    case 14: /* GE = !(LT) = !(S^OV) */
        e32(0xEA4F, 0x0150); /* LSR.W r1, r0, #1 */
        e32(0xEA4F, 0x0290); /* LSR.W r2, r0, #2 */
        e32(0xEA81, 0x0102); /* EOR.W r1, r1, r2 */
        e32(0xF011, 0x0F01); /* TST.W r1, #1 */
        fwd_idx = jit_code_pos;
        e16(0xD100);         /* BNE .not_taken (S!=OV → not GE) */
        break;
    case 7:  /* LE = (S^OV)||Z */
        e32(0xEA4F, 0x0150); /* LSR.W r1, r0, #1 */
        e32(0xEA4F, 0x0290); /* LSR.W r2, r0, #2 */
        e32(0xEA81, 0x0102); /* EOR.W r1, r1, r2  (r1 bit0 = S^OV) */
        e32(0xEA41, 0x0100); /* ORR.W r1, r1, r0  (r1 bit0 |= Z from PSW bit0) */
        e32(0xF011, 0x0F01); /* TST.W r1, #1 */
        fwd_idx = jit_code_pos;
        e16(0xD000);         /* BEQ .not_taken */
        break;
    case 15: /* GT = !((S^OV)||Z) */
        e32(0xEA4F, 0x0150); /* LSR.W r1, r0, #1 */
        e32(0xEA4F, 0x0290); /* LSR.W r2, r0, #2 */
        e32(0xEA81, 0x0102);
        e32(0xEA41, 0x0100);
        e32(0xF011, 0x0F01);
        fwd_idx = jit_code_pos;
        e16(0xD100);         /* BNE .not_taken */
        break;

    case 5:  /* T = always taken — no test needed */
        emit_addclock(3);
        emit_exit(taken);
        return;              /* done, both paths handled */

    case 13: /* F = always not taken */
    default:
        emit_addclock(1);
        emit_exit(not_taken);
        return;
    }

    /* ── Taken path ─────────────────────────────────────────────────── */
    emit_addclock(3);
    emit_exit(taken);

    /* ── Patch forward branch to point here (not-taken path) ─────────── */
    {
        int32_t imm8 = (int32_t)jit_code_pos - (int32_t)fwd_idx - 2;
        /* imm8 should be 0-127: taken path is ~10 halfwords */
        jit_code_buf[fwd_idx] = (uint16_t)((jit_code_buf[fwd_idx] & 0xFF00) | (uint8_t)imm8);
    }

    /* ── Not-taken path ─────────────────────────────────────────────── */
    emit_addclock(1);
    emit_exit(not_taken);
}

/* ── SETF helper ─────────────────────────────────────────────────────── */

/*
 * Emit code to test condition cond against S_REG[PSW] and store 0 or 1
 * in P_REG[rd].
 * Uses r0, r1, r2 as scratch.
 */
static void emit_setf(uint32_t cond, int rd)
{
    emit_ldr_sreg(0, 20); /* r0 = PSW */

    /* Compute r1 = condition result (0 or 1) */
    switch (cond & 0xF) {
    case 0:  /* V */  e32(0xEA4F,(uint16_t)((1<<8)|(2<<6)|(1<<4)|0)); break; /* LSR r1,r0,#2; will AND below */
    case 1:  /* L */  e32(0xEA4F,(uint16_t)((1<<8)|(3<<6)|(1<<4)|0)); break; /* LSR r1,r0,#3 */
    case 2:  /* Z */  emit_mov_reg(1,0); e32(0xF001,0x0101); break;           /* AND r1,r0,#1 */
    case 4:  /* S */  e32(0xEA4F,(uint16_t)((1<<8)|(1<<6)|(1<<4)|0)); break; /* LSR r1,r0,#1 */
    case 8:  /* NV */ e32(0xEA4F,(uint16_t)((1<<8)|(2<<6)|(1<<4)|0)); /* LSR r1,r0,#2 */
             e32(0xF091,0x0101); /* EORS r1,r1,#1 */ break;
    case 9:  /* NC */ e32(0xEA4F,(uint16_t)((1<<8)|(3<<6)|(1<<4)|0));
             e32(0xF091,0x0101); break;
    case 10: /* NZ */ emit_mov_reg(1,0); e32(0xF001,0x0101);
             e32(0xF091,0x0101); break;
    case 12: /* NS */ e32(0xEA4F,(uint16_t)((1<<8)|(1<<6)|(1<<4)|0));
             e32(0xF091,0x0101); break;
    case 3:  /* NH = Z||CY: r1 = (CY bit | Z bit) & 1 */
             e32(0xEA4F,(uint16_t)((1<<8)|(3<<6)|(1<<4)|0)); /* LSR r1,r0,#3 (CY→bit0) */
             e32(0xEA41,(uint16_t)(0x0100));                  /* ORR r1,r1,r0 (|Z) */
             e32(0xF001,0x0101);                              /* AND r1,r1,#1 */
             break;
    case 11: /* H = !(Z||CY) */
             e32(0xEA4F,(uint16_t)((1<<8)|(3<<6)|(1<<4)|0));
             e32(0xEA41,(uint16_t)(0x0100));
             e32(0xF001,0x0101);
             e32(0xF091,0x0101); /* EOR #1 (NOT) */
             break;
    case 6:  /* LT = S^OV */
        e32(0xEA4F, 0x0150); /* LSR.W r1, r0, #1 (S bit) */
        e32(0xEA4F, 0x0290); /* LSR.W r2, r0, #2 (OV bit) */
        e32(0xEA81, 0x0102); /* EOR.W r1, r1, r2 */
        e32(0xF001, 0x0101); /* AND.W r1, r1, #1 */
        break;
    case 14: /* GE = !LT */
        e32(0xEA4F, 0x0150);
        e32(0xEA4F, 0x0290);
        e32(0xEA81, 0x0102);
        e32(0xF001, 0x0101);
        e32(0xF091, 0x0101);
        break;
    case 7:  /* LE = (S^OV)||Z */
        e32(0xEA4F, 0x0150);
        e32(0xEA4F, 0x0290);
        e32(0xEA81, 0x0102);
        e32(0xEA41, 0x0100); /* ORR.W r1, r1, r0 (|Z) */
        e32(0xF001, 0x0101);
        break;
    case 15: /* GT = !LE */
        e32(0xEA4F, 0x0150);
        e32(0xEA4F, 0x0290);
        e32(0xEA81, 0x0102);
        e32(0xEA41, 0x0100);
        e32(0xF001, 0x0101);
        e32(0xF091, 0x0101);
        break;
    case 5:  /* T = always 1 */
        /* MOVW r1, #1 */
        e32(0xF240, (uint16_t)((1 << 8) | 1));
        break;
    case 13: /* F = always 0 */
    default:
        e32(0xF240, (uint16_t)(1 << 8)); /* MOVW r1, #0 */
        break;
    }
    /* AND r1, r1, #1  (ensure exactly 0 or 1) */
    e32(0xF001, 0x0101);
    emit_str_preg(1, rd);
}

/* ── Main translator ─────────────────────────────────────────────────── */

static int jit_translate(JitBlock *blk, uint32_t start_pc)
{
    uint32_t  cur_pc = start_pc;
    uint32_t  n      = 0;
    int       ended  = 0;

    blk->vb_pc      = start_pc;
    blk->code_thumb = (uintptr_t)ep_bytes() | 1u; /* Thumb bit */
    uint32_t code_start = jit_code_pos;

    emit_prologue();

    while (!ended
           && n < JIT_MAX_OPS
           && (jit_code_pos + JIT_MAX_BLOCK_HW) < JIT_CODE_WORDS)
    {
        uint16_t hw1   = rom_read16(cur_pc);
        uint32_t op7   = hw1 >> 9;         /* 7-bit dispatch index */
        uint32_t op6   = op7 >> 1;         /* 6-bit opcode         */
        uint32_t arg1, arg2, arg3;
        int32_t  imm;

        /* Format I/II: arg1=bits[4:0] arg2=bits[9:5] */
        arg1 = hw1 & 0x1F;
        arg2 = (hw1 >> 5) & 0x1F;
        arg3 = 0;

        /* Read second halfword for 4-byte instructions */
        uint16_t hw2 = 0;
        int is4 = (op6 >= 0x28);   /* Format IV/V/VI */
        if (is4) {
            hw2 = rom_read16(cur_pc + 2);
        }

        switch (op6) {

        /* ── MOV reg ──────────────────────────────────────────────── */
        case 0x00:
            /* P_REG[arg2] = P_REG[arg1]; ADDCLOCK(1) */
            emit_ldr_preg(0, (int)arg1);
            emit_str_preg(0, (int)arg2);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── ADD reg ──────────────────────────────────────────────── */
        case 0x01:
            emit_ldr_preg(0, (int)arg2);  /* r0 = P_REG[arg2] */
            emit_ldr_preg(1, (int)arg1);  /* r1 = P_REG[arg1] */
            /* ADDS.W r0, r0, r1 */
            e32(0xEB10, 0x0001);
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0xF);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── SUB reg ──────────────────────────────────────────────── */
        case 0x02:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            /* SUBS.W r0, r0, r1 */
            e32(0xEBB0, 0x0001);
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(1, 0xF);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── CMP reg ──────────────────────────────────────────────── */
        case 0x03:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            /* SUBS.W r0, r0, r1 (result discarded) */
            e32(0xEBB0, 0x0001);
            emit_psw_flags(1, 0xF);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── SHL reg (arg2 <<= arg1 & 31) ────────────────────────── */
        case 0x04:
            emit_ldr_preg(0, (int)arg2);  /* r0 = operand */
            emit_ldr_preg(1, (int)arg1);  /* r1 = shift count */
            /* AND.W r1, r1, #31 */
            e32(0xF001, 0x011F);
            /* LSLS.W r0, r0, r1 — sets ARM C = last bit shifted out */
            e32(0xFA10, (uint16_t)(0xF000 | 1)); /* LSLS T2 Rd=r0 Rm=r1 */
            emit_str_preg(0, (int)arg2);
            /* CY from ARM C (no inversion); but if shift=0, ARM C is stale →
               handle: CBZ r1 skips CY update. We just use mask 0xB (S,Z,CY no OV)
               and then zero CY if r1==0. */
            emit_psw_flags(0, 0xB);
            /* If shift count was 0, re-clear CY: test r1 then BIC PSW_CY */
            {
                /* CBZ r1, .skip (only 6 bytes = 3 halfwords away) */
                uint32_t cbz_pos = jit_code_pos;
                e16(0xB101); /* CBZ r1, offset=TBD */
                /* BIC.W r1-scratch, PSW, #8 */
                emit_ldr_sreg(1, 20);
                e32(0xF021, 0x0108); /* BIC.W r1, r1, #8 */
                emit_str_sreg(1, 20);
                /* Patch CBZ: target = jit_code_pos, imm = (pos - cbz_pos - 2) halfwords */
                int cbz_imm = (int)jit_code_pos - (int)cbz_pos - 2; /* in halfwords */
                /* CBZ T1: 1011 0 i imm5 Rn  (Rn=r1=001) */
                /* offset in bytes = imm*2, so imm_field = cbz_imm */
                /* CBZ r1: 0xB101 | ((cbz_imm & 0x1F) << 3) | ((cbz_imm >> 5) << 9) */
                jit_code_buf[cbz_pos] = (uint16_t)(0xB101
                    | ((cbz_imm & 0x1F) << 3)
                    | (((cbz_imm >> 5) & 1) << 9));
            }
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── SHR reg (logical, arg2 >>= arg1 & 31) ───────────────── */
        case 0x05:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            e32(0xF001, 0x011F); /* AND r1,r1,#31 */
            /* LSRS T2: FA30 F000|rm */
            e32(0xFA30, (uint16_t)(0xF000 | 1));
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0xB);
            /* zero CY if shift=0 */
            {
                uint32_t cbz_pos = jit_code_pos;
                e16(0xB101);
                emit_ldr_sreg(1, 20);
                e32(0xF021, 0x0108);
                emit_str_sreg(1, 20);
                int cbz_imm = (int)jit_code_pos - (int)cbz_pos - 2;
                jit_code_buf[cbz_pos] = (uint16_t)(0xB101
                    | ((cbz_imm & 0x1F) << 3)
                    | (((cbz_imm >> 5) & 1) << 9));
            }
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── JMP reg ──────────────────────────────────────────────── */
        case 0x06:
            emit_ldr_preg(0, (int)arg1);
            /* BIC.W r0, r0, #1 (clear low bit) */
            e32(0xF020, 0x0001);
            emit_addclock(3);
            emit_epilogue();
            ended = 1; cur_pc += 2; n++;
            break;

        /* ── SAR reg (arithmetic right shift) ─────────────────────── */
        case 0x07:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            e32(0xF001, 0x011F);
            /* ASRS T2: FA50 F000|rm */
            e32(0xFA50, (uint16_t)(0xF000 | 1));
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0xB);
            {
                uint32_t cbz_pos = jit_code_pos;
                e16(0xB101);
                emit_ldr_sreg(1, 20);
                e32(0xF021, 0x0108);
                emit_str_sreg(1, 20);
                int cbz_imm = (int)jit_code_pos - (int)cbz_pos - 2;
                jit_code_buf[cbz_pos] = (uint16_t)(0xB101
                    | ((cbz_imm & 0x1F) << 3)
                    | (((cbz_imm >> 5) & 1) << 9));
            }
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── MUL / DIV / MULU / DIVU ─── fallback ────────────────── */
        case 0x08: case 0x09: case 0x0A: case 0x0B:
            ended = 1; /* exit; interpreter handles these */
            break;

        /* ── OR ───────────────────────────────────────────────────── */
        case 0x0C:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            /* ORRS.W r0, r0, r1 */
            e32(0xEA50, 0x0001);
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── AND ──────────────────────────────────────────────────── */
        case 0x0D:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            /* ANDS.W r0, r0, r1 */
            e32(0xEA10, 0x0001);
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── XOR ──────────────────────────────────────────────────── */
        case 0x0E:
            emit_ldr_preg(0, (int)arg2);
            emit_ldr_preg(1, (int)arg1);
            /* EORS.W r0, r0, r1 */
            e32(0xEA90, 0x0001);
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── NOT ──────────────────────────────────────────────────── */
        case 0x0F:
            emit_ldr_preg(0, (int)arg1);
            /* MVNS.W r0, r0 */
            e32(0xEA7F, 0x0000);
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── MOV_I (5-bit sign-extended immediate) ────────────────── */
        case 0x10:
            imm = sx5(arg1);
            emit_mov32(0, (uint32_t)imm);
            emit_str_preg(0, (int)arg2);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── ADD_I ────────────────────────────────────────────────── */
        case 0x11:
            imm = sx5(arg1);
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)imm);
            e32(0xEB10, 0x0001); /* ADDS.W r0, r0, r1 */
            emit_str_preg(0, (int)arg2);
            emit_psw_flags(0, 0xF);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── SETF ─────────────────────────────────────────────────── */
        case 0x12:
            emit_setf(arg1, (int)arg2);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── CMP_I ────────────────────────────────────────────────── */
        case 0x13:
            imm = sx5(arg1);
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)imm);
            e32(0xEBB0, 0x0001); /* SUBS.W r0, r0, r1 */
            emit_psw_flags(1, 0xF);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;

        /* ── SHL_I ────────────────────────────────────────────────── */
        case 0x14: {
            uint32_t sh = arg1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            if (sh == 0) {
                /* no shift, CY=0, update S and Z only */
                /* MOVS r1, r0 to get N/Z flags */
                e32(0xEA5F, 0x0100); /* MOVS.W r1, r0 */
                emit_psw_flags(0, 0x3);
            } else {
                /* LSL.W r0, r0, #sh: hw1=0xEA4F, hw2=(imm3<<12)|(Rd<<8)|(imm2<<6)|(type<<4)|Rm */
                uint32_t imm3 = sh >> 2, imm2 = sh & 3;
                /* For LSLS (with flag): hw1=0xEA5F */
                e32(0xEA5F, (uint16_t)((imm3<<12)|(0<<8)|(imm2<<6)|(0<<4)|0));
                emit_psw_flags(0, 0xB);
            }
            emit_str_preg(0, (int)arg2);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;
        }

        /* ── SHR_I ────────────────────────────────────────────────── */
        case 0x15: {
            uint32_t sh = arg1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            if (sh == 0) {
                e32(0xEA5F, 0x0100);
                emit_psw_flags(0, 0x3);
            } else {
                /* LSRS.W r0, r0, #sh */
                uint32_t imm3 = sh >> 2, imm2 = sh & 3;
                e32(0xEA5F, (uint16_t)((imm3<<12)|(0<<8)|(imm2<<6)|(1<<4)|0));
                emit_psw_flags(0, 0xB);
            }
            emit_str_preg(0, (int)arg2);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;
        }

        /* ── EI (enable interrupts) ───────────────────────────────── */
        case 0x16:
            /* S_REG[PSW] &= ~PSW_ID; then RecalcIPendingCache can't be
               called from JIT → end block so interpreter handles it */
            ended = 1;
            break;

        /* ── SAR_I ────────────────────────────────────────────────── */
        case 0x17: {
            uint32_t sh = arg1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            if (sh == 0) {
                e32(0xEA5F, 0x0100);
                emit_psw_flags(0, 0x3);
            } else {
                /* ASRS.W r0, r0, #sh */
                uint32_t imm3 = sh >> 2, imm2 = sh & 3;
                e32(0xEA5F, (uint16_t)((imm3<<12)|(0<<8)|(imm2<<6)|(2<<4)|0));
                emit_psw_flags(0, 0xB);
            }
            emit_str_preg(0, (int)arg2);
            emit_addclock(1);
            cur_pc += 2; n++;
            break;
        }

        /* ── TRAP RETI HALT ─── fallback ─────────────────────────── */
        case 0x18: case 0x19: case 0x1A:
            ended = 1;
            break;

        /* ── LDSR / STSR / DI / BSTR ─── fallback ────────────────── */
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            ended = 1;
            break;

        /* ── MOVEA: P_REG[arg3] = P_REG[arg2] + sign_16(hw2) ─────── */
        case 0x28:
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            imm  = sx16(hw2);
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)imm);
            e32(0xEB00, 0x0001); /* ADD.W r0, r0, r1 (no flags) */
            emit_str_preg(0, (int)arg3);
            emit_addclock(1);
            cur_pc += 4; n++;
            break;

        /* ── ADDI: same as MOVEA but with PSW update ──────────────── */
        case 0x29:
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            imm  = sx16(hw2);
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)imm);
            e32(0xEB10, 0x0001); /* ADDS.W r0, r0, r1 */
            emit_str_preg(0, (int)arg3);
            emit_psw_flags(0, 0xF);
            emit_addclock(1);
            cur_pc += 4; n++;
            break;

        /* ── JR: PC += sign_26(arg1) ──────────────────────────────── */
        case 0x2A: {
            uint32_t disp26 = ((uint32_t)(hw1 & 0x3FF) << 16) | hw2;
            int32_t  off    = sx26(disp26) & (int32_t)0xFFFFFFFE;
            uint32_t target = (uint32_t)((int32_t)cur_pc + off);
            emit_addclock(3);
            emit_exit(target);
            ended = 1; cur_pc += 4; n++;
            break;
        }

        /* ── JAL: P_REG[31]=PC+4; PC+=sign_26(arg1) ─────────────── */
        case 0x2B: {
            uint32_t disp26 = ((uint32_t)(hw1 & 0x3FF) << 16) | hw2;
            int32_t  off    = sx26(disp26) & (int32_t)0xFFFFFFFE;
            uint32_t target = (uint32_t)((int32_t)cur_pc + off);
            uint32_t link   = cur_pc + 4;
            emit_mov32(0, link);
            emit_str_preg(0, 31);    /* P_REG[31] = link */
            emit_addclock(3);
            emit_exit(target);
            ended = 1; cur_pc += 4; n++;
            break;
        }

        /* ── ORI ──────────────────────────────────────────────────── */
        case 0x2C:
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)(uint16_t)hw2); /* zero-extended */
            e32(0xEA50, 0x0001); /* ORRS.W r0, r0, r1 */
            emit_str_preg(0, (int)arg3);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 4; n++;
            break;

        /* ── ANDI ─────────────────────────────────────────────────── */
        case 0x2D:
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)(uint16_t)hw2); /* zero-extended */
            e32(0xEA10, 0x0001); /* ANDS.W r0, r0, r1 */
            emit_str_preg(0, (int)arg3);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 4; n++;
            break;

        /* ── XORI ─────────────────────────────────────────────────── */
        case 0x2E:
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)(uint16_t)hw2); /* zero-extended */
            e32(0xEA90, 0x0001); /* EORS.W r0, r0, r1 */
            emit_str_preg(0, (int)arg3);
            emit_psw_flags(0, 0x3);
            emit_addclock(1);
            cur_pc += 4; n++;
            break;

        /* ── MOVHI: P_REG[arg3] = P_REG[arg2] + (hw2 << 16) ─────── */
        case 0x2F:
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            emit_ldr_preg(0, (int)arg2);
            emit_mov32(1, (uint32_t)hw2 << 16);
            e32(0xEB00, 0x0001); /* ADD.W r0, r0, r1 */
            emit_str_preg(0, (int)arg3);
            emit_addclock(1);
            cur_pc += 4; n++;
            break;

        /* ── LD_B: P_REG[arg3] = sign_ext8(mem[P_REG[arg2]+sign16(hw2)]) */
        case 0x30: {
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            imm  = sx16(hw2);
            emit_ldr_preg(1, (int)arg2);       /* r1 = base */
            emit_mov32(2, (uint32_t)imm);       /* r2 = disp */
            e32(0xEB01, 0x0102);                /* ADD.W r1, r1, r2 → EA */
            emit_mov_reg(0, 6);                 /* r0 = timestamp */
            emit_bl((const void *)jit_mem_r8s);
            emit_str_preg(0, (int)arg3);
            emit_addclock(3);
            cur_pc += 4; n++;
            break;
        }

        /* ── LD_H: P_REG[arg3] = sign_ext16(mem[...]) ────────────── */
        case 0x31: {
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            imm  = sx16(hw2);
            emit_ldr_preg(1, (int)arg2);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);
            /* align: addr &= ~1 */
            e32(0xF021, 0x0101); /* BIC.W r1, r1, #1 */
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_r16s);
            emit_str_preg(0, (int)arg3);
            emit_addclock(3);
            cur_pc += 4; n++;
            break;
        }

        /* ── LD_W: 32-bit load via two 16-bit reads ──────────────── */
        case 0x33: {
            arg3 = (hw1 >> 5) & 0x1F;
            arg2 = hw1 & 0x1F;
            imm  = sx16(hw2);
            /* Read low halfword */
            emit_ldr_preg(1, (int)arg2);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);        /* r1 = base + disp (EA) */
            e32(0xF021, 0x0103);        /* r1 &= ~3 (align to word) */
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_r16s);
            /* UXTH T2: hw1=0xFA1F, hw2=(0xF<<12)|(Rd<<8)|0x80|Rm */
            e32(0xFA1F, 0xF380);        /* UXTH r3, r0 — save zero-extended low 16 bits */
            /* Read high halfword at EA+2 */
            emit_ldr_preg(1, (int)arg2);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);
            e32(0xF021, 0x0103);        /* r1 &= ~3 */
            e32(0xF101, 0x0102);        /* ADD.W r1, r1, #2 → EA+2 */
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_r16s);
            /* LSL.W r0, r0, #16: imm3=4,imm2=0,type=0,Rd=0,Rm=0 → hw2=0x4000 */
            e32(0xEA4F, 0x4000);        /* LSL.W r0, r0, #16 */
            e32(0xEA40, 0x0003);        /* ORR r0, r0, r3 */
            emit_str_preg(0, (int)arg3);
            emit_addclock(4);
            cur_pc += 4; n++;
            break;
        }

        /* ── ST_B ─────────────────────────────────────────────────── */
        case 0x34: {
            arg1 = (hw1 >> 5) & 0x1F;  /* src reg */
            arg3 = hw1 & 0x1F;          /* base reg */
            imm  = sx16(hw2);
            emit_ldr_preg(1, (int)arg3);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);        /* r1 = EA */
            emit_ldr_preg(2, (int)arg1);
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_w8);
            emit_addclock(2);
            cur_pc += 4; n++;
            break;
        }

        /* ── ST_H ─────────────────────────────────────────────────── */
        case 0x35: {
            arg1 = (hw1 >> 5) & 0x1F;
            arg3 = hw1 & 0x1F;
            imm  = sx16(hw2);
            emit_ldr_preg(1, (int)arg3);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);
            e32(0xF021, 0x0101); /* BIC r1, r1, #1 */
            emit_ldr_preg(2, (int)arg1);
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_w16);
            emit_addclock(2);
            cur_pc += 4; n++;
            break;
        }

        /* ── ST_W ─────────────────────────────────────────────────── */
        case 0x37: {
            arg1 = (hw1 >> 5) & 0x1F;
            arg3 = hw1 & 0x1F;
            imm  = sx16(hw2);
            /* Write low halfword */
            emit_ldr_preg(1, (int)arg3);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);
            e32(0xF021, 0x0103); /* BIC r1, r1, #3 */
            emit_ldr_preg(2, (int)arg1);
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_w16);
            /* Write high halfword */
            emit_ldr_preg(1, (int)arg3);
            emit_mov32(2, (uint32_t)imm);
            e32(0xEB01, 0x0102);
            e32(0xF021, 0x0103);        /* r1 &= ~3 */
            e32(0xF101, 0x0102);        /* ADD.W r1, r1, #2 → EA+2 */
            emit_ldr_preg(2, (int)arg1);
            /* LSR.W r2, r2, #16: imm3=4,imm2=0,type=01,Rd=2,Rm=2 → hw2=0x4212 */
            e32(0xEA4F, 0x4212); /* LSR.W r2, r2, #16 */
            emit_mov_reg(0, 6);
            emit_bl((const void *)jit_mem_w16);
            emit_addclock(2);
            cur_pc += 4; n++;
            break;
        }

        /* ── Branches (Format III, op7 in 0x40-0x4F) ──────────────── */
        /* Note: op6 for branches = 0x20-0x27 (handled separately below) */

        /* ── NOP / BP ────────────────────────────────────────────── */
        case 0x26: /* op7=0x4D (NOP) or op7=0x4C (BP, cond=12) */
            if (op7 == 0x4D) {
                emit_addclock(1);
                cur_pc += 2; n++;
            } else { /* BP */
                int32_t disp9 = sx9(hw1 & 0x1FE) & (int32_t)0xFFFFFFFE;
                uint32_t taken_pc    = (uint32_t)((int32_t)cur_pc + disp9);
                uint32_t not_taken_pc = cur_pc + 2;
                emit_cond_branch(12, taken_pc, not_taken_pc);
                ended = 1; cur_pc += 2; n++;
            }
            break;

        /* ── IN / OUT / FPP / CAXI ─── fallback ──────────────────── */
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            ended = 1;
            break;

        /* ── Invalid / unhandled ──────────────────────────────────── */
        default:
            if (op6 >= 0x20 && op6 <= 0x27) {
                /* op7 0x40-0x4F: conditional branches (16 conditions).
                   op6=op7>>1, so two op7 values share each op6.
                   Condition = op7 - 0x40 (not op6 - 0x20). */
                uint32_t bcond = op7 - 0x40;
                int32_t disp9 = sx9(hw1 & 0x1FE) & (int32_t)0xFFFFFFFE;
                uint32_t taken    = (uint32_t)((int32_t)cur_pc + disp9);
                uint32_t not_taken = cur_pc + 2;
                emit_cond_branch(bcond, taken, not_taken);
                ended = 1; cur_pc += 2; n++;
            } else {
                ended = 1; /* unhandled → exit to interpreter */
            }
            break;
        }
    }

    if (!ended) {
        /* Reached instruction limit or code buffer boundary */
        emit_exit(cur_pc);
    }

    blk->hw_count = jit_code_pos - code_start;

    /* Flush D-cache and invalidate I-cache for the newly written code */
    __builtin___clear_cache((char *)&jit_code_buf[code_start],
                            (char *)&jit_code_buf[jit_code_pos]);
    return 1;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void jit_init(const uint8_t *rom, uint32_t rom_mask)
{
    jit_rom      = rom;
    jit_rom_mask = rom_mask;
    jit_flush();
}

void jit_flush(void)
{
    jit_code_pos = 0;
    memset(jit_table, 0, sizeof(jit_table));
}

JitBlock *jit_lookup(uint32_t vb_pc)
{
    /* Only translate ROM-space code (upper byte = 0x07) */
    if ((vb_pc >> 24) != 7)
        return (JitBlock *)0;

    /* Direct-mapped hash: slot = (pc >> 1) & (SIZE-1) */
    uint32_t  slot = (vb_pc >> 1) & (JIT_HTAB_SIZE - 1u);
    JitBlock *blk  = &jit_table[slot];

    if (blk->vb_pc == vb_pc && blk->code_thumb != 0)
        return blk; /* cache hit */

    /* Miss: wrap the write pointer rather than flushing everything. */
    if (jit_code_pos + JIT_MAX_BLOCK_HW > JIT_CODE_WORDS)
        jit_code_pos = 0;

    /* Targeted eviction: invalidate any block whose code overlaps the region
       we are about to write.  O(HTAB_SIZE) scan — only runs on a miss, which
       is rare once the working set is warm. */
    {
        uint32_t cs = jit_code_pos;
        uint32_t ce = cs + JIT_MAX_BLOCK_HW;
        for (uint32_t i = 0; i < JIT_HTAB_SIZE; i++) {
            if (!jit_table[i].code_thumb) continue;
            uint32_t pos = (uint32_t)(
                ((jit_table[i].code_thumb & ~(uintptr_t)1u) - (uintptr_t)jit_code_buf)
                / sizeof(uint16_t));
            if (pos < ce && pos + jit_table[i].hw_count > cs) {
                jit_table[i].vb_pc      = 0;
                jit_table[i].code_thumb = 0;
            }
        }
    }

    blk->vb_pc      = 0;
    blk->code_thumb = 0;
    blk->hw_count   = 0;

    if (!jit_translate(blk, vb_pc))
        return (JitBlock *)0;

    return blk;
}

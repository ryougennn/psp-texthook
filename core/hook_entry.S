# Common hook entry, copied into a small block of user memory by capture.c
# (the game's thread runs it in user mode, so it can't live in the core).
# It saves the game's registers and passes them to the core with a syscall
# whose number capture.c writes over thk_entry_syscall.
#
# On entry the hook's thunk has already:
#   - lowered sp by FRAME_SIZE
#   - stored at and ra into the frame's th_ctx
#   - set at = the hook's index
#   - jal'd here (ra = back into the thunk)
#
# Frame layout (from sp):
#   0   .. 15   outgoing argument area (o32 ABI)
#   16  .. 159  th_ctx: r[32], hi, lo, _ret, hook
#
# Everything except at, ra and sp is restored here; the thunk restores
# those three itself before replaying the displaced instructions. Only
# relative addressing: the code runs from wherever it was copied.

#include "hook_frame.h"

    .set    noreorder
    .set    noat
    .text
    .globl  thk_entry_start
    .globl  thk_entry_syscall
    .globl  thk_entry_end
thk_entry_start:
    # save the registers into the frame's th_ctx
    sw      $2,  R(2)($sp)
    sw      $3,  R(3)($sp)
    sw      $4,  R(4)($sp)
    sw      $5,  R(5)($sp)
    sw      $6,  R(6)($sp)
    sw      $7,  R(7)($sp)
    sw      $8,  R(8)($sp)
    sw      $9,  R(9)($sp)
    sw      $10, R(10)($sp)
    sw      $11, R(11)($sp)
    sw      $12, R(12)($sp)
    sw      $13, R(13)($sp)
    sw      $14, R(14)($sp)
    sw      $15, R(15)($sp)
    sw      $16, R(16)($sp)
    sw      $17, R(17)($sp)
    sw      $18, R(18)($sp)
    sw      $19, R(19)($sp)
    sw      $20, R(20)($sp)
    sw      $21, R(21)($sp)
    sw      $22, R(22)($sp)
    sw      $23, R(23)($sp)
    sw      $24, R(24)($sp)
    sw      $25, R(25)($sp)
    sw      $26, R(26)($sp)
    sw      $27, R(27)($sp)
    sw      $28, R(28)($sp)
    sw      $30, R(30)($sp)
    sw      $0,  R(0)($sp)

    # ...and ra, the game's sp, hi and lo
    sw      $31, CTX_RET($sp)
    addiu   $2, $sp, FRAME_SIZE             # the game's sp at the hooked instruction
    sw      $2,  R(29)($sp)
    mfhi    $2
    sw      $2,  CTX_HI($sp)
    mflo    $2
    sw      $2,  CTX_LO($sp)

    # thkHookEntry(index, ctx)
    move    $4, $1                          # a0 = the hook's index
    addiu   $5, $sp, CTX_BASE               # a1 = th_ctx *
thk_entry_syscall:
    syscall 0                               # replaced by capture.c

    # restore everything but at, ra and sp (the thunk does those)
    lw      $2,  CTX_HI($sp)
    mthi    $2
    lw      $2,  CTX_LO($sp)
    mtlo    $2
    lw      $31, CTX_RET($sp)
    lw      $2,  R(2)($sp)
    lw      $3,  R(3)($sp)
    lw      $4,  R(4)($sp)
    lw      $5,  R(5)($sp)
    lw      $6,  R(6)($sp)
    lw      $7,  R(7)($sp)
    lw      $8,  R(8)($sp)
    lw      $9,  R(9)($sp)
    lw      $10, R(10)($sp)
    lw      $11, R(11)($sp)
    lw      $12, R(12)($sp)
    lw      $13, R(13)($sp)
    lw      $14, R(14)($sp)
    lw      $15, R(15)($sp)
    lw      $16, R(16)($sp)
    lw      $17, R(17)($sp)
    lw      $18, R(18)($sp)
    lw      $19, R(19)($sp)
    lw      $20, R(20)($sp)
    lw      $21, R(21)($sp)
    lw      $22, R(22)($sp)
    lw      $23, R(23)($sp)
    lw      $24, R(24)($sp)
    lw      $25, R(25)($sp)
    lw      $28, R(28)($sp)
    lw      $30, R(30)($sp)

    jr      $31
    nop
thk_entry_end:

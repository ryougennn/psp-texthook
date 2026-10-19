/*
 * Patching the game's code: each hook replaces two instructions with a
 * jump to its thunk (built by capture.c) and a nop. The thunk's tail
 * replays the two instructions and jumps back.
 *
 * The core refuses a hook it can't patch safely (THK_ERR_*): an address in
 * a branch delay slot, a branch at addr+4 (its delay slot would be left
 * behind), an instruction that isn't the one expected, or a thunk out of
 * the jump's reach. A branch at addr itself is relocated into the thunk.
 *
 * A hook whose code isn't loaded yet stays pending and is patched when its
 * module starts (hook_module_started, called by main.c's module watcher).
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman.h>
#include <psploadcore.h>
#include <systemctrl.h>

#include "core.h"
#include "hook_frame.h"
#include "mini.h"

typedef struct {
    u32  addr;
    u32  thunk;
    u32  *tail;         /* THK_TAIL_WORDS in the thunk, filled at patch time */
    u32  expect;
    int  state;         /* THK_PENDING, THK_OK or THK_ERR_* */
} hook;

static hook hooks[MAX_HOOKS];
static int  hook_count;

/* --- MIPS decoding ------------------------------------------------------- */

/*
 * True for any instruction with a delay slot (or otherwise PC-relative),
 * which can't be relocated into a thunk. Allegrex adds VFPU branches.
 */
static int is_branch(u32 op)
{
    switch (op >> 26) {
    case 0x00:                                      /* SPECIAL: jr, jalr */
        return (op & 0x3F) == 0x08 || (op & 0x3F) == 0x09;

    case 0x01: {                                    /* REGIMM: bltz/bgez(l), bltzal/bgezal(l) */
        u32 rt = (op >> 16) & 0x1F;
        return rt <= 0x03 || (rt >= 0x10 && rt <= 0x13);
    }

    case 0x02: case 0x03:                           /* j, jal */
    case 0x04: case 0x05: case 0x06: case 0x07:     /* beq bne blez bgtz */
    case 0x14: case 0x15: case 0x16: case 0x17:     /* ...likely */
        return 1;

    case 0x11:                                      /* COP1 bc1x */
    case 0x12:                                      /* COP2 / VFPU bvf, bvt */
        return ((op >> 21) & 0x1F) == 0x08;
    }
    return 0;
}

/* Branches whose target is relative to their own address. */
static int is_pc_relative(u32 op)
{
    u32 opc = op >> 26;

    return is_branch(op) && opc != 0x00 && opc != MIPS_J && opc != MIPS_JAL;
}

static u32 pc_relative_target(u32 addr, u32 op)
{
    return addr + 4 + ((s32)(s16)(op & 0xFFFF) << 2);
}

/* --- patching ------------------------------------------------------------ */

static int check_target(hook *h)
{
    u32 *p = (u32 *)h->addr;

    if (is_branch(p[-1]))
        return THK_ERR_DELAY_SLOT;

    /*
     * A branch at addr is fine (addr+4 is its delay slot and moves with
     * it), but a branch at addr+4 would leave its delay slot behind.
     */
    if (is_branch(p[1]))
        return THK_ERR_BRANCH;

    if (h->expect && p[0] != h->expect)
        return THK_ERR_EXPECT;

    /* j keeps the top 4 bits of the delay slot's PC. */
    if (((h->addr + 4) & 0xF0000000) != (h->thunk & 0xF0000000))
        return THK_ERR_REACH;
    if (is_pc_relative(p[0]) &&
        (pc_relative_target(h->addr, p[0]) & 0xF0000000) != (h->thunk & 0xF0000000))
        return THK_ERR_REACH;

    return THK_OK;
}

/*
 * Fills the thunk's tail (THK_TAIL_WORDS) so it replays the two displaced
 * instructions and resumes the game:
 *
 *   ordinary pair            PC-relative branch + delay slot
 *   t0: insn0                t0: branch, offset rewritten to t4
 *   t1: insn1                t1: delay slot
 *   t2: j addr+8             t2: j addr+8        (not taken)
 *   t3: nop                  t3: nop
 *   t4: nop                  t4: j branch target (taken)
 *   t5: nop                  t5: nop
 *
 * Absolute jumps (j/jal/jr/jalr) are copied as-is: the thunk is in the same
 * 256 MB region, and anything that links (jal, bal, ...) sets ra to t2,
 * which continues at addr+8 exactly as the original return address would.
 */
static void build_tail(u32 *t, u32 addr, u32 insn0, u32 insn1)
{
    memset(t, 0, THK_TAIL_WORDS * 4);

    t[1] = insn1;
    t[2] = mips_jump(MIPS_J, addr + 8);

    if (is_pc_relative(insn0)) {
        t[0] = (insn0 & 0xFFFF0000) | 3;    /* from t1 (delay slot PC) to t4 */
        t[4] = mips_jump(MIPS_J, pc_relative_target(addr, insn0));
    } else {
        t[0] = insn0;
    }
}

static void apply(hook *h)
{
    u32 *p    = (u32 *)h->addr;
    u32 orig0 = p[0], orig1 = p[1];

    h->state = check_target(h);
    if (h->state != THK_OK) {
        klog("hook %08lx refused (%08x): prev=%08lx insn=%08lx next=%08lx",
             h->addr, h->state, p[-1], orig0, orig1);
        return;
    }

    /* Nothing else may run while the two words are inconsistent. */
    int intr = sceKernelCpuSuspendIntr();
    build_tail(h->tail, h->addr, orig0, orig1);
    p[0] = mips_jump(MIPS_J, h->thunk);
    p[1] = 0;                               /* nop */
    sctrlFlushCache();
    sceKernelCpuResumeIntr(intr);

    klog("hook %08lx -> %08lx installed (orig %08lx %08lx%s)",
         h->addr, h->thunk, orig0, orig1,
         is_branch(orig0) ? ", branch relocated" : "");
}

/* --- installing ---------------------------------------------------------- */

int hook_install(u32 addr, u32 thunk, u32 *tail, u32 expect)
{
    if ((addr & 3) || !user_range(addr - 4, 12))
        return THK_ERR_ARGS;
    if (hook_count >= MAX_HOOKS)
        return THK_ERR_FULL;

    hook *h = &hooks[hook_count++];

    h->addr   = addr;
    h->thunk  = thunk;
    h->tail   = tail;
    h->expect = expect;
    h->state  = THK_PENDING;

    SceModule *mod = (SceModule *)sceKernelFindModuleByAddress(addr);
    if (mod) {
        klog("hook %08lx: target already loaded in %s", addr, mod->modname);
        apply(h);
    } else {
        klog("hook %08lx: waiting for owning module", addr);
    }
    return h->state;
}

/* A module occupying [start, end) is starting: patch its pending hooks. */
void hook_module_started(const char *name, u32 start, u32 end)
{
    for (int i = 0; i < hook_count; i++) {
        hook *h = &hooks[i];

        if (h->state == THK_PENDING && h->addr >= start && h->addr + 8 <= end) {
            klog("module %s (%08lx-%08lx) owns hook %08lx", name, start, end, h->addr);
            apply(h);
        }
    }
}

int thkGetHookState(u32 addr)
{
    for (int i = 0; i < hook_count; i++)
        if (hooks[i].addr == addr)
            return hooks[i].state;
    return THK_ERR_ARGS;
}

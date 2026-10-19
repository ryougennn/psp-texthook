/*
 * 向日葵の教会と長い夏休み (Himawari no Kyoukai to Nagai Natsuyasumi), PSP.
 * Hook from LunaHook (LunaTranslator's ppsspp_1.cpp, GPL-3.0); works on
 * hardware. The executable module is named "fileio".
 */

#include "texthook.h"

/*
 * a0 points at NUL-terminated Shift-JIS. The same call delivers both the
 * speaker's name and the dialogue, so they arrive as separate lines.
 */
static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->a0, 0);
}

/*
 * The hooked instruction is a branch (bne v0, zero); the core relocates it
 * together with its delay slot.
 */
TH_GAME("Himawari no Kyoukai to Nagai Natsuyasumi", TH_IDS("ULJM06321"),
    /* bne v0, zero, +0x46 */
    TH_HOOK_EXPECT(0x0881C444, 0x14400046, on_text));

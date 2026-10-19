/*
 * 涼宮ハルヒの約束 (PSP). Two IDs, probably the regular and limited editions.
 * Ported from LunaHook (LunaTranslator's ppsspp_1.cpp, GPL-3.0:
 * {0x882C6B4, {0, 6, 0, 0, NewLineCharFilterA, ...}}): Shift-JIS, argument
 * index 6.
 */

#include "texthook.h"

/*
 * LunaHook's index 6 is r[4 + 6] = t2: confirmed on hardware with this game,
 * so the r[4 + index] mapping holds beyond index 0.
 */
static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->t2, 0);
}

static void clean(char *s, int size)
{
    th_remove(s, "\n");
}

TH_GAME("Suzumiya Haruhi no Yakusoku", TH_IDS("ULJS00123", "ULJS00124"),
    /* lui v0, 0x0894 */
    TH_HOOK_EXPECT(0x0882C6B4, 0x3C020894, on_text, .filter = clean));

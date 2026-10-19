/*
 * ましろ色シンフォニー *mutsu-no-hana (PSP), HuneX engine. The '^' filter is
 * from LunaHook (LunaTranslator's ppsspp_1.cpp, GPL-3.0); its hook is at a
 * different address (0x08868AB8).
 */

#include "texthook.h"

/* a0 points at the current line as NUL-terminated Shift-JIS. */
static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->a0, 0);
}

/* HuneX text uses '^' as markup (LunaHook strips it for this game too). */
static void clean(char *s, int size)
{
    th_remove(s, "^");
}

TH_GAME("Mashiro-iro Symphony", TH_IDS("ULJM05889"),
    /* addiu sp, sp, -0x10 */
    TH_HOOK_EXPECT(0x08866AF4, 0x27BDFFF0, on_text, .filter = clean));

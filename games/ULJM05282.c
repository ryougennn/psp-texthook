/*
 * AIR (PSP). Same engine as CLANNAD (Prototype). Hook from LunaHook
 * (LunaTranslator's ppsspp_1.cpp, GPL-3.0).
 */

#include "texthook.h"

/* a0 points at the current line as NUL-terminated UTF-16. */
static void on_text(th_ctx *c)
{
    th_emit_utf16(c, c->a0, 0);
}

/* Like CLANNAD, the game wraps long lines with embedded newlines. */
static void clean(char *s, int size)
{
    th_remove(s, "\n");
}

TH_GAME("AIR", TH_IDS("ULJM05282"),
    /* addiu sp, sp, -0x60 */
    TH_HOOK_EXPECT(0x0880C774, 0x27BDFFA0, on_text, .filter = clean));

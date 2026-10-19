/*
 * Kanon (PSP). Same engine as CLANNAD and AIR (Prototype). Hook and
 * newline filter from LunaHook (LunaTranslator's ppsspp_1.cpp, GPL-3.0).
 */

#include "texthook.h"

/* a0 points at the current line as NUL-terminated UTF-16. */
static void on_text(th_ctx *c)
{
    th_emit_utf16(c, c->a0, 0);
}

/*
 * The game wraps long lines with embedded newlines (LunaHook strips them
 * for this game too).
 */
static void clean(char *s, int size)
{
    th_remove(s, "\n");
}

TH_GAME("Kanon", TH_IDS("ULJM05203"),
    /* addiu sp, sp, -0x60 */
    TH_HOOK_EXPECT(0x0880B6E8, 0x27BDFFA0, on_text, .filter = clean));

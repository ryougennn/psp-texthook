/*
 * Tomoyo After ~It's a Wonderful Life~ CS Edition (PSP). Same engine as
 * CLANNAD and AIR (Prototype). Hook from LunaHook (LunaTranslator's
 * ppsspp_1.cpp, GPL-3.0).
 */

#include "texthook.h"

/* a0 points at the current line as NUL-terminated UTF-16. */
static void on_text(th_ctx *c)
{
    th_emit_utf16(c, c->a0, 0);
}

/*
 * Like CLANNAD: wrapped lines carry embedded newlines, and CLANNAD pads the
 * wrapped part with full-width spaces (U+3000). Assumed to be the same here
 * since it's the same engine; not yet seen in a Tomoyo After log.
 */
static void clean(char *s, int size)
{
    th_remove(s, "\n");
    th_remove(s, "\xE3\x80\x80");
}

TH_GAME("Tomoyo After", TH_IDS("ULJM05411"),
    /* addiu sp, sp, -0x60 */
    TH_HOOK_EXPECT(0x0880ED98, 0x27BDFFA0, on_text, .filter = clean));

/*
 * CLANNAD (PSP). Disc 1 is ULJM05338, disc 2 is ULJM05339; both carry the
 * same executable, so one script covers both. Hook from LunaHook
 * (LunaTranslator's ppsspp_1.cpp, GPL-3.0).
 */

#include "texthook.h"

/*
 * Function entry; a0 points at the current line as NUL-terminated UTF-16.
 * Runs once per line.
 */
static void on_text(th_ctx *c)
{
    th_emit_utf16(c, c->a0, 0);
}

/*
 * The game wraps long lines itself with embedded newlines, and pads the
 * wrapped part with full-width spaces (U+3000), which end up mid-sentence
 * once the newline is gone: 「ことで　　　きるかっ！」
 */
static void clean(char *s, int size)
{
    th_remove(s, "\n");
    th_remove(s, "\xE3\x80\x80");
}

TH_GAME("CLANNAD", TH_IDS("ULJM05338", "ULJM05339"),
    /* addiu sp, sp, -0x60 */
    TH_HOOK_EXPECT(0x0880F240, 0x27BDFFA0, on_text, .filter = clean));

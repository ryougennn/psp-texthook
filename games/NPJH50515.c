/*
 * Danganronpa: Kibou no Gakuen to Zetsubou no Koukousei (PSP the Best).
 * Hook and filter from LunaHook (LunaTranslator's ppsspp_1.cpp, GPL-3.0).
 * a0 is the line as UTF-16, with \n and <CLT n> colour tags.
 */

#include "texthook.h"

static void on_text(th_ctx *c)
{
    th_emit_utf16(c, c->a0, 0);
}

static void clean(char *s, int size)
{
    th_remove_between(s, "<CLT", ">");
    th_remove(s, "\n");
}

TH_GAME("Danganronpa", TH_IDS("NPJH50515"),
    /* addiu sp, sp, -0x30 */
    TH_HOOK_EXPECT(0x088540B0, 0x27BDFFD0, on_text, .filter = clean));

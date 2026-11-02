/*
 * Akatsuki no Goei Trinity (PSP). Filter from LunaHook (LunaTranslator's
 * ppsspp_1.cpp, GPL-3.0); the hook is just after the call LunaHook hooks.
 * s1 is a new string as UTF-16. The game's line-breaking tables and the
 * backlog's entries ("#ffffe1e6;" and the line) come through too and are
 * skipped.
 */

#include "texthook.h"

static const u16 tables[][3] = {
    { '%', ')', ',' },
    { '\\', '$', '(' },
    { 0x300C, 0x300E, 0xFF08 },     /* 「『（ */
    { 0x300D, 0x300F, 0xFF09 },     /* 」』） */
};

static void on_text(th_ctx *c)
{
    if (th_read16(c->s1) == '#')
        return;
    for (int i = 0; i < 4; i++)
        if (th_read16(c->s1) == tables[i][0] && th_read16(c->s1 + 2) == tables[i][1] &&
            th_read16(c->s1 + 4) == tables[i][2])
            return;
    th_emit_utf16(c, c->s1, 0);
}

/* % and a letter (%B %S size, %C %L %R alignment...); %d %w %D take a number. */
static void remove_codes(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (r[0] == '%' && ((r[1] >= 'A' && r[1] <= 'Z') || (r[1] >= 'a' && r[1] <= 'z'))) {
            char k = r[1];

            r += 2;
            if (k == 'd' || k == 'w' || k == 'D')
                while (*r >= '0' && *r <= '9')
                    r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

static void clean(char *s, int size)
{
    th_remove(s, "\n");
    remove_codes(s);
    th_remove_between(s, "[", "]");
    th_replace(s, size, "\\\\", "\\");
    th_trim(s);
}

TH_GAME("Akatsuki no Goei Trinity", TH_IDS("ULJM06174", "ULJM06175"),
    /* move a2, v0 */
    TH_HOOK_EXPECT(0x089210C4, 0x00403021, on_text, .filter = clean));

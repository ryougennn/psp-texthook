/*
 * Umineko no Naku Koro ni Portable 1 (PSP). One hook from LunaHook
 * (LunaTranslator's ppsspp_1.cpp, GPL-3.0). The text routine passes the
 * line (a0, Shift-JIS with ASCII markup) on to be drawn on one of two
 * paths, and the PSP takes the other one, so both are hooked.
 */

#include "texthook.h"
#include "mini.h"

static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->a0, 0);
}

/* The text is all full-width, so any ASCII left after the ruby is markup. */
static void remove_markup(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if ((*r >= 'a' && *r <= 'z') || (*r >= '0' && *r <= '9')) {
            r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* rb<reading>rs<base>re -> base. */
static void keep_ruby_base(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (r[0] == 'r' && r[1] == 'b') {
            char *rs = strstr(r + 2, "rs");
            char *re = rs ? strstr(rs + 2, "re") : 0;

            if (re) {
                for (r = rs + 2; r < re; )
                    *w++ = *r++;
                r = re + 2;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static void clean(char *s, int size)
{
    keep_ruby_base(s);
    remove_markup(s);
}

TH_GAME("Umineko no Naku Koro ni Portable 1", TH_IDS("ULJM05968"),
    /* move a1, a0: the two paths */
    TH_HOOK_EXPECT(0x0884B120, 0x00802825, on_text, .filter = clean),
    TH_HOOK_EXPECT(0x0884B138, 0x00802825, on_text, .filter = clean));

/*
 * Corpse Party BloodCovered: ...Repeated Fear (PSP). The hook starts a
 * message: a1 is up to three rows of Shift-JIS, each ending in /n. Rows are
 * padded with spaces, and some kanji have their reading after them.
 */

#include "texthook.h"
#include "mini.h"

static void on_text(th_ctx *c)
{
    if (c->a1)
        th_emit_sjis(c, c->a1, 0);
}

static int space_len(const char *p)
{
    if (*p == ' ')
        return 1;
    return strncmp(p, "\xE3\x80\x80", 3) ? 0 : 3;
}

/* Is the UTF-8 character just before w a kanji (or 々 〆 ヶ)? */
static int kanji_before(const char *s, const char *w)
{
    const unsigned char *p = (const unsigned char *)w - 3;

    if (w - s < 3)
        return 0;
    if (p[0] >= 0xE4 && p[0] <= 0xE9)
        return 1;
    return !memcmp(p, "\xE3\x80\x85", 3) || !memcmp(p, "\xE3\x80\x86", 3) ||
           !memcmp(p, "\xE3\x83\xB6", 3);
}

/* Length of a ruby gloss "(かな)" / "（かな）" at p, or 0. */
static int ruby_len(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int n, kana = 0;

    if (p[0] == '(')
        n = 1;
    else if (!memcmp(p, "\xEF\xBC\x88", 3))
        n = 3;
    else
        return 0;
    for (;;) {
        const unsigned char *q = p + n;

        if (q[0] == ')')
            return kana ? n + 1 : 0;
        if (q[0] == 0xEF && q[1] == 0xBC && q[2] == 0x89)
            return kana ? n + 3 : 0;
        if (q[0] == 0xE3 && ((q[1] == 0x81 && q[2] >= 0x81) || (q[1] == 0x82 && q[2] <= 0x96))) {
            kana = 1;
            n += 3;
        } else if (q[0] == 0xE3 && q[1] == 0x83 && q[2] == 0xBC) {
            n += 3;                                     /* ー */
        } else if (space_len((const char *)q)) {
            n += space_len((const char *)q);
        } else {
            return 0;
        }
    }
}

static void clean(char *s, int size)
{
    char *r = s, *w = s;
    int n;

    while (*r) {
        if (r[0] == '/' && r[1] == 'n') {
            int indent = 0;

            /* the padding at the end of a row is layout, not text */
            while (w > s && (w[-1] == ' ' || (w - s >= 3 && !memcmp(w - 3, "\xE3\x80\x80", 3))))
                w -= w[-1] == ' ' ? 1 : 3;
            for (r += 2; (n = space_len(r)); r += n)
                indent = 1;
            /* an indented row keeps one space; a plain row joins on */
            if (indent && w > s && *r && !(r[0] == '/' && r[1] == 'n')) {
                memcpy(w, "\xE3\x80\x80", 3);
                w += 3;
            }
            continue;
        }
        if (kanji_before(s, w) && (n = ruby_len(r))) {
            r += n;                                     /* 鋏(はさみ) -> 鋏 */
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
    th_trim(s);
}

TH_GAME("Corpse Party BloodCovered Repeated Fear", TH_IDS("ULJM05704"),
    /* addiu sp, sp, -0x20 */
    TH_HOOK_EXPECT(0x08865A0C, 0x27BDFFE0, on_text, .filter = clean));

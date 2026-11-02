/*
 * Dies irae ~Amantes amentes~ (PSP). Page hook from LunaHook
 * (LunaTranslator's ppsspp_1.cpp, GPL-3.0); each part of the page is sent
 * as it's shown (characters s3 to s1).
 */

#include "texthook.h"

static char page[TH_MAX_TEXT + 1];
static char part[TH_MAX_TEXT + 1];

static int sjis_lead(u8 b)
{
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

static void on_page(th_ctx *c)
{
    int n = 0;

    if (c->a0 != 0xCD)
        return;
    while (n < TH_MAX_TEXT && (page[n] = th_read8(c->a3 + n)))
        n++;
    page[n] = 0;
}

static void on_part(th_ctx *c)
{
    u32 i = 0;
    int n = 0;

    for (const char *p = page; *p; ) {
        int len = *p != '\n' && sjis_lead(*p) && p[1] ? 2 : 1;

        if (i >= c->s3 && i < c->s1)
            for (int k = 0; k < len; k++)
                part[n++] = p[k];
        if (*p != '\n')
            i++;
        p += len;
    }
    if (n) {
        part[n] = 0;
        th_emit_sjis(c, (u32)part, n);
    }
}

static const char *const latin[][2] = {
    { "\xE2\x94\xA0", "\xC3\xA4" },     /* ┠ ä */
    { "\xE2\x94\xAF", "\xC3\xBC" },     /* ┯ ü */
    { "\xE2\x94\xB7", "\xC3\xB6" },     /* ┷ ö */
    { "\xE2\x94\xA8", "\xC3\x9F" },     /* ┨ ß */
    { "\xE2\x94\x81", "\xC3\xB0" },     /* ━ ð */
    { "\xE2\x94\xBF", "\xC3\xA6" },     /* ┿ æ */
};

/* Line breaks go; between ASCII they become spaces. */
static void clean(char *s, int size)
{
    char *r = s, *w = s;

    for (; *r; r++) {
        if (*r != '\n')
            *w++ = *r;
        else if (w > s && (u8)w[-1] < 0x80 && r[1] && (u8)r[1] < 0x80 && r[1] != '\n')
            *w++ = ' ';
    }
    *w = 0;

    th_replace(s, size, "\xE7\xBE\x85??", "\xE7\xBE\x85\xE7\x9D\xBA");    /* 羅睺 */
    th_replace(s, size, "\xE6\xAF\x94??", "\xE6\xAF\x94\xE6\xB0\x90");    /* 比氐 */
    th_remove(s, "??");
    for (unsigned i = 0; i < sizeof(latin) / sizeof(latin[0]); i++)
        th_replace(s, size, latin[i][0], latin[i][1]);
    th_trim(s);
}

TH_GAME("Dies irae ~Amantes amentes~", TH_IDS("ULJM06107", "ULJM06108"),
    /* addiu sp, sp, -0x10 */
    TH_HOOK_EXPECT(0x088E08A8, 0x27BDFFF0, on_page),
    /* sw a0, 108(v0) */
    TH_HOOK_EXPECT(0x08884074, 0xAC44006C, on_part, .filter = clean));

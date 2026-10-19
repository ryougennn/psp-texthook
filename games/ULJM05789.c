/*
 * Little Busters! Converted Edition (PSP). A port of the PS2 version
 * (SLPM55203, Cybelle engine), found by matching a PS2 hook against
 * this executable:
 *
 *   PS2 0x126CE8 / PSP 0x88287D0  the line parser. Both set up the same local
 *       state (the engine's 256-byte character-class table, identical in both
 *       executables, and a static text buffer), then call the routine that
 *       fills the buffer: PS2 0x12E070, PSP 0x8827440.
 *   PS2 0x126DFC  the PS2 hook, just after that call returns. On the PSP
 *       the matching instruction is followed by a branch, so the hook goes on
 *       the first instruction after the return instead.
 *   PS2 0x49DCB0 / PSP 0x8A2A97C  the text buffer (uninitialized data in both).
 *
 * The buffer is the engine's "Shift-JIS but not exactly": hiragana are single
 * bytes in the half-width katakana range, everything else is normal Shift-JIS.
 * It also carries the script's markup, which the filter below cleans up. The
 * filter follows the PS2 script, except that the half-width punctuation and
 * the ｢ ｣ brackets are kept, as the game draws them.
 */

#include "texthook.h"
#include "mini.h"

#define TEXT_BUFFER 0x08A2A97C

/*
 * The line is in a static buffer, not in a register: the hook only says it's
 * ready.
 */
static void on_line(th_ctx *c)
{
    th_emit_sjis(c, TEXT_BUFFER, 0);
}

/*
 * Half-width katakana U+FF61..U+FF9D as the engine uses them, by code - 0xFF61.
 * 0 = leave as is: the punctuation ｡ ｢ ｣ ､ ･ stays half-width, as the game
 * sends it, so the text lines up with a screenshot.
 */
static const u16 kana[] = {
    0,      0,      0,      0,      0,                      /* ｡ ｢ ｣ ､ ･ */
    0x3092,                                                 /* ｦ */
    0x3041, 0x3043, 0x3045, 0x3047, 0x3049,                 /* ｧ ｨ ｩ ｪ ｫ */
    0x3083, 0x3085, 0x3087, 0x3063, 0x30FC,                 /* ｬ ｭ ｮ ｯ ｰ */
    0x3042, 0x3044, 0x3046, 0x3048, 0x304A,                 /* ｱ行 */
    0x304B, 0x304D, 0x304F, 0x3051, 0x3053,                 /* ｶ行 */
    0x3055, 0x3057, 0x3059, 0x305B, 0x305D,                 /* ｻ行 */
    0x305F, 0x3061, 0x3064, 0x3066, 0x3068,                 /* ﾀ行 */
    0x306A, 0x306B, 0x306C, 0x306D, 0x306E,                 /* ﾅ行 */
    0x306F, 0x3072, 0x3075, 0x3078, 0x307B,                 /* ﾊ行 */
    0x307E, 0x307F, 0x3080, 0x3081, 0x3082,                 /* ﾏ行 */
    0x3084, 0x3086, 0x3088,                                 /* ﾔ行 */
    0x3089, 0x308A, 0x308B, 0x308C, 0x308D,                 /* ﾗ行 */
    0x308F, 0x3093,                                         /* ﾜ ﾝ */
};

/* U+FF61..U+FF9F in UTF-8, or 0. */
static int halfwidth_at(const char *p)
{
    const u8 *b = (const u8 *)p;

    if (b[0] != 0xEF)
        return 0;
    if (b[1] == 0xBD && b[2] >= 0xA1 && b[2] <= 0xBF)
        return 0xFF00 + (b[2] - 0x80) + 0x40;
    if (b[1] == 0xBE && b[2] >= 0x80 && b[2] <= 0x9F)
        return 0xFF80 + (b[2] - 0x80);
    return 0;
}

static char *put3(char *w, int cp)
{
    *w++ = (char)(0xE0 | (cp >> 12));
    *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *w++ = (char)(0x80 | (cp & 0x3F));
    return w;
}

/*
 * The voiced form of a hiragana, for a following ﾞ (dakuten, +1) or ﾟ
 * (handakuten, +2), or 0. Not in the PS2 script's table; handled in case the
 * engine stores が as ｶﾞ.
 */
static int voiced(int cp, int mark)
{
    int ha = cp == 0x306F || cp == 0x3072 || cp == 0x3075 || cp == 0x3078 || cp == 0x307B;

    if (mark == 0xFF9F)
        return ha ? cp + 2 : 0;                       /* ぱ行 */
    if (cp >= 0x304B && cp <= 0x3061 && (cp & 1))
        return cp + 1;                                /* か..ち -> が..ぢ */
    if (cp == 0x3064 || cp == 0x3066 || cp == 0x3068 || ha)
        return cp + 1;                                /* づ で ど, ば行 */
    if (cp == 0x3046)
        return 0x3094;                                /* ゔ */
    return 0;
}

/* Every replacement is 3 bytes for 3, or shorter, so this works in place. */
static void kana_to_hiragana(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        int hw = halfwidth_at(r);
        int cp = hw && hw <= 0xFF9D ? kana[hw - 0xFF61] : 0;

        if (!cp) {
            *w++ = *r++;
            continue;
        }
        r += 3;

        /* a following ﾞ or ﾟ joins it */
        int mark = halfwidth_at(r);
        if (mark == 0xFF9E || mark == 0xFF9F) {
            int v = voiced(cp, mark);
            if (v) {
                cp = v;
                r += 3;
            }
        }

        w = put3(w, cp);
    }
    *w = 0;
}

/* JavaScript's s.split(mark)[1]: what's between the first and second mark. */
static void keep_after(char *s, const char *mark)
{
    char *a = strstr(s, mark);

    if (!a)
        return;
    a += strlen(mark);

    char *b = strstr(a, mark);
    int   n = b ? b - a : (int)strlen(a);

    memmove(s, a, n);
    s[n] = 0;
}

/*
 * Style codes in system messages, S plus three digits starting with 0, e.g.
 * "メモリースティックS008TMS010を抜いたり" (S008/S010 around the TM mark).
 */
static void remove_style_codes(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (r[0] == 'S' && r[1] == '0' &&
            r[2] >= '0' && r[2] <= '9' &&
            r[3] >= '0' && r[3] <= '9')
            r += 4;
        else
            *w++ = *r++;
    }
    *w = 0;
}

/* Runs of the two characters \n (a script line break) become one space. */
static void breaks_to_space(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (r[0] == '\\' && r[1] == 'n') {
            while (r[0] == '\\' && r[1] == 'n')
                r += 2;
            *w++ = ' ';
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void clean(char *s, int size)
{
    kana_to_hiragana(s);

    /*
     * Speaker names: "name$dtext" and "name｢text｣". The game shows the name
     * in a box of its own, so it's dropped; the brackets stay, half-width as
     * the game draws them (so the text lines up with a screenshot).
     */
    keep_after(s, "$d");
    char *quote = strstr(s, "\xEF\xBD\xA2");   /* ｢ */
    if (quote)
        memmove(s, quote, strlen(quote) + 1);

    breaks_to_space(s);

    /* A trailing \d, a leading @command, #...#, and stray $. */
    int n = strlen(s);
    if (n >= 2 && s[n - 2] == '\\' && s[n - 1] == 'd')
        s[n - 2] = 0;
    if (s[0] == '@' && s[1] >= 'a' && s[1] <= 'z') {
        char *p = s + 1;

        while (*p >= 'a' && *p <= 'z')
            p++;
        memmove(s, p, strlen(p) + 1);
    }
    th_remove_between(s, "#", "#");
    th_remove(s, "$");

    th_remove(s, "\xE3\x80\x80");             /* U+3000 */
    th_remove(s, "@w");
    th_remove(s, "\\c");
    remove_style_codes(s);
}

/*
 * Two discs; the game boots from ULJM05790. Both carry the same executable
 * (identical code and data), checked by the expected values.
 */
TH_GAME("Little Busters! Converted Edition", TH_IDS("ULJM05789", "ULJM05790"),
    /* lw v0, 60(s0): first instruction after the buffer-filling call */
    TH_HOOK_EXPECT(0x0882885C, 0x8E02003C, on_line, .filter = clean));

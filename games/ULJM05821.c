/*
 * CHAOS;HEAD Love Chu☆Chu! (PSP), STEINS;GATE's engine. Filter from LunaHook
 * (LunaTranslator's ppsspp_1.cpp, GPL-3.0). The hook expands the codes in a
 * line (a0, Shift-JIS); the game runs it up to three times per line, so a
 * line is sent only when it differs from the last one (an empty one resets
 * that).
 */

#include "texthook.h"
#include "mini.h"

static char line[TH_MAX_TEXT + 1], last[TH_MAX_TEXT + 1];

static void on_text(th_ctx *c)
{
    int n = 0;

    while (n < TH_MAX_TEXT && (line[n] = th_read8(c->a0 + n)))
        n++;
    line[n] = 0;

    if (!n) {
        last[0] = 0;
        return;
    }
    if (!strcmp(line, last))
        return;
    memcpy(last, line, n + 1);
    th_emit_sjis(c, (u32)last, n);
}

static int is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

/* Removes every `code` followed by exactly `digits` ASCII digits. */
static void remove_numbered(char *s, const char *code, int digits)
{
    char *r = s, *w = s;

    while (*r) {
        int i, j;

        for (i = 0; code[i] && r[i] == code[i]; i++)
            ;
        if (!code[i]) {
            for (j = 0; j < digits && is_digit(r[i + j]); j++)
                ;
            if (j == digits) {
                r += i + j;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static int is_hex(char ch)
{
    return is_digit(ch) || (ch >= 'A' && ch <= 'F');
}

/* %C and up to six hex digits: a colour, or %CE (its end). */
static void remove_colour_c(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (r[0] == '%' && r[1] == 'C' && is_hex(r[2])) {
            int i = 2;

            while (i < 8 && is_hex(r[i]))
                i++;
            r += i;
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* Coloured text: %B<n>S starts it, %B<n>E ends it (n: digits). */
static void remove_colour(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (r[0] == '%' && r[1] == 'B' && is_digit(r[2])) {
            int i = 3;

            while (is_digit(r[i]))
                i++;
            if (r[i] == 'S' || r[i] == 'E') {
                r += i + 1;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* {base}[reading] -> base. */
static void strip_ruby(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '{') {
            char *close = r + 1, *end;

            while (*close && *close != '}')
                close++;
            if (*close && close[1] == '[') {
                for (end = close + 2; *end && *end != ']'; end++)
                    ;
                if (*end) {
                    for (r++; r < close; )
                        *w++ = *r++;
                    r = end + 1;
                    continue;
                }
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static void clean(char *s, int size)
{
    static const char *const codes[] = {
        "%K", "%P", "%p", "%N", "%n", "%V", "%LC", "%LE", "%FS", "%FE",
        "\\n", "\\rs", "\\re",       /* in the phone's mails */
        "\xE2\x98\x86",                 /* ☆ */
    };

    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        th_remove(s, codes[i]);

    /* ├ and ┤ are the halves of a ―― dash. */
    th_replace(s, size, "\xE2\x94\x9C", "\xE2\x80\x95");
    th_replace(s, size, "\xE2\x94\xA4", "\xE2\x80\x95");

    strip_ruby(s);
    remove_colour(s);
    remove_colour_c(s);
    remove_numbered(s, "%O", 3);
    remove_numbered(s, "%t", 3);
    remove_numbered(s, "%S", 3);
    remove_numbered(s, "%A", 1);
}

TH_GAME("CHAOS;HEAD Love Chu Chu!", TH_IDS("ULJM05821"),
    /* addiu sp, sp, -0x430 */
    TH_HOOK_EXPECT(0x088B5AB0, 0x27BDFBD0, on_text, .filter = clean));

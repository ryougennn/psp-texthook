/*
 * ToHeart PORTABLE (PSP). The script is 16-bit units: Shift-JIS characters,
 * or commands (below 0x8140), each followed by a count n and n pairs of
 * values. 0x3E8 starts a line, 0x3F5 a page and 0x3EC ends one; 0x44D +
 * 10 * i + j is character j of player name i. The hook reads each unit (s3)
 * and sends a line whole when it reaches the line's first character.
 */

#include "texthook.h"

#define NAMES 0x08AC9E0C

static char line[TH_MAX_TEXT + 1];

/* The last line sent: its units, and two of them to spot a new script. */
static u32 last_start, last_end;
static u16 last_first, last_final;

static int is_text(u16 u)
{
    return u >= 0x8140;
}

static int is_name(u16 u)
{
    return u >= 0x44D && u < 0x44D + 50 && (u - 0x44D) % 10 < 6;
}

/* Past the commands at p to the next text, or 0 if the line ends first. */
static u32 skip_commands(u32 p, int quoted)
{
    for (int k = 0; k < 8; k++) {
        u16 u = th_read16(p), n = th_read16(p + 2);

        if (is_text(u) || is_name(u))
            return p;
        if (n > 8 || u == 0x3EC || u == 0x3F5 || (u == 0x3E8 && !quoted))
            return 0;
        if (u == 0x3C || u == 0x49 || u == 0x4A)     /* goto, random pick, timed wait */
            return 0;
        p += 4 + 4 * n;
    }
    return 0;
}

static int put(int n, u16 u)
{
    if (u == 0x86A2)
        u = 0x815C;     /* ― */
    line[n++] = u >> 8;
    line[n++] = u & 0xFF;
    return n;
}

static void on_text(th_ctx *c)
{
    u32 p = c->s3;
    u16 u = th_read16(p);
    int n = 0, quoted = 0;      /* 「『 open */

    if (!is_text(u) && !is_name(u))
        return;
    if (p >= last_start && p < last_end && th_read16(last_start) == last_first
        && th_read16(last_end - 2) == last_final)
        return;

    while (n < TH_MAX_TEXT - 2) {
        u = th_read16(p);
        if (is_text(u)) {
            n = put(n, u);
            p += 2;
            if (u == 0x8175 || u == 0x8177)
                quoted++;
            else if ((u == 0x8176 || u == 0x8178) && quoted)
                quoted--;
        } else if (is_name(u)) {
            u32 i = (u - 0x44D) / 10, j = (u - 0x44D) % 10;
            u16 ch = th_read16(NAMES + (i * 6 + j) * 2);

            if (ch)
                n = put(n, ch);
            p += 4;
        } else if (u == 0x3EC || u == 0x3F5 || (u == 0x3E8 && !quoted)) {
            break;
        } else {
            u32 q = skip_commands(p, quoted);

            if (!q)
                break;
            p = q;
        }
    }

    last_start = c->s3;
    last_end = p;
    last_first = th_read16(last_start);
    last_final = th_read16(last_end - 2);

    line[n] = 0;
    th_emit_sjis(c, (u32)line, n);
}

/* Narration is indented with U+3000. */
static void clean(char *s, int size)
{
    th_trim(s);
}

TH_GAME("ToHeart PORTABLE", TH_IDS("ULJM05486"),
    /* lhu v0, 0(s3) */
    TH_HOOK_EXPECT(0x088B77A0, 0x96620000, on_text, .filter = clean));

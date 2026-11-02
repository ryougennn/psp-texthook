/*
 * Eien no Aselia -Kono Daichi no Hate de- (PSP). The hook adds one row of
 * the message box (a1, Shift-JIS). Rows are split by "#02 00" codes, so the
 * whole message is read from its first row and the later rows are skipped.
 */

#include "texthook.h"

static char line[TH_MAX_TEXT + 1];

static int sjis_lead(u8 b)
{
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

static int is_break(u32 p)
{
    return th_read8(p) == '#' && th_read8(p + 1) == 0x02 && th_read8(p + 2) == 0;
}

static void on_text(th_ctx *c)
{
    u32 p = c->a1;
    int n = 0;

    if (is_break(p - 3))
        return;

    while (n < TH_MAX_TEXT - 1) {
        u8 b = th_read8(p);

        if (is_break(p)) {
            p += 3;
            continue;
        }
        if (b == 0 || b == '#')
            break;
        line[n++] = b;
        p++;
        if (sjis_lead(b))
            line[n++] = th_read8(p++);
    }
    if (n) {
        line[n] = 0;
        th_emit_sjis(c, (u32)line, n);
    }
}

TH_GAME("Eien no Aselia -Kono Daichi no Hate de-", TH_IDS("ULJM06017"),
    /* addiu sp, sp, -0x20 */
    TH_HOOK_EXPECT(0x088907D0, 0x27BDFFE0, on_text));

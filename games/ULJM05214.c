/*
 * SNOW Portable (PSP). Not the PS2 engine: the PSP version runs on HuneX's
 * HBadv engine, with the scenario as Shift-JIS bytecode (the PS2 version's
 * custom glyph-index encoding is gone; checked against snow_data.afs).
 *
 * The executable is relocatable. In PPSSPP its code loads at 0x08804000 and
 * its data segment at 0x088DAE00; the addresses below assume the same on
 * the PSP, and the instruction check refuses the hook if they differ.
 *
 * Bytecode, as seen in RAM:
 *   D0 1C "女のコ" D5 「そなた、…見たであろうーー 00 D5 　ーーっ！」 00 D0 02 ...
 * 0x1C sets the speaker name, D5 starts a piece of text, 00 ends it. A line
 * the game wraps by hand is several pieces in a row (00 D5 ...). The text
 * loop (file 0x1ADAC) reads one byte per step; for D5 it branches to 0x1B184,
 * which steps over it, with s0 pointing at the D5. Hooking there sees each
 * piece once, before it's typed out.
 */

#include "texthook.h"

static char line[TH_MAX_TEXT];

static int sjis_lead(u8 b)
{
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

/*
 * Copies the text pieces starting at the D5 at p, joined, into line.
 * Returns 0 if it isn't text: script setup code also uses D5 between
 * arguments at the same level (D6 #101 D5 0 D5 = D5 ...). Text starts with
 * a double-byte character and never contains the engine's single-byte
 * separators (0xD0-0xDF, half-width katakana in Shift-JIS).
 */
static int collect(u32 p)
{
    int n = 0;

    if (!th_valid(p + 1, 1) || !sjis_lead(th_read8(p + 1)))
        return 0;

    while (th_valid(p, 1) && th_read8(p) == 0xD5) {
        p++;
        while (th_valid(p, 2) && n < (int)sizeof(line) - 2) {
            u8 b = th_read8(p++);

            if (!b)
                break;
            if (sjis_lead(b)) {
                line[n++] = (char)b;
                line[n++] = (char)th_read8(p++);
            } else if (b >= 0xD0 && b <= 0xDF) {
                return 0;
            } else {
                line[n++] = (char)b;
            }
        }
        /* p is past the 00; another D5 continues the same line. */
    }
    line[n] = 0;

    return n;
}

static void on_text(th_ctx *c)
{
    u32 p = c->s0;

    /* A piece right after another (00 D5) was already sent with the first. */
    if (!th_valid(p - 1, 2) || th_read8(p - 1) == 0x00)
        return;
    if (collect(p))
        th_emit_sjis(c, (u32)line, 0);
}

/* Wrapped pieces start with an ideographic space to indent under 「. */
static void clean(char *s, int size)
{
    th_remove(s, "\xE3\x80\x80");
}

TH_GAME("SNOW Portable", TH_IDS("ULJM05214"),
    /*
     * lui v1, 0x08B4: relocated hi half of the script pointer's address
     * (data offset 0x264438 + 0x088DAE00). File 0x1B184, code base 0x08804000.
     */
    TH_HOOK_EXPECT(0x0881F184, 0x3C0308B4, on_text, .filter = clean));

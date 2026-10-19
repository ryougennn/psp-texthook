/*
 * アマガミ (PSP), K2X script engine. Ported from the Agent texthooker script
 * PSP_ULJS00339_Amagami.js v1.02 (LunaHook's entry, in LunaTranslator's
 * ppsspp_1.cpp, GPL-3.0, is a port of the same script). The hook sits on
 * the script VM's String.length(), so it sees every string the scripts
 * measure; the rules below keep the dialogue.
 *
 * A VM string object is [[[a0+0x28]]+8]: its length in bytes is at +4, and
 * its characters at [[obj+8]+0xC], which is where the game's own
 * toByteArray (0x08867778) reads them. Agent reads obj+0x20 and falls back
 * to that pointer when it looks empty ("pink text"); LunaHook's fallback
 * reads [[obj]+0xC], which is wrong. Reading the game's way needs neither.
 *
 * Each character is a 16-bit unit holding its Shift-JIS code (lead byte in
 * the high half), so the hook rebuilds plain Shift-JIS. Units below 0x100
 * are control codes.
 */

#include "texthook.h"

/*
 * The hook runs on the game's stack, so the buffer is static; only the VM's
 * thread calls it. A unit becomes at most 6 bytes ("PLAYER").
 */
static char line[TH_MAX_TEXT + 1];

/*
 * Strings still to drop: the one after an 'N' control code is the player's
 * name, which the VM measures on its own. Counts every call, emitted or
 * not, as Agent does.
 */
static int skip;

static void on_text(th_ctx *c)
{
    u32 obj      = th_read32(th_read32(th_read32(c->a0 + 0x28)) + 8);
    u32 units    = th_read32(obj + 4) / 2;
    u32 p        = th_read32(th_read32(obj + 8) + 0xC);
    int skipping = skip;
    int n        = 0;

    /*
     * A string starting with a control code other than an uppercase
     * letter isn't dialogue (Agent: "make choice cleaner").
     */
    u16 first = units ? th_read16(p) : 0;
    if (first >= 0x100 || (first >= 'A' && first <= 'Z')) {
        for (u32 i = 0; i < units; i++) {
            u16 u = th_read16(p + i * 2);

            if (u == 0 || n > TH_MAX_TEXT - 6)
                break;
            if (u == 0x815E) {          /* ／ marks a line break */
                line[n++] = ' ';
            } else if (u >= 0x100) {
                line[n++] = u >> 8;
                line[n++] = u & 0xFF;
            } else if (u == 'N') {      /* the player's name goes here */
                for (const char *s = "PLAYER"; *s; s++)
                    line[n++] = *s;
                skip++;
            }                           /* other control codes: dropped */
        }
    }

    if (skipping) {
        skip--;
        return;
    }
    if (n) {
        line[n] = 0;
        th_emit_sjis(c, (u32)line, n);
    }
}

/*
 * The game reserves a fixed 20,992 KB heap and needs nearly all of it, so it
 * only runs with the plugin since nothing of the plugin stays in user
 * memory (it loads into volatile memory when Wi-Fi is switched on).
 */
TH_GAME("Amagami", TH_IDS("ULJS00339"),
    /* lw a2, 0x28(a0): String.length() entry, read from the EBOOT */
    TH_HOOK_EXPECT(0x0886775C, 0x8C860028, on_text));

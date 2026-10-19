/*
 * your diary+ (PSP). Found by static analysis of the decrypted EBOOT
 * (relocatable, code at 0x08804000), cross-checked against LunaHook.
 *
 * LunaHook hooks two spots in mid-function, right after a call to strlen
 * (0x08931084), whose delay slot does `move a1, a0`: there a1 is the string
 * strlen was given. The hooks below are the entries of the same two
 * functions, with the string in the register it arrives in.
 *
 * Text is Shift-JIS with markup: #n (line break), #Color[N], #Scale[x].
 */

#include "texthook.h"

/*
 * Measures the width of a message (strlen x glyph width). Its only caller
 * runs it once when a new message comes up, on the message buffer
 * (0x096B362F), and then moves the message box to its next state.
 */
static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->a1, 0);
}

/*
 * Counts the visible characters of a string, skipping #...] and #n. Its
 * only caller looks per-frame (it steps an animation counter), on a second
 * buffer (0x09905874); repeats are dropped. LunaHook keeps only what comes
 * before "#n「" here, i.e. the speaker's name when the string is
 * "name#n「line」", and discards everything else.
 */
static void on_name(th_ctx *c)
{
    th_emit_sjis(c, c->a0, 0);
}

/* #n first, so that removing #...] can't start at a #n. */
static void strip_markup(char *s)
{
    th_remove(s, "#n");
    th_remove_between(s, "#", "]");
}

static void clean(char *s, int size)
{
    strip_markup(s);
}

static void name_only(char *s, int size)
{
    static const char mark[] = "#n\xE3\x80\x8C";   /* #n「 */
    char *p;

    for (p = s; *p; p++) {
        int i;

        for (i = 0; mark[i] && p[i] == mark[i]; i++)
            ;
        if (!mark[i])
            break;
    }
    if (!*p)
        *s = 0;   /* no name: discard */
    *p = 0;
    strip_markup(s);
}

TH_GAME("your diary+", TH_IDS("NPJH50831"),
    /* addiu sp, sp, -0x10 */
    TH_HOOK_EXPECT(0x0884D684, 0x27BDFFF0, on_text, .filter = clean),
    /* addiu sp, sp, -0x30 */
    TH_HOOK_EXPECT(0x08829278, 0x27BDFFD0, on_name, .filter = name_only));

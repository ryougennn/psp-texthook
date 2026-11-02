/*
 * CROSS†CHANNEL ~To all people~ (PSP). The hook is where the text routine
 * moves its cursor to a new line (s1, Shift-JIS, %N for line breaks).
 */

#include "texthook.h"

static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->s1, 0);
}

static void clean(char *s, int size)
{
    th_remove(s, "%N");
}

TH_GAME("CROSS CHANNEL To all people", TH_IDS("ULJM05623"),
    /* sw s1, 0(s2) */
    TH_HOOK_EXPECT(0x0885F644, 0xAE510000, on_text, .filter = clean));

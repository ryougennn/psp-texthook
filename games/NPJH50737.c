/*
 * 1/2 summer+ (PSP), GSL script engine. Found by static analysis of the
 * decrypted EBOOT (relocatable, code at 0x08804000); LunaHook has the same
 * address and register.
 *
 * The hooked function lays out one message for display: it's the only code
 * that uses the engine's ruby/size/color markup builders
 * ("#Pos[%d,0]#Rubi[%s]#Pos[%d,0]" and friends). a1 is the message as
 * NUL-terminated Shift-JIS, still in script form:
 *   {ruby:BASE&READING}  ruby; only BASE is drawn inline
 *   {size:N} {color:RRGGBB}  style changes, no text
 *   #n  line break
 */

#include "texthook.h"

static void on_text(th_ctx *c)
{
    th_emit_sjis(c, c->a1, 0);
}

/*
 * Keeps the base text of each ruby tag and drops every other {...} tag, the
 * way the game draws them. In place: the output never outgrows the input.
 */
static void strip_tags(char *s)
{
    char *w = s;

    while (*s) {
        char *end;

        if (*s != '{') {
            *w++ = *s++;
            continue;
        }
        for (end = s; *end && *end != '}'; end++)
            ;
        if (!*end) {
            *w++ = *s++;
            continue;
        }
        if (s[1] == 'r' && s[2] == 'u' && s[3] == 'b' && s[4] == 'y' && s[5] == ':') {
            for (s += 6; s < end && *s != '&'; )
                *w++ = *s++;
        }
        s = end + 1;
    }
    *w = 0;
}

static void clean(char *s, int size)
{
    strip_tags(s);
    th_remove(s, "#n");
}

TH_GAME("1/2 summer+", TH_IDS("NPJH50737"),
    /* addiu sp, sp, -0x520 */
    TH_HOOK_EXPECT(0x088225D4, 0x27BDFAE0, on_text, .filter = clean));

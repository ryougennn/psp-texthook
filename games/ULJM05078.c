/*
 * Tokimeki Memorial ~forever with you~ (PSP). Lines are drawn row by row
 * with the player's name filled in, so the rows are collected as drawn and
 * sent once the line or page is complete.
 */

#include "texthook.h"
#include "mini.h"

typedef struct window {
    char text[TH_MAX_TEXT + 1];
    int  len, collecting;
} window;

static window message, event, lone;
static u32    event_last, lone_last;    /* the last line sent of each, hashed */

static void start(window *w)
{
    w->collecting = 1;
    w->len = 0;
}

static void add_row(window *w, u32 p)
{
    if (!w->collecting)
        return;

    for (; w->len < TH_MAX_TEXT; p++) {
        u8 b = th_read8(p);

        if (!b)
            break;
        w->text[w->len++] = b;
    }
}

/* Sends w's text unless it's the same as the last one (*last, a hash). */
static void send_new(th_ctx *c, window *w, u32 *last)
{
    u32 h = 5381;

    w->text[w->len] = 0;
    for (int i = 0; i < w->len; i++)
        h = h * 33 + (u8)w->text[i];
    if (!w->len || h == *last)
        return;
    *last = h;
    th_emit_sjis(c, (u32)w->text, w->len);
}

static void flush_lone(th_ctx *c);

static void on_line(th_ctx *c)
{
    flush_lone(c);
    start(&message);
}

/* Rows drawn outside a message line (phone calls, prompts); a1 is the row. */
static u32 row_index;

static void flush_lone(th_ctx *c)
{
    if (lone.collecting) {
        lone.collecting = 0;
        send_new(c, &lone, &lone_last);
    }
}

static void on_row_start(th_ctx *c)
{
    row_index = c->a1;
}

/* The helper's finished row is at sp+56 as it returns. */
static void on_row(th_ctx *c)
{
    if (message.collecting) {
        add_row(&message, c->sp + 56);
        return;
    }
    if (row_index == 0 || !lone.collecting) {
        flush_lone(c);
        start(&lone);
    }
    add_row(&lone, c->sp + 56);
}

static void on_rows_done(th_ctx *c)
{
    flush_lone(c);
}

static void on_end(th_ctx *c)
{
    if (!message.collecting)
        return;
    message.collecting = 0;
    message.text[message.len] = 0;
    th_emit_sjis(c, (u32)message.text, message.len);
}

static void on_event_line(th_ctx *c)
{
    start(&event);
}

/* A row is at sp+656 after it's drawn; the last one at the end. */
static void on_event_row(th_ctx *c)
{
    add_row(&event, c->sp + 656);
}

/* v0 is 0 when the function left without drawing (no text). */
static void on_event_end(th_ctx *c)
{
    if (!event.collecting)
        return;
    if (c->v0)
        add_row(&event, c->sp + 656);
    event.collecting = 0;
    if (c->v0)
        send_new(c, &event, &event_last);
}

/* Scenes drawn directly (a3), sent at the next frame. */
static const struct { u32 from, to; int choice; } direct_callers[] = {
    { 0x08839900, 0x0883A800, 0 },      /* opening and ending */
    { 0x089A6B48, 0x089A6C24, 0 },      /* festival speech: title */
    { 0x089A70F0, 0x089A70F0, 0 },      /* festival speech */
    { 0x08834688, 0x088346CC, 1 },      /* career choices */
    { 0x0888FF3C, 0x0888FF80, 0 },      /* New Year fortune */
    { 0x088903AC, 0x088903AC, 0 },
    { 0x0889056C, 0x0889056C, 0 },
    { 0x08896F88, 0x08896FDC, 0 },      /* school trip */
};

static window direct;
static u32    direct_last;

static void on_draw(th_ctx *c)
{
    for (unsigned i = 0; i < sizeof(direct_callers) / sizeof(direct_callers[0]); i++) {
        if (c->ra < direct_callers[i].from || c->ra > direct_callers[i].to || !c->a3)
            continue;
        if (!direct.collecting)
            start(&direct);
        else if (direct_callers[i].choice && direct.len < TH_MAX_TEXT - 2) {
            direct.text[direct.len++] = 0x81;       /* U+3000 between choices */
            direct.text[direct.len++] = 0x40;
        }
        add_row(&direct, c->a3);
        return;
    }
}

static void on_flip(th_ctx *c)
{
    if (direct.collecting) {
        direct.collecting = 0;
        send_new(c, &direct, &direct_last);
    }
}

static const char *const brackets[][2] = {
    { "\xE3\x80\x8C", "\xE3\x80\x8D" },     /* 「 」 */
    { "\xE3\x80\x8E", "\xE3\x80\x8F" },     /* 『 』 */
    { "\xEF\xBC\x88", "\xEF\xBC\x89" },     /* （ ） */
};

/* The bracket pair at s (side 0 opening, 1 closing), or -1. */
static int bracket(const char *s, int side)
{
    for (int i = 0; i < 3; i++)
        if (!memcmp(s, brackets[i][side], 3))
            return i;
    return -1;
}

/* Drops ASCII placeholders and unmatched closing brackets. */
static void clean(char *s, int size)
{
    int  open[3] = { 0, 0, 0 }, k;
    char *r = s, *w = s;

    while (*r) {
        if (*r >= 0x20 && *r <= 0x7E) {
            r++;
        } else if ((k = bracket(r, 1)) >= 0 && !open[k]) {
            r += 3;
            if (*r) {
                memcpy(w, "\xE3\x80\x80", 3);
                w += 3;
            }
        } else {
            if ((k = bracket(r, 0)) >= 0)
                open[k]++;
            else if ((k = bracket(r, 1)) >= 0)
                open[k]--;
            *w++ = *r++;
        }
    }
    *w = 0;
}

TH_GAME("Tokimeki Memorial ~forever with you~", TH_IDS("ULJM05078"),
    /* addiu sp, sp, -0x130 */
    TH_HOOK_EXPECT(0x08834784, 0x27BDFED0, on_line, .filter = clean),
    /* addiu sp, sp, -0xF0 */
    TH_HOOK_EXPECT(0x08834910, 0x27BDFF10, on_row_start),
    /* lw s6, 24(sp) */
    TH_HOOK_EXPECT(0x08834CD4, 0x8FB60018, on_row, .filter = clean),
    /* lui v1, 0x8F9 (relocated) */
    TH_HOOK_EXPECT(0x08827000, 0x3C0308F9, on_rows_done, .filter = clean),
    /* li v1, 0x80 */
    TH_HOOK_EXPECT(0x088348E0, 0x24030080, on_end, .filter = clean),
    /* addiu sp, sp, -0x2D0 */
    TH_HOOK_EXPECT(0x08862890, 0x27BDFD30, on_event_line),
    /* addiu v0, v0, 1 */
    TH_HOOK_EXPECT(0x0886365C, 0x24420001, on_event_row),
    /* lw s8, 40(sp) */
    TH_HOOK_EXPECT(0x08864108, 0x8FBE0028, on_event_end, .filter = clean),
    /* addiu sp, sp, -0x10 */
    TH_HOOK_EXPECT(0x08826AE0, 0x27BDFFF0, on_draw),
    /* addiu sp, sp, -0x10 */
    TH_HOOK_EXPECT(0x0880570C, 0x27BDFFF0, on_flip, .filter = clean));

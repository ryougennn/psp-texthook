/*
 * Drawing the dictionary popup into its own RGB565 frame: the game's last
 * frame darkened behind it, the line at the top with the selected word in
 * yellow, a rule, the word's entry (headword, reading, glosses, then its
 * kanji), and the controls at the bottom.
 *
 * Pure drawing, no I/O beyond the font's glyphs, so it can be tested on a
 * PC. Popup thread only.
 */

#include "popup.h"
#include "mini.h"
#include "thk_abi.h"

#define SCREEN_W        480
#define SCREEN_H        272

#define MARGIN_X        10
#define TOP             5
#define SENTENCE_LINES  3
#define RULE_GAP        5
#define HINT_Y          (SCREEN_H - 4 - TH_LINE_H)

#define RGB(r, g, b) (u16)(((r) >> 3) | ((g) >> 2) << 5 | ((b) >> 3) << 11)

/* Colours of the Vita popup this one copies. */
#define C_TEXT      RGB(0xF0, 0xF0, 0xF0)
#define C_PICK      RGB(0xFF, 0xE4, 0x4A)
#define C_GLOSS     RGB(0x9C, 0xCF, 0xFF)
#define C_DIM       RGB(0x6E, 0x9C, 0xD0)
#define C_RULE      RGB(0x3A, 0x6E, 0xB4)

#define HINTS       "◀▶ word · L R entry · ▲▼ scroll · %s close"

static u16 *fb;
static int clip_top, clip_bottom;

/* --- background ---------------------------------------------------------- */

/*
 * Each pixel at about a seventh of its brightness, tinted blue: the game
 * stays recognisable behind the text without competing with it. x * 37 >> 8
 * is x / 7 to within 1, without the division (slow on the PSP: dividing
 * took 57 ms a frame in CLANNAD, 115 ms in Amagami).
 */
static u16 darken(int r, int g, int b)
{
    return RGB((r * 37 >> 8) + 6, (g * 37 >> 8) + 12, (b * 37 >> 8) + 26);
}

void th_draw_background(u16 *bg, const void *frame, int stride, int fmt)
{
    for (int y = 0; y < SCREEN_H; y++) {
        u16 *out = bg + y * THK_POPUP_STRIDE;

        if (!frame) {
            for (int x = 0; x < SCREEN_W; x++)
                out[x] = darken(0, 0, 0);
            continue;
        }
        if (fmt == 3) {         /* 8888 */
            const u32 *in = (const u32 *)frame + y * stride;

            for (int x = 0; x < SCREEN_W; x++)
                out[x] = darken(in[x] & 0xFF, (in[x] >> 8) & 0xFF, (in[x] >> 16) & 0xFF);
            continue;
        }

        const u16 *in = (const u16 *)frame + y * stride;

        for (int x = 0; x < SCREEN_W; x++) {
            u32 c = in[x];
            int r, g, b;

            if (fmt == 0) {             /* 565 */
                r = (c & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = ((c >> 11) & 0x1F) << 3;
            } else if (fmt == 1) {      /* 5551 */
                r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
            } else {                    /* 4444 */
                r = (c & 0xF) << 4, g = ((c >> 4) & 0xF) << 4, b = ((c >> 8) & 0xF) << 4;
            }
            out[x] = darken(r, g, b);
        }
    }
}

/* --- pixels and glyphs --------------------------------------------------- */

static void hline(int x, int y, int w, u16 color)
{
    if (y < clip_top || y >= clip_bottom)
        return;
    for (int i = 0; i < w; i++)
        fb[y * THK_POPUP_STRIDE + x + i] = color;
}

/* Blends one glyph in; `baseline` is in screen pixels. Returns its advance (26.6). */
static int draw_glyph(int pen, int baseline, u32 cp, u16 color)
{
    const th_glyph *g = th_pgf_glyph(cp);

    if (!g)
        g = th_pgf_glyph('?');
    if (!g)
        return 0;

    int cr = color & 0x1F, cg = (color >> 5) & 0x3F, cb = color >> 11;
    int x0 = (pen >> 6) + g->left, y0 = baseline - g->top;

    for (int j = 0; j < g->h; j++) {
        int y = y0 + j;

        if (y < clip_top || y >= clip_bottom)
            continue;
        for (int i = 0; i < g->w; i++) {
            int x = x0 + i, a = g->alpha[j * g->w + i];

            if (!a || x < 0 || x >= SCREEN_W)
                continue;

            u16 *d  = &fb[y * THK_POPUP_STRIDE + x];
            int dr = *d & 0x1F, dg = (*d >> 5) & 0x3F, db = *d >> 11;

            dr += (cr - dr) * a / 15;
            dg += (cg - dg) * a / 15;
            db += (cb - db) * a / 15;
            *d = dr | dg << 5 | db << 11;
        }
    }
    return g->advance;
}

static int advance(u32 cp)
{
    const th_glyph *g = th_pgf_glyph(cp);

    return g ? g->advance : 0;
}

/* --- text ---------------------------------------------------------------- */

/* Kana and kanji can break anywhere; other text only at spaces. */
static int breaks_anywhere(u32 cp)
{
    return cp >= 0x2E80;
}

/*
 * Draws UTF-8 text from (x, y) (the line's top), wrapping at `right`.
 * Wrapped lines start at `indent`. Returns the y below the last line.
 */
static int draw_wrapped(const char *s, int x, int y, int indent, int right, u16 color)
{
    int pen = x << 6, base = th_pgf_ascent();

    while (*s) {
        /* the next unbreakable run: one wide character, or up to a space */
        const char *p = s;
        u32        cp = th_next_utf8(&p);
        int        w  = advance(cp);
        const char *end = p;

        if (!breaks_anywhere(cp) && cp != ' ') {
            const char *q = p;
            u32        c;

            while ((c = th_next_utf8(&q)) && c != ' ' && !breaks_anywhere(c)) {
                w  += advance(c);
                end = q;
            }
        }

        if ((pen + w) >> 6 > right && pen > indent << 6) {
            pen = indent << 6;
            y  += TH_LINE_H;
            if (cp == ' ') {
                s = p;
                continue;
            }
        }

        /* a run longer than a whole line breaks wherever it has to */
        while (s < end) {
            const char *q = s;
            u32        c  = th_next_utf8(&q);

            if ((pen + advance(c)) >> 6 > right && pen > indent << 6) {
                pen = indent << 6;
                y  += TH_LINE_H;
            }
            pen += draw_glyph(pen, y + base, c, color);
            s = q;
        }
    }
    return y + TH_LINE_H;
}

/* --- the line ------------------------------------------------------------ */

static int no_line_start(u32 cp)
{
    static const u16 marks[] = {
        0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x300D, 0x300F, 0x3011, 0xFF09, 0x3009, 0x300B,
        0xFF01, 0xFF1F, 0x30FC, 0x2026, 0x2025, 0x3063, 0x30C3, 0x3041, 0x3043, 0x3045,
        0x3047, 0x3049, 0x3083, 0x3085, 0x3087, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,
        0x30E3, 0x30E5, 0x30E7,
    };     /* 、。，．」』】）〉》！？ー…‥ and small kana */

    for (unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (cp == marks[i])
            return 1;
    return 0;
}

/*
 * The captured line, wrapped by character, at most SENTENCE_LINES lines:
 * a longer one scrolls to keep the selected word in view. Returns the y
 * below it.
 */
static int draw_sentence(th_popup *p)
{
    static u16 line_of[TH_POPUP_TEXT];
    const th_token *t = p->ntok ? &p->tok[p->cur] : NULL;
    int right = SCREEN_W - MARGIN_X, pen = MARGIN_X << 6, line = 0;

    for (int i = 0, start = 0; i < p->n; i++) {
        int w = advance(p->text[i]);

        if ((pen + w) >> 6 > right) {
            pen = MARGIN_X << 6;
            line++;

            /* 」 and 。 can't start a line: take the characters before them along */
            int k = i;
            while (k > start + 1 && no_line_start(p->text[k]))
                k--;
            if (k < i && !no_line_start(p->text[k])) {
                for (int j = k; j < i; j++) {
                    line_of[j] = line;
                    pen += advance(p->text[j]);
                }
            }
            start = k < i ? k : i;
        }
        line_of[i] = line;
        pen += w;
    }

    int nlines = p->n ? line + 1 : 1;
    int first  = 0;

    if (t && nlines > SENTENCE_LINES) {
        first = line_of[t->start + t->len - 1] - (SENTENCE_LINES - 1);
        if (first < 0)
            first = 0;
    }

    int shown = nlines - first < SENTENCE_LINES ? nlines - first : SENTENCE_LINES;
    int base  = th_pgf_ascent();

    clip_top    = TOP;
    clip_bottom = TOP + shown * TH_LINE_H;
    pen         = MARGIN_X << 6;
    line        = 0;
    for (int i = 0; i < p->n; i++) {
        if (line_of[i] != line) {
            pen  = MARGIN_X << 6;
            line = line_of[i];
        }
        if (line < first || line >= first + shown) {
            pen += advance(p->text[i]);
            continue;
        }

        int picked = t && i >= t->start && i < t->start + t->len;
        int y      = TOP + (line - first) * TH_LINE_H + base;

        pen += draw_glyph(pen, y, p->text[i], picked ? C_PICK : C_TEXT);
    }
    return TOP + shown * TH_LINE_H;
}

/* --- the entry ----------------------------------------------------------- */

static int draw_entry(th_popup *p, int y)
{
    char tmp[16];
    int  right = SCREEN_W - MARGIN_X;

    if (p->status)
        return draw_wrapped(p->status, MARGIN_X, y, MARGIN_X, right, C_GLOSS);

    /* 彼女 (かのじょ) 1/3 */
    int base = th_pgf_ascent(), pen = MARGIN_X << 6;
    const char *s;
    u32 c;

    for (s = p->head; (c = th_next_utf8(&s));)
        pen += draw_glyph(pen, y + base, c, C_PICK);
    if (p->reading[0]) {
        pen += draw_glyph(pen, y + base, ' ', C_PICK);
        pen += draw_glyph(pen, y + base, '(', C_PICK);
        for (s = p->reading; (c = th_next_utf8(&s));)
            pen += draw_glyph(pen, y + base, c, C_PICK);
        pen += draw_glyph(pen, y + base, ')', C_PICK);
    }
    if (p->ntok && p->tok[p->cur].nrefs > 1) {
        mini_snprintf(tmp, sizeof(tmp), " %d/%d", p->entry + 1, p->tok[p->cur].nrefs);
        pen += 4 << 6;
        for (s = tmp; (c = th_next_utf8(&s));)
            pen += draw_glyph(pen, y + base, c, C_DIM);
    }
    if (p->source) {
        pen += 8 << 6;
        for (s = p->source; (c = th_next_utf8(&s)) && pen >> 6 < SCREEN_W - MARGIN_X;)
            pen += draw_glyph(pen, y + base, c, C_DIM);
    }
    y += TH_LINE_H;

    y = draw_wrapped(p->gloss, MARGIN_X, y, MARGIN_X, right, C_GLOSS);

    /* 彼=he, that */
    if (p->nkanji)
        y += TH_LINE_H / 2;
    for (int k = 0; k < p->nkanji; k++) {
        pen  = MARGIN_X << 6;
        pen += draw_glyph(pen, y + base, p->kanji[k], C_TEXT);
        pen += draw_glyph(pen, y + base, '=', C_GLOSS);
        y    = draw_wrapped(p->meaning[k], pen >> 6, y, pen >> 6, right, C_GLOSS);
    }
    return y;
}

/* --- the frame ----------------------------------------------------------- */

static int content_top;

void th_draw_popup(u16 *frame, const u16 *bg, th_popup *p)
{
    fb = frame;
    memcpy(fb, bg, THK_POPUP_FB_SIZE);

    int y = draw_sentence(p) + RULE_GAP;

    clip_top    = 0;
    clip_bottom = SCREEN_H;
    hline(MARGIN_X, y, SCREEN_W - 2 * MARGIN_X, C_RULE);
    content_top = y + RULE_GAP + 1;

    /* the entry scrolls between the rule and the controls */
    clip_top    = content_top;
    clip_bottom = HINT_Y - 2;
    p->content_h = draw_entry(p, content_top - p->scroll) + p->scroll - content_top;

    char mark[8], hints[80];

    mark[th_put_utf8(mark, p->close_mark ? p->close_mark : 0x25CB)] = 0;
    mini_snprintf(hints, sizeof(hints), HINTS, mark);
    clip_top    = HINT_Y;
    clip_bottom = SCREEN_H;
    draw_wrapped(hints, MARGIN_X, HINT_Y, MARGIN_X, SCREEN_W, C_GLOSS);
}

int th_draw_content_height(void)
{
    return HINT_Y - 2 - content_top;
}

/*
 * thkpopup.prx, the dictionary popup: its own parts. It shares decoding,
 * filters and logging with texthook.prx (shared/shared.h). Popup thread only.
 */

#ifndef POPUP_H
#define POPUP_H

#include "shared.h"

/* --- what the popup shows ------------------------------------------------ */

#define TH_POPUP_TEXT       160     /* code points of the line */
#define TH_POPUP_TOKENS     96
#define TH_POPUP_REFS       8       /* words per token */
#define TH_POPUP_KANJI      6

#define TH_LINE_H           21      /* pixels per line of text; also the scroll step */

typedef struct th_token {
    u16  start, len;                /* in th_popup.text */
    u8   nrefs;
    u32  refs[TH_POPUP_REFS];
} th_token;

/* popup.c fills it (with dict.c's help), popup_draw.c draws it. */
typedef struct th_popup {
    u32         text[TH_POPUP_TEXT];
    int         n;
    th_token    tok[TH_POPUP_TOKENS];
    int         ntok;
    int         cur, entry, scroll; /* selected token, its word, pixels scrolled */
    int         content_h;          /* set by the drawing: height of the entry */
    const char  *status;            /* shown instead of an entry, if set */
    const char  *source;            /* the entry's dictionary, unless it's the first */
    u32         close_mark;         /* × or ○, for the controls line */
    char        head[96], reading[96], gloss[640];
    int         nkanji;
    u32         kanji[TH_POPUP_KANJI];
    char        meaning[TH_POPUP_KANJI][68];
} th_popup;

/* --- dict.c: the dictionaries -------------------------------------------- */

int  th_dict_open(void);                    /* < 0: no dictionaries (logged) */
void th_dict_close(void);
void th_dict_tokenize(th_popup *p);         /* splits p->text into words */
void th_dict_select(th_popup *p);           /* fills in the selected word's entry */

/* --- pgf.c: the PSP's own font ------------------------------------------- */

#define TH_FONT_PATH    "flash0:/font/jpn0.pgf"

typedef struct th_glyph {
    u32  cp;
    s8   left, top;     /* bitmap position: x from the pen, y up from the baseline */
    u8   w, h;
    s32  advance;       /* 26.6 fixed point */
    u8   *alpha;        /* w * h coverage values, 0-15 */
} th_glyph;

int             th_pgf_open(const char *path);  /* < 0: no font (logged) */
void            th_pgf_close(void);
const th_glyph *th_pgf_glyph(u32 cp);           /* NULL: not in the font */
int             th_pgf_ascent(void);            /* pixels above the baseline */

/* --- popup_draw.c: drawing it -------------------------------------------- */

/* The game's frame, darkened, as the popup's background. */
void th_draw_background(u16 *bg, const void *frame, int stride, int fmt);
void th_draw_popup(u16 *fb, const u16 *bg, th_popup *p);
int  th_draw_content_height(void);  /* visible height of the entry area */

#endif /* POPUP_H */

/*
 * The reading session: the lines the pages counted, their characters and the
 * reading time, kept on the PSP so any device can pick the session up.
 *
 * The pages decide what counts (their replacement rules, the pause, undo)
 * and post each accepted line back; this file only keeps the totals and the
 * timer, and appends every change to ms0:/SEPLUGINS/texthook/session.jsonl,
 * one JSON object per line:
 *
 *   {"op":"line","boot":B,"seq":S,"at":MS,"chars":N,"text":"..."}
 *   {"op":"undo","boot":B,"seq":S}
 *   {"op":"pause","at":MS}
 *   {"op":"resume","at":MS}
 *
 * "at" is the reading time in ms. A reset empties the file. When the module
 * starts, the file is read once to rebuild the totals. Only the worker
 * thread calls this, so there is no locking.
 */

#include <pspkernel.h>

#include "internal.h"
#include "mini.h"

#define SESSION_PATH    "ms0:/SEPLUGINS/texthook/session.jsonl"

/*
 * Like Kizuna: a minute without a new line pauses the timer and takes that
 * idle minute back off.
 */
#define AUTO_PAUSE      (60 * 1000)

/* Newest lines remembered for undo. */
#define UNDO_DEPTH      64

typedef struct {
    u32 boot, seq;
    u32 chars;
} line_key;

static struct {
    u32      boot;          /* this load; seq restarts at 1 with it */
    u32      last_seq;      /* newest line counted in this load */
    u32      lines, chars;
    u32      elapsed;       /* reading time, ms */
    u32      active_at;     /* elapsed at the last line or resume */
    int      running;
    u32      tick, frac;    /* last bank (system time, us), and the us left over */
    line_key recent[UNDO_DEPTH];
    int      top, nrecent;  /* ring: newest at top */
} s;

/* --- helpers ------------------------------------------------------------- */

static void bank(void)
{
    u32 t = sceKernelGetSystemTimeLow();

    if (s.running) {
        s.frac    += t - s.tick;
        s.elapsed += s.frac / 1000;
        s.frac    %= 1000;
    }
    s.tick = t;
}

static void push_recent(u32 boot, u32 seq, u32 chars)
{
    s.top = (s.top + 1) % UNDO_DEPTH;
    s.recent[s.top].boot  = boot;
    s.recent[s.top].seq   = seq;
    s.recent[s.top].chars = chars;
    if (s.nrecent < UNDO_DEPTH)
        s.nrecent++;

    s.lines++;
    s.chars += chars;
}

/* Takes the newest line off if it's boot/seq. Returns 0 if it isn't. */
static int pop_recent(u32 boot, u32 seq)
{
    const line_key *k = &s.recent[s.top];

    if (!s.nrecent || k->boot != boot || k->seq != seq)
        return 0;

    s.lines--;
    s.chars -= k->chars < s.chars ? k->chars : s.chars;
    s.top    = (s.top + UNDO_DEPTH - 1) % UNDO_DEPTH;
    s.nrecent--;
    return 1;
}

static int append(const char *rec, int n)
{
    SceUID fd = sceIoOpen(SESSION_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

    if (fd < 0) {
        th_log("session: can't open %s (%08x)", SESSION_PATH, fd);
        return -1;
    }

    int w = sceIoWrite(fd, rec, n);
    sceIoClose(fd);
    if (w != n) {
        th_log("session: write failed (%08x)", w);
        return -1;
    }

    return 0;
}

static int append_at(const char *op, u32 at)
{
    char rec[64];
    int  n = mini_snprintf(rec, sizeof(rec), "{\"op\":\"%s\",\"at\":%lu}\n", op, at);

    return append(rec, n);
}

/* --- loading ------------------------------------------------------------- */

/* The number after `name` (e.g. "\"at\":") in a record, or 0. */
static u32 field(const char *rec, const char *name)
{
    const char *p = strstr(rec, name);

    return p ? (u32)atoi_simple(p + strlen(name)) : 0;
}

/* One record's start (the text, which comes last, may be cut off). */
static void replay(const char *rec)
{
    if (!strncmp(rec, "{\"op\":\"line\"", 12)) {
        push_recent(field(rec, "\"boot\":"), field(rec, "\"seq\":"), field(rec, "\"chars\":"));
        s.active_at = field(rec, "\"at\":");
    } else if (!strncmp(rec, "{\"op\":\"undo\"", 12)) {
        pop_recent(field(rec, "\"boot\":"), field(rec, "\"seq\":"));
    } else if (!strncmp(rec, "{\"op\":\"pause\"", 13) || !strncmp(rec, "{\"op\":\"resume\"", 14)) {
        s.active_at = field(rec, "\"at\":");
    }
}

void th_session_load(void)
{
    static char chunk[4096];
    char        head[128];
    int         hn = 0, r;

    memset(&s, 0, sizeof(s));

    /*
     * A new number for each load. It only has to differ from the previous
     * one; kept below 2^31 for atoi_simple.
     */
    s.boot = (sceKernelGetSystemTimeLow() & 0x7FFFFFFF) | 1;
    s.tick = sceKernelGetSystemTimeLow();

    SceUID fd = sceIoOpen(SESSION_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        th_log("session: new (no %s yet)", SESSION_PATH);
        return;
    }

    while ((r = sceIoRead(fd, chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < r; i++) {
            if (chunk[i] == '\n') {
                head[hn] = 0;
                replay(head);
                hn = 0;
            } else if (hn < (int)sizeof(head) - 1) {
                head[hn++] = chunk[i];
            }
        }
    }
    sceIoClose(fd);
    if (r < 0)
        th_log("session: read failed (%08x)", r);

    /*
     * Whatever was running when the module went away stopped at its last
     * activity, as auto-pause would have done.
     */
    s.elapsed = s.active_at;
    th_log("session: %lu lines, %lu chars, %lu s", s.lines, s.chars, s.elapsed / 1000);
}

/* --- changes ------------------------------------------------------------- */

int th_session_tick(void)
{
    bank();
    if (!s.running || s.elapsed - s.active_at < AUTO_PAUSE)
        return 0;

    s.elapsed = s.active_at;
    s.running = 0;
    append_at("pause", s.elapsed);  /* failure is logged; the pause stands */
    return 1;
}

int th_session_line(u32 boot, u32 seq, u32 chars, int resume, const char *text)
{
    static char esc[TH_LINE_MAX * 2];
    static char rec[TH_LINE_MAX * 2 + 128];

    /* Another page counted it already, or it's from before a restart. */
    if (boot != s.boot || seq <= s.last_seq || seq > th_lines_latest())
        return TH_SESSION_CONFLICT;

    bank();
    th_json_escape(esc, sizeof(esc), text);
    int n = mini_snprintf(rec, sizeof(rec),
                          "{\"op\":\"line\",\"boot\":%lu,\"seq\":%lu,\"at\":%lu,\"chars\":%lu,"
                          "\"text\":\"%s\"}\n", boot, seq, s.elapsed, chars, esc);
    if (n >= (int)sizeof(rec))
        return TH_SESSION_IO;   /* can't happen: esc is cut to fit */
    if (append(rec, n) < 0)
        return TH_SESSION_IO;

    if (resume && !s.running) {
        s.running = 1;
        s.frac    = 0;
    }
    s.last_seq  = seq;
    s.active_at = s.elapsed;
    push_recent(boot, seq, chars);
    return 0;
}

int th_session_undo(u32 boot, u32 seq)
{
    char rec[96];

    /* Only the newest line, so two pages can't each take one off. */
    const line_key *k = &s.recent[s.top];
    if (!s.nrecent || k->boot != boot || k->seq != seq)
        return TH_SESSION_CONFLICT;

    int n = mini_snprintf(rec, sizeof(rec), "{\"op\":\"undo\",\"boot\":%lu,\"seq\":%lu}\n",
                          boot, seq);
    if (append(rec, n) < 0)
        return TH_SESSION_IO;

    pop_recent(boot, seq);
    return 0;
}

int th_session_run(int on)
{
    bank();
    if (on == s.running)
        return 0;
    if (append_at(on ? "resume" : "pause", s.elapsed) < 0)
        return TH_SESSION_IO;

    s.running = on;
    s.frac    = 0;
    if (on)
        s.active_at = s.elapsed;
    return 0;
}

int th_session_reset(void)
{
    SceUID fd = sceIoOpen(SESSION_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

    if (fd < 0) {
        th_log("session: can't empty %s (%08x)", SESSION_PATH, fd);
        return TH_SESSION_IO;
    }
    sceIoClose(fd);

    /* last_seq stays: lines from before the reset can't come back. */
    s.lines   = s.chars     = 0;
    s.elapsed = s.active_at = 0;
    s.running = s.nrecent   = 0;
    s.frac    = 0;
    th_log("session: reset");
    return 0;
}

/* --- reading ------------------------------------------------------------- */

int th_session_json(char *buf, int size)
{
    bank();
    return mini_snprintf(buf, size,
                         "\"boot\":%lu,\"lines\":%lu,\"chars\":%lu,\"elapsed\":%lu,\"running\":%s",
                         s.boot, s.lines, s.chars, s.elapsed, s.running ? "true" : "false");
}

const char *th_session_path(void)
{
    return SESSION_PATH;
}

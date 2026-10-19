/*
 * Internals of texthook.prx, the network module. What it shares with the
 * popup is in shared/shared.h.
 */

#ifndef INTERNAL_H
#define INTERNAL_H

#include "shared.h"

/* --- types --------------------------------------------------------------- */

/* A processed line, kept for SSE replay. */
typedef struct th_line {
    u32  seq;           /* 1, 2, 3, ...; 0 means empty */
    u8   hook;
    char text[TH_LINE_MAX];
} th_line;

/* Global settings from ms0:/SEPLUGINS/texthook/config.ini. */
typedef struct th_config {
    /*
     * Network profiles to use, in order, as written in config.ini: numbers,
     * names or SSIDs, separated by commas. "0" or empty = automatic.
     */
    char profile[256];
    int  port;
    int  overlay;           /* 0 = never draw on screen */
    int  overlay_corner;    /* THK_CORNER_* */
    char hostname[32];      /* mDNS name, without ".local" */
} th_config;

/* --- main.c: the worker -------------------------------------------------- */

extern char th_title_id[16];

u32 th_events_dropped(void);    /* overwritten in the core before they were read */

/* --- lines.c: line history (worker thread only) -------------------------- */

void           th_lines_add(int hook, const char *text);
const th_line *th_lines_get(u32 seq);   /* NULL if not (or no longer) stored */
u32            th_lines_latest(void);   /* 0 if no lines yet */
u32            th_lines_oldest(void);

/* --- config.c ------------------------------------------------------------ */

extern th_config th_cfg;

void th_config_load(void);

/* --- net.c: the Wi-Fi, driven by the worker thread ----------------------- */

extern char th_ip[16];                      /* "" while not connected */

void th_net_start(void);
void th_net_poll(void);                     /* does all pending work, then waits up to ~20 ms */
void th_net_stop(int unload_modules);       /* closes everything; see net.c */

/* --- http.c: the HTTP server, while the Wi-Fi is up ---------------------- */

void th_http_init(void);
int  th_http_open(void);                    /* listens on th_cfg.port; < 0 on failure */
void th_http_poll(void);                    /* serves; waits up to ~20 ms */
void th_http_close(void);                   /* every connection, and the listener */
void th_http_session_event(const char *op); /* tells every page the session changed */
int  th_json_escape(char *out, int size, const char *in);

/* --- shot.c: screenshots ------------------------------------------------- */

void th_shot_free(void);                    /* the module is stopping */

/* --- session.c: the shared reading session (worker thread only) ---------- */

/* Errors of the th_session_* changes; 0 = done. */
#define TH_SESSION_CONFLICT (-1)        /* already counted, stale, or not the newest line */
#define TH_SESSION_IO       (-2)        /* session.jsonl couldn't be written */

void        th_session_load(void);      /* worker start: totals from session.jsonl */
int         th_session_tick(void);      /* every loop; 1 = the timer just paused itself */
int         th_session_line(u32 boot, u32 seq, u32 chars, int resume, const char *text);
int         th_session_undo(u32 boot, u32 seq);
int         th_session_run(int on);
int         th_session_reset(void);
int         th_session_json(char *buf, int size);   /* the state, as JSON members */
const char *th_session_path(void);

/* --- mdns.c: "<hostname>.local" responder, driven by net.c --------------- */

int  th_mdns_open(const char *ip);      /* after each connect; returns the socket or -1 */
int  th_mdns_fd(void);                  /* -1 if not open */
void th_mdns_read(void);                /* the socket is readable */
void th_mdns_tick(void);                /* every loop: announcements */
void th_mdns_close(void);

#endif /* INTERNAL_H */

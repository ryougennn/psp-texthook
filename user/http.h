/*
 * The HTTP server's internals, shared by http.c (connections, requests,
 * server-sent events) and shot.c (screenshots, sent in the background).
 * net.c brings the Wi-Fi up and down around them (internal.h).
 */

#ifndef HTTP_H
#define HTTP_H

#include "internal.h"

#define MAX_CLIENTS     4
#define REQ_MAX         4096            /* headers plus a posted line */

#define SEC             (1000 * 1000)

typedef struct client {
    int         fd;             /* -1 = free slot */
    int         sse;            /* streaming /events */
    u32         seq;            /* last line sent (SSE) */
    u32         last_tx;        /* time of last send (SSE heartbeat) */

    /* A screenshot in progress (shot.c). */
    int         tx_shot;        /* a screenshot transfer is in progress */
    int         tx_encoding;    /* ... and the JPEG is still being encoded */
    const u8    *tx_data;       /* the response: HTTP header, then the image */
    int         tx_len, tx_off;
    int         tx_total, tx_sent;  /* whole response */
    u32         tx_start, tx_progress;

    int         reqlen;
    char        req[REQ_MAX];
} client;

extern client clients[MAX_CLIENTS];

/* --- http.c -------------------------------------------------------------- */

u32  now(void);
int  would_block(void);         /* the last socket call would have blocked */
void send_error(int fd, const char *status);
void close_client(client *c);

/*
 * Tells every page a screenshot started ({"state":"start"}) or ended
 * ({"state":"done","ok":...}), so they can explain lagging text.
 */
void broadcast_screenshot(const char *json);

/* --- shot.c -------------------------------------------------------------- */

int  shot_busy(void);           /* one at a time */
void shot_start(client *c, const char *query, int bmp);
void shot_poll(void);           /* sends what the sockets take, hands over a finished JPEG */

#endif /* HTTP_H */

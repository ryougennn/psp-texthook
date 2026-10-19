/*
 * The HTTP server: connections, requests, server-sent events and the web
 * page's files. Screenshots are sent from shot.c, in the background.
 *
 * Everything runs on the worker thread, polled by net.c's state machine
 * while the Wi-Fi is up (th_http_poll), so there is no locking against the
 * line history.
 *
 * Endpoints:
 *   GET /events          server-sent events: lines, screenshot start/done,
 *                        keep-alives; resumes from Last-Event-ID or ?since=N
 *   GET /status          JSON: title, IP, hook states, counters
 *   GET /screenshot      the current frame as a JPEG (?q=1..100);
 *                        /screenshot.bmp (or ?format=bmp) for a BMP
 *   GET /session         JSON: the shared reading session's totals and timer
 *   GET /session/log     session.jsonl, or its last ?tail=N bytes
 *   POST /session/line, /undo, /pause, /resume, /reset
 *                        change the session (session.c); every page gets a
 *                        "session" event
 *   GET /<path>          a file from ms0:/SEPLUGINS/texthook/web/ ("/" is
 *                        index.html, /name is name.html)
 * All responses carry Access-Control-Allow-Origin: *.
 */

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "http.h"
#include "mini.h"

#define WEB_ROOT        "ms0:/SEPLUGINS/texthook/web"
#define HEARTBEAT       (5 * SEC)

/*
 * sceNetInit's pool (net.c) holds every socket's queued data, so the send
 * buffer size has to fit it: four clients with full send buffers must not
 * drain it.
 */
#define NET_SNDBUF      (64 * 1024)

client     clients[MAX_CLIENTS];
static int listen_fd = -1;

/* --- socket helpers ------------------------------------------------------ */

u32 now(void)
{
    return sceKernelGetSystemTimeLow();
}

static u16 htons16(u16 v)
{
    return (v >> 8) | (v << 8);
}

/* EAGAIN / EWOULDBLOCK (newlib and BSD numbering). */
int would_block(void)
{
    int e = sceNetInetGetErrno();

    return e == 11 || e == 35;
}

static void set_nonblock(int fd)
{
    int one = 1;

    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
}

/* Waits up to `ms` for the socket to accept more data. */
static void wait_writable(int fd, int ms)
{
    struct SceNetInetPollfd pf = { fd, SCE_NET_INET_POLLOUT, 0 };

    if (sceNetInetPoll(&pf, 1, ms) < 0)
        sceKernelDelayThread(ms * 1000);
}

/* Blocking send for small responses; gives up after 10 s without progress. */
static int send_all(int fd, const void *buf, int len)
{
    const char *p = buf;
    u32 progress = now();

    while (len > 0) {
        int n = (int)sceNetInetSend(fd, p, len, 0);

        if (n > 0) {
            p   += n;
            len -= n;
            progress = now();
        } else if (n < 0 && would_block() && (u32)(now() - progress) < 10 * SEC) {
            wait_writable(fd, 50);
        } else {
            return -1;
        }
    }

    return 0;
}

static int send_str(int fd, const char *s)
{
    return send_all(fd, s, strlen(s));
}

static int send_head(int fd, const char *status, const char *type, int length)
{
    char buf[384];
    int n = mini_snprintf(buf, sizeof(buf),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n",
        status, type);

    if (length >= 0)
        n += mini_snprintf(buf + n, sizeof(buf) - n, "Content-Length: %d\r\n", length);
    n += mini_snprintf(buf + n, sizeof(buf) - n, "\r\n");
    return send_all(fd, buf, n);
}

void send_error(int fd, const char *status)
{
    send_head(fd, status, "text/plain; charset=utf-8", strlen(status) + 1);
    send_str(fd, status);
    send_str(fd, "\n");
}

void close_client(client *c)
{
    int had_tx = c->tx_shot;
    int tx_ok  = had_tx && c->tx_sent >= c->tx_total;

    if (c->fd >= 0)
        sceNetInetClose(c->fd);

    c->fd          = -1;
    c->tx_shot     = 0;
    c->tx_encoding = 0;
    c->tx_data     = NULL;

    /* Every screenshot transfer ends here, however it ended. */
    if (had_tx)
        broadcast_screenshot(tx_ok ? "{\"state\":\"done\",\"ok\":true}"
                                   : "{\"state\":\"done\",\"ok\":false}");
}

/* --- JSON / SSE ---------------------------------------------------------- */

static int send_screenshot_event(client *c, const char *json);

/* Escapes `in` as the body of a JSON string. Never splits an escape. */
int th_json_escape(char *out, int size, const char *in)
{
    int n = 0;

    for (; *in; in++) {
        unsigned char ch = *in;
        char          tmp[8];
        int           k;

        if (ch == '"' || ch == '\\') {
            tmp[0] = '\\', tmp[1] = ch, k = 2;
        } else if (ch == '\n') {
            tmp[0] = '\\', tmp[1] = 'n', k = 2;
        } else if (ch < 0x20) {
            k = mini_snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
        } else {
            tmp[0] = ch, k = 1;
        }

        if (n + k >= size)
            break;
        memcpy(out + n, tmp, k);
        n += k;
    }
    out[n] = 0;

    return n;
}

static const char *hook_name(int hook)
{
    if (th_current_game && hook < th_current_game->nhooks)
        return th_current_game->hooks[hook].name;
    return "?";
}

static int send_event(client *c, const th_line *l)
{
    static char text[TH_LINE_MAX * 2];
    static char buf[TH_LINE_MAX * 2 + 128];

    th_json_escape(text, sizeof(text), l->text);
    int n = mini_snprintf(buf, sizeof(buf),
                          "id: %lu\ndata: {\"seq\":%lu,\"hook\":\"%s\",\"text\":\"%s\"}\n\n",
                          l->seq, l->seq, hook_name(l->hook), text);
    if (n >= (int)sizeof(buf))
        n = sizeof(buf) - 1;

    c->last_tx = now();
    return send_all(c->fd, buf, n);
}

/*
 * Named SSE event (no id, so it never affects Last-Event-ID replay):
 *   event: session
 *   data: {"op":"line","boot":B,"lines":N,"chars":C,"elapsed":MS,"running":true,
 *          "line":{"boot":B,"seq":S,"text":"..."}}
 * The session's state, sent when a stream opens (op "state") and to every
 * page after each change, so they all show the same session. "line" is the
 * line added (with its text) or taken off by undo.
 */
static char session_buf[TH_LINE_MAX * 2 + 256];

static int format_session(const char *op, u32 boot, u32 seq, const char *text)
{
    static char esc[TH_LINE_MAX * 2];
    char        *b   = session_buf;
    int         size = sizeof(session_buf);
    int         n;

    n  = mini_snprintf(b, size, "event: session\ndata: {\"op\":\"%s\",", op);
    n += th_session_json(b + n, size - n);
    if (seq) {
        n += mini_snprintf(b + n, size - n, ",\"line\":{\"boot\":%lu,\"seq\":%lu", boot, seq);
        if (text) {
            th_json_escape(esc, sizeof(esc), text);
            n += mini_snprintf(b + n, size - n, ",\"text\":\"%s\"", esc);
        }
        n += mini_snprintf(b + n, size - n, "}");
    }
    n += mini_snprintf(b + n, size - n, "}\n\n");

    return n;
}

static void broadcast_session(const char *op, u32 boot, u32 seq, const char *text)
{
    int n = format_session(op, boot, seq, text);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &clients[i];

        if (c->fd >= 0 && c->sse) {
            c->last_tx = now();
            if (send_all(c->fd, session_buf, n) < 0)
                close_client(c);
        }
    }
}

void th_http_session_event(const char *op)
{
    broadcast_session(op, 0, 0, NULL);
}

/* Value of header `name` (e.g. "last-event-id:") in the request, or NULL. */
static const char *header(const char *req, const char *name)
{
    int n = strlen(name);

    for (const char *p = strstr(req, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        if (!strncasecmp(p + 2, name, n)) {
            const char *v = p + 2 + n;

            while (*v == ' ')
                v++;
            return v;
        }
    }

    return NULL;
}

static void start_events(client *c, const char *query)
{
    const char *since = header(c->req, "last-event-id:");

    if (!since && query && !strncmp(query, "since=", 6))
        since = query + 6;

    /* Where to start: after the client's last line, or with the newest one. */
    u32 latest = th_lines_latest();
    u32 after  = since ? (u32)atoi_simple(since) : (latest ? latest - 1 : 0);
    u32 oldest = th_lines_oldest();

    if (after > latest)
        after = latest;     /* PSP restarted since the client last saw us */
    if (oldest && after < oldest - 1)
        after = oldest - 1;

    /*
     * Events are tiny and must go out at once. With Nagle on, one unacked
     * segment (a lost frame, WLAN power save) holds back every later line,
     * which then arrive together.
     */
    int one = 1;
    if (sceNetInetSetsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
        th_log("events: TCP_NODELAY refused (errno %d)", sceNetInetGetErrno());

    if (send_str(c->fd, "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream; charset=utf-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n"
                        "retry: 2000\n\n") < 0) {
        close_client(c);
        return;
    }

    c->sse     = 1;
    c->seq     = after;
    c->last_tx = now();

    /* The session first: its boot number goes with the lines that follow. */
    int n = format_session("state", 0, 0, NULL);
    if (send_all(c->fd, session_buf, n) < 0) {
        close_client(c);
        return;
    }

    if (shot_busy() && send_screenshot_event(c, "{\"state\":\"start\"}") < 0)
        close_client(c);
}

/*
 * Named SSE event (no id, so it never affects Last-Event-ID replay):
 *   event: screenshot
 *   data: {"state":"start"} | {"state":"done","ok":true|false}
 * Lets pages explain why text may lag while a screenshot hogs the Wi-Fi.
 */
static int send_screenshot_event(client *c, const char *json)
{
    char buf[96];
    int n = mini_snprintf(buf, sizeof(buf), "event: screenshot\ndata: %s\n\n", json);

    c->last_tx = now();
    return send_all(c->fd, buf, n);
}

void broadcast_screenshot(const char *json)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].sse && send_screenshot_event(&clients[i], json) < 0)
            close_client(&clients[i]);
}

static void pump_events(client *c)
{
    u32 latest = th_lines_latest();

    while (c->seq < latest) {
        const th_line *l = th_lines_get(c->seq + 1);

        c->seq++;
        if (l && send_event(c, l) < 0) {
            close_client(c);
            return;
        }
    }

    if ((u32)(now() - c->last_tx) > HEARTBEAT) {
        c->last_tx = now();
        /*
         * A named event rather than an SSE comment: pages can't see comments,
         * and they use this to tell a live stream from a silent dead one.
         */
        if (send_str(c->fd, "event: alive\ndata: {}\n\n") < 0)
            close_client(c);
    }
}

/* --- other endpoints ----------------------------------------------------- */

static void send_status(int fd)
{
    static char buf[1024];

    int n = mini_snprintf(buf, sizeof(buf),
                          "{\"title\":\"%s\",\"game\":\"%s\",\"ip\":\"%s\",\"port\":%d,"
                          "\"lines\":%lu,\"dropped\":%lu,\"hooks\":[",
                          th_title_id, th_current_game ? th_current_game->title : "",
                          th_ip, th_cfg.port, th_lines_latest(), th_events_dropped());

    for (int i = 0; th_current_game && i < th_current_game->nhooks; i++) {
        const th_hook *h = &th_current_game->hooks[i];

        n += mini_snprintf(buf + n, sizeof(buf) - n,
                           "%s{\"name\":\"%s\",\"addr\":\"%08lx\",\"state\":%d}",
                           i ? "," : "", h->name, h->addr, thkGetHookState(h->addr));
    }
    n += mini_snprintf(buf + n, sizeof(buf) - n, "]}\n");
    if (n >= (int)sizeof(buf))
        n = sizeof(buf) - 1;

    send_head(fd, "200 OK", "application/json", n);
    send_all(fd, buf, n);
}

/* Value of `name` in a query string ("a=1&b=2"), or NULL. */
static const char *param(const char *query, const char *name)
{
    int n = strlen(name);

    for (const char *p = query; p; p = strstr(p, "&")) {
        if (*p == '&')
            p++;
        if (!strncmp(p, name, n) && p[n] == '=')
            return p + n + 1;
    }

    return NULL;
}

static u32 param_u32(const char *query, const char *name)
{
    const char *v = param(query, name);

    return v ? (u32)atoi_simple(v) : 0;
}

static void send_session(int fd)
{
    char buf[160];
    int  n = 0;

    n += mini_snprintf(buf + n, sizeof(buf) - n, "{");
    n += th_session_json(buf + n, sizeof(buf) - n);
    n += mini_snprintf(buf + n, sizeof(buf) - n, "}\n");

    send_head(fd, "200 OK", "application/json", n);
    send_all(fd, buf, n);
}

/*
 * session.jsonl, or (?tail=N) about its last N bytes, starting at the first
 * whole record in them.
 */
static void send_session_log(int fd, const char *query)
{
    static char chunk[4096];
    const char  *type = "application/x-ndjson; charset=utf-8";

    SceUID f = sceIoOpen(th_session_path(), PSP_O_RDONLY, 0);
    if (f < 0) {
        send_head(fd, "200 OK", type, 0);   /* no session yet */
        return;
    }

    int size = (int)sceIoLseek(f, 0, PSP_SEEK_END);
    int tail = (int)param_u32(query, "tail");
    int from = tail > 0 && tail < size ? size - tail : 0;
    int r = 0, skip = 0;

    /* From the byte before `from`: skip up to and including its record's '\n'. */
    if (from > 0) {
        sceIoLseek(f, from - 1, PSP_SEEK_SET);
        r = sceIoRead(f, chunk, sizeof(chunk));
        if (r < 0)
            r = 0;
        while (skip < r && chunk[skip] != '\n')
            skip++;
        if (skip < r)
            skip++;
        from = from - 1 + skip;
    } else {
        sceIoLseek(f, 0, PSP_SEEK_SET);
    }

    if (send_head(fd, "200 OK", type, size - from) >= 0 &&
        (r <= skip || send_all(fd, chunk + skip, r - skip) >= 0)) {
        while ((r = sceIoRead(f, chunk, sizeof(chunk))) > 0)
            if (send_all(fd, chunk, r) < 0)
                break;
    }

    sceIoClose(f);
}

/* POST /session/<op>; the body is the line's text. */
static void post_session(int fd, const char *op, const char *query, const char *body)
{
    u32 boot = param_u32(query, "boot");
    u32 seq  = param_u32(query, "seq");
    int r;

    if (!strcmp(op, "line")) {
        if (!*body || strlen(body) >= TH_LINE_MAX) {
            send_error(fd, "400 Bad Request");
            return;
        }
        r = th_session_line(boot, seq, param_u32(query, "chars"), param_u32(query, "resume"),
                            body);
        if (!r)
            broadcast_session(op, boot, seq, body);
    } else if (!strcmp(op, "undo")) {
        r = th_session_undo(boot, seq);
        if (!r)
            broadcast_session(op, boot, seq, NULL);
    } else if (!strcmp(op, "pause") || !strcmp(op, "resume")) {
        r = th_session_run(!strcmp(op, "resume"));
        if (!r)
            broadcast_session(op, 0, 0, NULL);
    } else if (!strcmp(op, "reset")) {
        r = th_session_reset();
        if (!r)
            broadcast_session(op, 0, 0, NULL);
    } else {
        send_error(fd, "404 Not Found");
        return;
    }

    if (r == TH_SESSION_CONFLICT)
        send_error(fd, "409 Conflict");
    else if (r < 0)
        send_error(fd, "503 Service Unavailable");
    else
        send_head(fd, "204 No Content", "text/plain", 0);
}

/* --- files --------------------------------------------------------------- */

static const char *content_type(const char *path)
{
    static const char *const types[][2] = {
        { ".html",  "text/html; charset=utf-8"       },
        { ".htm",   "text/html; charset=utf-8"       },
        { ".js",    "text/javascript; charset=utf-8" },
        { ".mjs",   "text/javascript; charset=utf-8" },
        { ".css",   "text/css; charset=utf-8"        },
        { ".json",  "application/json"               },
        { ".txt",   "text/plain; charset=utf-8"      },
        { ".svg",   "image/svg+xml"                  },
        { ".png",   "image/png"                      },
        { ".jpg",   "image/jpeg"                     },
        { ".jpeg",  "image/jpeg"                     },
        { ".gif",   "image/gif"                      },
        { ".woff",  "font/woff"                      },
        { ".woff2", "font/woff2"                     },
        { ".ttf",   "font/ttf"                       },
        { ".wasm",  "application/wasm"               },
    };
    int len = strlen(path);

    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        int n = strlen(types[i][0]);

        if (len >= n && !strncasecmp(path + len - n, types[i][0], n))
            return types[i][1];
    }

    return "application/octet-stream";
}

static void send_file(int fd, const char *path)
{
    static char full[256];
    static char chunk[4096];

    if (strstr(path, "..") || strstr(path, "\\") || strstr(path, ":")) {
        send_error(fd, "403 Forbidden");
        return;
    }

    int n = mini_snprintf(full, sizeof(full), "%s%s", WEB_ROOT, path);
    if (n >= (int)sizeof(full) - 16) {
        send_error(fd, "414 URI Too Long");
        return;
    }
    if (full[n - 1] == '/')
        strncpy0(full + n, "index.html", sizeof(full) - n);

    SceUID f = sceIoOpen(full, PSP_O_RDONLY, 0);
    if (f < 0) {
        /*
         * Extensionless page names: /name serves name.html. The 414
         * check above leaves room for the suffix.
         */
        int i = n;

        while (i > 0 && full[i - 1] != '/' && full[i - 1] != '.')
            i--;
        if (full[n - 1] != '/' && full[i - 1] == '/') {
            strncpy0(full + n, ".html", sizeof(full) - n);
            f = sceIoOpen(full, PSP_O_RDONLY, 0);
        }
    }
    if (f < 0) {
        send_error(fd, "404 Not Found");
        return;
    }

    int size = (int)sceIoLseek(f, 0, PSP_SEEK_END);
    sceIoLseek(f, 0, PSP_SEEK_SET);

    if (send_head(fd, "200 OK", content_type(full), size) >= 0) {
        int r;

        while ((r = sceIoRead(f, chunk, sizeof(chunk))) > 0)
            if (send_all(fd, chunk, r) < 0)
                break;
    }

    sceIoClose(f);
}

/* --- request handling ---------------------------------------------------- */

static void handle_request(client *c)
{
    char *body = strstr(c->req, "\r\n\r\n") + 4;   /* read_client found it */
    char *sp1  = strstr(c->req, " ");
    char *sp2 = sp1 ? strstr(sp1 + 1, " ") : NULL;

    if (!sp1 || !sp2) {
        send_error(c->fd, "400 Bad Request");
        close_client(c);
        return;
    }

    /* "GET /path?query HTTP/1.1": split in place. */
    *sp1 = 0;
    *sp2 = 0;
    char *path  = sp1 + 1;
    char *query = strstr(path, "?");
    if (query)
        *query++ = 0;

    if (!strcmp(c->req, "POST")) {
        if (!strncmp(path, "/session/", 9))
            post_session(c->fd, path + 9, query, body);
        else
            send_error(c->fd, "405 Method Not Allowed");
    } else if (strcmp(c->req, "GET") && strcmp(c->req, "HEAD")) {
        send_error(c->fd, "405 Method Not Allowed");
    } else if (!strcmp(path, "/events")) {
        *sp1 = ' ';     /* header() needs the request lines intact */
        *sp2 = ' ';
        if (query)
            query[-1] = '?';
        start_events(c, query);
        return;         /* stays open */
    } else if (!strcmp(path, "/status")) {
        send_status(c->fd);
    } else if (!strcmp(path, "/session")) {
        send_session(c->fd);
    } else if (!strcmp(path, "/session/log")) {
        send_session_log(c->fd, query);
    } else if (!strcmp(path, "/screenshot") || !strcmp(path, "/screenshot.jpg") ||
               !strcmp(path, "/screenshot.bmp")) {
        shot_start(c, query, !strcmp(path, "/screenshot.bmp"));
        return;         /* sent in the background (or already closed) */
    } else if (path[0] == '/') {
        send_file(c->fd, path);
    } else {
        send_error(c->fd, "400 Bad Request");
    }

    close_client(c);
}

static void accept_clients(void)
{
    for (;;) {
        int fd = sceNetInetAccept(listen_fd, NULL, NULL);
        if (fd < 0)
            return;

        /* a free slot, or turn it away */
        client *c = NULL;
        for (int i = 0; i < MAX_CLIENTS && !c; i++)
            if (clients[i].fd < 0)
                c = &clients[i];
        if (!c) {
            send_error(fd, "503 Service Unavailable");
            sceNetInetClose(fd);
            continue;
        }

        set_nonblock(fd);
        int sndbuf = NET_SNDBUF;
        sceNetInetSetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        memset(c, 0, sizeof(*c) - sizeof(c->req));
        c->fd     = fd;
        c->req[0] = 0;
    }
}

/* Content-Length of a request whose headers end at `end`; 0 if none. */
static int content_length(char *req, char *end)
{
    char save = end[2];

    end[2] = 0;     /* header() stops at the end of the headers */
    const char *v = header(req, "content-length:");
    end[2] = save;

    int n = v ? atoi_simple(v) : 0;
    return n > 0 ? n : 0;
}

static void read_client(client *c)
{
    char discard[256];

    if (c->sse) {
        /* Nothing is expected; recv only to notice the client leaving. */
        int n = (int)sceNetInetRecv(c->fd, discard, sizeof(discard), 0);

        if (n == 0 || (n < 0 && !would_block()))
            close_client(c);
        return;
    }

    int n = (int)sceNetInetRecv(c->fd, c->req + c->reqlen, REQ_MAX - 1 - c->reqlen, 0);
    if (n == 0 || (n < 0 && !would_block())) {
        close_client(c);
        return;
    }
    if (n < 0)
        return;

    c->reqlen += n;
    c->req[c->reqlen] = 0;

    char *end = strstr(c->req, "\r\n\r\n");
    if (!end) {
        if (c->reqlen >= REQ_MAX - 1) {
            send_error(c->fd, "431 Request Header Fields Too Large");
            close_client(c);
        }
        return;
    }

    /* A POST's body follows the headers: wait for all of it. */
    int need = end + 4 - c->req + content_length(c->req, end);
    if (need > REQ_MAX - 1) {
        send_error(c->fd, "413 Content Too Large");
        close_client(c);
    } else if (c->reqlen >= need) {
        c->req[need] = 0;
        handle_request(c);
    }
}

void th_http_poll(void)
{
    struct SceNetInetPollfd pf[2 + MAX_CLIENTS];
    client                  *owner[2 + MAX_CLIENTS];
    int                     n = 0;

    /* What to wait for: the listener, each client, mDNS. */
    pf[n].fd = listen_fd, pf[n].events = SCE_NET_INET_POLLIN, pf[n].revents = 0;
    owner[n++] = NULL;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0 || clients[i].tx_encoding)
            continue;
        pf[n].fd     = clients[i].fd, pf[n].revents = 0;
        pf[n].events = clients[i].tx_shot ? SCE_NET_INET_POLLOUT : SCE_NET_INET_POLLIN;
        owner[n++]   = &clients[i];
    }

    int mdns = th_mdns_fd(), mdns_slot = -1;
    if (mdns >= 0) {
        pf[n].fd  = mdns, pf[n].events = SCE_NET_INET_POLLIN, pf[n].revents = 0;
        owner[n]  = NULL;
        mdns_slot = n++;
    }

    if (sceNetInetPoll(pf, n, 20) < 0)
        sceKernelDelayThread(20 * 1000);

    /* Input. */
    if (pf[0].revents)
        accept_clients();
    for (int i = 1; i < n; i++)
        if (i != mdns_slot && pf[i].revents && owner[i]->fd >= 0 && !owner[i]->tx_shot)
            read_client(owner[i]);
    if (mdns_slot >= 0 && pf[mdns_slot].revents)
        th_mdns_read();
    th_mdns_tick();

    /* Output: screenshots, then lines. */
    shot_poll();
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].sse)
            pump_events(&clients[i]);
}

/* --- the listener -------------------------------------------------------- */

void th_http_init(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
}

int th_http_open(void)
{
    int fd = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        th_log("net: socket failed (%08x, errno %d)", fd, sceNetInetGetErrno());
        return -1;
    }

    int one = 1;
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_len         = sizeof(a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons16(th_cfg.port);
    a.sin_addr.s_addr = 0;      /* INADDR_ANY */

    if (sceNetInetBind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        sceNetInetListen(fd, MAX_CLIENTS) < 0) {
        th_log("net: bind/listen on port %d failed (errno %d)", th_cfg.port, sceNetInetGetErrno());
        sceNetInetClose(fd);
        return -1;
    }

    set_nonblock(fd);
    listen_fd = fd;
    return 0;
}

void th_http_close(void)
{
    th_mdns_close();

    for (int i = 0; i < MAX_CLIENTS; i++)
        close_client(&clients[i]);

    if (listen_fd >= 0)
        sceNetInetClose(listen_fd);
    listen_fd = -1;
}

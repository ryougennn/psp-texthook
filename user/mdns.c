/*
 * mDNS responder: answers "<hostname>.local" (config.ini `hostname`,
 * default "psp") with the PSP's IP, so the page is at http://psp.local/.
 *
 * Multicast DNS (RFC 6762) in its smallest useful form: one UDP socket on
 * port 5353 in the group 224.0.0.251. A queries (and ANY) for our name get
 * one A record back, multicast; queries from a port other than 5353
 * ("legacy unicast", e.g. a plain resolver) get a normal DNS reply to the
 * sender. After each connect the name is announced twice, a second apart,
 * so devices that cached an older address pick up the new one. No IPv6, no
 * conflict probing (the name is the user's choice). Worker thread only.
 */

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <netinet/in.h>

#include "internal.h"
#include "mini.h"

#define MDNS_PORT       5353
#define TTL             120             /* seconds, for multicast answers */
#define LEGACY_TTL      10              /* RFC 6762 10.2: for legacy unicast replies */
#define ANNOUNCE_GAP    (1000 * 1000)
#define MAX_LOGGED      5               /* answers logged per connection */

static int fd = -1;
static u8  my_ip[4];
static int announces_left;
static u32 next_announce;
static int answers_logged;

/* --- building messages --------------------------------------------------- */

static int parse_ip(const char *s, u8 out[4])
{
    for (int i = 0; i < 4; i++) {
        int v = 0, digits = 0;

        while (*s >= '0' && *s <= '9' && digits < 3)
            v = v * 10 + (*s++ - '0'), digits++;
        if (!digits || v > 255 || (i < 3 && *s++ != '.'))
            return 0;
        out[i] = v;
    }
    return 1;
}

/* Appends "<hostname>.local" as DNS labels. Returns the new end. */
static u8 *put_name(u8 *p)
{
    int n = strlen(th_cfg.hostname);

    *p++ = n;
    memcpy(p, th_cfg.hostname, n);
    p += n;
    memcpy(p, "\x05local", 7);  /* including the root label */
    return p + 7;
}

static u8 *put16(u8 *p, u32 v)
{
    p[0] = v >> 8, p[1] = v;
    return p + 2;
}

static u8 *put_answer(u8 *p, int cls, u32 ttl)
{
    p = put_name(p);
    p = put16(p, 1);            /* type A */
    p = put16(p, cls);          /* class */
    p = put16(p, ttl >> 16);    /* TTL */
    p = put16(p, ttl & 0xFFFF);
    p = put16(p, 4);            /* data length */
    memcpy(p, my_ip, 4);
    return p + 4;
}

static void send_to(const u8 *buf, int len, u32 addr, u16 port)
{
    struct sockaddr_in to;

    memset(&to, 0, sizeof(to));
    to.sin_len         = sizeof(to);
    to.sin_family      = AF_INET;
    to.sin_port        = (port >> 8) | ((port & 0xFF) << 8);
    to.sin_addr.s_addr = addr;

    if ((int)sceNetInetSendto(fd, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
        th_log("mdns: send failed (errno %d)", sceNetInetGetErrno());
}

/*
 * Multicast announcement / answer: id 0, response + authoritative, one A
 * record with the cache-flush bit (we're the only owner of the name).
 */
static void send_multicast_answer(void)
{
    u8 buf[128], *p = buf;

    p = put16(p, 0);                    /* id */
    p = put16(p, 0x8400);               /* flags */
    p = put16(p, 0), p = put16(p, 1), p = put16(p, 0), p = put16(p, 0);
    p = put_answer(p, 0x8001, TTL);

    u32 group;
    memcpy(&group, "\xE0\x00\x00\xFB", 4);  /* 224.0.0.251, network order */
    send_to(buf, p - buf, group, MDNS_PORT);
}

/* --- reading queries ----------------------------------------------------- */

/*
 * Reads a (possibly compressed) name at `off` and compares it with ours,
 * ignoring ASCII case. Sets *end to just past the name in place. Returns 1
 * on a match, 0 otherwise, -1 if malformed.
 */
static int match_name(const u8 *msg, int len, int off, int *end)
{
    char want[64];

    mini_snprintf(want, sizeof(want), "%s.local", th_cfg.hostname);

    char got[256];
    int  g = 0, jumps = 0, pos = off;

    *end = -1;
    while (pos < len) {
        int l = msg[pos];

        /* compression pointer */
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++jumps > 8)
                return -1;
            if (*end < 0)
                *end = pos + 2;
            pos = ((l & 0x3F) << 8) | msg[pos + 1];
            continue;
        }
        if (l & 0xC0)
            return -1;

        /* the root label: compare */
        if (l == 0) {
            if (*end < 0)
                *end = pos + 1;
            got[g] = 0;
            for (int i = 0; got[i] || want[i]; i++) {
                char a = got[i], b = want[i];

                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b)
                    return 0;
            }
            return 1;
        }

        /* an ordinary label */
        if (pos + 1 + l > len || g + l + 1 >= (int)sizeof(got))
            return -1;
        if (g)
            got[g++] = '.';
        memcpy(got + g, msg + pos + 1, l);
        g   += l;
        pos += 1 + l;
    }

    return -1;
}

static void handle_query(const u8 *msg, int len, const struct sockaddr_in *from)
{
    if (len < 12 || (msg[2] & 0x80))    /* too short, or a response */
        return;

    int qd  = (msg[4] << 8) | msg[5];
    int off = 12;

    for (int q = 0; q < qd; q++) {
        int end;
        int m = match_name(msg, len, off, &end);

        if (m < 0 || end + 4 > len)
            return;

        int type         = (msg[end] << 8) | msg[end + 1];
        int question_end = end + 4;

        if (m && (type == 1 || type == 255)) {
            u16      port = (from->sin_port >> 8) | ((from->sin_port & 0xFF) << 8);
            const u8 *ip  = (const u8 *)&from->sin_addr.s_addr;

            if (answers_logged < MAX_LOGGED) {
                th_log("mdns: answering %s.local for %d.%d.%d.%d%s", th_cfg.hostname,
                       ip[0], ip[1], ip[2], ip[3], port == MDNS_PORT ? "" : " (unicast)");
                if (++answers_logged == MAX_LOGGED)
                    th_log("mdns: (further answers not logged)");
            }

            if (port == MDNS_PORT) {
                send_multicast_answer();
            } else {
                /*
                 * Legacy unicast: a normal DNS reply with the question
                 * echoed, to the sender's port.
                 */
                u8 buf[160], *p = buf;

                memcpy(p, msg, 2), p += 2;      /* id */
                p = put16(p, 0x8400);           /* flags */
                p = put16(p, 1), p = put16(p, 1), p = put16(p, 0), p = put16(p, 0);
                p = put_name(p);                /* the question */
                p = put16(p, type);
                p = put16(p, 1);
                p = put_answer(p, 1, LEGACY_TTL);
                send_to(buf, p - buf, from->sin_addr.s_addr, port);
            }
            return;
        }
        off = question_end;
    }
}

/* --- socket -------------------------------------------------------------- */

int th_mdns_open(const char *ip)
{
    th_mdns_close();

    if (!parse_ip(ip, my_ip))
        return -1;

    fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        th_log("mdns: socket failed (%08x, errno %d)", fd, sceNetInetGetErrno());
        return fd = -1;
    }

    int one = 1;
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_len    = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port   = (MDNS_PORT >> 8) | ((MDNS_PORT & 0xFF) << 8);

    if (sceNetInetBind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        th_log("mdns: bind to port %d failed (errno %d)", MDNS_PORT, sceNetInetGetErrno());
        th_mdns_close();
        return -1;
    }

    struct ip_mreq mreq;

    memset(&mreq, 0, sizeof(mreq));
    memcpy(&mreq.imr_multiaddr.s_addr, "\xE0\x00\x00\xFB", 4);   /* 224.0.0.251 */
    memcpy(&mreq.imr_interface.s_addr, my_ip, 4);

    if (sceNetInetSetsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        th_log("mdns: joining 224.0.0.251 failed (errno %d); psp.local won't work",
               sceNetInetGetErrno());
        th_mdns_close();
        return -1;
    }

    u8 ttl = 255;   /* RFC 6762 11: receivers may check for 255 */
    sceNetInetSetsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    int nb = 1;
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    answers_logged = 0;
    announces_left = 2;
    next_announce  = sceKernelGetSystemTimeLow();

    th_log("mdns: http://%s.local/ -> %s", th_cfg.hostname, ip);
    return fd;
}

int th_mdns_fd(void)
{
    return fd;
}

/* Socket readable: answer every query waiting. */
void th_mdns_read(void)
{
    u8 msg[512];

    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = (int)sceNetInetRecvfrom(fd, msg, sizeof(msg), 0,
                                        (struct sockaddr *)&from, &fromlen);

        if (n <= 0)
            return;
        handle_query(msg, n, &from);
    }
}

/* Every loop: the announcements after a connect. */
void th_mdns_tick(void)
{
    if (fd < 0 || !announces_left)
        return;

    u32 now = sceKernelGetSystemTimeLow();
    if ((int)(now - next_announce) < 0)
        return;

    send_multicast_answer();
    announces_left--;
    next_announce = now + ANNOUNCE_GAP;
}

void th_mdns_close(void)
{
    if (fd >= 0)
        sceNetInetClose(fd);

    fd             = -1;
    announces_left = 0;
}

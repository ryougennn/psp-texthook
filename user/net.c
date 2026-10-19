/*
 * The Wi-Fi: picking a network profile, connecting, and keeping the
 * connection; the HTTP server (http.c) runs while it's up.
 *
 * Everything runs on the worker thread as a polled state machine
 * (th_net_poll). Losing the connection (hotspot gone, out of range) is
 * detected by polling the access point state; all sockets are dropped and
 * the connection retried. The Wi-Fi switch, sleep and quitting are handled
 * by the core, which stops this module (th_net_stop) instead of leaving it
 * to notice.
 */

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility_netparam.h>
#include <pspreg.h>
#include <pspwlan.h>

#include "internal.h"
#include "mini.h"

#define SEC             (1000 * 1000)
#define CONNECT_TIMEOUT (30 * SEC)
#define RETRY_DELAY     (5 * SEC)

/*
 * sceNetInit's pool holds every socket's queued data (http.c sizes its
 * send buffers to fit). It lives in the volatile memory, with the network
 * modules and the apctl thread.
 */
#define NET_POOL        (128 * 1024)

/*
 * Access point events, recorded by the apctl handler (on the network
 * library's thread) and logged from the worker.
 */
#define AP_EVENTS       16

/*
 * WPA handshakes that keep failing show up as established/reconnect
 * loops; give up after this many instead of waiting for the timeout.
 */
#define MAX_RECONNECTS  3

#define APCTL_ERROR_WLAN_SWITCH_OFF ((int)0x80410A06)

enum net_state { NET_OFF, NET_CONNECT, NET_WAIT_IP, NET_UP, NET_RETRY, NET_FAILED };

char th_ip[16];                         /* "" while not connected */

static enum net_state state = NET_OFF;
static u32            state_since, last_ap_check;
static int            profiles[10], nprofiles, profile_idx;

static struct { int from, to, event, error; } ap_events[AP_EVENTS];
static volatile u32   ap_head, ap_tail;
static volatile int   ap_error;         /* set on PSP_NET_APCTL_EVENT_ERROR */
static volatile int   ap_reconnects;    /* during this attempt */

/* --- helpers ------------------------------------------------------------- */

static u32 now(void)
{
    return sceKernelGetSystemTimeLow();
}

static void set_state(enum net_state s)
{
    state       = s;
    state_since = now();
}

/* --- access point and profiles ------------------------------------------- */

static int registry_latest_profile(void)
{
    struct RegParam reg;
    REGHANDLE       h, hd;
    unsigned int    value = 0;
    int             ret   = -1;

    memset(&reg, 0, sizeof(reg));
    reg.regtype = 1;
    reg.namelen = strlen("/system");
    reg.unk2    = 1;
    reg.unk3    = 1;
    strncpy0(reg.name, "/system", sizeof(reg.name));

    int r = sceRegOpenRegistry(&reg, 1, &h);
    if (r < 0) {
        th_log("net: registry open failed (%08x)", r);
        return -1;
    }

    r = sceRegOpenCategory(h, "/CONFIG/NETWORK/INFRASTRUCTURE", 1, &hd);
    if (r >= 0) {
        r = sceRegGetKeyValueByName(hd, "latest_id", &value, sizeof(value));
        if (r >= 0)
            ret = value;
        sceRegCloseCategory(hd);
    }
    if (r < 0)
        th_log("net: registry latest_id lookup failed (%08x)", r);

    sceRegCloseRegistry(h);
    return ret;
}

static void ap_handler(int old_state, int new_state, int event, int error, void *arg)
{
    if (ap_head - ap_tail < AP_EVENTS) {
        int i = ap_head % AP_EVENTS;

        ap_events[i].from  = old_state;
        ap_events[i].to    = new_state;
        ap_events[i].event = event;
        ap_events[i].error = error;
        ap_head++;
    }

    if (event == PSP_NET_APCTL_EVENT_ERROR)
        ap_error = error ? error : -1;
    if (event == PSP_NET_APCTL_EVENT_RECONNECT)
        ap_reconnects++;
}

/* sceNetApctl error codes (0x80410A01..0B). */
static const char *ap_error_name(int error)
{
    static const char *const names[] = {
        "already initialized",  "invalid code",     "invalid IP",   "not disconnected",
        "not in BSS",           "Wi-Fi switch off", "beacon lost",  "disassociated",
        "invalid profile id",   "Wi-Fi suspended",  "timeout",
    };
    u32 i = (u32)error - 0x80410A01;

    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "";
}

static void log_ap_events(void)
{
    static const char *const names[] = {
        "connect request",  "scan request",         "scan complete",    "established",
        "got IP",           "disconnect request",   "ERROR",            "info",
        "EAP auth",         "key exchange",         "reconnect",
    };

    while (ap_tail != ap_head) {
        int i = ap_tail % AP_EVENTS;
        int e = ap_events[i].event;

        th_log("net: ap state %d -> %d, event %s, error %08x %s",
               ap_events[i].from, ap_events[i].to,
               e >= 0 && e <= 10 ? names[e] : "?", ap_events[i].error,
               ap_error_name(ap_events[i].error));
        ap_tail++;
    }
}

/*
 * Looks up one config.ini profile entry among the valid profiles: all
 * digits is a profile number, anything else a profile name, then an SSID
 * (ASCII case ignored, other bytes compared as they are). 0 = no match.
 */
static int find_profile(const char *want)
{
    const char *p = want;

    /* all digits: a profile number */
    while (*p >= '0' && *p <= '9')
        p++;
    if (!*p) {
        int n = atoi_simple(want);

        for (int i = 0; i < nprofiles; i++)
            if (profiles[i] == n)
                return n;
        return 0;
    }

    /* a profile name (pass 0), then an SSID (pass 1) */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nprofiles; i++) {
            netData d;

            memset(&d, 0, sizeof(d));
            sceUtilityGetNetParam(profiles[i], pass ? PSP_NETPARAM_SSID : PSP_NETPARAM_NAME, &d);
            if (!strncasecmp(d.asString, want, sizeof(d.asString)))
                return profiles[i];
        }
    }

    return 0;
}

/*
 * Narrows profiles[] to the ones listed in config.ini, in that order.
 * Returns 0 (leaving profiles[] alone) if none of them exist.
 */
static int use_config_profiles(void)
{
    char list[sizeof(th_cfg.profile)];
    int  chosen[10], n = 0;

    strncpy0(list, th_cfg.profile, sizeof(list));
    for (char *tok = list, *next; tok; tok = next) {
        next = strstr(tok, ",");
        if (next)
            *next++ = 0;

        tok = trim(tok);
        if (!*tok)
            continue;

        int id = find_profile(tok);
        if (!id) {
            th_log("net: config.ini profile \"%s\": no profile with that number, name or SSID",
                   tok);
            continue;
        }

        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= chosen[i] == id;
        if (!dup && n < 10) {
            th_log("net: config.ini profile \"%s\" = profile %d", tok, id);
            chosen[n++] = id;
        }
    }

    if (!n)
        return 0;

    memcpy(profiles, chosen, n * sizeof(chosen[0]));
    nprofiles = n;
    return 1;
}

/*
 * Fills profiles[] with the valid profiles to try: the ones config.ini
 * lists, else all of them with the most recently used first.
 */
static void pick_profiles(void)
{
    for (int i = 1; i <= 10; i++) {
        if (sceUtilityCheckNetParam(i) != 0)
            continue;

        netData name, ssid;

        memset(&name, 0, sizeof(name));
        memset(&ssid, 0, sizeof(ssid));
        sceUtilityGetNetParam(i, PSP_NETPARAM_NAME, &name);
        sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &ssid);
        th_log("net: profile %d: \"%s\" (SSID \"%s\")", i, name.asString, ssid.asString);
        profiles[nprofiles++] = i;
    }

    if (!nprofiles)
        return;

    if (th_cfg.profile[0] && strcmp(th_cfg.profile, "0")) {
        if (use_config_profiles())
            return;
        /* A typo shouldn't leave the PSP offline: fall back to automatic. */
        th_log("net: no profile in config.ini matched; trying all of them");
        thkOverlayShow("Wi-Fi: config.ini profile not found", 10000, 0);
    }

    /* the most recently used one first */
    int preferred = registry_latest_profile();
    th_log("net: registry latest_id = %d", preferred);

    for (int i = 1; i < nprofiles; i++) {
        if (profiles[i] == preferred) {
            profiles[i] = profiles[0];
            profiles[0] = preferred;
        }
    }
}

static int ap_state(void)
{
    int s = -1;

    return sceNetApctlGetState(&s) < 0 ? -1 : s;
}

/* --- bringing the network up and down ------------------------------------ */

/*
 * This module runs from the volatile memory, which the core holds while it's
 * loaded; the network modules, the sceNetInit pool and the apctl thread go
 * there too, so the game's user memory never moves.
 */
static int modules_loaded, net_inited, inet_inited, apctl_inited;

static void log_memory(const char *what)
{
    u32 user[2] = { 0, 0 }, vol[2] = { 0, 0 };

    thkMemInfo(2, user);
    thkMemInfo(5, vol);
    th_log("net: %s; user memory free %lu KB (largest %lu KB), "
           "volatile free %lu KB (largest %lu KB)",
           what, user[0] / 1024, user[1] / 1024, vol[0] / 1024, vol[1] / 1024);
}

/*
 * unload_modules = 0 when the module is being stopped: module_stop runs
 * inside the module manager, so asking it to unload the network modules
 * would wait for itself (it did, for 3 s, on every quit and sleep). The
 * core unloads them once this module is gone.
 */
void th_net_stop(int unload_modules)
{
    th_shot_free();
    th_http_close();
    th_ip[0] = 0;

    /* Tear down in the reverse order of net_up(). */
    if (apctl_inited) {
        if (ap_state() != PSP_NET_APCTL_STATE_DISCONNECTED)
            sceNetApctlDisconnect();
        sceNetApctlTerm();
    }
    if (inet_inited)
        sceNetInetTerm();
    if (net_inited)
        sceNetTerm();
    apctl_inited = inet_inited = net_inited = 0;

    if (modules_loaded && unload_modules)
        thkNetModules(0);
    modules_loaded = 0;

    set_state(NET_OFF);
    th_log("net: stopped");
}

/* Returns 0, or < 0 with everything unloaded again. */
static int net_up(void)
{
    int r;

    if ((r = thkNetModules(5)) < 0) {
        if (r != (int)THK_ERR_STOPPING)
            th_log("net: loading the network modules failed (%08x)", r);
        goto fail;
    }
    modules_loaded = 1;

    if ((r = sceNetInit(NET_POOL, 42, 4 * 1024, 42, 4 * 1024)) < 0) {
        th_log("net: sceNetInit failed (%08x)", r);
        goto fail;
    }
    net_inited = 1;

    if ((r = sceNetInetInit()) < 0) {
        th_log("net: sceNetInetInit failed (%08x)", r);
        goto fail;
    }
    inet_inited = 1;

    if ((r = sceNetApctlInit(0x8000, 48)) < 0) {
        th_log("net: sceNetApctlInit failed (%08x)", r);
        goto fail;
    }
    apctl_inited = 1;

    if ((r = sceNetApctlAddHandler(ap_handler, NULL)) < 0)
        th_log("net: could not add apctl handler (%08x)", r);

    log_memory("started");
    return 0;

fail:
    th_net_stop(1);
    return r < 0 ? r : -1;
}

void th_net_start(void)
{
    th_http_init();

    pick_profiles();
    if (!nprofiles) {
        th_log("net: no network profiles; create one in the XMB");
        thkOverlayShow("Wi-Fi: no network profiles", 10000, 0);
        set_state(NET_FAILED);
        return;
    }

    int r = net_up();
    if (r < 0) {
        /* Switched off (or asleep, or quitting) while starting: not a failure. */
        if (r != (int)THK_ERR_STOPPING)
            thkOverlayShow("Wi-Fi: network init failed", 10000, 0);
        set_state(NET_FAILED);
        return;
    }

    set_state(NET_CONNECT);
}

/* --- the connection state machine ---------------------------------------- */

/*
 * After a failed attempt, move on to the next profile (if any). A
 * switched-off radio isn't the profile's fault, so that doesn't rotate.
 */
static int failed_attempts;

static void attempt_failed(int error)
{
    if (++failed_attempts == 3)
        thkOverlayShow("Wi-Fi: can't connect, retrying", 5000, 0);

    if (error != APCTL_ERROR_WLAN_SWITCH_OFF && nprofiles > 1) {
        profile_idx = (profile_idx + 1) % nprofiles;
        th_log("net: trying profile %d next", profiles[profile_idx]);
    }

    set_state(NET_RETRY);
}

void th_net_poll(void)
{
    u32 elapsed = now() - state_since;

    log_ap_events();

    switch (state) {
    case NET_CONNECT: {
        if (ap_state() != PSP_NET_APCTL_STATE_DISCONNECTED)
            sceNetApctlDisconnect();
        ap_error      = 0;
        ap_reconnects = 0;

        int r = sceNetApctlConnect(profiles[profile_idx]);
        if (r < 0) {
            th_log("net: connect to profile %d failed (%08x)", profiles[profile_idx], r);
            attempt_failed(r);
        } else {
            th_log("net: connecting with profile %d", profiles[profile_idx]);
            set_state(NET_WAIT_IP);
        }
        break;
    }

    case NET_WAIT_IP: {
        int s = ap_state();

        if (s == PSP_NET_APCTL_STATE_GOT_IP) {
            union SceNetApctlInfo info;

            memset(&info, 0, sizeof(info));
            sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info);
            strncpy0(th_ip, info.ip, sizeof(th_ip));

            if (th_http_open() < 0) {
                set_state(NET_RETRY);
                break;
            }

            char url[48];
            if (th_cfg.port == 80)
                mini_snprintf(url, sizeof(url), "http://%s/", th_ip);
            else
                mini_snprintf(url, sizeof(url), "http://%s:%d/", th_ip, th_cfg.port);

            th_log("net: serving %s", url);
            th_mdns_open(th_ip);
            thkOverlayShow(url, 10000, THK_OVERLAY_QR);

            failed_attempts = 0;
            last_ap_check   = now();
            set_state(NET_UP);
        } else if (ap_reconnects >= MAX_RECONNECTS) {
            log_ap_events();
            th_log("net: profile %d: handshake keeps failing; check the password and that the "
                   "access point offers WPA/WPA2-PSK on 2.4 GHz", profiles[profile_idx]);
            sceNetApctlDisconnect();
            attempt_failed(0);
        } else if (ap_error || elapsed > CONNECT_TIMEOUT ||
                   (s == PSP_NET_APCTL_STATE_DISCONNECTED && elapsed > 8 * SEC)) {
            log_ap_events();
            th_log("net: profile %d failed (state %d, error %08x %s; switch state %d)",
                   profiles[profile_idx], s, ap_error, ap_error_name(ap_error),
                   sceWlanGetSwitchState());

            int error = ap_error;
            sceNetApctlDisconnect();
            attempt_failed(error);
        }
        break;
    }

    case NET_UP:
        if ((u32)(now() - last_ap_check) > SEC) {
            last_ap_check = now();
            if (ap_state() != PSP_NET_APCTL_STATE_GOT_IP) {
                th_log("net: Wi-Fi connection lost; reconnecting");
                thkOverlayShow("Wi-Fi lost, reconnecting", 5000, 0);
                th_http_close();
                th_ip[0] = 0;
                set_state(NET_RETRY);
                break;
            }
        }
        th_http_poll();     /* waits in poll() instead of sleeping */
        return;

    case NET_RETRY:
        if (elapsed > RETRY_DELAY)
            set_state(NET_CONNECT);
        break;

    case NET_OFF:
    case NET_FAILED:
        break;
    }

    sceKernelDelayThread(20 * 1000);
}

/*
 * Global settings. There is deliberately no per-game configuration.
 *
 * ms0:/SEPLUGINS/texthook/config.ini, all keys optional:
 *
 *   ; network profiles to use, in order: numbers, profile names or SSIDs,
 *   ; separated by commas; 0 = most recently used, then the others
 *   profile = 0
 *   ; HTTP port
 *   port = 80
 *   ; on-screen messages (URL + QR code after connecting, Wi-Fi problems)
 *   overlay = 1
 *   ; top-left, top-right, bottom-left or bottom-right
 *   overlay_corner = top-left
 *   ; the page is also at http://<hostname>.local/ (letters, digits, '-')
 *   hostname = psp
 */

#include <pspkernel.h>

#include "internal.h"
#include "mini.h"
#include "thk_abi.h"

#define CONFIG_PATH "ms0:/SEPLUGINS/texthook/config.ini"

th_config th_cfg = {
    .profile        = "0",
    .port           = 80,
    .overlay        = 1,
    .overlay_corner = THK_CORNER_TOP_LEFT,
    .hostname       = "psp",
};

/*
 * A DNS label: 1-31 letters, digits or '-', not starting or ending with
 * '-'. Lowercased, since names are matched without case.
 */
static int set_hostname(const char *v)
{
    int n = strlen(v);

    if (n < 1 || n >= (int)sizeof(th_cfg.hostname) || v[0] == '-' || v[n - 1] == '-')
        return 0;

    for (int i = 0; i < n; i++) {
        char c = v[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-'))
            return 0;
    }

    for (int i = 0; i <= n; i++)
        th_cfg.hostname[i] = v[i] >= 'A' && v[i] <= 'Z' ? v[i] + 32 : v[i];
    return 1;
}

static const char *const corners[] = {
    [THK_CORNER_TOP_LEFT]     = "top-left",
    [THK_CORNER_TOP_RIGHT]    = "top-right",
    [THK_CORNER_BOTTOM_LEFT]  = "bottom-left",
    [THK_CORNER_BOTTOM_RIGHT] = "bottom-right",
};

static void apply(char *key, char *value)
{
    if (!strcmp(key, "profile")) {
        strncpy0(th_cfg.profile, value, sizeof(th_cfg.profile));
    } else if (!strcmp(key, "port")) {
        th_cfg.port = atoi_simple(value);
    } else if (!strcmp(key, "overlay")) {
        th_cfg.overlay = atoi_simple(value) != 0;
    } else if (!strcmp(key, "overlay_corner")) {
        for (int i = 0; i < 4; i++)
            if (!strcmp(value, corners[i]))
                th_cfg.overlay_corner = i;
    } else if (!strcmp(key, "hostname")) {
        if (!set_hostname(value))
            th_log("config: hostname '%s' isn't usable (letters, digits and '-' only); "
                   "keeping '%s'", value, th_cfg.hostname);
    } else {
        th_log("config: unknown key '%s'", key);
    }
}

void th_config_load(void)
{
    static char buf[2048];

    SceUID fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        th_log("config: %s not found, using defaults", CONFIG_PATH);
        goto apply_overlay;
    }

    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;

    /* key = value lines; ';' and '#' start comments */
    for (char *line = buf; line && *line;) {
        char *next = strstr(line, "\n");
        if (next)
            *next++ = 0;

        char *s  = trim(line);
        char *eq = strstr(s, "=");

        if (*s && *s != ';' && *s != '#' && eq) {
            *eq = 0;
            apply(trim(s), trim(eq + 1));
        }
        line = next;
    }

    if (th_cfg.port <= 0 || th_cfg.port > 65535)
        th_cfg.port = 80;

    th_log("config: profile \"%s\", port %d, overlay %d (%s), hostname %s", th_cfg.profile,
           th_cfg.port, th_cfg.overlay, corners[th_cfg.overlay_corner], th_cfg.hostname);

apply_overlay:
    thkOverlayConfig(th_cfg.overlay, th_cfg.overlay_corner);
}

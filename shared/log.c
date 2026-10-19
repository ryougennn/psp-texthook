/* th_log(): the user modules' log, tagged with th_log_name. */

#include <pspkernel.h>

#include "mini.h"
#include "shared.h"

#define LOG_PATH "ms0:/SEPLUGINS/texthook/log.txt"

/*
 * Goes to stdout (the PSPLink shell when attached) and to a file on the
 * memory stick. Opens and closes the file each time so nothing is lost if
 * the game crashes.
 */
void th_log(const char *fmt, ...)
{
    char    buf[1536];
    va_list ap;

    va_start(ap, fmt);
    u32 ms = sceKernelGetSystemTimeLow() / 1000;
    int n  = mini_snprintf(buf, sizeof(buf), "%5lu.%03lu [%s] ", ms / 1000, ms % 1000,
                           th_log_name);
    n     += mini_vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    if (n > (int)sizeof(buf) - 2) {
        n = sizeof(buf) - 2;
        /*
         * Cut before a character, not inside one: a single broken UTF-8
         * sequence makes editors misread the whole file's encoding.
         */
        while (n > 0 && ((u8)buf[n] & 0xC0) == 0x80)
            n--;
    }

    /*
     * Control bytes (e.g. from a hook that caught bytecode) have the same
     * effect, and can break the line structure.
     */
    for (int i = 0; i < n; i++)
        if ((u8)buf[i] < 0x20 && buf[i] != '\t')
            buf[i] = '?';
    buf[n++] = '\n';

    sceIoWrite(1, buf, n);

#ifdef TH_LOG_FILE
    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, n);
        sceIoClose(fd);
    }
#endif
}

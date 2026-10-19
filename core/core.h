/*
 * Internals of the kernel core (thkcore.prx), shared between its files.
 * What the user modules may call is in common/thk_abi.h.
 */

#ifndef CORE_H
#define CORE_H

#include <psptypes.h>

#include "thk_abi.h"

#define MAX_HOOKS       16      /* per game */

/* --- MIPS ---------------------------------------------------------------- */

#define MIPS_J          0x02    /* opcodes of the absolute jumps */
#define MIPS_JAL        0x03

static inline u32 mips_jump(u32 opcode, u32 target)
{
    return (opcode << 26) | ((target >> 2) & 0x03FFFFFF);
}

/* --- user pointers ------------------------------------------------------- */

/*
 * True if [addr, addr + len) lies in user RAM, where a caller's buffers
 * must be: an export checks every pointer it is given before using it.
 */
static inline int user_range(u32 addr, u32 len)
{
    return addr >= THK_USER_RAM_START && addr < THK_USER_RAM_END &&
           len <= THK_USER_RAM_END - addr;
}

/* --- main.c -------------------------------------------------------------- */

void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  title_id(char *out, int size);     /* "ULJM05338"; < 0 if not known yet */

/* --- hook.c: patching the game's code ------------------------------------ */

/*
 * Points the instruction at addr to thunk, whose last THK_TAIL_WORDS words
 * (tail) are filled to replay what it displaces. Patches at once if the
 * code is loaded, else when its module starts. Returns THK_OK, THK_PENDING
 * or THK_ERR_*.
 */
int  hook_install(u32 addr, u32 thunk, u32 *tail, u32 expect);
void hook_module_started(const char *name, u32 start, u32 end);

/* --- capture.c ----------------------------------------------------------- */

int  capture_start(void);       /* 1 once done (or given up); call until then */

/* --- overlay.c ----------------------------------------------------------- */

void overlay_init(void);
void overlay_message(const char *msg, int ms);
int  overlay_popup(void *fb);           /* show fb instead of the game, or NULL */
int  overlay_game_frame(u32 *out);      /* the game's last frame: address, width, format */

/* --- popup.c ------------------------------------------------------------- */

void popup_init(void);
void popup_reset(void);         /* the popup is gone: screen back, buttons once released */
int  popup_requested(void);     /* L+R was pressed (cleared by reading) */
int  popup_finished(void);      /* the popup closed itself (cleared by reading) */

#endif /* CORE_H */

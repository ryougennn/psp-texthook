/*
 * Public API for per-title hook scripts (games/<TITLEID>.c).
 *
 * A script declares one game and its hooks:
 *
 *     static void on_text(th_ctx *c)
 *     {
 *         th_emit_utf16(c, c->a0, 0);
 *     }
 *
 *     static void clean(char *s, int size)
 *     {
 *         th_remove(s, "\n");
 *     }
 *
 *     TH_GAME("CLANNAD", TH_IDS("ULJM05338", "ULJM05339"),
 *         TH_HOOK(0x0880F240, on_text, .filter = clean));
 *
 * Hook functions run on the game's own thread, at the moment the hooked
 * instruction is about to execute, inside the kernel core (thkcore.prx):
 * scripts are compiled into it, and a small stub makes a syscall. Keep them
 * short: read registers and memory, call a th_emit_* function, return. No
 * floats (FPU registers are not saved), no blocking calls, no logging, and
 * little stack (it's the thread's kernel stack); big buffers are static.
 *
 * Filters are optional and run later, in the user modules (texthook.prx,
 * thkpopup.prx), on the decoded UTF-8 line, before de-duplication. They
 * edit the line in place (`size` is the buffer size) and may do as much
 * string work as they like. Leaving the line empty discards it. Each
 * script is compiled into those modules too, with TH_FILTERS_ONLY, which
 * leaves its hook functions out.
 *
 * Hooks are installed about 2 s after the game's executable starts, so
 * there is no way to change the game before its code runs.
 *
 * Hook placement rules (checked by the core, which refuses violations):
 *   - neither addr nor addr+4 may be a branch/jump
 *   - addr may not sit in a branch delay slot
 *   - nothing may branch to addr+4 (cannot be checked; the core
 *     overwrites it with a nop)
 */

#ifndef TEXTHOOK_H
#define TEXTHOOK_H

#include <psptypes.h>

/* Max bytes captured per emit; longer text is truncated. */
#define TH_MAX_TEXT 1024

/* --- types --------------------------------------------------------------- */

/*
 * Register snapshot at the hooked instruction. Read-only: changes are
 * not written back.
 */
typedef struct th_ctx {
    union {
        u32 r[32];
        struct {
            u32 zero, at, v0, v1, a0, a1, a2, a3;
            u32 t0,   t1, t2, t3, t4, t5, t6, t7;
            u32 s0,   s1, s2, s3, s4, s5, s6, s7;
            u32 t8,   t9, k0, k1, gp, sp, fp, ra;
        };
    };
    u32 hi, lo;
    u32 _ret;   /* internal: return address into the thunk */
    u32 hook;   /* index of the hook that fired, within its game */
} th_ctx;

typedef struct th_hook {
    u32                 addr;
    void                (*fn)(th_ctx *c);
    u32                 expect;     /* original instruction at addr, or 0 to skip the check */
    const char          *name;
    void                (*filter)(char *s, int size);   /* optional, see above */
} th_hook;

typedef struct th_game {
    const char          *title;
    const char *const   *ids;       /* NULL-terminated */
    const th_hook       *hooks;
    int                 nhooks;
} th_game;

/* --- declaring a game ---------------------------------------------------- */

#define TH_IDS(...) ((const char *const[]){ __VA_ARGS__, 0 })

/*
 * In the user modules (TH_FILTERS_ONLY) the hook functions are left out:
 * the expression still names them, so they count as used, but folds to
 * NULL, so the compiler drops them and everything they call.
 */
#ifdef TH_FILTERS_ONLY
#define TH__FN(fn_)     (0 ? (fn_) : 0)
#else
#define TH__FN(fn_)     (fn_)
#endif

/* Optional trailing designated initializers, e.g. `.filter = clean`. */
#define TH_HOOK(addr_, fn_, ...) \
    { .addr = (addr_), .fn = TH__FN(fn_), .name = #fn_, ##__VA_ARGS__ }

#define TH_HOOK_EXPECT(addr_, insn_, fn_, ...) \
    { .addr = (addr_), .fn = TH__FN(fn_), .expect = (insn_), .name = #fn_, ##__VA_ARGS__ }

/* TH_GAME_SYM is set per file by the Makefile (th_game_<filename>). */
#define TH_GAME(title, ids, ...)                                \
    static const th_hook th__hooks[] = { __VA_ARGS__ };         \
    const th_game TH_GAME_SYM = {                               \
        (title), (ids), th__hooks,                              \
        sizeof(th__hooks) / sizeof(th__hooks[0])                \
    }

/* --- emitting text (hook context) ---------------------------------------- */

/*
 * Emit a string for processing. `max` bounds the scan (units of the
 * encoding: UTF-16 code units or bytes); 0 means "up to TH_MAX_TEXT bytes".
 * Strings are NUL-terminated unless the *_len variants are used.
 */
void th_emit_utf16(th_ctx *c, u32 addr, int max);
void th_emit_utf16_len(th_ctx *c, u32 addr, int units);
void th_emit_utf8(th_ctx *c, u32 addr, int max);
void th_emit_sjis(th_ctx *c, u32 addr, int max);

/*
 * Emit text the script already decoded into UTF-8 (e.g. via its own
 * table). `len` < 0 means NUL-terminated.
 */
void th_emit_text(th_ctx *c, const char *utf8, int len);

/* --- reading game memory ------------------------------------------------- */

/* Safe accessors for game memory. Return 0 for addresses outside user RAM. */
int th_valid(u32 addr, u32 len);
u8  th_read8(u32 addr);
u16 th_read16(u32 addr);
u32 th_read32(u32 addr);

/* --- filter helpers ------------------------------------------------------ */

/*
 * String helpers for filters (UTF-8, in place). Each returns the number of
 * changes made.
 */
int th_remove(char *s, const char *needle);
int th_replace(char *s, int size, const char *from, const char *to);
int th_remove_between(char *s, const char *open, const char *close);   /* inclusive */
int th_trim(char *s);   /* ASCII whitespace and U+3000 at both ends */

#endif /* TEXTHOOK_H */

/*
 * Stack frame shared by hook_entry.S and the thunks built in capture.c.
 * Must match struct th_ctx in texthook.h (checked in capture.c).
 */

#ifndef HOOK_FRAME_H
#define HOOK_FRAME_H

#define CTX_BASE    16                      /* after the o32 arg area */
#define R(n)        (CTX_BASE + 4 * (n))
#define CTX_HI      (CTX_BASE + 128)
#define CTX_LO      (CTX_BASE + 132)
#define CTX_RET     (CTX_BASE + 136)
#define CTX_SIZE    144

#define FRAME_SIZE  (CTX_BASE + CTX_SIZE)   /* 160, keeps sp 16-byte aligned */

/*
 * Words at the end of each thunk that hook_install() fills when it patches:
 * the two displaced instructions (a branch is relocated) and the jumps
 * back into the game. See build_tail() in hook.c.
 */
#define THK_TAIL_WORDS  6

#endif /* HOOK_FRAME_H */

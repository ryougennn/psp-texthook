/*
 * Minimal freestanding libc subset shared by all three modules (and the
 * vendored code in third_party/). Every PRX links with -nostdlib, so
 * nothing here may depend on newlib.
 */

#ifndef MINI_H
#define MINI_H

#include <stdarg.h>
#include <stddef.h>

/* --- memory -------------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* --- numbers ------------------------------------------------------------- */

int  abs(int v);                /* abs and labs: for qrcodegen */
long labs(long v);
int  atoi_simple(const char *s);

/* --- strings ------------------------------------------------------------- */

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strstr(const char *hay, const char *needle);

/* Always NUL-terminates. */
char *strncpy0(char *dst, const char *src, size_t size);

/* Strips spaces, tabs and \r in place; returns the new start. */
char *trim(char *s);

/* --- formatting ---------------------------------------------------------- */

/*
 * Supports %d %u %x %X %p %s %c %% with optional '0' flag, width and
 * 'l' (a no-op: int and long are both 32-bit).
 */
int mini_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int mini_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* MINI_H */

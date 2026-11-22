/*
    string.h for native kernels (no libc on the Pi side; sdk/native/narm_rt.c has these).
    Only used when building the .narm and .nx64 modules; the 68k build of a kernel gets its
    compiler's own.
*/
#ifndef NC_NATIVE_STRING_H
#define NC_NATIVE_STRING_H

#include <stddef.h>

/* Hidden: a default-visibility declaration would make -fPIE call through a GOT (kernel.h) */
#pragma GCC visibility push(hidden)
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
#pragma GCC visibility pop

#endif

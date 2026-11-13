/* Minimal runtime for native modules: GCC may emit calls to these even without libc. */

#include <nativecall/kernel.h>
#include <nativecall/nc_abi.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--)
            d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;

    for (; n; n--, p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (uint8_t)*a - (uint8_t)*b;
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++)
    {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (;; s++)
    {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

/* The heap of malloc() & co. (nativecall/nc_heap.h, sdk/native/include/stdlib.h) */
struct NCHeap;
struct NCHeap *nc__heap_current;

/* ---- Log (hypervisor 7+, nc_abi.h struct NCEnv) ----------------------------------------------- */

#include <stdarg.h>

/* The hypervisor's environment of this core, or NULL (older hypervisor). TPIDR_EL2 points at its
   per-core state while native code runs; before hypervisor 7 the word at NC_CPU_ENV is 0. */
static const struct NCEnv *nc__env(void)
{
    uintptr_t cpu;
    const struct NCEnv *env;

    __asm__ volatile("mrs %0, TPIDR_EL2" : "=r"(cpu));
    if (cpu < 0x01000000UL)
        return NULL;
    env = *(const struct NCEnv * const *)(cpu + NC_CPU_ENV);
    if ((uintptr_t)env < 0x01000000UL || env->magic != NC_ENV_MAGIC)
        return NULL;
    return env;
}

void nc_log(const char *text)
{
    const struct NCEnv *env = nc__env();
    size_t n;

    if (!env || !text)
        return;
    n = strlen(text);
    while (n)
    {
        uint32_t part = n > 255 ? 255 : (uint32_t)n;

        env->log_write(text, part);
        text += part;
        n -= part;
    }
}

struct nc__out
{
    char buf[256];
    uint32_t n;
};

static void nc__putc(struct nc__out *o, char c)
{
    if (o->n < sizeof(o->buf) - 1)
        o->buf[o->n++] = c;
}

static void nc__pad(struct nc__out *o, int count, char c)
{
    while (count-- > 0)
        nc__putc(o, c);
}

static void nc__num(struct nc__out *o, uint64_t v, unsigned base, int upper, int neg, int width, int zero, int left)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int k = 0, len;

    do
    {
        tmp[k++] = digits[v % base];
        v /= base;
    } while (v);
    len = k + neg;
    if (!left && !zero)
        nc__pad(o, width - len, ' ');
    if (neg)
        nc__putc(o, '-');
    if (!left && zero)
        nc__pad(o, width - len, '0');
    while (k)
        nc__putc(o, tmp[--k]);
    if (left)
        nc__pad(o, width - len, ' ');
}

void nc_printf(const char *format, ...)
{
    const struct NCEnv *env = nc__env();
    struct nc__out o;
    const char *f;
    va_list ap;

    if (!env || !format)
        return;
    o.n = 0;
    va_start(ap, format);
    for (f = format; *f; f++)
    {
        int left = 0, zero = 0, width = 0, lng = 0;

        if (*f != '%')
        {
            nc__putc(&o, *f);
            continue;
        }
        f++;
        for (;; f++)
        {
            if (*f == '-')
                left = 1;
            else if (*f == '0')
                zero = 1;
            else
                break;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z')
        {
            lng++;
            f++;
        }
        switch (*f)
        {
            case 'd':
            case 'i':
            {
                int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
                nc__num(&o, v < 0 ? (uint64_t)-v : (uint64_t)v, 10, 0, v < 0, width, zero, left);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            {
                uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
                nc__num(&o, v, *f == 'u' ? 10 : 16, *f == 'X', 0, width, zero, left);
                break;
            }
            case 'p':
                nc__putc(&o, '0');
                nc__putc(&o, 'x');
                nc__num(&o, (uintptr_t)va_arg(ap, void *), 16, 0, 0, width, zero, left);
                break;
            case 's':
            {
                const char *s = va_arg(ap, const char *);
                int len;

                if (!s)
                    s = "(null)";
                len = (int)strlen(s);
                if (!left)
                    nc__pad(&o, width - len, ' ');
                while (*s)
                    nc__putc(&o, *s++);
                if (left)
                    nc__pad(&o, width - len, ' ');
                break;
            }
            case 'c':
                nc__putc(&o, (char)va_arg(ap, int));
                break;
            case '%':
                nc__putc(&o, '%');
                break;
            case 0:
                f--;
                break;
            default:
                nc__putc(&o, '%');
                nc__putc(&o, *f);
                break;
        }
    }
    va_end(ap);
    env->log_write(o.buf, o.n);
}

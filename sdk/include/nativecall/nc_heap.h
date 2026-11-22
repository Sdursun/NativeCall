/*
    nativecall/nc_heap.h - a small memory allocator for kernels (malloc for code that needs it,
    e.g. a decoder library compiled into a kernel).

    The heap is a block of memory the 68k program gives the kernel (nc_alloc), so it survives
    between calls and both sides can use it. It holds offsets, not pointers: the same heap works
    for the native and the 68k version of a kernel.

        68k:     heap = nc_alloc(256 * 1024);  p->heap = (NC_U32)heap;
        kernel:  struct NCHeap *h = nc_heap_init(NC_PTR(void, p->heap), 256 * 1024);  // once
                 void *buf = nc_heap_alloc(h, 4096);
                 nc_heap_free(h, buf);

    malloc()/calloc()/realloc()/free() of <stdlib.h> in native kernels (sdk/native/include) use
    the heap chosen with nc_heap_select(h) - one per module, shared by all cores. The heap has a
    lock, so pieces of nc_run_mp may allocate at the same time.

    First fit over an address ordered free list, neighbours merged on free, 16-byte alignment.
*/

#ifndef NATIVECALL_NC_HEAP_H
#define NATIVECALL_NC_HEAP_H

#include <nativecall/kernel.h>

#define NC_HEAP_MAGIC   0x4e434850UL        /* 'NCHP' */
#define NC_HEAP_ALIGN   16
#define NC_HEAP_USED    1                   /* block.next of an allocated block */

struct NCHeap
{
    NC_U32 magic;
    NC_U32 size;            /* bytes from the start of this struct */
    NC_U32 free;            /* offset of the first free block, 0: none */
    NC_U32 lock;
};

struct NCHeapBlock
{
    NC_U32 size;            /* including this header, multiple of 16 */
    NC_U32 next;            /* free: offset of the next free block (0: last); used: NC_HEAP_USED */
    NC_U32 pad[2];
};

#define NC__HB(h, off)  ((struct NCHeapBlock *)((char *)(h) + (off)))
#define NC__OFF(h, b)   ((NC_U32)((char *)(b) - (char *)(h)))

/* The 68k side has one core: no lock. On the PC (WinUAE x64) the pool's threads share the heap
   as the Pi's cores do; the field is big-endian there, so the lock word is reached through its
   offset (GCC does not take the address of a reversed field). 0 is free either way. */
#if NC_X64
#define NC__HEAP_LOCKW(h)   ((NC_U32 *)((char *)(h) + offsetof(struct NCHeap, lock)))
#endif

static inline void nc__heap_lock(struct NCHeap *h)
{
#if NC_NATIVE
    while (__atomic_exchange_n(&h->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&h->lock, __ATOMIC_RELAXED))
            __asm__ volatile("yield");
#elif NC_X64
    while (__atomic_exchange_n(NC__HEAP_LOCKW(h), 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(NC__HEAP_LOCKW(h), __ATOMIC_RELAXED))
            __builtin_ia32_pause();
#endif
}

static inline void nc__heap_unlock(struct NCHeap *h)
{
#if NC_NATIVE
    __atomic_store_n(&h->lock, 0, __ATOMIC_RELEASE);
#elif NC_X64
    __atomic_store_n(NC__HEAP_LOCKW(h), 0, __ATOMIC_RELEASE);
#endif
}

/* Set up a heap in [mem, mem + size). Returns NULL if it is too small. Calling it again on the
   same memory throws everything away; nc_heap_valid() tells whether it is set up already. */
static inline struct NCHeap *nc_heap_init(void *mem, NC_U32 size)
{
    NC_U32 skip = (NC_U32)(-(NC_U32)(NC_UPTR)mem & (NC_HEAP_ALIGN - 1));
    struct NCHeap *h = (struct NCHeap *)((char *)mem + skip);
    NC_U32 total = (size - skip) & ~(NC_U32)(NC_HEAP_ALIGN - 1);

    if (size < skip + 2 * NC_HEAP_ALIGN + NC_HEAP_ALIGN || total < 3 * NC_HEAP_ALIGN)
        return 0;
    h->magic = NC_HEAP_MAGIC;
    h->size = total;
    h->free = NC_HEAP_ALIGN;
    h->lock = 0;
    NC__HB(h, NC_HEAP_ALIGN)->size = total - NC_HEAP_ALIGN;
    NC__HB(h, NC_HEAP_ALIGN)->next = 0;
    return h;
}

static inline int nc_heap_valid(const void *mem)
{
    NC_U32 skip = (NC_U32)(-(NC_U32)(NC_UPTR)mem & (NC_HEAP_ALIGN - 1));
    return ((const struct NCHeap *)((const char *)mem + skip))->magic == NC_HEAP_MAGIC;
}

/* The heap in [mem, mem + size): the one set up there already, or a new one. For kernels that
   keep their heap between calls. */
static inline struct NCHeap *nc_heap_open(void *mem, NC_U32 size)
{
    NC_U32 skip = (NC_U32)(-(NC_U32)(NC_UPTR)mem & (NC_HEAP_ALIGN - 1));

    if (nc_heap_valid(mem))
        return (struct NCHeap *)((char *)mem + skip);
    return nc_heap_init(mem, size);
}

static inline void *nc_heap_alloc(struct NCHeap *h, NC_U32 n)
{
    NC_U32 need, prev = 0, off;
    void *result = 0;

    if (!h || n == 0 || n > h->size)
        return 0;
    need = (n + sizeof(struct NCHeapBlock) + NC_HEAP_ALIGN - 1) & ~(NC_U32)(NC_HEAP_ALIGN - 1);

    nc__heap_lock(h);
    for (off = h->free; off; prev = off, off = NC__HB(h, off)->next)
    {
        struct NCHeapBlock *b = NC__HB(h, off);
        NC_U32 rest;

        if (b->size < need)
            continue;
        rest = b->next;
        if (b->size - need >= 2 * NC_HEAP_ALIGN)
        {
            /* split: the tail stays free */
            struct NCHeapBlock *t = NC__HB(h, off + need);
            t->size = b->size - need;
            t->next = b->next;
            rest = off + need;
            b->size = need;
        }
        if (prev)
            NC__HB(h, prev)->next = rest;
        else
            h->free = rest;
        b->next = NC_HEAP_USED;
        result = (char *)b + sizeof(struct NCHeapBlock);
        break;
    }
    nc__heap_unlock(h);
    return result;
}

static inline void nc_heap_free(struct NCHeap *h, void *p)
{
    struct NCHeapBlock *b;
    NC_U32 off, prev = 0, next;

    if (!h || !p)
        return;
    b = (struct NCHeapBlock *)((char *)p - sizeof(struct NCHeapBlock));
    if (b->next != NC_HEAP_USED)
        return;                         /* not allocated (or freed twice) */
    off = NC__OFF(h, b);

    nc__heap_lock(h);
    for (next = h->free; next && next < off; prev = next, next = NC__HB(h, next)->next)
        ;
    b->next = next;
    if (next && off + b->size == next)
    {
        b->size += NC__HB(h, next)->size;
        b->next = NC__HB(h, next)->next;
    }
    if (prev)
    {
        struct NCHeapBlock *pb = NC__HB(h, prev);
        if (prev + pb->size == off)
        {
            pb->size += b->size;
            pb->next = b->next;
        }
        else
            pb->next = off;
    }
    else
        h->free = off;
    nc__heap_unlock(h);
}

/* Usable size of an allocated block */
static inline NC_U32 nc_heap_size(const void *p)
{
    return ((const struct NCHeapBlock *)((const char *)p - sizeof(struct NCHeapBlock)))->size
           - (NC_U32)sizeof(struct NCHeapBlock);
}

static inline void *nc_heap_realloc(struct NCHeap *h, void *p, NC_U32 n)
{
    void *q;
    NC_U32 old, i;

    if (!p)
        return nc_heap_alloc(h, n);
    if (n == 0)
    {
        nc_heap_free(h, p);
        return 0;
    }
    old = nc_heap_size(p);
    if (n <= old)
        return p;
    q = nc_heap_alloc(h, n);
    if (q)
    {
        for (i = 0; i < old; i++)
            ((char *)q)[i] = ((char *)p)[i];
        nc_heap_free(h, p);
    }
    return q;
}

/* Bytes free in total (walks the list) */
static inline NC_U32 nc_heap_avail(struct NCHeap *h)
{
    NC_U32 off, sum = 0;

    nc__heap_lock(h);
    for (off = h->free; off; off = NC__HB(h, off)->next)
        sum += NC__HB(h, off)->size - (NC_U32)sizeof(struct NCHeapBlock);
    nc__heap_unlock(h);
    return sum;
}

#if NC_NATIVE
/* The heap malloc() & co. use in this module (native side; <stdlib.h> of sdk/native/include) */
extern struct NCHeap *nc__heap_current __attribute__((visibility("hidden")));   /* narm_rt.c */
static inline void nc_heap_select(struct NCHeap *h) { nc__heap_current = h; }
#elif NC_X64
extern struct NCHeap *nc__heap_current;                                         /* narm_rt.c */
static inline void nc_heap_select(struct NCHeap *h) { nc__heap_current = h; }
#endif

#endif

/*
    Finds the block Emu68 will take its own memory from and takes the hypervisor's region off its
    top. It reads the tree the way Emu68 does (src/devicetree.c, src/aarch64/mmu.c:243-290): the
    first root child called "memory" or "memory@...", its "reg", cells from the node or else the
    root, default 1.

    Anything unexpected leaves the tree untouched and returns an error: the caller then starts
    Emu68 as the stock armstub would, without the hypervisor. No native calls, but no black screen.
*/
#include "hv_fdt.h"

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

#define MB2             0x200000ULL
#define GB1             0x40000000ULL
#define MAP_LIMIT       0x200000000ULL  /* the EL2 map reaches 8 GB */
#define MIN_LEFT        0x4000000ULL    /* Emu68 takes its kernel (32 MB), JIT and tables from this block */

static uint32_t rd32(const void *p)
{
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void wr32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static uint64_t rdcells(const uint8_t *p, uint32_t cells)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < cells; i++)
        v = (v << 32) | rd32(p + 4 * i);
    return v;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* "memory" or "memory@..." */
static int is_memory(const char *s)
{
    const char *m = "memory";
    while (*m)
        if (*s++ != *m++)
            return 0;
    return *s == 0 || *s == '@';
}

static int overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

int hv_fdt_reserve_top(void *dtb, uint64_t bytes, struct hv_fdt_result *out)
{
    const uint8_t *fdt = dtb;
    uint32_t total, off_struct, off_strings, size_struct, size_strings;

    if (rd32(fdt) != FDT_MAGIC)
        return HV_FDT_BADHEADER;
    total = rd32(fdt + 4);
    off_struct = rd32(fdt + 8);
    off_strings = rd32(fdt + 12);
    size_strings = rd32(fdt + 32);
    size_struct = rd32(fdt + 36);
    if (rd32(fdt + 20) < 17 || total < 40 || total > 0x4000000 ||
        off_struct > total || size_struct > total - off_struct ||
        off_strings > total || size_strings > total - off_strings || (off_struct & 3))
        return HV_FDT_BADHEADER;

    const uint8_t *p = fdt + off_struct, *end = p + size_struct;
    int depth = 0, section = 0;     /* section: 1 in /memory, 2 in /chosen (only directly in them) */
    int found_memory = 0, done_memory = 0;
    uint32_t root_ac = 1, root_sc = 1, mem_ac = 0, mem_sc = 0, reg_len = 0;
    uint8_t *reg = 0;
    uint64_t initrd0 = 0, initrd1 = 0;

    while (p + 4 <= end)
    {
        uint32_t tok = rd32(p);
        p += 4;
        if (tok == FDT_END)
            break;
        if (tok == FDT_NOP)
            continue;
        if (tok == FDT_BEGIN_NODE)
        {
            const char *name = (const char *)p;
            while (p < end && *p)
                p++;
            if (p >= end)
                return HV_FDT_BADHEADER;
            p = fdt + ((((uintptr_t)(p + 1 - fdt)) + 3) & ~(uintptr_t)3);
            depth++;
            section = 0;
            if (depth == 2 && is_memory(name) && !done_memory)
            {
                section = 1;
                found_memory = 1;
            }
            else if (depth == 2 && str_eq(name, "chosen"))
                section = 2;
        }
        else if (tok == FDT_END_NODE)
        {
            if (depth == 2 && section == 1)
                done_memory = 1;
            if (--depth < 0)
                return HV_FDT_BADHEADER;
            section = 0;
        }
        else if (tok == FDT_PROP)
        {
            if (p + 8 > end)
                return HV_FDT_BADHEADER;
            uint32_t len = rd32(p), nameoff = rd32(p + 4);
            const uint8_t *val = p + 8;
            if (len > (uint64_t)(end - val) || nameoff >= size_strings)
                return HV_FDT_BADHEADER;
            const char *pname = (const char *)fdt + off_strings + nameoff;

            if (depth == 1 && len == 4 && str_eq(pname, "#address-cells"))
                root_ac = rd32(val);
            else if (depth == 1 && len == 4 && str_eq(pname, "#size-cells"))
                root_sc = rd32(val);
            else if (section == 1 && len == 4 && str_eq(pname, "#address-cells"))
                mem_ac = rd32(val);
            else if (section == 1 && len == 4 && str_eq(pname, "#size-cells"))
                mem_sc = rd32(val);
            else if (section == 1 && str_eq(pname, "reg"))
            {
                reg = (uint8_t *)val;
                reg_len = len;
            }
            else if (section == 2 && (len == 4 || len == 8) && str_eq(pname, "linux,initrd-start"))
                initrd0 = len == 4 ? rd32(val) : rdcells(val, 2);
            else if (section == 2 && (len == 4 || len == 8) && str_eq(pname, "linux,initrd-end"))
                initrd1 = len == 4 ? rd32(val) : rdcells(val, 2);

            p = val + ((len + 3) & ~3u);
        }
        else
            return HV_FDT_BADHEADER;
    }

    if (!found_memory || !reg)
        return HV_FDT_NOMEMORY;

    uint32_t ac = mem_ac ? mem_ac : root_ac, sc = mem_sc ? mem_sc : root_sc;
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
        return HV_FDT_BADCELLS;
    uint32_t entry = 4 * (ac + sc);
    if (reg_len == 0 || reg_len % entry)
        return HV_FDT_NOMEMORY;

    uint32_t blocks = reg_len / entry, mask = 0;
    uint64_t base = 0, size = 0;
    for (uint32_t b = 0; b < blocks; b++)
    {
        base = rdcells(reg + b * entry, ac);
        size = rdcells(reg + b * entry + 4 * ac, sc);
        if (size == 0)
            return HV_FDT_NOMEMORY;     /* a zero-size block: the firmware did not fill the node in */
        for (uint64_t g = base / GB1; g <= (base + size - 1) / GB1 && g < 32; g++)
            mask |= 1u << g;
    }

    /* base, size: the last block, the one Emu68 takes its memory from */
    uint64_t top = base + size;
    if (bytes == 0 || (bytes & (MB2 - 1)))
        return HV_FDT_UNALIGNED;
    if (size < bytes + MIN_LEFT)
        return HV_FDT_TOOSMALL;
    if (top & (MB2 - 1))
        return HV_FDT_UNALIGNED;
    if (top > MAP_LIMIT)
        return HV_FDT_OUTOFMAP;

    uint64_t r0 = top - bytes, r1 = top, d0 = (uintptr_t)dtb;
    if (overlaps(r0, r1, d0, d0 + total))
        return HV_FDT_OVERLAP;
    if (initrd1 > initrd0 && overlaps(r0, r1, initrd0, initrd1))
        return HV_FDT_OVERLAP;

    /* All checks passed: write the new size of the last block */
    uint8_t *cell = reg + (blocks - 1) * entry + 4 * ac;
    uint64_t left = size - bytes;
    if (sc == 2)
    {
        wr32(cell, (uint32_t)(left >> 32));
        wr32(cell + 4, (uint32_t)left);
    }
    else
    {
        if (left >> 32)
            return HV_FDT_BADCELLS;
        wr32(cell, (uint32_t)left);
    }

    out->base = r0;
    out->gb_mask = mask;
    out->blocks = blocks;
    return HV_FDT_OK;
}

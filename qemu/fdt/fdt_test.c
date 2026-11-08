/* Host test of hv_fdt.c: fdt_test <dtb> <expected status> [bytes] */
#include <stdio.h>
#include <stdlib.h>
#include "hv_fdt.h"

int main(int argc, char **argv)
{
    static uint8_t buf[0x400000], orig[0x400000];
    FILE *f = fopen(argv[1], "rb");
    size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
    struct hv_fdt_result r = {0, 0, 0};
    uint64_t bytes = argc > 3 ? strtoull(argv[3], 0, 0) : 0x200000;

    for (size_t i = 0; i < n; i++)
        orig[i] = buf[i];
    int st = hv_fdt_reserve_top(buf, bytes, &r), changed = 0;
    for (size_t i = 0; i < n; i++)
        changed += buf[i] != orig[i];
    printf("status %d  base 0x%09llx  gb_mask 0x%02x  blocks %u  bytes changed %d\n", st,
           (unsigned long long)r.base, r.gb_mask, r.blocks, changed);
    return st == atoi(argv[2]) && (st != 0) == (changed == 0) ? 0 : 1;
}

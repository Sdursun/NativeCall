/* Reserve the hypervisor's memory from the device tree the firmware hands over. */
#ifndef HV_FDT_H
#define HV_FDT_H

#include <stdint.h>

#define HV_FDT_OK           0
#define HV_FDT_BADHEADER    1   /* not a device tree, or truncated */
#define HV_FDT_NOMEMORY     2   /* no /memory node with a usable reg */
#define HV_FDT_BADCELLS     3   /* #address-cells / #size-cells we do not handle */
#define HV_FDT_TOOSMALL     4   /* last block too small to give anything away */
#define HV_FDT_UNALIGNED    5   /* block top not on a 2 MB boundary: Emu68 needs that */
#define HV_FDT_OUTOFMAP     6   /* beyond what the EL2 map can reach */
#define HV_FDT_OVERLAP      7   /* the region holds the device tree or the initrd */

struct hv_fdt_result
{
    uint64_t base;      /* start of the reserved region */
    uint32_t gb_mask;   /* bit n: GB n of the address space holds reported RAM */
    uint32_t blocks;
};

/* Takes 'bytes' off the top of the last /memory block, in place. Nothing is written unless it
   returns HV_FDT_OK. Reads big-endian data bytewise: works in any data endianness, with the
   MMU off. */
int hv_fdt_reserve_top(void *dtb, uint64_t bytes, struct hv_fdt_result *out);

#endif

# QEMU 9.2.4, for the test only (qemu/README.md): python3 qemu9-patch.py <qemu source dir>
#  1. WFE takes the HCR_EL2.TWE trap as on real cores (see tools/qemu/wfe-trap.py).
#  2. raspi4b reports its RAM like the Pi firmware: one /memory node with two blocks, not a second
#     node. Emu68 reads only the first node, so with two nodes it would never see the upper block.
#  3. initrd properties in 32 bits as the Pi firmware writes them.
#  4. raspi4b's GIC with the Security Extensions, as on the BCM2711 (see the end).
import sys

root = sys.argv[1]


def patch(path, old, new):
    p = root + "/" + path
    s = open(p).read()
    assert s.count(old) == 1, (path, old[:60])
    open(p, "w").write(s.replace(old, new))


patch("target/arm/tcg/op_helper.c",
      """    HELPER(yield)(env);
}

void HELPER(yield)(CPUARMState *env)""",
      """#ifndef CONFIG_USER_ONLY
    uint32_t excp;
    int target_el = check_wfx_trap(env, true, &excp);

    if (target_el && env->aarch64) {
        env->pc -= 4;       /* the translator already advanced it (DISAS_WFE) */
        raise_exception(env, excp, syn_wfx(1, 0xe, 1, false), target_el);
    }
#endif
    HELPER(yield)(env);
}

void HELPER(yield)(CPUARMState *env)""")

patch("hw/arm/raspi4b.c",
      """    if (info->ram_size > UPPER_RAM_BASE) {
        raspi_add_memory_node(fdt, UPPER_RAM_BASE, ram_size - UPPER_RAM_BASE);
    }
""",
      """    if (ram_size > UPPER_RAM_BASE) {
        uint32_t ac = qemu_fdt_getprop_cell(fdt, "/", "#address-cells", NULL, &error_fatal);
        uint32_t sc = qemu_fdt_getprop_cell(fdt, "/", "#size-cells", NULL, &error_fatal);

        qemu_fdt_setprop_sized_cells(fdt, "/memory", "reg",
                                     ac, 0, sc, info->ram_size,
                                     ac, UPPER_RAM_BASE, sc, ram_size - UPPER_RAM_BASE);
    }

    /* 32 bits as the Pi firmware writes them. Emu68 reads them so: with QEMU's 64-bit values it
       sees a start of 0 and copies "the initrd" until it runs off the end of RAM. */
    if (info->initrd_size) {
        qemu_fdt_setprop_cell(fdt, "/chosen", "linux,initrd-start", (uint32_t)info->initrd_start);
        qemu_fdt_setprop_cell(fdt, "/chosen", "linux,initrd-end",
                              (uint32_t)(info->initrd_start + info->initrd_size));
    }
""")
patch("hw/arm/raspi4b.c",
      "static int raspi_add_memory_node(void *fdt, hwaddr mem_base, hwaddr mem_len)",
      "__attribute__((unused)) static int raspi_add_memory_node(void *fdt, hwaddr mem_base, hwaddr mem_len)")

# 4. The BCM2711's GIC-400 has the Security Extensions (QEMU's raspi4b leaves them out). With them
#    the armstub's Secure setup at EL3 (Group 0 = the hypervisor's timer as FIQ, FIQEn) survives
#    Emu68's own Non-secure GICD_CTLR / GICC_CTLR writes, as on the Pi (NOTES.md 26). Without
#    them Emu68's "GICC_CTLR = 1" clears FIQEn and the time limit comes as an IRQ nobody takes.
patch("hw/arm/bcm2838.c",
      """    if (!object_property_set_bool(OBJECT(&s->gic),
                                  "has-virtualization-extensions", true,
                                  errp)) {
        return;
    }
""",
      """    if (!object_property_set_bool(OBJECT(&s->gic),
                                  "has-virtualization-extensions", true,
                                  errp)) {
        return;
    }

    if (!object_property_set_bool(OBJECT(&s->gic),
                                  "has-security-extensions", true,
                                  errp)) {
        return;
    }
""")

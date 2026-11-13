/*
    PiReboot - restarts the Raspberry Pi under Emu68 (PiStorm) from the Amiga side, like a power
    cycle for what matters here: the Pi firmware starts again from the SD card and reads
    config.txt and the armstub (nativecall hypervisor) anew. The Amiga is reset with it.

        PiReboot NOW        restart (without NOW it only says what it would do)
        PiReboot NOW DELAY=<s>   wait that long first (default 2 s: disk buffers settle)

    How: the watchdog of the BCM2711/BCM2837 power management block, as Emu68's own pi_reset()
    (src/pistorm/ps_protocol.c) and Linux's reboot. Emu68 maps the Pi peripherals for the 68k
    at $F2000000 (bus 0x7E000000), little-endian registers. Needs no nativecall library and no
    hypervisor: it also works when the hypervisor is broken, as long as the Amiga still runs.
    It cannot help when the Amiga is frozen: then only a power cycle does.

    Built with VBCC by the SDK's Makefile (build/PiReboot).
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define PERI            0xF2000000UL
#define PM_RSTC         ((volatile ULONG *)(PERI + 0x0010001c))
#define PM_RSTS         ((volatile ULONG *)(PERI + 0x00100020))
#define PM_WDOG         ((volatile ULONG *)(PERI + 0x00100024))
#define PM_MAGIC        0x5a000000UL
#define PM_RSTC_FULLRST 0x00000020UL

#define TEMPLATE "NOW/S,DELAY/N"
enum { ARG_NOW, ARG_DELAY, ARG_COUNT };

static const char version[] = "$VER: PiReboot 1.0 (12.11.2026)";

/* the registers are little-endian, the 68k is big-endian */
static ULONG swap(ULONG v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    ULONG delay = 2, r;

    (void)version;
    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "PiReboot");
        return RETURN_FAIL;
    }
    if (args[ARG_DELAY])
        delay = *(ULONG *)args[ARG_DELAY];
    if (!args[ARG_NOW])
    {
        PutStr("PiReboot NOW restarts the Raspberry Pi (and the Amiga with it): the Pi firmware\n"
               "reads config.txt and the armstub again, as after a power cycle.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }
    FreeArgs(rda);

    PutStr("Restarting the Pi...\n");
    Flush(Output());
    Delay(delay * 50);

    Disable();
    /* boot from partition 0, as Emu68's pi_reset() */
    r = swap(*PM_RSTS) & 0xfffffaaaUL;
    *PM_RSTS = swap(PM_MAGIC | r);
    *PM_WDOG = swap(PM_MAGIC | 10);
    r = (swap(*PM_RSTC) & ~0x30UL) | PM_MAGIC | PM_RSTC_FULLRST;
    *PM_RSTC = swap(r);
    for (;;)
        ;
    return 0;
}

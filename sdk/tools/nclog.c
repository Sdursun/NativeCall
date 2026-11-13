/*
    NCLog - the nativecall hypervisor's log and the last native fault (hypervisor 7, library 1.6).

        NCLog           print what is in the log: nc_printf() of kernels, the hypervisor's lines
                        about faults and time-outs
        NCLog FOLLOW    the same, then keep printing new text until Ctrl-C
        NCLog FAULT     details of the last native fault or time-out: registers and a backtrace
                        of module offsets for "nc addr2line <module>.elf ..." on the PC
        NCLog STATS     what nativecall does right now, measured over one second: native time
                        of each ARM core, temperature, the programs with native modules
                        (hypervisor 8, library 1.7; the window NCStatus shows the same)

    Built with VBCC by the SDK's Makefile (build/NCLog).
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/nativecall.h>

#include <nativecall/nc_abi.h>

struct Library *NativeCallBase;

#define TEMPLATE "FOLLOW/S,FAULT/S,STATS/S"
enum { ARG_FOLLOW, ARG_FAULT, ARG_STATS, ARG_COUNT };

static const char version[] = "$VER: NCLog 1.1 (12.11.2026)";

#define MAX_MODULES 16

static void print_stats(void)
{
    static struct NCModuleInfo m0[MAX_MODULES], m1[MAX_MODULES];
    struct NCStats s0, s1;
    ULONG args[6], i, j, n0, n1, ticks;

    if ((NativeCallBase->lib_Version == 1 && NativeCallBase->lib_Revision < 7) || !NC_GetStats(&s0))
    {
        PutStr("STATS needs nativecall.library 1.7 and hypervisor 8\n");
        return;
    }
    n0 = NC_ListModules(m0, MAX_MODULES);
    Delay(50);
    if (!NC_GetStats(&s1))
        return;
    n1 = NC_ListModules(m1, MAX_MODULES);
    if (n0 > MAX_MODULES) n0 = MAX_MODULES;
    if (n1 > MAX_MODULES) n1 = MAX_MODULES;
    ticks = s1.now_lo - s0.now_lo;
    if (ticks < 100)
        ticks = 100;

    args[0] = s1.hv_version;
    args[1] = NativeCallBase->lib_Version;
    args[2] = NativeCallBase->lib_Revision;
    args[3] = (ULONG)((s1.features & NC_FEAT_TIMEOUT) ? "works" : "not available");
    VPrintf("Hypervisor %lu, nativecall.library %lu.%lu, time limit %s\n", args);
    if (s1.temperature)
    {
        args[0] = s1.temperature / 1000;
        args[1] = s1.temperature % 1000 / 100;
        VPrintf("Temperature %lu.%lu C\n", args);
    }
    for (i = 0; i < 4; i++)
    {
        args[0] = i;
        if (!(s1.core[i].flags & NC_CORE_NATIVE))
        {
            VPrintf("Core %lu: used by Emu68\n", args);
            continue;
        }
        args[1] = (s1.core[i].busy_lo - s0.core[i].busy_lo) / (ticks / 100);
        args[2] = s1.core[i].calls - s0.core[i].calls;
        args[3] = (ULONG)(i == 0 ? " (68k)" : " (helper)");
        VPrintf("Core %lu: %3lu %% native, %lu calls in the last second%s\n", args);
    }
    args[0] = s1.jobs;
    args[1] = s1.faults;
    VPrintf("Jobs running: %lu, faults since the Pi started: %lu\n", args);

    if (n1 == 0)
        PutStr("No program has a native module loaded\n");
    for (i = 0; i < n1; i++)
    {
        ULONG calls = 0, us = 0;

        for (j = 0; j < n0; j++)
            if (m0[j].module == m1[i].module)
            {
                calls = m1[i].calls - m0[j].calls;
                us = m1[i].total_us - m0[j].total_us;
            }
        args[0] = (ULONG)(m1[i].owner[0] ? m1[i].owner : "(unknown)");
        args[1] = m1[i].kernels;
        args[2] = calls;
        args[3] = us / 1000;
        args[4] = m1[i].jobs;
        args[5] = m1[i].faults;
        VPrintf("%-20s %lu kernels, %lu calls and %lu ms of ARM time in the last second, %lu jobs, %lu faults\n", args);
    }
}

static void print_fault(void)
{
    struct NCFaultInfo fi;
    ULONG i, args[4];

    if (!NC_GetFaultInfo(&fi))
    {
        PutStr("No native fault since the Pi started (or no hypervisor 7)\n");
        return;
    }
    args[0] = (ULONG)(fi.status == NC_ST_TIMEOUT ? "Time-out" : "Fault");
    args[1] = fi.core;
    args[2] = fi.module;
    args[3] = fi.count;
    VPrintf("%s on ARM core %lu, module at 0x%08lx (number %lu since the Pi started)\n", args);
    args[0] = fi.esr;
    args[1] = fi.far_hi;
    args[2] = fi.far_lo;
    VPrintf("ESR %08lx  FAR %08lx%08lx\n", args);
    args[0] = fi.elr_hi;
    args[1] = fi.elr_lo;
    args[2] = fi.sp_hi;
    args[3] = fi.sp_lo;
    VPrintf("ELR %08lx%08lx  SP %08lx%08lx\n", args);
    for (i = 0; i < 31; i++)
    {
        args[0] = i;
        args[1] = fi.x[i][0];
        args[2] = fi.x[i][1];
        VPrintf(i < 10 ? "x%ld  %08lx%08lx" : "x%ld %08lx%08lx", args);
        PutStr((i & 3) == 3 || i == 30 ? "\n" : "  ");
    }
    PutStr("Backtrace (module offsets; the first is where it happened):\n  nc addr2line <module>.elf");
    for (i = 0; i < NC_FAULT_FRAMES; i++)
    {
        if (fi.backtrace[i] == NC_FAULT_NOOFFSET)
            continue;
        args[0] = fi.backtrace[i];
        VPrintf(" 0x%lx", args);
    }
    PutStr("\n");
}

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    UBYTE buf[1024];
    ULONG pos = 0, n;
    int rc = RETURN_OK;

    (void)version;
    rda = ReadArgs(TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "NCLog");
        return RETURN_FAIL;
    }
    NativeCallBase = OpenLibrary("nativecall.library", 1);
    if (!NativeCallBase || (NativeCallBase->lib_Version == 1 && NativeCallBase->lib_Revision < 6))
    {
        PutStr("NCLog needs nativecall.library 1.6\n");
        rc = RETURN_FAIL;
        goto out;
    }
    if (!NC_Available())
    {
        PutStr("Native calls are not available on this machine\n");
        rc = RETURN_WARN;
        goto out;
    }

    if (args[ARG_FAULT])
    {
        print_fault();
        goto out;
    }
    if (args[ARG_STATS])
    {
        print_stats();
        goto out;
    }

    for (;;)
    {
        n = NC_ReadLog(buf, sizeof(buf), &pos);
        if (n)
        {
            Flush(Output());
            Write(Output(), buf, n);
            continue;
        }
        if (!args[ARG_FOLLOW])
            break;
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
        Delay(10);
    }

out:
    Flush(Output());        /* VPrintf/PutStr are buffered: out before the next command's output */
    if (NativeCallBase)
        CloseLibrary(NativeCallBase);
    FreeArgs(rda);
    return rc;
}

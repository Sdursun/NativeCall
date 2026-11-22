/*
    NCStatus - a window that shows what nativecall does right now (MUI 3.8+).

      - is the hypervisor there, which version, does the time limit work
      - how much of its time each ARM core spends in native code, and the SoC temperature
        (hypervisor 8, library 1.7)
      - the programs that have native modules loaded: calls per second, native time, jobs, faults
      - the hypervisor's log (nc_printf of kernels, faults, time-outs)

    Runs without nativecall.library or without the hypervisor too, and says so.
    Built with GCC by the SDK's Makefile (build/NCStatus); start it from Workbench or the Shell.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <libraries/gadtools.h>
#include <libraries/iffparse.h>
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
/* Not the MUI SDK's inline MUI_NewObject(): it passes the address of its first tag as the tag
   list, and once GCC inlines it the other tags are not behind that address any more (GCC 16:
   objects made of garbage, the machine went down). MUI_NewObject() below copies the tags. */
#define NO_INLINE_STDARG
#include <proto/muimaster.h>
#undef NO_INLINE_STDARG
#include <proto/nativecall.h>
#include <clib/alib_protos.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <nativecall/nc_abi.h>

struct Library *MUIMasterBase;
struct Library *NativeCallBase;

#define MAX_TAGS 40

Object *MUI_NewObject(CONST_STRPTR cl, Tag tag1, ...)
{
    ULONG tags[2 * MAX_TAGS + 2];
    Tag tag = tag1;
    va_list ap;
    int n = 0;

    va_start(ap, tag1);
    while (tag != TAG_DONE && n < 2 * MAX_TAGS)
    {
        tags[n++] = tag;
        tags[n++] = va_arg(ap, ULONG);
        tag = va_arg(ap, Tag);
    }
    va_end(ap);
    tags[n] = TAG_DONE;
    tags[n + 1] = 0;
    return MUI_NewObjectA(cl, (struct TagItem *)tags);
}

static const char version[] = "$VER: NCStatus 1.0 (12.11.2026)";

/* MUI needs more than the 4 KB a Shell gives by default: libnix swaps the stack at startup */
unsigned long __stack = 65536;

#define MAX_MODULES 16
#define LOG_LINES   300
#define PERIOD_MS   1000
#define LABEL_WIDTH "Core 0 (helper) "

struct Row
{
    char name[34], kernels[8], calls[14], load[12], jobs[6], faults[10];
};

static Object *app, *win, *txt_state, *txt_detail, *txt_totals, *gauge_core[4], *label_core[4],
              *gauge_temp, *list_mod, *list_log, *btn_fault, *btn_clear;
static char core_text[4][40], temp_text[40];

static struct NCStats prev_stats;
static BOOL have_prev;
static struct NCModuleInfo mods[MAX_MODULES], prev_mods[MAX_MODULES];
static ULONG prev_count;
static struct Row rows[MAX_MODULES];
static ULONG shown_rows;
static ULONG log_pos;
static char log_line[256];
static ULONG log_len;

static BOOL lib_has(UWORD revision)
{
    return NativeCallBase && (NativeCallBase->lib_Version > 1 || NativeCallBase->lib_Revision >= revision);
}

static void jump_to_end(Object *list)
{
    LONG entries = 0;

    get(list, MUIA_List_Entries, &entries);
    if (entries > 0)
        DoMethod(list, MUIM_List_Jump, entries - 1);
}

/* ---- module list ---- */

static LONG display_fn(register struct Hook *h __asm("a0"), register char **col __asm("a2"),
                       register struct Row *r __asm("a1"))
{
    (void)h;
    if (r)
    {
        col[0] = r->name;
        col[1] = r->kernels;
        col[2] = r->calls;
        col[3] = r->load;
        col[4] = r->jobs;
        col[5] = r->faults;
    }
    else
    {
        col[0] = "\33bProgram";
        col[1] = "\33bKernels";
        col[2] = "\33bCalls/s";
        col[3] = "\33bARM time";
        col[4] = "\33bJobs";
        col[5] = "\33bFaults";
    }
    return 0;
}

static struct Hook display_hook = { { NULL, NULL }, (HOOKFUNC)display_fn, NULL, NULL };

static void update_modules(ULONG elapsed_us)
{
    ULONG count, i, j;

    if (!lib_has(7))
        return;
    count = NC_ListModules(mods, MAX_MODULES);
    if (count > MAX_MODULES)
        count = MAX_MODULES;

    for (i = 0; i < count; i++)
    {
        struct NCModuleInfo *m = &mods[i], *p = NULL;
        struct Row *r = &rows[i];

        for (j = 0; j < prev_count; j++)
            if (prev_mods[j].module == m->module)
                p = &prev_mods[j];

        memset(r, 0, sizeof(*r));
        strncpy(r->name, m->owner[0] ? m->owner : "(unknown)", sizeof(r->name) - 1);
        sprintf(r->kernels, "%lu", (unsigned long)m->kernels);
        if (p && elapsed_us)
        {
            /* per second; ARM time as a share of one core's time */
            ULONG per_s = (ULONG)((unsigned long long)(m->calls - p->calls) * 1000000ULL / elapsed_us);
            ULONG tenth = (ULONG)((unsigned long long)(m->total_us - p->total_us) * 1000ULL / elapsed_us);

            sprintf(r->calls, "%lu", (unsigned long)per_s);
            sprintf(r->load, "%lu.%lu %%", (unsigned long)(tenth / 10), (unsigned long)(tenth % 10));
        }
        else
        {
            strcpy(r->calls, "-");
            strcpy(r->load, "-");
        }
        sprintf(r->jobs, "%lu", (unsigned long)m->jobs);
        sprintf(r->faults, "%lu", (unsigned long)m->faults);
    }
    memcpy(prev_mods, mods, sizeof(mods));
    prev_count = count;

    if (count != shown_rows)
    {
        set(list_mod, MUIA_List_Quiet, TRUE);
        DoMethod(list_mod, MUIM_List_Clear);
        for (i = 0; i < count; i++)
            DoMethod(list_mod, MUIM_List_InsertSingle, (ULONG)&rows[i], MUIV_List_Insert_Bottom);
        set(list_mod, MUIA_List_Quiet, FALSE);
        shown_rows = count;
    }
    else if (count)
        DoMethod(list_mod, MUIM_List_Redraw, MUIV_List_Redraw_All);
}

/* ---- log ---- */

static void log_add(const char *line)
{
    LONG entries = 0;

    get(list_log, MUIA_List_Entries, &entries);
    if (entries >= LOG_LINES)
        DoMethod(list_log, MUIM_List_Remove, MUIV_List_Remove_First);
    DoMethod(list_log, MUIM_List_InsertSingle, (ULONG)line, MUIV_List_Insert_Bottom);
}

static void update_log(void)
{
    static char buf[1024];
    ULONG n, i;
    BOOL added = FALSE;

    if (!lib_has(6))
        return;
    while ((n = NC_ReadLog(buf, sizeof(buf), &log_pos)) > 0)
    {
        if (!added)
            set(list_log, MUIA_List_Quiet, TRUE);
        added = TRUE;
        for (i = 0; i < n; i++)
        {
            char c = buf[i];

            if (c == '\n' || log_len == sizeof(log_line) - 1)
            {
                log_line[log_len] = 0;
                log_add(log_line);
                log_len = 0;
                if (c == '\n')
                    continue;
            }
            if (c != '\r')
                log_line[log_len++] = (c == 27 || c == '\t') ? ' ' : c;
        }
    }
    if (added)
    {
        set(list_log, MUIA_List_Quiet, FALSE);
        jump_to_end(list_log);
    }
}

static void show_fault(void)
{
    struct NCFaultInfo fi;
    char line[200];
    int i, n;

    if (!lib_has(6) || !NC_GetFaultInfo(&fi))
        log_add("No native fault since the Pi started.");
    else
    {
        sprintf(line, "\33b%s on ARM core %lu, module at 0x%08lx (number %lu since the Pi started)",
                fi.status == NC_ST_TIMEOUT ? "Time-out" : "Fault", (unsigned long)fi.core,
                (unsigned long)fi.module, (unsigned long)fi.count);
        log_add(line);
        sprintf(line, "  ESR %08lx  FAR %08lx%08lx  ELR %08lx%08lx", (unsigned long)fi.esr,
                (unsigned long)fi.far_hi, (unsigned long)fi.far_lo, (unsigned long)fi.elr_hi,
                (unsigned long)fi.elr_lo);
        log_add(line);
        n = sprintf(line, "  nc addr2line <module>.elf");
        for (i = 0; i < NC_FAULT_FRAMES && fi.backtrace[i] != NC_FAULT_NOOFFSET; i++)
            n += sprintf(line + n, " %lx", (unsigned long)fi.backtrace[i]);
        log_add(line);
    }
    jump_to_end(list_log);
}

/* ---- cores, temperature, state ---- */

/* Emu68's tools keep the temperature in ENV: too (older hypervisors do not report it) */
static ULONG env_temperature(void)
{
    char buf[16];
    LONG len = GetVar((STRPTR)"Emu68/Temperature", (STRPTR)buf, sizeof(buf) - 1, GVF_GLOBAL_ONLY);
    ULONG whole = 0, frac = 0;
    int i = 0;

    if (len <= 0)
        return 0;
    buf[len] = 0;
    while (buf[i] >= '0' && buf[i] <= '9')
        whole = whole * 10 + (ULONG)(buf[i++] - '0');
    if (buf[i] == '.' && buf[i + 1] >= '0' && buf[i + 1] <= '9')
        frac = (ULONG)(buf[i + 1] - '0');
    return whole * 1000 + frac * 100;
}

static void show_temperature(ULONG milli)
{
    if (milli)
    {
        sprintf(temp_text, "%lu.%lu \260C", (unsigned long)(milli / 1000), (unsigned long)(milli % 1000 / 100));
        set(gauge_temp, MUIA_Gauge_Current, milli / 1000 > 100 ? 100 : milli / 1000);
    }
    else
    {
        strcpy(temp_text, "not known");
        set(gauge_temp, MUIA_Gauge_Current, 0);
    }
    set(gauge_temp, MUIA_Gauge_InfoText, (ULONG)temp_text);
}

static void idle_cores(const char *text)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        set(gauge_core[i], MUIA_Gauge_Current, 0);
        set(gauge_core[i], MUIA_Gauge_InfoText, (ULONG)text);
    }
}

static const char *core_name(int i, ULONG flags)
{
    static const char *plain[4] = { "Core 0 (68k)", "Core 1", "Core 2", "Core 3" };
    static const char *helper[4] = { "Core 0 (68k)", "Core 1 (helper)", "Core 2 (helper)", "Core 3 (helper)" };

    return (flags & NC_CORE_HELPER) ? helper[i] : plain[i];
}

/* WinUAE x64: "core" 0 is the emulation thread, 1 the PC's worker threads together */
static const char *x64_core_name(int i)
{
    static const char *names[4] = { "Emulation", "PC threads", "-", "-" };

    return names[i];
}

static void update(void)
{
    static char state[160], detail[200], totals[200];
    struct NCStats s;
    ULONG elapsed_us = 0;
    int i;

    if (!NativeCallBase)
        NativeCallBase = OpenLibrary((STRPTR)"nativecall.library", 1);
    if (!NativeCallBase)
    {
        set(txt_state, MUIA_Text_Contents, (ULONG)"\33bNot installed:\33n nativecall.library is not in LIBS:");
        set(txt_detail, MUIA_Text_Contents, (ULONG)"");
        idle_cores("");
        show_temperature(env_temperature());
        return;
    }
    if (!NC_Available())
    {
        sprintf(state, "\33bNot active:\33n no hypervisor answers (library %lu.%lu)",
                (unsigned long)NativeCallBase->lib_Version, (unsigned long)NativeCallBase->lib_Revision);
        set(txt_state, MUIA_Text_Contents, (ULONG)state);
        set(txt_detail, MUIA_Text_Contents,
            (ULONG)"The nativecall armstub is not loaded (config.txt: armstub=nativecall-armstub.bin).");
        idle_cores("");
        show_temperature(env_temperature());
        return;
    }

    memset(&s, 0, sizeof(s));
    if (lib_has(7) && NC_GetStats(&s) && s.freq)
    {
        ULONG helpers = 0, calls = 0, dcalls = 0, dnow = s.now_lo - prev_stats.now_lo;

        if (have_prev && dnow)
            elapsed_us = (ULONG)((unsigned long long)dnow * 1000000ULL / s.freq);
        for (i = 0; i < 4; i++)
        {
            ULONG percent = 0;

            if (elapsed_us)
            {
                percent = (ULONG)((unsigned long long)(s.core[i].busy_lo - prev_stats.core[i].busy_lo) * 100ULL / dnow);
                dcalls += s.core[i].calls - prev_stats.core[i].calls;
            }
            if (percent > 100)
                percent = 100;
            calls += s.core[i].calls;
            if (s.core[i].flags & NC_CORE_HELPER)
                helpers++;
            if (s.core[i].flags & NC_CORE_NATIVE)
                sprintf(core_text[i], "%lu %%", (unsigned long)percent);
            else
                strcpy(core_text[i], (s.features & NC_FEAT_X64) ? "-" : "used by Emu68");
            set(gauge_core[i], MUIA_Gauge_Current, percent);
            set(gauge_core[i], MUIA_Gauge_InfoText, (ULONG)core_text[i]);
            set(label_core[i], MUIA_Text_Contents,
                (ULONG)((s.features & NC_FEAT_X64) ? x64_core_name(i) : core_name(i, s.core[i].flags)));
        }
        if (s.features & NC_FEAT_X64)
        {
            /* WinUAE: the kernels run on the PC (library 1.8, sdk/x64) */
            sprintf(state, "\33bActive:\33n WinUAE x64 (kernels on the PC), nativecall.library %lu.%lu",
                    (unsigned long)NativeCallBase->lib_Version, (unsigned long)NativeCallBase->lib_Revision);
            sprintf(detail, "%lu PC threads run native code. Time limit: %s.", (unsigned long)NC_Cores(),
                    (s.features & NC_FEAT_TIMEOUT) ? "works" : "not available");
        }
        else
        {
            sprintf(state, "\33bActive:\33n hypervisor %lu, nativecall.library %lu.%lu", (unsigned long)s.hv_version,
                    (unsigned long)NativeCallBase->lib_Version, (unsigned long)NativeCallBase->lib_Revision);
            sprintf(detail, "%lu ARM cores run native code (%lu helper%s). Time limit: %s.",
                    (unsigned long)(1 + helpers), (unsigned long)helpers, helpers == 1 ? "" : "s",
                    (s.features & NC_FEAT_TIMEOUT) ? "works" : "not available");
        }
        sprintf(totals, "Native calls: %lu per second, %lu since the Pi started. Jobs running: %lu. Faults: %lu.",
                (unsigned long)(elapsed_us ? (ULONG)((unsigned long long)dcalls * 1000000ULL / elapsed_us) : 0),
                (unsigned long)calls, (unsigned long)s.jobs, (unsigned long)s.faults);
        set(txt_totals, MUIA_Text_Contents, (ULONG)totals);
        show_temperature(s.temperature ? s.temperature : env_temperature());
        prev_stats = s;
        have_prev = TRUE;
    }
    else
    {
        ULONG cores = NC_Cores();

        sprintf(state, "\33bActive:\33n nativecall.library %lu.%lu, %lu ARM core%s for native code",
                (unsigned long)NativeCallBase->lib_Version, (unsigned long)NativeCallBase->lib_Revision, (unsigned long)cores,
                cores == 1 ? "" : "s");
        strcpy(detail, "The load of the cores needs hypervisor 8 and nativecall.library 1.7.");
        idle_cores("not measured");
        show_temperature(env_temperature());
    }
    set(txt_state, MUIA_Text_Contents, (ULONG)state);
    set(txt_detail, MUIA_Text_Contents, (ULONG)detail);

    update_modules(elapsed_us);
    update_log();
}

/* ---- window ---- */

enum { ID_FAULT = 1, ID_CLEAR, ID_ABOUT };

static Object *button(const char *label)
{
    ULONG params[1];

    params[0] = (ULONG)label;
    return MUI_MakeObjectA(MUIO_Button, params);
}

static Object *core_row(int i)
{
    label_core[i] = TextObject, MUIA_Text_Contents, (ULONG)core_name(i, 0), MUIA_Text_SetMin, FALSE,
                    MUIA_FixWidthTxt, (ULONG)LABEL_WIDTH, End;
    gauge_core[i] = GaugeObject, GaugeFrame, MUIA_Gauge_Horiz, TRUE, MUIA_Gauge_Max, 100,
                    MUIA_Gauge_InfoText, (ULONG)"", End;
    return HGroup, Child, label_core[i], Child, gauge_core[i], End;
}

static BOOL build(void)
{
    Object *menu, *row[4];
    int i;

    for (i = 0; i < 4; i++)
        row[i] = core_row(i);

    menu = MenustripObject,
        Child, MenuObject, MUIA_Menu_Title, (ULONG)"Project",
            Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"About...", MUIA_Menuitem_Shortcut, (ULONG)"?",
                   MUIA_UserData, ID_ABOUT, End,
            Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)NM_BARLABEL, End,
            Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Quit", MUIA_Menuitem_Shortcut, (ULONG)"Q",
                   MUIA_UserData, MUIV_Application_ReturnID_Quit, End,
            End,
        End;

    app = ApplicationObject,
        MUIA_Application_Title, (ULONG)"NCStatus",
        MUIA_Application_Version, (ULONG)version,
        MUIA_Application_Copyright, (ULONG)"NativeCall",
        MUIA_Application_Description, (ULONG)"Shows what nativecall does",
        MUIA_Application_Base, (ULONG)"NCSTATUS",
        MUIA_Application_Menustrip, (ULONG)menu,
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (ULONG)"NativeCall status",
            MUIA_Window_ID, MAKE_ID('N', 'C', 'S', 'T'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(36),
            MUIA_Window_Height, MUIV_Window_Height_Screen(62),
            WindowContents, VGroup,
                Child, VGroup, GroupFrameT("Hypervisor"),
                    Child, txt_state = TextObject, MUIA_Text_Contents, (ULONG)"", End,
                    Child, txt_detail = TextObject, MUIA_Text_Contents, (ULONG)"", End,
                    End,
                Child, VGroup, GroupFrameT("ARM cores: share of time in native code"),
                    Child, row[0],
                    Child, row[1],
                    Child, row[2],
                    Child, row[3],
                    Child, HGroup,
                        Child, TextObject, MUIA_Text_Contents, (ULONG)"Temperature", MUIA_Text_SetMin, FALSE,
                               MUIA_FixWidthTxt, (ULONG)LABEL_WIDTH, End,
                        Child, gauge_temp = GaugeObject, GaugeFrame, MUIA_Gauge_Horiz, TRUE, MUIA_Gauge_Max, 100,
                               MUIA_Gauge_InfoText, (ULONG)"", End,
                        End,
                    Child, txt_totals = TextObject, MUIA_Text_Contents, (ULONG)"", End,
                    End,
                Child, VGroup, GroupFrameT("Programs with native modules"),
                    Child, ListviewObject, MUIA_Listview_Input, FALSE, MUIA_Weight, 60,
                        MUIA_Listview_List, list_mod = ListObject, ReadListFrame,
                            MUIA_List_Format, (ULONG)"BAR,P=\33r BAR,P=\33r BAR,P=\33r BAR,P=\33r BAR,P=\33r",
                            MUIA_List_Title, TRUE,
                            MUIA_List_DisplayHook, (ULONG)&display_hook,
                            End,
                        End,
                    End,
                Child, VGroup, GroupFrameT("Log of the hypervisor"),
                    Child, ListviewObject, MUIA_Listview_Input, FALSE,
                        MUIA_Listview_List, list_log = ListObject, ReadListFrame,
                            MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                            MUIA_List_DestructHook, MUIV_List_DestructHook_String,
                            End,
                        End,
                    Child, HGroup,
                        Child, btn_fault = button("Show last _fault"),
                        Child, btn_clear = button("_Clear"),
                        End,
                    End,
                End,
            End,
        End;
    if (!app)
        return FALSE;

    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (ULONG)app, 2, MUIM_Application_ReturnID,
             MUIV_Application_ReturnID_Quit);
    DoMethod(btn_fault, MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID, ID_FAULT);
    DoMethod(btn_clear, MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID, ID_CLEAR);
    return TRUE;
}

static void start_timer(struct timerequest *tr)
{
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = PERIOD_MS / 1000;
    tr->tr_time.tv_micro = (PERIOD_MS % 1000) * 1000;
    SendIO((struct IORequest *)tr);
}

int main(void)
{
    struct MsgPort *port = NULL;
    struct timerequest *tr = NULL;
    BOOL timer_open = FALSE, pending = FALSE;
    int rc = 20;

    MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, 19);
    if (!MUIMasterBase)
    {
        printf("NCStatus needs MUI (muimaster.library 19 or newer).\n");
        return 20;
    }
    port = CreateMsgPort();
    tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;
    if (tr && OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)tr, 0) == 0)
        timer_open = TRUE;

    if (timer_open && build())
    {
        ULONG sigs = 0, timer_sig = 1UL << port->mp_SigBit;
        BOOL running = TRUE;

        update();
        set(win, MUIA_Window_Open, TRUE);
        {
            LONG open = 0;

            get(win, MUIA_Window_Open, &open);
            if (!open)
                printf("NCStatus: the window did not open.\n");
        }
        start_timer(tr);
        pending = TRUE;

        while (running)
        {
            ULONG id = DoMethod(app, MUIM_Application_NewInput, (ULONG)&sigs);

            switch (id)
            {
                case MUIV_Application_ReturnID_Quit:
                    running = FALSE;
                    break;
                case ID_FAULT:
                    show_fault();
                    break;
                case ID_CLEAR:
                    DoMethod(list_log, MUIM_List_Clear);
                    break;
                case ID_ABOUT:
                    MUI_RequestA(app, win, 0, (STRPTR)"NCStatus", (STRPTR)"*_OK",
                                (STRPTR)"\33c\33bNCStatus 1.0\33n\n\nShows what nativecall does:\n"
                                "native AArch64 code for 68k programs\non Emu68 (PiStorm).", NULL);
                    break;
            }
            if (running && sigs)
            {
                sigs = Wait(sigs | SIGBREAKF_CTRL_C | timer_sig);
                if (sigs & SIGBREAKF_CTRL_C)
                    running = FALSE;
                if (sigs & timer_sig)
                {
                    WaitIO((struct IORequest *)tr);
                    update();
                    start_timer(tr);
                }
            }
        }
        set(win, MUIA_Window_Open, FALSE);
        rc = 0;
    }

    if (pending)
    {
        AbortIO((struct IORequest *)tr);
        WaitIO((struct IORequest *)tr);
    }
    if (app)
        MUI_DisposeObject(app);
    if (timer_open)
        CloseDevice((struct IORequest *)tr);
    if (tr)
        DeleteIORequest((struct IORequest *)tr);
    if (port)
        DeleteMsgPort(port);
    if (NativeCallBase)
        CloseLibrary(NativeCallBase);
    CloseLibrary(MUIMasterBase);
    return rc;
}

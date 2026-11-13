/*
    bigstack.c - run a function in a process of its own with a stack of a given size, and wait
    for it. Included by the programs that need it (one compilation unit).

    A Shell gives a program 4 KB of stack unless told otherwise. The 68k versions of kernels
    run on the stack of whoever calls them, and with printf and graphics.library on top that is
    soon too little; the MP3 decoder alone wants 16 KB. (Not the compilers' "__stack" variable:
    with VBCC's startup code the demos then corrupted memory.)

    The function may use the C library and the program's output; the caller waits meanwhile.
*/

#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

static int (*bigstack_body)(void);
static struct Task *bigstack_parent;
static ULONG bigstack_done;         /* signal mask */
static int bigstack_rc;

static void bigstack_entry(void)
{
    bigstack_rc = bigstack_body();
    Forbid();                       /* until this process is gone: the program may end after the signal */
    Signal(bigstack_parent, bigstack_done);
}

/* Returns what body() returned. If no process can be made, body() runs on the caller's stack. */
static int run_with_stack(int (*body)(void), ULONG stack, const char *name)
{
    LONG sig = AllocSignal(-1);
    struct Process *proc;

    if (sig < 0)
        return body();
    bigstack_body = body;
    bigstack_parent = FindTask(NULL);
    bigstack_done = 1UL << sig;
    fflush(stdout);
    proc = CreateNewProcTags(NP_Entry, (ULONG)bigstack_entry, NP_Name, (ULONG)name,
                             NP_StackSize, stack, NP_Priority, (LONG)bigstack_parent->tc_Node.ln_Pri,
                             NP_Input, (ULONG)Input(), NP_CloseInput, FALSE,
                             NP_Output, (ULONG)Output(), NP_CloseOutput, FALSE, TAG_END);
    if (proc)
    {
        /* Ctrl-C and "Break" reach this process, not the one that does the work: pass it on */
        while (!(Wait(bigstack_done | SIGBREAKF_CTRL_C) & bigstack_done))
        {
            Forbid();
            if (!(SetSignal(0, 0) & bigstack_done))     /* it is still there */
                Signal((struct Task *)proc, SIGBREAKF_CTRL_C);
            Permit();
        }
    }
    else
        bigstack_rc = body();
    FreeSignal(sig);
    return bigstack_rc;
}

# QEMU 8.2.2: let WFE take the HCR_EL2.TWE trap like real cores (upstream treats WFE as a
# YIELD and never traps it). Needed to test the nativecall helper cores (NOTES.md 22).
#
# Run QEMU with -accel tcg,thread=single: multi-threaded TCG does not call the WFE helper at
# all (and making it do so trips an assertion in cpu-exec.c). Single-threaded TCG runs the
# vCPUs in turns of up to 100 ms, so the helpers take pieces only of jobs longer than that.
#
#   python3 wfe-trap.py <qemu source dir>
import sys

p = sys.argv[1] + "/target/arm/tcg/op_helper.c"
s = open(p).read()
old = """    HELPER(yield)(env);
}

void HELPER(yield)(CPUARMState *env)"""
new = """#ifndef CONFIG_USER_ONLY
    int target_el = check_wfx_trap(env, true);

    if (target_el && env->aarch64) {
        env->pc -= 4;       /* the translator already advanced it (DISAS_WFE) */
        raise_exception(env, EXCP_UDEF, syn_wfx(1, 0xe, 1, false), target_el);
    }
#endif
    HELPER(yield)(env);
}

void HELPER(yield)(CPUARMState *env)"""
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))

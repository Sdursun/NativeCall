#!/usr/bin/env python3
"""Runs every QEMU scenario and checks the results. Usage: python3 test_all.py [scenario ...]
A scenario is mode or mode-machine, for example stub, hv-raspi4b."""
import os
import re
import subprocess
import sys

os.chdir(os.path.dirname(os.path.abspath(__file__)))

# Where the hypervisor must have moved Emu68's top of RAM to; stock is 2 MB higher. The last field:
# the time limit works (only with the real armstub: loader.S has no Secure GIC setup, NOTES.md 26).
SCENARIOS = {
    'base-raspi3b':       ('base', 'raspi3b', False, '34800000', False),
    'hv-raspi3b':         ('hv', 'raspi3b', True, '34600000', False),
    'refuse-raspi3b':     ('refuse', 'raspi3b', False, '34800000', False),
    'base-raspi4b':       ('base', 'raspi4b', False, '79e00000', False),
    'hv-raspi4b':         ('hv', 'raspi4b', True, '79c00000', False),
    'refuse-raspi4b':     ('refuse', 'raspi4b', False, '79e00000', False),
    'stub-raspi4b':       ('stub', 'raspi4b', True, '79c00000', True),
    'stubrefuse-raspi4b': ('stubrefuse', 'raspi4b', False, '79e00000', False),
}

WITH_HV = [
    (r'SPOOF\s+result=4e43414c status=00000000', 'spoofed block left alone'),
    (r'PING\s+result=4e41524d status=00000001', 'ping'),
    (r'SYNC\s+result=ffffffff status=00000001', 'sync'),
    (r'ADD\s+result=0000002a status=00000001', 'native add'),
    (r'FAULT\s+result=ffffffff status=00000084', 'fault caught'),
    (r'BADENTRY\s+result=ffffffff status=00000082', 'bad entry'),
    (r'LOOP1000 errors=00000000', '1000 calls'),
    (r'HELPERS\s+cores=0000000a', 'cores 1 and 3 captured'),
    (r'RUNMP\s+result=00001000 status=00000001', 'shared job'),
    (r'RUNMPFLT\s+result=ffffffff status=00000084', 'fault in shared job'),
    (r'RUNMP200 errors=00000000', '200 shared jobs'),
    (r'ASTART\s+result=00000000 status=00000001', 'async start'),
    (r'AWAIT\s+result=00001000 status=00000001\s+cores=0000000a', 'async result, helpers only'),
    (r'ABUSY\s+result=ffffffff status=00000085', 'slots full'),
    (r'AWAIT4\s+result=00000040 status=00000001', 'four jobs'),
    (r'AFAULT\s+result=ffffffff status=00000084', 'fault in async job'),
    (r'ABADJOB\s+result=ffffffff status=00000086', 'no such job'),
    # hypervisor 7: log and fault details
    (r'LOGCALL\s+result=00000001 status=00000001', 'nc_printf kernel'),
    (r'logtest: core 0, text, -42, 0x00c0ffee, \[    7\] \[3   \] z%', 'nc_printf text in the log'),
    (r'nc: fault on core 0, ESR 96000006, FAR 0000000000000004', 'fault line in the log'),
    (r'DEEPFLT\s+result=ffffffff status=00000084', 'deep fault caught'),
    (r'nc: fault on core 0, ESR 96000006, FAR 0000000000000008', 'deep fault line in the log'),
    (r'FINFO\s+result=00000150 status=00000001\s+status=00000084 far=00000008 x0=00000008', 'fault details'),
    (r'backtrace:( (?!ffffffff)[0-9a-f]{8}){4}', 'backtrace: where, LR and two frames in the module'),
    # hypervisor 7: memory protection
    (r'nc: native code sees the RAM of the 68k only: \d+ MB, [1-9]\d* MB hidden', 'native map restricted'),
    (r'PROTECT0 result=ffffffff status=00000084', 'hypervisor code is read-only'),
    (r'PROTECT1 result=ffffffff status=00000084', 'private memory of Emu68 is hidden'),
    (r'PROTECT2 result=ffffffff status=00000084', 'stack overflow hits the guard page'),
    (r'ESR 9600004f', 'permission fault for the code write'),
    (r'PROTADD\s+result=0000002a status=00000001', 'calls work after the protection faults'),
    # hypervisor 8: statistics
    (r'STATS\s+result=00000068 status=00000001\s+freq=(?!00000000)[0-9a-f]{8}', 'statistics'),
    (r'STCORE busy/calls/flags: (?!00000000)[0-9a-f]{8} (?!00000000)[0-9a-f]{8} 00000001', 'core 0 counted its native time'),
    (r'(STCORE busy/calls/flags: (?!00000000)[0-9a-f]{8} (?!00000000)[0-9a-f]{8} 00000003)', 'a helper core counted its native time'),
    # hypervisor 7: cancel
    (r'ACANCEL\s+result=00000000 status=00000001', 'cancel accepted'),
    (r'ACWAIT\s+result=[0-9a-f]{8} status=00000088\s+units per core 0-3:[ 0-9a-f]+missing=(?!00000000)', 'cancelled job: not all units ran'),
    (r'ACBADJOB\s+result=ffffffff status=00000086', 'cancel of no job'),
    (r'ACMARK\s+result=00000100 status=00000001', 'jobs work after a cancel'),
]
TIME_LIMIT = [
    (r'FEATURES 00000001', 'time limit works (self-test)'),
    (r'TIMEOUT\s+result=ffffffff status=00000087', 'endless call stopped'),
    (r'TMOADD\s+result=0000002a status=00000001', 'calls work after a time-out'),
    (r'TMORUNMP\s+result=ffffffff status=00000087', 'endless shared job stopped'),
    (r'TMOASYNC\s+result=ffffffff status=00000087\s+job status block=00000087', 'endless async job stopped'),
    (r'TMOMARK\s+result=00000100 status=00000001', 'shared jobs work after a time-out'),
    (r'nc: time-out on core 0, module', 'time-out line in the log'),
]
NO_TIME_LIMIT = [
    (r'FEATURES 00000000', 'no time limit reported'),
    (r'TIMEOUT\s+skipped', 'time limit tests skipped'),
]
NO_HV = [
    (r'SPOOF\s+result=4e43414c status=00000000', 'spoofed block left alone'),
    (r'PING\s+result=ffffffff status=00000000', 'no hypervisor answers'),
    (r'ADD\s+result=ffffffff status=00000000', 'no native call'),
    (r'HELPERS\s+cores=00000000', 'no helper cores'),
    (r'ASTART\s+result=ffffffff status=00000000', 'no async job'),
]

names = sys.argv[1:] or list(SCENARIOS)
failed = []
for name in names:
    if name not in SCENARIOS:
        name = name + '-raspi4b'
    mode, machine, hv, top, limit = SCENARIOS[name]
    env = dict(os.environ, MACHINE=machine, SECS=os.environ.get('SECS', '240'))
    out = subprocess.run(['bash', 'run.sh', mode], env=env, capture_output=True, text=True, errors='replace').stdout
    errors = []
    if 'NCTEST END' not in out:
        errors.append('the 68k test did not finish')
    if not re.search(r'Top of RAM \(32bit\): ' + top + r'\b', out):
        errors.append('top of RAM is not ' + top)
    checks = (WITH_HV + (TIME_LIMIT if limit else NO_TIME_LIMIT)) if hv else NO_HV
    for pattern, what in checks:
        if not re.search(pattern, out):
            errors.append(what)
    print('%-20s %s' % (name, 'ok' if not errors else 'FAILED: ' + ', '.join(errors)), flush=True)
    if errors:
        failed.append(name)
        open('build-%s.out' % name, 'w').write(out)

print('\n%d scenarios, %d failed' % (len(names), len(failed)))
sys.exit(1 if failed else 0)

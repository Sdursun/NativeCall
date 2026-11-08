#!/usr/bin/env python3
"""Runs every QEMU scenario and checks the results. Usage: python3 test_all.py [scenario ...]
A scenario is mode or mode-machine, for example stub, hv-raspi4b."""
import os
import re
import subprocess
import sys

os.chdir(os.path.dirname(os.path.abspath(__file__)))

# Where the hypervisor must have moved Emu68's top of RAM to; stock is 2 MB higher
SCENARIOS = {
    'base-raspi3b':       ('base', 'raspi3b', False, '34800000'),
    'hv-raspi3b':         ('hv', 'raspi3b', True, '34600000'),
    'refuse-raspi3b':     ('refuse', 'raspi3b', False, '34800000'),
    'base-raspi4b':       ('base', 'raspi4b', False, '79e00000'),
    'hv-raspi4b':         ('hv', 'raspi4b', True, '79c00000'),
    'refuse-raspi4b':     ('refuse', 'raspi4b', False, '79e00000'),
    'stub-raspi4b':       ('stub', 'raspi4b', True, '79c00000'),
    'stubrefuse-raspi4b': ('stubrefuse', 'raspi4b', False, '79e00000'),
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
    mode, machine, hv, top = SCENARIOS[name]
    env = dict(os.environ, MACHINE=machine, SECS=os.environ.get('SECS', '240'))
    out = subprocess.run(['bash', 'run.sh', mode], env=env, capture_output=True, text=True, errors='replace').stdout
    errors = []
    if 'NCTEST END' not in out:
        errors.append('the 68k test did not finish')
    if not re.search(r'Top of RAM \(32bit\): ' + top + r'\b', out):
        errors.append('top of RAM is not ' + top)
    for pattern, what in (WITH_HV if hv else NO_HV):
        if not re.search(pattern, out):
            errors.append(what)
    print('%-20s %s' % (name, 'ok' if not errors else 'FAILED: ' + ', '.join(errors)), flush=True)
    if errors:
        failed.append(name)
        open('build-%s.out' % name, 'w').write(out)

print('\n%d scenarios, %d failed' % (len(names), len(failed)))
sys.exit(1 if failed else 0)

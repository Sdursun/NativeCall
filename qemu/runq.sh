#!/bin/bash
# runq.sh <seconds> <uart log> <qemu command...>: runs QEMU until the 68k test ends or time is up.
secs=$1; log=$2; shift 2
"$@" &
pid=$!
for ((i = 0; i < secs; i++)); do
    sleep 1
    kill -0 $pid 2>/dev/null || break
    if grep -aq 'NCTEST END' "$log" 2>/dev/null; then sleep 1; break; fi
done
kill $pid 2>/dev/null
wait $pid 2>/dev/null
exit 0

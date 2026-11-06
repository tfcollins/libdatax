#!/bin/sh
# SPDX-License-Identifier: MIT
# The FHM trace (DATAX_TRACE=fhm): silent by default and for other trace
# names, one "datax-fhm:" line per pipeline event when enabled.
# Usage (under tests/emu/emu.sh): trace_fhm.sh <test_fhm binary>
t="$1"
case_=fhm_one_behind_pipeline
fail() { echo "trace_fhm: $*" >&2; exit 1; }

out=$(env -u DATAX_TRACE "$t" one $case_ 2>&1 >/dev/null) || fail "test failed untraced"
echo "$out" | grep -q "datax-fhm:" && fail "trace on by default"
out=$(DATAX_TRACE=fhmx,sched "$t" one $case_ 2>&1 >/dev/null) || fail "test failed (fhmx)"
echo "$out" | grep -q "datax-fhm:" && fail "trace on for DATAX_TRACE=fhmx,sched"

out=$(DATAX_TRACE=xfhm "$t" one $case_ 2>&1 >/dev/null) || fail "test failed (xfhm)"
echo "$out" | grep -q "datax-fhm:" && fail "trace on for DATAX_TRACE=xfhm"
out=$(DATAX_TRACE=fhmx,fhm "$t" one $case_ 2>&1 >/dev/null) || fail "test failed (fhmx,fhm)"
echo "$out" | grep -q "datax-fhm:" || fail "trace off for DATAX_TRACE=fhmx,fhm"

out=$(DATAX_TRACE=sched,fhm "$t" one $case_ 2>&1 >/dev/null) || fail "test failed traced"
for k in "staged table n=3" "queue E0 prime seq=" "queue E1 tune seq=" \
         "stage E1 seq=[0-9]* hz=2410000000 via=poll start~[0-9]* end=[0-9]*" \
         "stage-hold E2 seq=" "fired E0 seq=[0-9]* hw=[0-9]* last_hw=[0-9]* via=poll" \
         "fired E1 seq=[0-9]* hw=[0-9]* last_hw=[0-9]* via=record" \
         "stage E2 seq=[0-9]* hz=2420000000 via=poll" "stage E3 seq=[0-9]* hz=2430000000 via=record" \
         "verdict E1 seq=[0-9]* hw=[0-9]* exec=[0-9]* .* readback=2410000000 want=2410000000 status_in=0 status_out=0"; do
	echo "$out" | grep -q "^datax-fhm: mono=[0-9]* $k" || fail "missing: $k"
done
n=$(echo "$out" | grep -c "^datax-fhm: .*verdict E")
[ "$n" = 3 ] || fail "expected 3 verdict lines, got $n"
echo "trace_fhm: ok"

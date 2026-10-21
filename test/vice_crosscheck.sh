#!/bin/sh
# Runs basic/crosscheck.bas in VICE (x64sc) and on the 6502 oracle
# (build/runbasic) and checks that both print the same thing.  This is what
# vouches for the oracle itself.  VICE prints to a text file via a virtual
# printer on device 4, so the VICE copy of the program starts with OPEN4,4:CMD4.
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

{ echo '10 open4,4:cmd4'; sed 's/^990 end/990 print#4:close4:end/' basic/crosscheck.bas; } > "$tmp/vice.bas"
petcat -w2 -o "$tmp/vice.prg" -- "$tmp/vice.bas"
petcat -w2 -o "$tmp/oracle.prg" -- basic/crosscheck.bas

build/runbasic "$tmp/oracle.prg" > "$tmp/oracle.txt" 2>/dev/null
# -limitcycles ends the run (with exit status 1) once the program has had time to finish.
x64sc -silent -default -warp -sounddev dummy \
    -devicebackend4 1 -busdevice4 -pr4drv ascii -pr4output text -pr4txtdev 0 \
    -prtxtdev1 "$tmp/vice.txt" -limitcycles 400000000 \
    -autostart "$tmp/vice.prg" > "$tmp/vice.log" 2>&1 || true

# The printer lower-cases letters and tabs differently; compare the words.
norm() { tr 'a-z' 'A-Z' | tr -s ' \t' ' ' | sed 's/ *$//; /^$/d; /READY/d'; }
norm < "$tmp/oracle.txt" > "$tmp/oracle.norm"
norm < "$tmp/vice.txt" > "$tmp/vice.norm" 2>/dev/null || true
if [ -s "$tmp/vice.norm" ] && cmp -s "$tmp/oracle.norm" "$tmp/vice.norm"; then
    echo "OK   VICE and the oracle agree ($(wc -l < "$tmp/vice.norm" | tr -d ' ') lines)"
else
    echo "FAIL VICE and the oracle differ"
    diff "$tmp/oracle.norm" "$tmp/vice.norm" || true
    exit 1
fi

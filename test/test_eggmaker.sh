#!/bin/sh
# Checks eggmaker's output for some texts:
#  - each chunk's seed is the smallest that spells it (per fastsearch),
#  - a word that was split has no seed of its own,
#  - the generated .prg prints the chunks on the real ROMs (build/runbasic).
#
# usage: test/test_eggmaker.sh [eggmaker options] TEXT...
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0

run() {
    opts=$1 text=$2
    rm -f "$tmp/egg.prg" "$tmp/failed"
    # shellcheck disable=SC2086
    build/eggmaker $opts -o "$tmp/egg.prg" "$text" > "$tmp/egg.bas" 2> "$tmp/report.txt"
    scale=$(echo "$opts" | sed -n 's/.*--scale \([0-9]*\).*/\1/p')
    sflag=${scale:+--scale $scale}
    expected=
    # report lines look like: WORD -> CHUNK (seed) CHUNK (seed)
    grep -- '->' "$tmp/report.txt" | while read -r word arrow rest; do
        set -- $rest
        nchunks=0
        while [ $# -ge 2 ]; do
            chunk=$1 seed=$(echo "$2" | tr -d '()')
            shift 2
            nchunks=$((nchunks + 1))
            # shellcheck disable=SC2086
            first=$(build/fastsearch $sflag "$chunk" 1 "$seed" 2>/dev/null | head -1 | cut -d' ' -f1)
            if [ "$first" != "$seed" ]; then
                echo "FAIL $chunk: eggmaker chose $seed, fastsearch's first is ${first:-none}"
                echo 1 > "$tmp/failed"
            fi
        done
        if [ "$nchunks" -gt 1 ]; then
            # shellcheck disable=SC2086
            whole=$(build/fastsearch $sflag "$word" 1 4294967295 2>/dev/null | head -1)
            if [ -n "$whole" ]; then
                echo "FAIL $word was split but has seed $whole"
                echo 1 > "$tmp/failed"
            fi
        fi
    done
    expected=$(grep -- '->' "$tmp/report.txt" | sed 's/^[A-Z]* *-> //; s/ ([0-9]*)//g' | tr '\n' ' ' | tr -s ' ')
    printed=$(build/runbasic "$tmp/egg.prg" 2>/dev/null | grep -v READY | tr '\n' ' ' | tr -s ' ')
    if [ "$(echo $printed)" != "$(echo $expected)" ]; then
        echo "FAIL \"$text\": the program prints \"$printed\", expected \"$expected\""
        echo 1 > "$tmp/failed"
    fi
    if [ -f "$tmp/failed" ]; then
        status=1
    else
        echo "ok   \"$text\" $opts -> $(echo $printed)"
    fi
}

run "" "click subscribe"
run "" "bill gates sucks"
if build/eggmaker "the quick brown fox" > /dev/null 2>&1; then
    echo "FAIL eggmaker accepted W and X with the default scale"
    status=1
else
    echo "ok   \"the quick brown fox\" refused (W, X need --scale 27)"
fi
run "" "commodore basic rules"
run "--scale 27" "commodore sixty four forever"
run "--max 999999999" "abracadabra"
exit $status

#!/bin/sh
# Checks that fastsearch (both filter methods) and gpusearch find exactly
# what the reference seedsearch finds, for a range of words, both scaling
# modes, other scales, and the oddball seeds.
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0

check() {
    build/seedsearch "$@" > "$tmp/ref.txt"
    for prog in "build/fastsearch" "build/fastsearch --per-seed" "build/gpusearch"; do
        [ "$prog" = build/gpusearch ] && [ ! -x build/gpusearch ] && continue
        $prog "$@" > "$tmp/out.txt" 2>/dev/null
        if ! cmp -s "$tmp/ref.txt" "$tmp/out.txt"; then
            echo "FAIL $prog $*"
            diff "$tmp/ref.txt" "$tmp/out.txt" | head -5
            status=1
        fi
    done
    echo "ok   $* ($(wc -l < "$tmp/ref.txt" | tr -d ' ') seeds)"
}

check A 1 3000000
check E 4000000000 4003000000
check AN 1 20000000
check BILL 1 30000000
check THE 2147483000 2160000000
# seed 33721604 spells KOEHQHDLPRSMPMCE with INT() but LOEHQHDLPRSMPMCE with A%=
check KOEHQHDLPRSMPMCE 33721000 33722000
check --assign KOEHQHDLPRSMPMCE 33721000 33722000
check LOEHQHDLPRSMPMCE 33721000 33722000
check --assign LOEHQHDLPRSMPMCE 33721000 33722000
check V 715000000 716000000                 # RND = 1.0 exactly
check --scale 26 HI 1 20000000
check --scale 3 --offset 48 1212 1 5000000
check --scale 255 --offset 0 "$(printf '\001')" 4294000000 4294967295
exit $status

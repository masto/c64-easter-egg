#!/bin/sh
# Runs a BASIC seed search on the real ROMs (via build/runbasic) and the same
# search with build/seedsearch, and checks that they find the same seeds.
#
# usage: test/compare_basic.sh robin|egg WORD FIRST LAST
#   robin  Robin's program: A%=RND(A)*T     (seedsearch --assign)
#   egg    easter egg style: A=INT(RND(A)*22) (seedsearch default)
set -eu
cd "$(dirname "$0")/.."
kind=$1 word=$2 first=$3 last=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

lower=$(printf %s "$word" | tr 'A-Z' 'a-z')    # petcat: lower case = unshifted PETSCII
sed -e "s/@WORD@/$lower/" -e "s/@FIRST@/$first/" -e "s/@LAST@/$last/" \
    "basic/$kind-search.bas" > "$tmp/search.bas"
petcat -w2 -o "$tmp/search.prg" -- "$tmp/search.bas"

flags=
[ "$kind" = robin ] && flags=--assign
build/runbasic "$tmp/search.prg" 2>/dev/null | awk 'NF == 2 && $1 ~ /^[0-9]+$/ { print $1, $2 }' > "$tmp/rom.txt"
build/seedsearch $flags "$word" "$first" "$last" > "$tmp/c.txt"

if cmp -s "$tmp/rom.txt" "$tmp/c.txt"; then
    echo "OK   $kind $word $first..$last: $(wc -l < "$tmp/c.txt" | tr -d ' ') matching seeds agree"
else
    echo "FAIL $kind $word $first..$last"
    diff "$tmp/rom.txt" "$tmp/c.txt" | head -20
    exit 1
fi

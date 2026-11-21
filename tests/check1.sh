#!/bin/sh
# check1.sh FILE [cc flags...]
# One file through three checks:
#   parse   cparse reports no errors
#   round   print(parse(x)) is a fixed point
#   object  gcc -O1 on the original and on cparse's regenerated C give
#           identical machine code (skipped if the original fails to build)
#   types   every expression type, constant value, struct layout and
#           declared type that cparse computed, asserted to gcc
# Prints one line: FILE parse=.. round=.. object=.. types=..
# CC picks the reference compiler (default gcc); CPARSEFLAGS adds cparse
# options, e.g. CC=x86_64-w64-mingw32-gcc CPARSEFLAGS="--target llp64".
CPARSE=${CPARSE:-$(dirname "$0")/../cparse}
CC=${CC:-gcc}
CP="$CPARSE --cc $CC $CPARSEFLAGS"
f=$1; shift
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cpchk.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
parse=ok; round=ok; object=ok; types=ok
if ! $CP -O1 "$@" "$f" >/dev/null 2>"$tmp/err"; then
    parse=FAIL
fi
grep -q ': error:' "$tmp/err" && parse=FAIL
if ! $CP -O1 --roundtrip "$@" "$f" >/dev/null 2>"$tmp/rt"; then
    round=FAIL
fi
if $CC -w -O1 "$@" -c "$f" -o "$tmp/a.o" 2>/dev/null; then
    $CP -O1 --print "$@" "$f" > "$tmp/p.c" 2>/dev/null
    if $CC -w -O1 "$@" -c "$tmp/p.c" -o "$tmp/b.o" 2>"$tmp/gccerr"; then
        objdump -d --no-show-raw-insn "$tmp/a.o" | tail -n +4 > "$tmp/a.s"
        objdump -d --no-show-raw-insn "$tmp/b.o" | tail -n +4 > "$tmp/b.s"
        cmp -s "$tmp/a.s" "$tmp/b.s" || object=DIFF
    else
        object=NOCOMPILE
    fi
else
    object=skip
fi
if [ $object != skip ]; then
    $CP -O1 --typecheck "$@" "$f" > "$tmp/t.c" 2>/dev/null
    n=$($CC -w -O1 "$@" -fsyntax-only -fmax-errors=100000 "$tmp/t.c" 2>&1 | grep -c 'error:')
    [ "$n" != 0 ] && types="FAIL($n)"
else
    types=skip
fi
echo "$f parse=$parse round=$round object=$object types=$types"
if [ -n "$VERBOSE" ]; then
    [ $parse = FAIL ] && grep ': error:' "$tmp/err" | head -3
    [ $round = FAIL ] && head -5 "$tmp/rt"
    [ $object = NOCOMPILE ] && head -5 "$tmp/gccerr"
    [ "$types" != ok ] && [ "$types" != skip ] && $CC -w -O1 "$@" -fsyntax-only -fmax-errors=100000 "$tmp/t.c" 2>&1 | grep -A1 'error:' | grep -v 'error:' | head -5
fi
exit 0

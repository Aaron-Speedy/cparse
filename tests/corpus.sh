#!/bin/sh
# Run every corpus file through tests/check1.sh in parallel and summarize.
# Corpora live in corpus/src (see corpus/README). Usage: tests/corpus.sh [-jN]
# CC picks the reference compiler (e.g. CC=clang); results go to OUT
# (default corpus/results.txt, or corpus/results-$CC.txt for another CC).
cd "$(dirname "$0")/../corpus/src" || exit 1
J=${1:-8}; J=${J#-j}
C=../../tests/check1.sh
case "${CC:-gcc}" in gcc) dflt=../results.txt;; *) dflt=../results-$CC.txt;; esac
OUT=${OUT:-$dflt}
{
    ls sqlite-amalgamation-*/sqlite3.c | xargs -P$J -n1 $C
    ls lua-*/src/*.c | xargs -P$J -n1 $C
    ls zlib-*/*.c | xargs -P$J -n1 $C
    ls curl-*/docs/examples/*.c | xargs -P$J -I{} $C {} -Icurl-8.11.1/include
    ls tcc-*/tests/tests2/*.c | xargs -P$J -n1 $C
    MF="-std=c99 -nostdinc -ffreestanding -fno-strict-aliasing -D_XOPEN_SOURCE=700"
    for d in arch/x86_64 arch/generic obj/src/internal src/include src/internal obj/include include; do
        MF="$MF -Imusl-1.2.5/$d"
    done
    find musl-*/src -name '*.c' | grep -vE '/(i386|arm|aarch64|mips[^/]*|powerpc[^/]*|s390x|sh|microblaze|or1k|riscv[^/]*|m68k|loongarch64|x32)/' |
        xargs -P$J -I{} $C {} $MF
    ls ../extra/*.c 2>/dev/null | xargs -r -P$J -n1 $C
} > $OUT
awk '{n++; for (i=2;i<=NF;i++) {split($i,kv,"="); c[kv[1]" "kv[2]]++}} END {
    print n " files"; for (k in c) print "  " k ": " c[k] }' $OUT | sort
grep -E 'FAIL|DIFF|NOCOMPILE' $OUT | grep -v 'object=skip' | head -40

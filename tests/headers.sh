#!/bin/sh
# System header sweep: every header under /usr/include (and the compiler's
# own include directory) that gcc accepts on its own must parse cleanly and
# roundtrip. Wrappers go in build/hdr. Usage: tests/headers.sh [-jN]
cd "$(dirname "$0")/.." || exit 1
J=${1:-8}; J=${J#-j}
out=build/hdr
mkdir -p $out
gccinc=$(gcc -print-file-name=include)
# one wrapper per header: <sys/stat.h> becomes sys_stat_h.c
for root in /usr/include "$gccinc"; do
    (cd "$root" && find . -name '*.h' -type f) | sed 's|^\./||' | while read -r h; do
        w=$(printf '%s' "$h" | tr '/.+-' '____').c
        [ -f "$out/$w" ] || printf '#include <%s>\n' "$h" > "$out/$w"
    done
done
cd $out
ls | grep '\.c$' | xargs -P"$J" -n1 ../../tests/hdr1.sh > ../hdr-results.txt 2>&1
cd ../..
echo "$(grep -c ' ok$' build/hdr-results.txt) ok," \
     "$(grep -c 'gcc-rejects' build/hdr-results.txt) rejected by gcc," \
     "$(grep -cE 'PARSEFAIL|ROUNDFAIL' build/hdr-results.txt) failed"
grep -E 'PARSEFAIL|ROUNDFAIL' build/hdr-results.txt | head -20
! grep -qE 'PARSEFAIL|ROUNDFAIL' build/hdr-results.txt

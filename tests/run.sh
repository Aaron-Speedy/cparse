#!/bin/sh
# Regression tests. Fast by default; "tests/run.sh full" also runs the
# real-world corpus (see corpus/README) and a longer layout fuzz.
cd "$(dirname "$0")/.." || exit 1
fail=0
note() { printf '%-40s %s\n' "$1" "$2"; [ "$2" = ok ] || fail=1; }

# dialect torture files: parse, roundtrip, identical object code, types
for f in gnu c23; do
    r=$(tests/check1.sh tests/cases/$f.c)
    case "$r" in *"parse=ok round=ok object=ok types=ok"*) note $f.c ok;; *) note $f.c "$r";; esac
done
r=$(tests/check1.sh tests/cases/kr.c -std=gnu89 -fpermissive)
case "$r" in *"parse=ok round=ok object=ok types=ok"*) note kr.c ok;; *) note kr.c "$r";; esac
r=$(tests/check1.sh tests/cases/simd.c -mavx2 -mfma)
case "$r" in *"parse=ok round=ok object=ok types=ok"*) note simd.c ok;; *) note simd.c "$r";; esac

# Windows: the same four checks against mingw-w64 gcc, when installed
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
    r=$(CC=x86_64-w64-mingw32-gcc CPARSEFLAGS="--target llp64" tests/check1.sh tests/cases/windows.c)
    case "$r" in *"parse=ok round=ok object=ok types=ok"*) note "windows.c (mingw)" ok;; *) note windows.c "$r";; esac
else
    printf '%-40s %s\n' "windows.c" "skipped (no x86_64-w64-mingw32-gcc)"
fi

# SDCC (8051): parse cleanly, roundtrip, and identical sdcc assembly
if command -v sdcc >/dev/null; then
    t=$(mktemp -d "${TMPDIR:-/tmp}/cpsdcc.XXXXXX")
    I=$(dirname "$(dirname "$(find /usr/share/sdcc /usr/local/share/sdcc -name 8051.h 2>/dev/null | head -1)")")
    r=ok
    ./cparse --cc "sdcc -mmcs51" -I$I/mcs51 -I$I tests/cases/sdcc.c 2>&1 | grep -q ': error:' && r="parse errors"
    ./cparse --cc "sdcc -mmcs51" -I$I/mcs51 -I$I --roundtrip tests/cases/sdcc.c >/dev/null 2>&1 || r=roundtrip
    ./cparse --cc "sdcc -mmcs51" -I$I/mcs51 -I$I --print tests/cases/sdcc.c > $t/p.c 2>/dev/null
    cp tests/cases/sdcc.c $t/o.c
    (cd $t && sdcc -mmcs51 -c o.c && sdcc -mmcs51 -c p.c) >/dev/null 2>&1 || r="sdcc rejects output"
    grep -v '^;\|\.module' $t/o.asm > $t/o.s 2>/dev/null
    grep -v '^;\|\.module' $t/p.asm > $t/p.s 2>/dev/null
    [ "$r" = ok ] && ! cmp -s $t/o.s $t/p.s && r="assembly differs"
    rm -rf "$t"
    note "sdcc.c (sdcc 8051)" "$r"
else
    printf '%-40s %s\n' "sdcc.c" "skipped (no sdcc)"
fi

# dialects gcc cannot compile, and code with missing headers: parse
# cleanly and roundtrip
for f in msvc blocks embedded missing; do
    if ./cparse --cc none tests/cases/$f.c 2>&1 | grep -q ': error:'; then note $f.c "parse errors"
    elif ! ./cparse --cc none --roundtrip tests/cases/$f.c >/dev/null 2>&1; then note $f.c "roundtrip"
    else note $f.c ok; fi
done

# symbol uses and call graph: against the hand-checked expected output
r=$(for n in g h k m arr ptr s fp callee helper use; do ./cparse --cc none --xref $n tests/cases/symbols.c 2>/dev/null; done
    ./cparse --cc none --callgraph tests/cases/symbols.c 2>/dev/null)
[ "$(printf '%s\n' "$r" | sed 's|tests/cases/symbols.c:||')" = "$(cat tests/cases/symbols.expect)" ] &&
    note "symbols.c vs expected" ok || note "symbols.c vs expected" differs

# preprocessor: same token stream as gcc -E
a=$(gcc -E -P tests/cases/pp.c 2>/dev/null | tr -d ' \t\n')
b=$(./cparse -E tests/cases/pp.c 2>/dev/null | tr -d ' \t\n')
[ "$a" = "$b" ] && note "pp.c vs gcc -E" ok || note "pp.c vs gcc -E" differs
for cc in gcc clang; do
    command -v $cc >/dev/null || continue
    a=$($cc -E -P tests/cases/hasbuiltin.c 2>/dev/null | tr -d ' \t\n')
    b=$(./cparse --cc $cc -E tests/cases/hasbuiltin.c 2>/dev/null | tr -d ' \t\n')
    [ "$a" = "$b" ] && note "hasbuiltin.c vs $cc -E" ok || note "hasbuiltin.c vs $cc -E" differs
done
a=$(gcc -std=c99 -E -P tests/cases/trigraphs.c 2>/dev/null | tr -d ' \t\n')
b=$(./cparse -std=c99 -E tests/cases/trigraphs.c 2>/dev/null | tr -d ' \t\n')
[ "$a" = "$b" ] && note "trigraphs.c vs gcc -E" ok || note "trigraphs.c vs gcc -E" differs

# struct layout against gcc
n=${LAYOUTSEEDS:-3}; [ "$1" = full ] && n=20
lf=ok
for s in $(seq 1 $n); do
    python3 tests/layoutfuzz.py 300 $s >/dev/null || { note "layout seed $s" "mismatch"; lf=bad; }
    MS=1 python3 tests/layoutfuzz.py 300 $s >/dev/null || { note "ms_struct layout seed $s" "mismatch"; lf=bad; }
done
[ $lf = ok ] && note "layout fuzz ($n seeds, gcc and ms)" ok
# the other target models against their own compilers, when available
if gcc -m32 -x c -c /dev/null -o /dev/null 2>/dev/null; then
    lf=ok
    for s in $(seq 1 $n); do
        CC="gcc -m32" CPARSEFLAGS="--target ilp32" python3 tests/layoutfuzz.py 300 $s >/dev/null ||
            { note "ilp32 layout seed $s" "mismatch"; lf=bad; }
    done
    [ $lf = ok ] && note "layout fuzz ilp32 (gcc -m32)" ok
fi
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
    lf=ok
    for s in $(seq 1 $n); do
        CC=x86_64-w64-mingw32-gcc CPARSEFLAGS="--target llp64" python3 tests/layoutfuzz.py 300 $s >/dev/null ||
            { note "llp64 layout seed $s" "mismatch"; lf=bad; }
    done
    [ $lf = ok ] && note "layout fuzz llp64 (mingw)" ok
fi

# every crash AFL++ has found: no sanitizer trap under cparse-debug, and
# error-free inputs print to a fixed point
if [ -x ./cparse-debug ]; then
    export ASAN_OPTIONS=abort_on_error=1:detect_leaks=0
    bad=0
    for f in fuzz/crashes-fixed/*; do
        (./cparse-debug --cc none --dump "$f") >/dev/null 2>&1; s=$?
        if [ $s -ge 128 ]; then note "crash ${f#fuzz/}" "sanitizer"; bad=1; continue; fi
        if [ $s = 0 ] && ! ./cparse-debug --cc none --roundtrip "$f" >/dev/null 2>&1; then
            note "crash ${f#fuzz/}" "roundtrip"; bad=1
        fi
    done
    [ $bad = 0 ] && note "fuzz crashes ($(ls fuzz/crashes-fixed | wc -l))" ok
else
    note "fuzz crashes" "no cparse-debug (run ./build.sh)"
fi

if [ "$1" = full ]; then
    tests/corpus.sh -j8
fi
exit $fail

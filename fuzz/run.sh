#!/bin/sh
# Run AFL++ in parallel: one main and N-1 secondary instances.
# Usage: fuzz/run.sh [instances] [seconds]
cd "$(dirname "$0")"
N=${1:-6}
T=${2:-0}
export AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_NO_UI=1
export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:symbolize=0:detect_leaks=0:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1"
V=""; [ "$T" != 0 ] && V="-V $T"
# seeds: the dialect test cases, the fixed crashes, and tcc's tests from
# the corpus (tests/fetch-corpus.sh), made once
if [ ! -d in ]; then
    mkdir in
    cp ../tests/cases/*.c crashes-fixed/*.c in/
    cp ../corpus/src/tcc-*/tests/tests2/*.c in/ 2>/dev/null || true
fi
IN=in; [ -d out/main/queue ] && IN=-   # resume
mkdir -p out
afl-fuzz -M main -i $IN -o out -x c.dict -m none -t 3000 $V -- ./cparse-fuzz > out/main.log 2>&1 &
i=1
while [ $i -lt $N ]; do
    afl-fuzz -S s$i -i $IN -o out -x c.dict -m none -t 3000 $V -- ./cparse-fuzz > out/s$i.log 2>&1 &
    i=$((i+1))
done
wait

#!/bin/sh
# wild1.sh PROJECTDIR FILE: one file of a project with no build system run.
# Include paths are guessed from the layout. If the compiler accepts the
# file, it gets all four checks of tests/check1.sh; if not (a missing
# generated header, say), cparse must still not crash or hang.
# Prints one line: FILE STATUS...
d=$1; f=$2
CC=${CC:-gcc}
CPARSE=${CPARSE:-$(dirname "$0")/../cparse}
flags="-I$(dirname "$f")"
for i in "$d" "$d/include" "$d/src" "$d/lib"; do
    [ -d "$i" ] && flags="$flags -I$i"
done
if timeout 60 $CC -O1 -fsyntax-only -w $flags "$f" >/dev/null 2>&1; then
    r=$(CPARSE=$CPARSE timeout 600 "$(dirname "$0")/check1.sh" "$f" $flags)
    [ -n "$r" ] && echo "$r" || echo "$f TIMEOUT"
else
    timeout 60 "$CPARSE" --cc "$CC" $flags "$f" >/dev/null 2>&1
    s=$?
    if [ $s = 124 ]; then echo "$f gcc-rejects HANG"
    elif [ $s -ge 128 ]; then echo "$f gcc-rejects CRASH($s)"
    else echo "$f gcc-rejects"; fi
fi

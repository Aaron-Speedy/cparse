#!/bin/sh
# hdr1.sh FILE: if gcc accepts FILE, cparse must too (and roundtrip).
CPARSE=${CPARSE:-$(dirname "$0")/../cparse}
f=$1
gcc -fsyntax-only -w "$f" 2>/dev/null || { echo "$f gcc-rejects"; exit 0; }
err=$("$CPARSE" "$f" 2>&1 | grep -m3 ': error:')
[ -n "$err" ] && { echo "$f PARSEFAIL $err" | tr '\n' ' '; echo; exit 0; }
"$CPARSE" --roundtrip "$f" >/dev/null 2>&1 || { echo "$f ROUNDFAIL"; exit 0; }
echo "$f ok"

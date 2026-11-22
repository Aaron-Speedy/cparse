#!/bin/sh
# Test cparse on real projects straight from GitHub, one at a time so disk
# use stays small: download a project (C sources and headers only), run
# tests/wild1.sh on every .c file, append the results, and delete the
# project unless something failed (KEEP=1 keeps everything).
# Usage: tests/wild.sh LIST WORKDIR [-jN]
# LIST has one owner/repo per line; results go to WORKDIR/results.txt.
LIST=$1; W=$2; J=${3:-8}; J=${J#-j}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W"
while read -r r; do
    case "$r" in ''|'#'*) continue;; esac
    name=$(echo "$r" | tr / _)
    d="$W/$name"
    grep -q "^# $r " "$W/results.txt" 2>/dev/null && continue  # done before
    if [ ! -d "$d" ]; then
        t="$W/.$name.tgz"
        if ! curl -fsSL --max-time 600 --max-filesize 104857600 -o "$t" \
                "https://codeload.github.com/$r/tar.gz/HEAD" 2>/dev/null; then
            rm -f "$t"
            echo "# $r download-failed" >> "$W/results.txt"
            continue
        fi
        mkdir -p "$d"
        tar xzf "$t" -C "$d" --strip-components=1 --wildcards --no-anchored '*.c' '*.h' 2>/dev/null
        rm -f "$t"
    fi
    out="$W/.$name.results"
    find "$d" -name '*.c' -type f | sort |
        xargs -r -P"$J" -I{} "$here/wild1.sh" "$d" {} > "$out" 2>/dev/null
    n=$(wc -l < "$out")
    bad=$(grep -cE 'FAIL|DIFF|NOCOMPILE|CRASH|HANG|TIMEOUT' "$out")
    echo "# $r files=$n bad=$bad" >> "$W/results.txt"
    sed "s|^$W/||" "$out" >> "$W/results.txt"
    rm -f "$out"
    [ "$bad" = 0 ] && [ -z "$KEEP" ] && rm -rf "$d"
    echo "$r: $n files, $bad bad"
done < "$LIST"

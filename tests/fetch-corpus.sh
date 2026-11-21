#!/bin/sh
# Download the real-world corpus used by tests/corpus.sh, check it against
# the archives the test results were made with, and unpack it into
# corpus/src. Archives are kept in corpus/dl, so a second run is offline.
# Usage: tests/fetch-corpus.sh
set -e
cd "$(dirname "$0")/../corpus"
mkdir -p dl src

fetch() {  # fetch FILE URL SHA256
    if [ ! -f "dl/$1" ]; then
        echo "downloading $1"
        curl -fsSL -o "dl/$1.part" "$2"
        mv "dl/$1.part" "dl/$1"
    fi
    echo "$3  dl/$1" | sha256sum -c --quiet - ||
        { echo "checksum mismatch: dl/$1" >&2; exit 1; }
}

fetch sqlite-amalgamation-3490100.zip https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip \
    6cebd1d8403fc58c30e93939b246f3e6e58d0765a5cd50546f16c00fd805d2c3
fetch lua-5.4.7.tar.gz https://www.lua.org/ftp/lua-5.4.7.tar.gz \
    9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
fetch zlib.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
fetch curl.tar.gz https://curl.se/download/curl-8.11.1.tar.gz \
    a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80
fetch musl.tar.gz https://musl.libc.org/releases/musl-1.2.5.tar.gz \
    a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
fetch tcc.tar.bz2 https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2 \
    de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c

cd src
[ -d sqlite-amalgamation-3490100 ] || unzip -q ../dl/sqlite-amalgamation-3490100.zip
[ -d lua-5.4.7 ] || tar xzf ../dl/lua-5.4.7.tar.gz
[ -d zlib-1.3.1 ] || tar xzf ../dl/zlib.tar.gz
[ -d curl-8.11.1 ] || tar xzf ../dl/curl.tar.gz
[ -d tcc-0.9.27 ] || tar xjf ../dl/tcc.tar.bz2
if [ ! -d musl-1.2.5 ]; then
    tar xzf ../dl/musl.tar.gz
    # the generated headers, made as musl's Makefile does
    cd musl-1.2.5
    mkdir -p obj/include/bits obj/src/internal
    sed -f tools/mkalltypes.sed arch/x86_64/bits/alltypes.h.in \
        include/alltypes.h.in > obj/include/bits/alltypes.h
    cp arch/x86_64/bits/syscall.h.in obj/include/bits/syscall.h
    sed -n s/__NR_/SYS_/p < arch/x86_64/bits/syscall.h.in >> obj/include/bits/syscall.h
    echo '#define VERSION "1.2.5"' > obj/src/internal/version.h
    cd ..
fi
echo "corpus ready in corpus/src"

#!/bin/sh
# Build the AFL++ fuzz target: persistent mode, ASan + UBSan, assertions on.
set -e
cd "$(dirname "$0")/.."
AFL_QUIET=1 AFL_GCC_DISABLE_VERSION_CHECK=1 afl-gcc-fast -std=c99 -g3 -O1 -DCPARSE_DEBUG -DCPARSE_MAXDEPTH=200 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o fuzz/cparse-fuzz cparse.c

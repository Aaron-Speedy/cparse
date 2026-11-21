#!/bin/sh
# Build the cparse tool: a debug build with sanitizers and a release build.
set -e
cd "$(dirname "$0")"
WARN="-Wall -Wextra -Wconversion -Wdouble-promotion -Wno-unused-parameter -Wno-unused-function -Wno-sign-conversion"
cc -std=c99 -O2 $WARN -o cparse cparse.c
cc -std=c99 -g3 $WARN -DCPARSE_DEBUG -fsanitize=address,undefined -fsanitize-trap=undefined -o cparse-debug cparse.c

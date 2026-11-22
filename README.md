# cparse

cparse is a single-header C front end: a preprocessor, parser, and type
checker. It is meant for C as people actually write it. It handles
pre-standard K&R C and every standard since: C89, C99, C11, C17 and C23.
It also covers most of what compilers add on top: GNU extensions
(attributes, statement expressions, `typeof`, computed `goto`, inline
`asm`, `__int128`, vector types), MSVC (`__declspec`, SEH, `__asm` blocks,
`__int64`), Clang blocks and nullability, and embedded dialects like
SDCC's 8051 storage classes, `__far` and `__interrupt`. Struct layouts
follow GCC or Microsoft rules for 64-bit Unix, 64-bit Windows, or 32-bit
x86. Code with missing headers parses too, with guesses about unknown
names. The test files in `tests/cases/`, one per dialect, show in detail
what is covered.

It's written in C99. It uses arenas instead of malloc, does no I/O, and
needs nothing from libc except `setjmp`.

The full API is documented in comments in `cparse.h`.

WIP! There still may be bugs! If you find any, or have any ideas or suggestions,
feel free to contact me at my email (aaronspeedy@protonmail.com) or through
issues or pull requests on GitHub.

## Command-line tool

    $ cc -O2 -o cparse cparse.c
    $ ./cparse --layout packet.c
    struct packet  size=24 align=8
           0  1     kind  char
           1  3     <padding>
           4  4     length  int
           8  :3    flags  (bit 0) short
           9  7     <padding>
          16  8     data  char *

Other modes:

    cparse file.c                  diagnostics
    cparse -E file.c               preprocessed tokens
    cparse --print file.c          regenerated C
    cparse --dump file.c           tree with types and constant values
    cparse --ast file.c            tree as JSON
    cparse --json file.c           symbols, layouts, macros, includes
    cparse --symbols file.c
    cparse --xref NAME file.c      every declaration and use of NAME
    cparse --callgraph [--dot] file.c
    cparse --macros file.c
    cparse --includes file.c

It takes the usual compiler flags (`-I`, `-D`, `-U`, `-include`,
`-std=`), so you can put it in a build command in place of the compiler.
Flags it doesn't know are ignored. It asks a real compiler (`--cc`,
default `cc`) for predefined macros and the include path, so name the one
the code is built with:

    cparse --cc clang ...
    cparse --cc x86_64-w64-mingw32-gcc --target llp64 ...
    cparse --cc "sdcc -mmcs51" ...

## Library

Define `CPARSE_IMPLEMENTATION` in one file. `CPARSE_HOSTED` adds helpers
that read files and query the compiler:

```c
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "cparse.h"

int main(int argc, char **argv)
{
    cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
    cp_unit *u = cp_parsefile(&a, argv[1], argc-2, argv+2);
    for (cp_error *e = u->errors; e; e = e->next) {
        puts(cp_errorstr(&a, e).data);
    }
    cp_sym *f = 0;
    while ((f = cp_nextsym(u, f, CP_KIND(CP_SYM_FUNC), CP_SEL_MAIN|CP_SEL_DEFINED))) {
        puts(cp_signature(&a, u, f).data);
    }
}
```

Without `CPARSE_HOSTED`, you supply the memory and a callback that reads
files, and the library does nothing else. Everything it returns lives in
the arena.

A unit holds:

- tokens, each with a position and the macro it came from
- macros, includes, comments, pragmas, and skipped `#if` blocks
- a syntax tree close enough to the source that `cp_print` reproduces it
- expression types, with identifiers resolved to their declarations
- constant values, including `sizeof` and `offsetof`
- struct layouts for LP64, LLP64, or ILP32, under GCC or Microsoft rules
- a symbol table with every use and a call graph

When a name was never declared, usually because a header is missing,
cparse guesses from context whether it is a type (`foo_t x;`) or an
annotation macro (`EXPORT int f(void);`, `void f(void) NORETURN;`) and
warns about the guess. Parse errors are recovered from at the next
declaration.

`examples/` has a few small programs that use it.

## Testing

Wherever possible, cparse is checked against a real compiler: gcc, clang,
mingw-w64, `gcc -m32`, and SDCC. Each corpus file (sqlite, lua, zlib,
musl, tcc's tests) has to parse, and printing it again has to give
identical output. When the regenerated C is compiled, it must produce the
same machine code as the original. `--typecheck` output, which asserts
every type, constant, size and offset cparse computed, must also compile.

The 1.0 release went through 125,480 files from 215 GitHub projects,
every header in `/usr/include`, random struct layouts compared against
three compilers, and AFL++.

    tests/run.sh          # fast checks
    tests/run.sh full     # plus the corpus (tests/fetch-corpus.sh first)

## Limits

- 128-bit constants that don't fit in 64 bits are left unfolded.
- Struct layout follows GCC where Clang differs (packed and `ms_struct`
  bit-fields, a few Windows corner cases).
- Calls to x86 builtins that cparse doesn't know have no type. Code using
  the intrinsics headers is still fully typed.
- No C++.

## License

Public domain. See `UNLICENSE`.

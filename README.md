# cparse

A C front end in one file: preprocessor, parser, and semantic analysis
for C as it is found in the wild. It is written in the style of Chris
Wellons (null program): C99, arenas instead of malloc, sized strings
instead of null-terminated ones, signed sizes, no I/O, and no libc beyond
`setjmp`.

- `cparse.h` is the library, stb-style.
- `cparse.c` is a command-line tool built on it, and also the AFL++ fuzz
  target.

## What it understands

- **Standards:** C89 through C23.
- **GNU:**
  - attributes anywhere, statement expressions, `typeof`, `__auto_type`;
  - labels as values and computed `goto`, case ranges, range designators,
    old-style `field:` designators;
  - nested functions, `__label__`, `?:` with an omitted middle operand;
  - `__int128`, `_FloatN`, vector types, `mode()`, `__real__`/`__imag__`;
  - zero-length and flexible arrays, `asm` with operands, `goto` and
    `inline`, asm labels;
  - `__builtin_offsetof`, `__builtin_va_arg`,
    `__builtin_types_compatible_p`, `__builtin_choose_expr`, and the
    `__sync_*` and `__atomic_*` families;
  - raw string literals `R"x(...)x"`.
- **C23:** `[[attributes]]`, `bool`, `true`/`false`, `nullptr`,
  `constexpr`, `typeof_unqual`, `_BitInt`, `static_assert` without a
  message, digit separators, binary literals, enums with fixed underlying
  types, `{}`, labels before declarations, `#embed`, `__VA_OPT__`,
  `#elifdef`.
- **MSVC and Windows:** `__declspec`, calling conventions,
  `__int8`..`__int64` and `i64` suffixes, `__forceinline`, `__ptr64`, SEH
  (`__try`/`__except`/`__finally`/`__leave`), `__asm` blocks and lines,
  `__pragma`, anonymous members with a tag, and the Microsoft record layout
  (`ms_struct`, or `--target llp64`). mingw-w64's `<windows.h>` parses and
  lays out exactly as mingw's gcc does.
- **Clang:** blocks (`^`), nullability qualifiers, `__has_feature` and
  `__has_extension`, `__is_identifier`, `__building_module`.
- **x86 SIMD:** the compilers' intrinsics headers, vector types, vector
  comparisons and `__builtin_shuffle`.
- **Embedded compilers:** `__far`/`__near`/`__huge`, `__sfr`, `__sbit`,
  `__xdata`, `__at(addr)` and `__at addr`, `__interrupt(n)`, `__irq`,
  `__packed`, and SDCC's `__data`, `__idata`, `__pdata`, `__code`,
  `__bit`, `__using(n)`, `__reentrant`, `__critical { }`, `__banked` and
  the rest. The SDCC words that other code uses as names are keywords
  only when compiling for SDCC.
- **K&R:** old-style definitions, implicit `int`, unprototyped functions.

The preprocessor handles:

- `#include`, `#include_next` and `#import`, `#pragma once` (by path,
  or by name and contents as GCC does), include guards;
- `#if` with `defined`, `__has_include` and `__has_attribute`, and
  `__has_builtin`, `__has_feature` and `__has_extension` answered as GCC
  or Clang does for the configured compiler, target and language standard;
- function-like and variadic macros, GNU `, ##__VA_ARGS__`, `__VA_OPT__`;
- `#` and `##`, `_Pragma`, `push_macro`/`pop_macro`, `#embed`;
- trigraphs, in ISO modes before C23 (`-std=c99`) or with `-trigraphs`;
- GCC's obsolete assertions (`#assert`, `#unassert`, `#if #machine(x86)`);
- `#line`, with macros in its operands, and line markers, so
  already-preprocessed input works too;
- `__DATE__` and `__TIME__` from `cp_config.time`, which the hosted part
  sets from the clock or `SOURCE_DATE_EPOCH`, as GCC does.

Where an identifier was never declared, for example because a header is
missing, cparse guesses what it must be from where it stands:

- a type name, when only a type fits (`foo_t x;`, `(foo_t *)p`);
- an attribute-like macro, when only an annotation fits
  (`EXPORT int f(void);`, `int WINAPI f(void);`, `void f(void) NORETURN;`,
  `int x UNUSED;`, `int a ALIGNED(16);`, `f(EV_P_ struct ev_io *w)`,
  `API size_t f(void);` and runs like `void f(void) NORETURN PRINTF(1, 2);`).
  It is kept in the tree as an attribute, so the declaration keeps its true
  type.

Each guess is reported as a warning. This lets cparse parse code whose
headers aren't available.

## What it produces

Everything hangs off one `cp_unit`.

### Tokens

Every token after preprocessing knows:

- its file, line and column;
- the outermost macro that produced it;
- which top-level expansion it belongs to;
- the doc comment directly before it.

### Macros

- Every definition, with parameters, body, location, doc comment and use
  count.
- Every top-level expansion, with its output token range.

### Files and the include graph

- Every file, with its contents and whether it is a system header.
- Every `#include` directive and what it resolved to.
- The skipped `#if` regions.
- All comments.
- All pragmas, kept out of the grammar.

### The syntax tree

It is faithful to the source: typedef names, declarator shapes and
attribute placement are kept, so `cp_print` regenerates equivalent C.
Every node has a source range.

### Semantic analysis

- Every expression's type (as `typeof` sees it). Expressions are also
  flagged as lvalues or as having side effects.
- Every identifier bound to its declaration.
- Integer constant expressions evaluated, including `sizeof`, `offsetof`
  and the `&((T *)0)->m` idiom; float constants too.
- Decoded string literal bytes.
- Typedefs and `typeof` resolved to canonical types.
- Struct and union layout with sizes, alignment, member offsets and
  bit-field positions. This honours `packed`, `aligned`, `#pragma pack`,
  `mode` and `vector_size`.
- Enum values and underlying types.
- `_Static_assert` evaluated.
- `goto` targets resolved.

### Symbols

Every entity (variables, functions, parameters, typedefs, enumeration
constants, tags and labels) gets a symbol with:

- its linkage and storage duration;
- all of its declarations and its definition;
- every use, flagged as a read, write, address-taken, or call, or as
  none of these inside `sizeof`, `typeof` and `_Generic`, which do not
  evaluate their operand;
- the enclosing function, for block-scope symbols;
- the call graph in both directions, for functions (a call through a
  function pointer is a call of the pointer, not an edge).

### Diagnostics

Errors recover at the declaration level: a bad declaration becomes an
error node and parsing continues. Semantic problems are warnings and never
stop the parse.

Types and sizes follow the target: LP64 (Linux, macOS, BSD; the default),
LLP64 (Windows) or ILP32 (i386). Where compilers for one target disagree,
the compiler's predefined macros decide (`long double` is 8 bytes for MSVC
and 16 for mingw).

## The command-line tool

```
cc -O2 -o cparse cparse.c        # or ./build.sh (also builds cparse-debug)

cparse file.c                    # diagnostics and a summary
cparse -E file.c                 # preprocessed tokens
cparse --print file.c            # regenerated C
cparse --dump file.c             # tree with types, values, references
cparse --ast file.c              # the tree as JSON
cparse --json file.c             # symbols, layouts, macros, includes, ...
cparse --symbols file.c          # symbol table
cparse --xref NAME file.c        # every declaration and use of NAME
cparse --callgraph [--dot] file.c
cparse --layout file.c           # struct layouts with padding
cparse --macros file.c
cparse --includes file.c
cparse --roundtrip file.c        # check print(parse(x)) is a fixed point
cparse --typecheck file.c        # C that asserts every computed type
cparse --version
```

Options:

- `-I`, `-isystem`, `-iquote`, `-D`, `-U`, `-include`, `-nostdinc` and
  `-trigraphs` work as in a compiler. As in gcc, an ISO `-std=` before
  `c23` turns on trigraphs.
- The tool asks a compiler (`--cc`, default `cc`) for its predefined
  macros and include path. It passes `-std=`, `-O*`, `-m*` and `-f*`
  along, so `__OPTIMIZE__` and friends match a real build. Other compiler
  flags are ignored, so cparse can be dropped into a build command line.
  Name the compiler the code is built with: `--cc clang`,
  `--cc x86_64-w64-mingw32-gcc --target llp64`, `--cc "gcc -m32" --target
  ilp32`, `--cc "sdcc -mmcs51"`.
- `--target` picks the type model, and `--nopp` treats the input as
  already preprocessed.
- `--symbols`, `--layout`, `--macros`, `--json` and `--ast` show only the
  main file unless you add `--all`.

## Using the library

The quickest start parses a file as a compiler would. Define
`CPARSE_HOSTED` along with `CPARSE_IMPLEMENTATION` for the helpers that
read files and ask the compiler for its settings:

```c
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "cparse.h"

int main(int argc, char **argv)
{
    cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
    cp_unit *u = cp_parsefile(&a, argv[1], argc-2, argv+2);  // -I, -D, --cc, ...
    for (cp_error *e = u->errors; e; e = e->next) {
        puts(cp_errorstr(&a, e).data);
    }
    // every function defined in the file, with its signature
    for (cp_sym *f = 0; (f = cp_nextsym(u, f, CP_KIND(CP_SYM_FUNC), CP_SEL_MAIN|CP_SEL_DEFINED));) {
        printf("%s: %s\n", cp_locstr(&a, u, f->def).data, cp_signature(&a, u, f).data);
    }
}
```

Without `CPARSE_HOSTED` the library never allocates on its own and never
does I/O: the caller supplies the arena and a callback that reads files.

```c
cp_arena  a   = {mem, mem + cap};
cp_config cfg = {0};
cfg.readfile    = readfile;   // yours; or cp_readfile from the hosted part
cfg.systemdirs  = dirs;  cfg.nsystemdirs = ndirs;
cfg.predefs     = predefined_macros;   // e.g. output of: cc -dM -E - </dev/null
cp_unit *u = cp_parse(&a, src, CP_STR("main.c"), &cfg);
```

Everything returned lives in the arena, so freeing it frees it all. If the
arena runs out, the unit comes back marked `oom` with whatever was
finished.

### Convenience helpers

Common questions take a line or two. They accept null where a node,
symbol or type may be missing, and strings they return are
null-terminated.

| Question | Helper |
| --- | --- |
| Step through symbols of some kinds, filtered | `cp_nextsym(u, prev, CP_KIND(CP_SYM_FUNC), CP_SEL_MAIN\|CP_SEL_DEFINED)` |
| Look up by name | `cp_findsym`, `cp_findtag`, `cp_macrodef` |
| Where something is, what it says | `cp_locof`, `cp_locstr`, `cp_symloc`, `cp_srcof`, `cp_docof`, `cp_symdoc` |
| From a header or a macro? | `cp_inmainfile`, `cp_macroof` |
| Declarations and code as C | `cp_declstr(&a, u, t, name)`, `cp_signature`, `cp_nodestr`, `cp_typename` |
| Struct fields with offsets and bit positions | `cp_fields` (anonymous members flattened) |
| Enum constants and values | `cp_enumconsts` |
| Walk the tree | `cp_walk(root, visitor, ctx)`, `cp_walkunit` |
| Calls | `cp_callsto(u, "malloc")` (each call and its caller), `cp_callee` |
| Type questions | `cp_typeis(t, CP_IS_INTEGER\|CP_IS_UNSIGNED)`, `cp_strip`, `cp_target` |
| Constant values, diagnostics | `cp_constint`, `cp_errorstr` |

`examples/` has short programs built on them: `functions.c` (functions
with signatures and doc comments), `calls.c` (every call to a function),
`structs.c` (layouts with padding) and `unused.c` (unused static
functions, variables and macros). Build one with
`cc -o calls examples/calls.c`.

## How it is tested

cparse is checked against real compilers wherever one can serve as the
reference: gcc, clang, mingw-w64's gcc for Windows, `gcc -m32` for i386,
and SDCC for the 8051. `tests/run.sh` runs the fast suite; checks whose
compiler is missing skip themselves. `tests/run.sh full` adds the corpus.

- **Corpus** (`tests/corpus.sh`, fetched by `tests/fetch-corpus.sh`):
  about 1,600 real files from sqlite, lua, zlib, curl's examples, musl, and
  tcc's torture tests. Each file must:
  - parse without errors;
  - print to a fixed point (`print(parse(print(x))) == print(x)`);
  - compile to *identical machine code* as the original does;
  - pass `--typecheck`. That output asserts every subexpression's computed
    type and constant value, every declared type, and every struct size,
    alignment and member offset as `_Static_assert`s, and the compiler must
    accept them all. sqlite alone generates about 320,000 such assertions.

  It runs against gcc by default and against any other compiler with
  `CC=clang tests/corpus.sh`.
- **Real projects** (`tests/wild.sh LIST DIR`): downloads GitHub projects
  one at a time (C files only), checks every `.c` file with
  `tests/wild1.sh`, and deletes each project that passes, so disk use stays
  small. No build system is run; include paths are guessed from the
  layout. A file the compiler accepts gets all four corpus checks; one it
  rejects (usually for a missing generated header) must still not crash
  or hang cparse. The 1.0 run covered 215 projects (Linux-adjacent tools,
  compilers, interpreters, emulators, embedded SDKs, game libraries and
  more): 125,480 files, with no crashes or hangs, and every failure it
  found has been fixed or traced to the harness.
- **System headers** (`tests/headers.sh`): every header under
  `/usr/include` that gcc accepts on its own (about 6,800) parses and
  roundtrips.
- **Layout fuzzing** (`tests/layoutfuzz.py`): random structs mixing
  bit-fields, `packed`, `aligned` and `#pragma pack`, checked under the GCC
  and Microsoft rules against gcc, against `gcc -m32` for ILP32 and against
  mingw's gcc for LLP64.
- **Preprocessor:** `tests/cases/pp.c` (the C standard's macro examples,
  GNU idioms, assertions) and `tests/cases/trigraphs.c` give the same
  tokens as `gcc -E`; `tests/cases/hasbuiltin.c` asks `__has_builtin`
  about every builtin that real headers query and must get gcc's and
  clang's answers.
- **Dialect files** in `tests/cases/`: GNU, C23, K&R and x86 SIMD get all
  four corpus checks against gcc; `windows.c` gets them against mingw;
  `sdcc.c` must give identical SDCC assembly; MSVC, blocks, embedded and
  code with missing headers must parse cleanly and roundtrip.
- **Symbols** (`tests/cases/symbols.c`): uses and the call graph against
  hand-checked expected output.
- **Helpers** (`tests/api.c`): every convenience and hosted helper against
  `tests/cases/api_input.c`, built as strict C99 and under ASan and UBSan.
  The programs in `examples/` are built and run too.
- **AFL++** (`fuzz/`): a persistent-mode harness under ASan and UBSan,
  across all three target models and with trigraphs on and off. It checks
  for crashes and that every error-free input prints to a fixed point. Run
  it with `fuzz/build.sh` and then `fuzz/run.sh [instances]`. Every crash
  it has found is kept in `fuzz/crashes-fixed/` and replayed by
  `tests/run.sh`.

## Limits

- Calls to target builtins missing from cparse's table (the hundreds of
  x86 intrinsics such as `__builtin_ia32_addps`) have no type; the
  intrinsics headers cast or return them, so code calling `_mm_add_ps` is
  typed in full.
- On Windows the layout follows mingw's gcc. Clang's MSVC mode differs on
  unions made of bit-fields and on structs holding only zero-width
  bit-fields.
- Packed and `ms_struct` bit-fields follow GCC where clang lays them out
  differently.
- Bit-fields wider than `int` keep their declared type in arithmetic. GCC
  gives them internal types that C cannot spell.
- Integer constants are held in 64 bits. A 128-bit constant expression
  whose value does not fit is left unfolded rather than given a wrong
  value.
- `__TIMESTAMP__` is the configured time; GCC uses the file's
  modification time.
- `#if` treats `__has_attribute` as true for any name.
- Guesses for missing headers can be ambiguous: in `int X y;` with both
  names unknown, `y` is taken for the annotation.
- C++ is not supported.

## License

Public domain; see `UNLICENSE`.

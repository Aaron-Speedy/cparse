// cparse.h: preprocess, parse, and type-check C as it is found in the wild
//
// A C front end in one file: preprocessor, parser, and semantic analysis.
// It accepts C89 through C23 plus the extensions real code leans on: GNU
// (attributes, statement expressions, typeof, computed goto, case ranges,
// asm, nested functions, __int128, vector types, ...), Clang (blocks,
// nullability), MSVC (__declspec, calling conventions, __int64, SEH,
// __asm blocks, Microsoft record layout), embedded compilers (__far,
// __sfr, __at(), __interrupt(), SDCC's storage classes), and pre-standard
// K&R C (old-style definitions, implicit int). C++ is not supported.
//
// Version CPARSE_VERSION (below). Checked against gcc, clang, mingw-w64,
// gcc -m32 and SDCC; see README.md for what that covers.
//
// This is a single-file library. In exactly one translation unit:
//
//     #define CPARSE_IMPLEMENTATION
//     #include "cparse.h"
//
// Everywhere else include it without the define.
//
// The library performs no I/O and never allocates on its own. The caller
// supplies an arena (one block of memory) and, for #include, a callback
// that reads files. Everything produced lives in the arena; discarding the
// arena discards it all.
//
//     cp_arena  a   = {mem, mem+cap};
//     cp_config cfg = {0};
//     cfg.readfile  = myread;          // for #include, may be null
//     cfg.predefs   = predefined;      // e.g. output of "cc -dM -E"
//     cp_unit *u = cp_parse(&a, src, CP_STR("main.c"), &cfg);
//     for (cp_error *e = u->errors; e; e = e->next) { ... }
//     for (cp_node *d = u->decls; d; d = d->next) { ... }
//     cp_str c = cp_print(&a, u);      // regenerate C source
//     cp_str t = cp_dump(&a, u);       // indented tree with types
//
// What the unit holds:
//
//   * Tokens after preprocessing. Each knows its source position and, if
//     it came out of a macro expansion, the macro (u->toks[i].macro).
//   * Every macro definition seen, in order (u->macros).
//   * #pragma lines and _Pragma/__pragma operators, positioned by token
//     index (u->pragmas). They are kept out of the grammar.
//   * The syntax tree (u->decls), faithful to the source: typedef names,
//     declarator shapes, and attribute placement are preserved, so
//     cp_print regenerates equivalent C.
//   * Semantic information hung on the tree: every expression's type
//     (node->ty, the type as typeof would see it, before array/function
//     decay), every identifier bound to its declaration (node->ref),
//     integer constant expressions evaluated (node->isconst, node->cval),
//     every declarator's resolved type, struct/union layout (type->size,
//     type->align, member offsets and bit-field positions), enum values,
//     and resolved goto targets. Semantic types (cp_type with sem set to
//     itself) are canonical: typedefs and typeof are gone.
//   * Diagnostics: errors stop at the external declaration level; a bad
//     declaration becomes a CP_D_ERROR node and parsing continues.
//     Semantic problems (undeclared names, unknown members, failed static
//     assertions) are reported as warnings and never stop the parse.
//
// Input may also be already preprocessed (cc -E output): line markers
// are honored. When an identifier was never declared (for example because
// a header was missing), it is guessed to be a type name where the syntax
// says so ("foo_t x;", "(foo_t *)p"), and an attribute-like macro where
// only an annotation fits ("int WINAPI f(void);", "int x UNUSED;").
//
// Out of memory stops processing and sets u->oom; the partial unit is
// still returned. cp_parse returns null only when the arena cannot hold
// even the unit header.
//
// This is free and unencumbered software released into the public domain.
#ifndef CPARSE_H
#define CPARSE_H

#define CPARSE_VERSION "1.0.0"
#define CPARSE_VERSION_MAJOR 1
#define CPARSE_VERSION_MINOR 0
#define CPARSE_VERSION_PATCH 0

#include <stddef.h>
#include <stdint.h>

typedef struct { char *beg; char *end; } cp_arena;
typedef struct { char *data; ptrdiff_t len; } cp_str;
#define CP_STR(s) (cp_str){(char *)(s), (ptrdiff_t)sizeof(s)-1}

typedef struct cp_token  cp_token;
typedef struct cp_macro  cp_macro;
typedef struct cp_node   cp_node;
typedef struct cp_type   cp_type;
typedef struct cp_attr   cp_attr;
typedef struct cp_pragma cp_pragma;
typedef struct cp_error  cp_error;
typedef struct cp_unit   cp_unit;
typedef struct cp_config cp_config;
typedef struct cp_sym    cp_sym;
typedef struct cp_symref cp_symref;
typedef struct cp_file   cp_file;
typedef struct cp_expansion cp_expansion;
typedef struct cp_include cp_include;
typedef struct cp_skip   cp_skip;
typedef struct cp_comment cp_comment;

// Token kinds. Keywords are canonical: every spelling of a keyword maps to
// one kind (const, __const, __const__ are all CP_K_CONST). The original
// spelling is always the token text.
enum {
    CP_EOF, CP_IDENT, CP_INTLIT, CP_FLOATLIT, CP_CHARLIT, CP_STRLIT,
    CP_OTHER,  // stray byte or unterminated literal

    // punctuators
    CP_LBRACK, CP_RBRACK, CP_LPAREN, CP_RPAREN, CP_LBRACE, CP_RBRACE,
    CP_DOT, CP_ARROW, CP_INC, CP_DEC, CP_AMP, CP_STAR, CP_PLUS, CP_MINUS,
    CP_TILDE, CP_NOT, CP_SLASH, CP_PERCENT, CP_SHL, CP_SHR, CP_LT, CP_GT,
    CP_LE, CP_GE, CP_EQ, CP_NE, CP_XOR, CP_OR, CP_ANDAND, CP_OROR,
    CP_QUESTION, CP_COLON, CP_SEMI, CP_ELLIPSIS, CP_ASSIGN, CP_MULEQ,
    CP_DIVEQ, CP_MODEQ, CP_ADDEQ, CP_SUBEQ, CP_SHLEQ, CP_SHREQ, CP_ANDEQ,
    CP_XOREQ, CP_OREQ, CP_COMMA, CP_HASH, CP_HASHHASH,

    // keywords
    CP_K_AUTO, CP_K_BREAK, CP_K_CASE, CP_K_CHAR, CP_K_CONST, CP_K_CONTINUE,
    CP_K_DEFAULT, CP_K_DO, CP_K_DOUBLE, CP_K_ELSE, CP_K_ENUM, CP_K_EXTERN,
    CP_K_FLOAT, CP_K_FOR, CP_K_GOTO, CP_K_IF, CP_K_INLINE, CP_K_INT,
    CP_K_LONG, CP_K_REGISTER, CP_K_RESTRICT, CP_K_RETURN, CP_K_SHORT,
    CP_K_SIGNED, CP_K_SIZEOF, CP_K_STATIC, CP_K_STRUCT, CP_K_SWITCH,
    CP_K_TYPEDEF, CP_K_UNION, CP_K_UNSIGNED, CP_K_VOID, CP_K_VOLATILE,
    CP_K_WHILE, CP_K_ALIGNAS, CP_K_ALIGNOF, CP_K_ATOMIC, CP_K_BOOL,
    CP_K_COMPLEX, CP_K_GENERIC, CP_K_IMAGINARY, CP_K_NORETURN,
    CP_K_STATIC_ASSERT, CP_K_THREAD_LOCAL, CP_K_CONSTEXPR, CP_K_TYPEOF,
    CP_K_TYPEOF_UNQUAL, CP_K_BITINT, CP_K_DECIMAL32, CP_K_DECIMAL64,
    CP_K_DECIMAL128,
    // GNU and Clang
    CP_K_ASM, CP_K_ATTRIBUTE, CP_K_EXTENSION, CP_K_LABEL, CP_K_REAL,
    CP_K_IMAG, CP_K_AUTO_TYPE, CP_K_INT128, CP_K_FLOAT128, CP_K_FLOAT80,
    CP_K_IBM128, CP_K_FLOAT16, CP_K_FLOAT32, CP_K_FLOAT64, CP_K_FLOAT32X,
    CP_K_FLOAT64X, CP_K_FLOAT128X, CP_K_BF16, CP_K_FP16,
    // MSVC
    CP_K_DECLSPEC, CP_K_INT8, CP_K_INT16, CP_K_INT32, CP_K_INT64,
    CP_K_FORCEINLINE, CP_K_TRY, CP_K_EXCEPT, CP_K_FINALLY, CP_K_LEAVE,
    // Attribute-like keywords: calling conventions, pointer modifiers,
    // nullability, address spaces, embedded storage classes. KWATTR is a
    // lone word, KWATTRARGS may take a parenthesized argument list.
    CP_K_KWATTR, CP_K_KWATTRARGS,
    CP_NTOKKINDS
};

enum {  // token flags
    CP_TF_BOL   = 1<<0,  // first token on its source line
    CP_TF_SPACE = 1<<1,  // whitespace precedes the token
};

struct cp_token {
    cp_str    text;
    int32_t   kind;
    int32_t   flags;
    int32_t   line;   // for macro output: the outermost invocation's line
    int32_t   col;    // byte column, 1-based
    int32_t   file;   // index into cp_unit.files
    int32_t   exp;    // 1 + index into cp_unit.expansions, 0 if none
    cp_macro *macro;  // outermost macro this token came from, or null
    cp_str    doc;    // comment block directly before the token, if any
};

enum {  // macro flags
    CP_MF_FUNCTION = 1<<0,  // function-like
    CP_MF_VARIADIC = 1<<1,  // last parameter is ... or name...
    CP_MF_PREDEF   = 1<<2,  // from cp_config.predefs
    CP_MF_UNDEF    = 1<<3,  // later #undef'd (or redefined)
};

struct cp_macro {
    cp_macro *next;    // in definition order
    cp_str    name;
    cp_str   *params;  // __VA_ARGS__ for a plain ...
    int32_t   nparams;
    int32_t   flags;
    int32_t   file;
    int32_t   line;
    cp_token *body;    // replacement list (kind is CP_IDENT for keywords)
    int32_t   nbody;
    int32_t   nuses;   // expansions, including nested ones
    cp_str    doc;     // comment block directly before the #define
};

// A top-level macro expansion in the main token stream. Nested expansions
// are folded into the outermost one that produced them.
struct cp_expansion {
    cp_macro *macro;
    int32_t   file, line, col;  // invocation site
    int32_t   tok, endtok;      // output tokens [tok, endtok)
};

struct cp_file {
    cp_str  path;     // as opened (search directory + name)
    cp_str  src;      // contents
    int32_t system;   // found in a system directory
    int32_t parent;   // file index that first included it, -1 for roots
};

struct cp_include {
    int32_t from, line;  // file index and line of the directive
    int32_t file;        // included file index, -1 if not found
    cp_str  spelled;     // as written, with the quotes or brackets
    int32_t next;        // #include_next
    int32_t skipped;     // not entered: #pragma once or include guard
};

struct cp_skip {         // a region dropped by #if/#ifdef/#elif/#else
    int32_t file, beg, end;  // lines beg..end inclusive (directives too)
};

struct cp_comment {
    int32_t file, line, col;
    cp_str  text;        // including the delimiters
};

// Node kinds. One node struct serves expressions (CP_E_), statements
// (CP_S_) and declarations (CP_D_). The comment on each kind says how its
// fields are used. Semantic fields (ty, ref, isconst, cval) are described
// at struct cp_node.
enum {
    CP_N_NONE,
    // expressions
    CP_E_IDENT,      // text; ref the declaration (CP_D_DECLARATOR or
                     //   CP_D_ENUMERATOR), null if undeclared
    CP_E_INT,        // text; ival value, flags CP_NF_UNSIGNED etc
    CP_E_FLOAT,      // text
    CP_E_CHAR,       // text; ival value
    CP_E_STRING,     // tokens tok..tok+ival-1 are the concatenated pieces
    CP_E_CALL,       // a callee, list arguments
    CP_E_INDEX,      // a[b]
    CP_E_MEMBER,     // a.text or a->text (op CP_DOT or CP_ARROW); ref the
                     //   member's CP_D_DECLARATOR
    CP_E_POSTFIX,    // a op, op CP_INC or CP_DEC
    CP_E_UNARY,      // op a; op is a punctuator, or CP_K_REAL, CP_K_IMAG,
                     //   CP_K_EXTENSION
    CP_E_LABELADDR,  // &&text (GNU); ref the label statement
    CP_E_SIZEOF,     // sizeof a, or sizeof(type) when a is null
    CP_E_ALIGNOF,    // like sizeof; spelling from tok
    CP_E_CAST,       // (type)a
    CP_E_COMPOUND,   // (type){...}: a is a CP_E_INITLIST
    CP_E_BINARY,     // a op b
    CP_E_ASSIGN,     // a op b, op CP_ASSIGN, CP_ADDEQ, ...
    CP_E_COND,       // a ? b : c; b null for GNU a ?: c
    CP_E_COMMA,      // a, b
    CP_E_GENERIC,    // _Generic(a, list): list of CP_E_ASSOC; ref the
                     //   selected CP_E_ASSOC
    CP_E_ASSOC,      // type: a; type null means default
    CP_E_STMTEXPR,   // ({ ... }): a is the CP_S_COMPOUND
    CP_E_TYPEARG,    // a type used as a builtin's argument (va_arg, ...)
    CP_E_INITLIST,   // { list }: list of CP_E_INIT
    CP_E_INIT,       // b designators (list via next), a value
    CP_E_DFIELD,     // designator .text (or GNU "text:")
    CP_E_DINDEX,     // designator [a] or [a ... b]
    CP_E_BLOCK,      // Clang block literal: type the function type, a body
    // statements
    CP_S_COMPOUND,   // { list }
    CP_S_EXPR,       // a;
    CP_S_IF,         // if (a) b else c
    CP_S_SWITCH,     // switch (a) b
    CP_S_WHILE,      // while (a) b
    CP_S_DO,         // do b while (a);
    CP_S_FOR,        // for (d; a; c) b; d is a declaration or CP_S_EXPR
    CP_S_GOTO,       // goto text; ref the label statement
    CP_S_GOTOPTR,    // goto *a; (GNU)
    CP_S_CONTINUE,
    CP_S_BREAK,
    CP_S_RETURN,     // return a;
    CP_S_LABEL,      // text: a; a may be null (label before a
                     //   declaration or at the end of a block); attrs
    CP_S_CASE,       // case a: c, or case a ... b: c; c may be null
    CP_S_DEFAULT,    // default: c
    CP_S_NULL,       // ;
    CP_S_ASM,        // GNU asm: a template (CP_E_STRING), b outputs,
                     //   c inputs, d clobbers (lists of CP_S_ASMOPERAND or
                     //   CP_E_STRING), list goto labels (CP_E_IDENT); ival
                     //   number of colon sections; flags CP_NF_VOLATILE..
    CP_S_ASMOPERAND, // [text] b(a): b constraint string, a expression
    CP_S_MSASM,      // MSVC __asm: raw tokens tok..ival-1
    CP_S_TRY,        // __try a, then b: CP_S_EXCEPT or CP_S_FINALLY
    CP_S_EXCEPT,     // __except (a) b
    CP_S_FINALLY,    // __finally b
    CP_S_LEAVE,      // __leave;
    CP_S_ATTR,       // attribute statement: attrs; (fallthrough etc)
    CP_S_LOCALLABEL, // __label__ list; list of CP_E_IDENT
    // declarations
    CP_D_DECL,       // flags storage class; type base type from the
                     //   declaration specifiers; attrs specifier
                     //   attributes; list CP_D_DECLARATOR (may be empty)
    CP_D_DECLARATOR, // text name (may be empty); type the full syntactic
                     //   type, ending in the CP_D_DECL's base type; ty its
                     //   semantic type; a initializer; b bit-field width;
                     //   attrs trailing attributes and asm labels; members:
                     //   offset, bitoffset, bitwidth
    CP_D_FUNC,       // function definition: flags, type, attrs as
                     //   CP_D_DECL; list the one CP_D_DECLARATOR; b K&R
                     //   parameter declarations (list via next); a body
    CP_D_ENUMERATOR, // text = a; attrs; cval its value, ty its type
    CP_D_STATICASSERT, // _Static_assert(a, b); spelling from tok
    CP_D_EMPTY,      // stray ; at file scope
    CP_D_ERROR,      // unparseable region: tokens tok..ival-1
    CP_NNODEKINDS
};

enum {  // node flags
    // storage classes and function specifiers (CP_D_DECL, CP_D_FUNC)
    CP_SC_TYPEDEF      = 1<<0,
    CP_SC_EXTERN       = 1<<1,
    CP_SC_STATIC       = 1<<2,
    CP_SC_AUTO         = 1<<3,
    CP_SC_REGISTER     = 1<<4,
    CP_SC_THREAD_LOCAL = 1<<5,
    CP_SC_CONSTEXPR    = 1<<6,
    CP_SC_INLINE       = 1<<7,
    CP_SC_NORETURN     = 1<<8,
    CP_SC_FORCEINLINE  = 1<<9,
    CP_SC_EXTENSION    = 1<<10,  // __extension__ prefix
    // integer literal suffixes (CP_E_INT)
    CP_NF_UNSIGNED     = 1<<0,
    CP_NF_LONG         = 1<<1,
    CP_NF_LONGLONG     = 1<<2,
    CP_NF_OVERFLOW     = 1<<3,
    CP_NF_BITINT       = 1<<4,   // wb suffix
    CP_NF_SIZE         = 1<<5,   // z suffix (C2y)
    // GNU asm qualifiers (CP_S_ASM)
    CP_NF_VOLATILE     = 1<<0,
    CP_NF_INLINE       = 1<<1,
    CP_NF_GOTO         = 1<<2,
};

struct cp_node {
    cp_node *next;      // next sibling in a list
    int32_t  kind;
    int32_t  op;        // operator token kind
    int32_t  flags;
    int32_t  tok;       // index of first token
    cp_str   text;
    int64_t  ival;
    cp_node *a, *b, *c, *d;
    cp_node *list;
    cp_type *type;      // syntactic type, as written
    cp_attr *attrs;

    // semantic information
    cp_type *ty;        // expression type before decay (as typeof sees
                        //   it); declared type of a declarator; null if
                        //   it could not be determined
    cp_node *ref;       // declaration an identifier/member/goto refers to
    int64_t  cval;      // value when isconst
    int32_t  isconst;   // 1 if an integer constant expression: cval holds
                        //   the value, truncated to the expression type
                        //   (pointer-typed when it is a known address
                        //   like (T *)0); 2 if an arithmetic floating
                        //   constant (fval)
    int32_t  bitwidth;  // bit-field width, or -1 for ordinary members
    int64_t  offset;    // member byte offset (bit-fields: bitoffset/8)
    int64_t  bitoffset; // member bit offset from the start of the struct
    int32_t  endtok;    // one past the last token of the node
    int32_t  eflags;    // CP_EF_* for expressions
    cp_sym  *sym;       // entity declared or referenced (identifiers,
                        //   declarators, enumerators, labels, functions)
    cp_str   sval;      // decoded string literal contents: the element
                        //   bytes, without the terminator, little-endian
                        //   for wide strings
    double   fval;      // floating value when isconst == 2
    cp_symref *useref_; // private
};

enum {  // expression flags (cp_node.eflags)
    CP_EF_LVALUE     = 1<<0,
    CP_EF_SIDEEFFECT = 1<<1,  // assigns, calls, increments, volatile, asm
    CP_EF_IMPLICIT   = 1<<2,  // call to an implicitly declared function
    CP_EF_ANONMEMBER = 1<<3,  // on a member declaration: an anonymous
                              //   struct or union member (C11, or the
                              //   Microsoft tagged form on LLP64)
};

// Entities: every variable, function, parameter, typedef, enumeration
// constant, tag and label. All declarations of one entity share a cp_sym.
enum {
    CP_SYM_VAR, CP_SYM_FUNC, CP_SYM_PARAM, CP_SYM_TYPEDEF,
    CP_SYM_ENUMCONST, CP_SYM_TAG, CP_SYM_LABEL,
};
enum { CP_LINK_NONE, CP_LINK_INTERNAL, CP_LINK_EXTERNAL };
enum {  // symbol flags
    CP_YF_DEFINED   = 1<<0,  // has a definition (body, initializer, or
                             //   file-scope tentative definition)
    CP_YF_READ      = 1<<1,  // value used somewhere
    CP_YF_WRITTEN   = 1<<2,  // assigned, incremented, or initialized
    CP_YF_ADDRTAKEN = 1<<3,  // operand of unary &
    CP_YF_CALLED    = 1<<4,
    CP_YF_IMPLICIT  = 1<<5,  // implicitly declared (C89 call, builtin)
    CP_YF_STATICDUR = 1<<6,  // static storage duration
    CP_YF_THREADDUR = 1<<7,  // thread storage duration
    CP_YF_INLINE    = 1<<8,
};
enum {  // reference flags (cp_symref.flags)
    CP_RF_DECL = 1<<0, CP_RF_READ = 1<<1, CP_RF_WRITE = 1<<2,
    CP_RF_ADDR = 1<<3, CP_RF_CALL = 1<<4,
};

struct cp_symref {
    cp_symref *next;
    cp_node   *node;   // the identifier, declarator, or call expression
    cp_sym    *sym;    // for call lists: the other end of the edge
    int32_t    flags;
};

struct cp_sym {
    cp_sym    *next;     // all symbols, in order of first declaration
    cp_str     name;
    int32_t    kind;
    int32_t    linkage;
    int32_t    flags;
    int32_t    depth;    // scope depth: 0 file scope
    cp_type   *ty;       // semantic type (composite of all declarations)
    cp_node   *decl;     // first declaration
    cp_node   *def;      // definition: CP_D_FUNC, initialized declarator,
                         //   enumerator, label statement, or tag's type
    cp_symref *decls;    // every declaration, in order
    cp_symref *refs;     // every use, in order
    int32_t    nrefs;
    cp_sym    *func;     // enclosing function for block-scope entities
    cp_symref *calls;    // functions: direct calls in the body
    cp_symref *callers;  // functions: direct calls to this one
    cp_symref *lastref_, *lastdecl_, *lastcall_, *lastcaller_;  // private
};

// Type kinds. Syntactic types are as written: a declaration's specifiers
// produce one base type, and each declarator wraps it in POINTER, ARRAY
// and FUNC layers. Semantic types are canonical and complete with size
// and alignment; each syntactic type points at its meaning through sem.
enum {
    CP_T_NONE,
    CP_T_BUILTIN,   // syntactic: bits (CP_B_*); semantic: basic (CP_BT_*)
    CP_T_STRUCT,    // name tag; members list of CP_D_DECL/STATICASSERT
    CP_T_UNION,     //   (when flags has CP_TF_BODY); attrs
    CP_T_ENUM,      // name tag; members CP_D_ENUMERATOR; base fixed or
                    //   (semantic) compatible integer type
    CP_T_NAME,      // typedef name (syntactic only)
    CP_T_TYPEOF,    // typeof(expr) or typeof(base) (syntactic only)
    CP_T_ATOMIC,    // _Atomic(base) (syntactic only)
    CP_T_BITINT,    // _BitInt(expr); semantic: len bits; unsigned in bits
    CP_T_AUTOTYPE,  // __auto_type (syntactic only)
    CP_T_IMPLICIT,  // no type specifier: implicit int, C23 auto
    CP_T_POINTER,   // base pointee
    CP_T_BLOCKPTR,  // Clang block pointer (^)
    CP_T_ARRAY,     // base element; expr size; semantic: len (-1 unknown)
    CP_T_FUNC,      // base return type; params list of CP_D_DECL, or
                    //   CP_E_IDENT for a K&R identifier list; semantic:
                    //   args/nargs parameter types
    CP_T_VECTOR,    // GNU vector_size: base element, size bytes
};

enum {  // builtin type keyword bits (syntactic)
    CP_B_VOID = 1<<0, CP_B_CHAR = 1<<1, CP_B_SHORT = 1<<2, CP_B_INT = 1<<3,
    CP_B_LONG = 1<<4, CP_B_LONGLONG = 1<<5, CP_B_SIGNED = 1<<6,
    CP_B_UNSIGNED = 1<<7, CP_B_FLOAT = 1<<8, CP_B_DOUBLE = 1<<9,
    CP_B_BOOL = 1<<10, CP_B_COMPLEX = 1<<11, CP_B_IMAGINARY = 1<<12,
    CP_B_INT128 = 1<<13, CP_B_FLOAT128 = 1<<14, CP_B_FLOAT80 = 1<<15,
    CP_B_IBM128 = 1<<16, CP_B_FLOAT16 = 1<<17, CP_B_FLOAT32 = 1<<18,
    CP_B_FLOAT64 = 1<<19, CP_B_FLOAT32X = 1<<20, CP_B_FLOAT64X = 1<<21,
    CP_B_FLOAT128X = 1<<22, CP_B_BF16 = 1<<23, CP_B_FP16 = 1<<24,
    CP_B_DEC32 = 1<<25, CP_B_DEC64 = 1<<26, CP_B_DEC128 = 1<<27,
    CP_B_INT8 = 1<<28, CP_B_INT16 = 1<<29, CP_B_INT32 = 1<<30,
    CP_B_INT64 = (int32_t)(1u<<31),
};

enum {  // semantic builtin types (cp_type.basic)
    CP_BT_VOID, CP_BT_BOOL, CP_BT_CHAR, CP_BT_SCHAR, CP_BT_UCHAR,
    CP_BT_SHORT, CP_BT_USHORT, CP_BT_INT, CP_BT_UINT, CP_BT_LONG,
    CP_BT_ULONG, CP_BT_LLONG, CP_BT_ULLONG, CP_BT_INT128, CP_BT_UINT128,
    CP_BT_FLOAT16, CP_BT_BF16, CP_BT_FLOAT, CP_BT_DOUBLE, CP_BT_LDOUBLE,
    CP_BT_FLOAT32, CP_BT_FLOAT64, CP_BT_FLOAT32X, CP_BT_FLOAT64X,
    CP_BT_FLOAT128, CP_BT_IBM128, CP_BT_DEC32, CP_BT_DEC64, CP_BT_DEC128,
    CP_BT_NULLPTR,  // C23 nullptr_t
    CP_NBASIC
};

enum {  // qualifiers
    CP_Q_CONST = 1<<0, CP_Q_VOLATILE = 1<<1, CP_Q_RESTRICT = 1<<2,
    CP_Q_ATOMIC = 1<<3,
};

enum {  // type flags
    CP_TF_BODY       = 1<<0,  // struct/union/enum with a { } body
    CP_TF_VARIADIC   = 1<<1,  // function ends in ...
    CP_TF_KR         = 1<<2,  // function has a K&R identifier list
    CP_TF_STATIC     = 1<<3,  // array [static n]
    CP_TF_VLASTAR    = 1<<4,  // array [*]
    CP_TF_UNQUAL     = 1<<5,  // typeof_unqual
    CP_TF_COMPLEX    = 1<<6,  // semantic: _Complex builtin
    CP_TF_VLA        = 1<<7,  // semantic: variable length array
    CP_TF_NOPROTO    = 1<<8,  // semantic: function without a prototype
    CP_TF_INCOMPLETE = 1<<9,  // semantic: incomplete struct/union/enum/array
    CP_TF_PACKED     = 1<<10, // semantic: packed struct
    CP_TF_FLEX       = 1<<11, // semantic: struct ends in a flexible array
    CP_TF_SDCCBIT    = 1<<12, // _Bool spelled __bit (SDCC bit storage)
    CP_TF_NORETURN   = 1<<13, // semantic function type reached through a
                              //   noreturn pointer: GCC makes it a variant
                              //   type C cannot spell (Clang does not)
    CP_TF_PROTOTAG   = 1<<14, // semantic tag first declared in a parameter
                              //   list: no code outside can name it
    CP_TF_ABI        = 1<<15, // semantic function type with ms_abi or
                              //   sysv_abi, which GCC keeps in the type
};

struct cp_type {
    int32_t   kind;
    int32_t   quals;
    int32_t   flags;
    int32_t   bits;     // syntactic builtin keywords
    int32_t   basic;    // semantic builtin type
    int32_t   tok;
    cp_str    name;
    cp_type  *base;
    cp_node  *expr;
    cp_node  *members;
    cp_node  *params;
    cp_attr  *attrs;

    // semantic information
    cp_type  *sem;      // the canonical semantic type (self if semantic)
    int64_t   size;     // in bytes; -1 if incomplete or variable
    int64_t   align;
    int64_t   len;      // array length, _BitInt width; -1 if unknown
    cp_type **args;     // function parameter types
    int32_t   nargs;
    cp_type  *unqual;   // the same type without qualifiers
    cp_type **variants_;  // private: qualified versions
};

// Attributes are kept as token ranges: tokens beg..end-1 of the unit,
// including the introducing keyword or brackets. The ones that affect
// layout (aligned, packed, vector_size, mode) are applied by the analyzer.
enum {
    CP_A_GNU,       // __attribute__((...))
    CP_A_DECLSPEC,  // __declspec(...)
    CP_A_STD,       // [[...]]
    CP_A_KEYWORD,   // CP_K_KWATTR/KWATTRARGS word, maybe with (args)
    CP_A_MACRO,     // undeclared name where only an annotation fits:
                    // a macro from a missing header (WINAPI, UNUSED)
    CP_A_ALIGNAS,   // _Alignas(...) / alignas(...)
    CP_A_ASMLABEL,  // asm("symbol") after a declarator
};

struct cp_attr {
    cp_attr *next;
    int32_t  kind;
    int32_t  before;  // appeared before a pointer's * rather than after
    int32_t  aftertype;  // in specifiers, came after the type: int WINAPI
    int32_t  aftername;  // right after a declarator's name: f [[x]] (int)
    int32_t  afterwidth;  // after a bit-field's width: int x : 3 PACKED
    int32_t  leading;  // an annotation macro before the type
    int32_t  beg;
    int32_t  end;
};

struct cp_pragma {
    int32_t tok;     // the pragma precedes this token
    int32_t isop;    // 1 for _Pragma(...)/__pragma(...), 0 for #pragma
    int32_t line;
    int32_t file;
    cp_str  text;    // the text after "#pragma", or the whole operator
};

struct cp_error {
    cp_error *next;
    int32_t   warning;  // 0 error, 1 warning
    int32_t   tok;      // token index, -1 if from the preprocessor
    int32_t   line;
    int32_t   col;
    cp_str    file;
    cp_str    msg;
};

enum {  // targets for type sizes and layout
    CP_TARGET_LP64,   // x86-64, AArch64 Linux/BSD/macOS (default)
    CP_TARGET_LLP64,  // 64-bit Windows
    CP_TARGET_ILP32,  // i386 Linux
};

struct cp_config {
    // Read a whole file, allocating its contents from perm. Return a null
    // data pointer if it does not exist. If null, #include always fails.
    cp_str  (*readfile)(void *ctx, cp_str path, cp_arena *perm);
    void     *ctx;
    cp_str   *quotedirs;    // searched for "..." only (-iquote)
    int32_t   nquotedirs;
    cp_str   *includedirs;  // searched for "..." and <...> (-I)
    int32_t   nincludedirs;
    cp_str   *systemdirs;   // searched last (-isystem, system headers)
    int32_t   nsystemdirs;
    cp_str    predefs;      // source processed first, e.g. #define lines
    int32_t   target;       // CP_TARGET_*
    int32_t   nopp;         // skip macro expansion and directives other
                            //   than line markers and #pragma
    int32_t   trigraphs;    // translate ??= ??/ and the rest, as ISO C
                            //   before C23 requires (cc -std=c99)
    int64_t   time;         // seconds since 1970 for __DATE__, __TIME__
                            //   and __TIMESTAMP__, shown as UTC (add the
                            //   zone offset for local time); 0 means
                            //   2000-01-01, for reproducible output
};

struct cp_unit {
    cp_node   *decls;     // external declarations
    cp_token  *toks;      // tokens after preprocessing
    ptrdiff_t  ntoks;     //   includes the final CP_EOF
    cp_file   *files;     // files[0] is the main file
    int32_t    nfiles;
    cp_sym    *syms;      // every entity, in order of first declaration
    cp_expansion *expansions;
    int32_t    nexpansions;
    cp_include *includes;
    int32_t    nincludes;
    cp_skip   *skips;
    int32_t    nskips;
    cp_comment *comments;
    int32_t    ncomments;
    cp_macro  *macros;    // every definition, in order
    cp_pragma *pragmas;   // in source order
    ptrdiff_t  npragmas;
    cp_error  *errors;    // in order of discovery
    int32_t    nerrors;   // count of errors (not warnings)
    int32_t    nwarnings;
    int32_t    oom;
    int32_t    target;
};

#ifndef CPARSE_MAXDEPTH
#  define CPARSE_MAXDEPTH 1000  // nesting limit, bounds stack use
#endif
#ifndef CPARSE_MAXERRORS
#  define CPARSE_MAXERRORS 100  // diagnostics kept, per kind (all are counted)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Preprocess, parse and analyze a translation unit. The source and any
// files read must stay alive as long as the unit: tokens point into them.
// The config may be null.
cp_unit *cp_parse(cp_arena *, cp_str src, cp_str filename, cp_config *);

// Regenerate C source from a unit. The output reparses to the same tree.
// Returns a null string if the arena runs out.
cp_str cp_print(cp_arena *, cp_unit *);

// Preprocessed token stream as text, like cc -E without line markers.
cp_str cp_print_tokens(cp_arena *, cp_unit *);

// Like cp_print, but after every statement and declaration adds
// _Static_asserts stating the computed type of each subexpression (via
// __builtin_types_compatible_p and __typeof__) and the value of each
// integer constant. Compiling the result with GCC or Clang verifies the
// analysis against the compiler; it is also a handy way to see the types.
cp_str cp_print_checked(cp_arena *, cp_unit *);

// Readable indented dump of the tree with semantic types.
cp_str cp_dump(cp_arena *, cp_unit *);

// Render a type, syntactic or semantic, as C ("unsigned long (*)[3]").
cp_str cp_typename(cp_arena *, cp_unit *, cp_type *);

// Names for kinds, e.g. "CALL", "->", "while".
cp_str cp_nodename(int32_t kind);
cp_str cp_tokname(int32_t kind);

// ---- Convenience
//
// Short answers to common questions. None of these allocate unless given
// an arena, and all accept null where a node, symbol or type may be
// missing. A few examples:
//
//     // every function defined in the main file, with its signature
//     for (cp_sym *f = 0; (f = cp_nextsym(u, f, CP_KIND(CP_SYM_FUNC), CP_SEL_MAIN|CP_SEL_DEFINED));)
//         printf("%s\n", cp_signature(&a, u, f).data);
//
//     // every call to malloc, with its location and argument
//     for (cp_symref *r = cp_callsto(u, CP_STR("malloc")); r; r = r->next)
//         printf("%s: %s\n", cp_locstr(&a, u, r->node).data,
//                cp_nodestr(&a, u, r->node->list).data);
//
//     // a struct's fields with offsets (anonymous members flattened)
//     cp_field *f;
//     int32_t n = cp_fields(&a, cp_findtag(u, CP_STR("point"))->ty, &f);
//
// Strings returned in an arena are also null-terminated, so .data can be
// handed to printf.

// Where a node starts: file, line and byte column (1-based). For code
// that came out of a macro, the place the macro was invoked.
typedef struct { cp_str file; int32_t line, col; } cp_loc;
cp_loc cp_locof(cp_unit *, cp_node *);
cp_str cp_locstr(cp_arena *, cp_unit *, cp_node *);  // "file.c:12:5"

// The source text a node spans, exactly as written; a null string when it
// does not lie in one stretch of one file (it came from a macro).
cp_str cp_srcof(cp_unit *, cp_node *);

// The comment block directly before a node (its doc comment), if any.
cp_str cp_docof(cp_unit *, cp_node *);

// The same for a symbol's first declaration, tags included (which have
// no declaration node).
cp_loc cp_symloc(cp_unit *, cp_sym *);
cp_str cp_symdoc(cp_unit *, cp_sym *);

// Whether a node lies in the main file rather than a header, and the
// outermost macro that produced it (null if written directly).
int32_t   cp_inmainfile(cp_unit *, cp_node *);
cp_macro *cp_macroof(cp_unit *, cp_node *);

// A node regenerated as C: an expression, statement or declaration.
cp_str cp_nodestr(cp_arena *, cp_unit *, cp_node *);

// File-scope entities by name: variables, functions, typedefs and
// enumeration constants; tags (struct, union, enum); macro definitions
// (the last one seen).
cp_sym   *cp_findsym(cp_unit *, cp_str name);
cp_sym   *cp_findtag(cp_unit *, cp_str name);
cp_macro *cp_macrodef(cp_unit *, cp_str name);

// Step through the symbols after prev (from the first when prev is null)
// whose kind is in kinds (a mask of CP_KIND(CP_SYM_*), 0 for all) and that
// pass every CP_SEL_* in sel. Null at the end.
#define CP_KIND(k) (1 << (k))
enum {
    CP_SEL_MAIN     = 1<<0,  // declared in the main file
    CP_SEL_DEFINED  = 1<<1,  // has a definition
    CP_SEL_FILE     = 1<<2,  // at file scope
    CP_SEL_EXTERNAL = 1<<3,  // external linkage
    CP_SEL_USED     = 1<<4,  // used at least once
};
cp_sym *cp_nextsym(cp_unit *, cp_sym *prev, int32_t kinds, int32_t sel);

// A declaration of name with type t, as C: cp_declstr(a, u, t, "fp") gives
// "int (*fp)(char *)". A function symbol's declaration as written, with
// its parameter names: "static int add(int x, int y)".
cp_str cp_declstr(cp_arena *, cp_unit *, cp_type *, cp_str name);
cp_str cp_signature(cp_arena *, cp_unit *, cp_sym *func);

// The named fields of a struct or union, anonymous members flattened into
// their parent, with offsets from the start of t. Returns the count and
// points *out at an array in the arena; -1 if the arena runs out.
typedef struct {
    cp_str   name;
    cp_type *type;
    int64_t  offset;     // bytes
    int64_t  bitoffset;  // bits, for bit-fields
    int32_t  bitwidth;   // -1 if not a bit-field
    cp_node *decl;       // the declarator
} cp_field;
int32_t cp_fields(cp_arena *, cp_type *, cp_field **out);

// The constants of an enum type, in order.
typedef struct { cp_str name; int64_t value; cp_node *decl; } cp_enumconst;
int32_t cp_enumconsts(cp_arena *, cp_type *, cp_enumconst **out);

// Visit every node under root, parents before children. The visitor
// returns 1 to go into the node's children, 0 to skip them, -1 to stop.
typedef int32_t (*cp_visitor)(void *ctx, cp_node *node, cp_node *parent);
void cp_walk(cp_node *root, cp_visitor, void *ctx);
void cp_walkunit(cp_unit *, cp_visitor, void *ctx);

// The function a call expression calls directly (null for calls through
// pointers), and every direct call of the named function: a list whose
// node is the call expression and sym the calling function.
cp_sym    *cp_callee(cp_node *call);
cp_symref *cp_callsto(cp_unit *, cp_str name);

// Type questions. cp_typeis is true when t has all the properties in
// what: cp_typeis(t, CP_IS_INTEGER|CP_IS_UNSIGNED). cp_strip gives the
// canonical type without typedefs or qualifiers; cp_target what a pointer
// points to, an array or vector holds, or a function returns.
enum {
    CP_IS_VOID     = 1<<0,  CP_IS_INTEGER  = 1<<1,  CP_IS_FLOATING = 1<<2,
    CP_IS_ARITH    = 1<<3,  CP_IS_SCALAR   = 1<<4,  CP_IS_POINTER  = 1<<5,
    CP_IS_ARRAY    = 1<<6,  CP_IS_FUNC     = 1<<7,  CP_IS_STRUCT   = 1<<8,
    CP_IS_UNION    = 1<<9,  CP_IS_RECORD   = 1<<10, CP_IS_ENUM     = 1<<11,
    CP_IS_SIGNED   = 1<<12, CP_IS_UNSIGNED = 1<<13, CP_IS_CONST    = 1<<14,
    CP_IS_VOLATILE = 1<<15, CP_IS_COMPLETE = 1<<16, CP_IS_VECTOR   = 1<<17,
    CP_IS_VARIADIC = 1<<18, CP_IS_BOOL     = 1<<19,
};
int32_t  cp_typeis(cp_type *, int32_t what);
cp_type *cp_strip(cp_type *);
cp_type *cp_target(cp_type *);

// The value of an integer constant expression; false if e is not one.
int32_t cp_constint(cp_node *e, int64_t *value);

// A diagnostic as a compiler prints it: "file.c:3:7: error: message".
cp_str cp_errorstr(cp_arena *, cp_error *);

#ifdef CPARSE_HOSTED
// ---- Hosted helpers (define CPARSE_HOSTED with CPARSE_IMPLEMENTATION)
//
// These do I/O, through the C library and POSIX popen, so the core above
// stays free of it. They make parsing a file as a compiler would a single
// call:
//
//     cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
//     cp_unit *u = cp_parsefile(&a, "main.c", argc-1, argv+1);
//
// args are compiler-style options: -I, -isystem, -iquote, -D, -U,
// -include, -std=, -O, -m, -f, -trigraphs, -nostdinc, plus --cc CC (the
// compiler asked for predefined macros and include directories; "none" to
// skip) and --target lp64|llp64|ilp32. Unknown options are ignored.

// An arena of cap bytes from malloc; a null arena on failure.
cp_arena cp_newarena(ptrdiff_t cap);

// A configuration from compiler-style options, as described above.
cp_config *cp_hostconfig(cp_arena *, int argc, char **argv);

// Read and parse a file. Null if it cannot be read or the arena is full.
cp_unit *cp_parsefile(cp_arena *, char const *path, int argc, char **argv);

// Read a whole file into the arena (null-terminated); a null string if
// it cannot be read. Usable as cp_config.readfile.
cp_str cp_readfile(void *ctx, cp_str path, cp_arena *);
#endif

#ifdef __cplusplus
}
#endif
#endif  // CPARSE_H

// ======================================================================
//                            IMPLEMENTATION
// ======================================================================
#ifdef CPARSE_IMPLEMENTATION
#include <setjmp.h>
#ifdef CPARSE_HOSTED
#  include <stdio.h>
#  include <stdlib.h>
#  include <time.h>
#endif

// Short names for the implementation only; undefined again at the end.
#define i8  int8_t
#define i32 int32_t
#define i64 int64_t
#define u8  uint8_t
#define u32 uint32_t
#define u64 uint64_t
#define iz  ptrdiff_t
#define uz  size_t
#define b32 int32_t

// Object sizes and alignments are clamped to these so that layout
// arithmetic (in bits) cannot overflow on hostile input. No real object
// comes close: GCC itself rejects alignments above 2^28.
#define CP_MAXSIZE  ((i64)1 << 48)
#define CP_MAXALIGN ((i64)1 << 28)

#if defined(__GNUC__) || defined(__clang__)
#  define cp_alignof(t) __alignof__(t)
#  ifdef CPARSE_DEBUG
#    define cp_assert(c) while (!(c)) __builtin_trap()
#  else
#    define cp_assert(c) while (!(c)) __builtin_unreachable()
#  endif
#elif defined(_MSC_VER)
#  define cp_alignof(t) __alignof(t)
#  ifdef CPARSE_DEBUG
#    define cp_assert(c) do if (!(c)) __debugbreak(); while (0)
#  else
#    define cp_assert(c) __assume(c)
#  endif
#else
#  define cp_alignof(t) 16
#  define cp_assert(c) (void)0
#endif

#define cp_countof(a)   ((iz)(sizeof(a) / sizeof(*(a))))
#define cp_new(a, n, t) (t *)cp_alloc(a, n, (iz)sizeof(t), (iz)cp_alignof(t))
#define S(s)            CP_STR(s)

typedef struct {
    cp_arena *arena;
    jmp_buf  *oom;   // longjmp(oom, 1) when the arena is exhausted
} cp_mem;

static void cp_memset0(void *p, iz n)
{
    char *d = (char *)p;
    for (iz i = 0; i < n; i++) d[i] = 0;
}

static void cp_memcpy(void *dst, void *src, iz n)
{
    char *d = (char *)dst, *s = (char *)src;
    for (iz i = 0; i < n; i++) d[i] = s[i];
}

static void *cp_alloc(cp_mem *m, iz count, iz size, iz align)
{
    cp_arena *a = m->arena;
    iz pad = -(uintptr_t)a->beg & (uintptr_t)(align - 1);
    if (count < 0 || count >= (a->end - a->beg - pad)/size) {
        longjmp(*m->oom, 1);
    }
    char *r = a->beg + pad;
    a->beg += pad + count*size;
    cp_memset0(r, count*size);
    return r;
}

static cp_str cp_span(char *beg, char *end)
{
    cp_str r = {0};
    r.data = beg;
    r.len  = beg ? end - beg : 0;
    return r;
}

static b32 cp_equals(cp_str a, cp_str b)
{
    if (a.len != b.len) return 0;
    for (iz i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) return 0;
    }
    return 1;
}

static b32 cp_startswith(cp_str s, cp_str prefix)
{
    return s.len >= prefix.len && cp_equals(cp_span(s.data, s.data+prefix.len), prefix);
}

static u64 cp_hash(cp_str s)
{
    u64 h = 0x100;
    for (iz i = 0; i < s.len; i++) {
        h ^= (u64)(s.data[i] & 255);
        h *= 1111111111111111111u;
    }
    return h;
}

static cp_str cp_copy(cp_mem *m, cp_str s)
{
    cp_str r = s;
    r.data = cp_new(m, s.len+1, char);
    if (s.len) cp_memcpy(r.data, s.data, s.len);
    return r;
}

static cp_str cp_concat(cp_mem *m, cp_str a, cp_str b)
{
    cp_str r = {0};
    r.len  = a.len + b.len;
    r.data = cp_new(m, r.len+1, char);
    if (a.len) cp_memcpy(r.data, a.data, a.len);
    if (b.len) cp_memcpy(r.data+a.len, b.data, b.len);
    return r;
}

// Hash trie (nullprogram.com/blog/2023/09/30/). A null map is empty, and
// passing no memory makes upsert a pure lookup.
typedef struct cp_map cp_map;
struct cp_map {
    cp_map *child[4];
    cp_str  key;
    void   *val;
    i64     ival;
};

static cp_map *cp_upsert(cp_map **m, cp_str key, cp_mem *perm)
{
    for (u64 h = cp_hash(key); *m; h <<= 2) {
        if (cp_equals(key, (*m)->key)) {
            return *m;
        }
        m = &(*m)->child[h>>62];
    }
    if (!perm) {
        return 0;
    }
    *m = cp_new(perm, 1, cp_map);
    (*m)->key = key;
    return *m;
}

static b32 cp_isdigit(i32 c)  { return c>='0' && c<='9'; }
static b32 cp_isxdigit(i32 c) { return cp_isdigit(c) || ((c|32)>='a' && (c|32)<='f'); }
static b32 cp_isalpha(i32 c)  { return (c|32)>='a' && (c|32)<='z'; }
static b32 cp_isident(i32 c)
{
    return cp_isalpha(c) || cp_isdigit(c) || c=='_' || c=='$' || c>=0x80;
}

// ----------------------------------------------------------------------
// Keywords

typedef struct { char *name; i32 kind; } cp_kwdef;

static const cp_kwdef cp_keywords[] = {
    {"auto", CP_K_AUTO}, {"break", CP_K_BREAK}, {"case", CP_K_CASE},
    {"char", CP_K_CHAR}, {"const", CP_K_CONST}, {"__const", CP_K_CONST},
    {"__const__", CP_K_CONST}, {"continue", CP_K_CONTINUE},
    {"default", CP_K_DEFAULT}, {"do", CP_K_DO}, {"double", CP_K_DOUBLE},
    {"else", CP_K_ELSE}, {"enum", CP_K_ENUM}, {"extern", CP_K_EXTERN},
    {"float", CP_K_FLOAT}, {"for", CP_K_FOR}, {"goto", CP_K_GOTO},
    {"if", CP_K_IF}, {"inline", CP_K_INLINE}, {"__inline", CP_K_INLINE},
    {"__inline__", CP_K_INLINE}, {"int", CP_K_INT}, {"long", CP_K_LONG},
    {"register", CP_K_REGISTER}, {"restrict", CP_K_RESTRICT},
    {"__restrict", CP_K_RESTRICT}, {"__restrict__", CP_K_RESTRICT},
    {"return", CP_K_RETURN}, {"short", CP_K_SHORT}, {"signed", CP_K_SIGNED},
    {"__signed", CP_K_SIGNED}, {"__signed__", CP_K_SIGNED},
    {"sizeof", CP_K_SIZEOF}, {"static", CP_K_STATIC},
    {"struct", CP_K_STRUCT}, {"switch", CP_K_SWITCH},
    {"typedef", CP_K_TYPEDEF}, {"union", CP_K_UNION},
    {"unsigned", CP_K_UNSIGNED}, {"void", CP_K_VOID},
    {"volatile", CP_K_VOLATILE}, {"__volatile", CP_K_VOLATILE},
    {"__volatile__", CP_K_VOLATILE}, {"while", CP_K_WHILE},
    {"_Alignas", CP_K_ALIGNAS}, {"alignas", CP_K_ALIGNAS},
    {"_Alignof", CP_K_ALIGNOF}, {"alignof", CP_K_ALIGNOF},
    {"__alignof", CP_K_ALIGNOF}, {"__alignof__", CP_K_ALIGNOF},
    {"_Atomic", CP_K_ATOMIC}, {"_Bool", CP_K_BOOL},
    {"_Complex", CP_K_COMPLEX}, {"__complex", CP_K_COMPLEX},
    {"__complex__", CP_K_COMPLEX}, {"_Generic", CP_K_GENERIC},
    {"_Imaginary", CP_K_IMAGINARY}, {"_Noreturn", CP_K_NORETURN},
    {"_Static_assert", CP_K_STATIC_ASSERT},
    {"static_assert", CP_K_STATIC_ASSERT},
    {"_Thread_local", CP_K_THREAD_LOCAL},
    {"thread_local", CP_K_THREAD_LOCAL}, {"__thread", CP_K_THREAD_LOCAL},
    {"constexpr", CP_K_CONSTEXPR}, {"typeof", CP_K_TYPEOF},
    {"__typeof", CP_K_TYPEOF}, {"__typeof__", CP_K_TYPEOF},
    {"typeof_unqual", CP_K_TYPEOF_UNQUAL},
    {"__typeof_unqual", CP_K_TYPEOF_UNQUAL},
    {"__typeof_unqual__", CP_K_TYPEOF_UNQUAL},
    {"_BitInt", CP_K_BITINT}, {"_Decimal32", CP_K_DECIMAL32},
    {"_Decimal64", CP_K_DECIMAL64}, {"_Decimal128", CP_K_DECIMAL128},
    {"asm", CP_K_ASM}, {"__asm", CP_K_ASM}, {"__asm__", CP_K_ASM},
    {"__attribute", CP_K_ATTRIBUTE}, {"__attribute__", CP_K_ATTRIBUTE},
    {"__extension__", CP_K_EXTENSION}, {"__label__", CP_K_LABEL},
    {"__real", CP_K_REAL}, {"__real__", CP_K_REAL},
    {"__imag", CP_K_IMAG}, {"__imag__", CP_K_IMAG},
    {"__auto_type", CP_K_AUTO_TYPE}, {"__int128", CP_K_INT128},
    {"__int128__", CP_K_INT128},
    {"__float128", CP_K_FLOAT128}, {"_Float128", CP_K_FLOAT128},
    {"__float80", CP_K_FLOAT80}, {"__ibm128", CP_K_IBM128},
    {"_Float16", CP_K_FLOAT16}, {"_Float32", CP_K_FLOAT32},
    {"_Float64", CP_K_FLOAT64}, {"_Float32x", CP_K_FLOAT32X},
    {"_Float64x", CP_K_FLOAT64X}, {"_Float128x", CP_K_FLOAT128X},
    {"__bf16", CP_K_BF16}, {"__fp16", CP_K_FP16},
    {"__declspec", CP_K_DECLSPEC}, {"__int8", CP_K_INT8},
    {"__int16", CP_K_INT16}, {"__int32", CP_K_INT32},
    {"__int64", CP_K_INT64}, {"_int8", CP_K_INT8}, {"_int16", CP_K_INT16},
    {"_int32", CP_K_INT32}, {"_int64", CP_K_INT64},
    {"__forceinline", CP_K_FORCEINLINE}, {"__try", CP_K_TRY},
    {"__except", CP_K_EXCEPT}, {"__finally", CP_K_FINALLY},
    {"__leave", CP_K_LEAVE},
    // attribute-like words
    {"__cdecl", CP_K_KWATTR}, {"_cdecl", CP_K_KWATTR},
    {"__stdcall", CP_K_KWATTR}, {"_stdcall", CP_K_KWATTR},
    {"__fastcall", CP_K_KWATTR}, {"_fastcall", CP_K_KWATTR},
    {"__thiscall", CP_K_KWATTR}, {"__vectorcall", CP_K_KWATTR},
    {"__clrcall", CP_K_KWATTR}, {"__pascal", CP_K_KWATTR},
    {"_pascal", CP_K_KWATTR}, {"__regcall", CP_K_KWATTR},
    {"__ptr32", CP_K_KWATTR}, {"__ptr64", CP_K_KWATTR},
    {"__unaligned", CP_K_KWATTR}, {"__w64", CP_K_KWATTR},
    {"__sptr", CP_K_KWATTR}, {"__uptr", CP_K_KWATTR},
    {"_Nonnull", CP_K_KWATTR}, {"_Nullable", CP_K_KWATTR},
    {"_Null_unspecified", CP_K_KWATTR}, {"_Nullable_result", CP_K_KWATTR},
    {"__nonnull", CP_K_KWATTR}, {"__nullable", CP_K_KWATTR},
    {"__null_unspecified", CP_K_KWATTR}, {"__seg_fs", CP_K_KWATTR}, {"__seg_gs", CP_K_KWATTR},
    {"__based", CP_K_KWATTRARGS},
    {"__ptrauth", CP_K_KWATTRARGS}, {"__address_space", CP_K_KWATTRARGS},
};

// Embedded compilers' words (Keil, IAR, SDCC, z88dk). GCC and Clang have
// none of them, and code they compile uses some as names ("} __packed;"
// with no such macro declares a variable), so these are keywords only
// when the compiler is not GCC-like. Elsewhere an unknown one is still
// read as an annotation, as for any missing macro.
static const cp_kwdef cp_embeddedkeywords[] = {
    {"__far", CP_K_KWATTR}, {"_far", CP_K_KWATTR},
    {"__near", CP_K_KWATTR}, {"_near", CP_K_KWATTR},
    {"__huge", CP_K_KWATTR}, {"_huge", CP_K_KWATTR},
    {"__packed", CP_K_KWATTR}, {"__irq", CP_K_KWATTR},
    {"__fiq", CP_K_KWATTR}, {"__swi", CP_K_KWATTR},
    {"__xdata", CP_K_KWATTR}, {"__sfr", CP_K_KWATTR}, {"__sfr16", CP_K_KWATTR},
    {"__sfr32", CP_K_KWATTR}, {"__sbit", CP_K_KWATTR},
    {"__no_init", CP_K_KWATTR}, {"__ramfunc", CP_K_KWATTR},
    {"__at", CP_K_KWATTRARGS}, {"__interrupt", CP_K_KWATTRARGS},
    // SDCC (8051, Z80, STM8, PIC): storage classes and function qualifiers
    {"__idata", CP_K_KWATTR}, {"__pdata", CP_K_KWATTR}, {"__eeprom", CP_K_KWATTR},
    {"__reentrant", CP_K_KWATTR}, {"__naked", CP_K_KWATTR}, {"__critical", CP_K_KWATTR},
    {"__banked", CP_K_KWATTR}, {"__nonbanked", CP_K_KWATTR}, {"__shadowregs", CP_K_KWATTR},
    {"__smallc", CP_K_KWATTR}, {"__z88dk_fastcall", CP_K_KWATTR},
    {"__z88dk_callee", CP_K_KWATTR}, {"__using", CP_K_KWATTRARGS},
    {"__sdcccall", CP_K_KWATTRARGS}, {"__preserves_regs", CP_K_KWATTRARGS},
};

// SDCC words that other code uses as ordinary names (glibc has __data and
// __code parameters), so keywords only when compiling for SDCC.
static const cp_kwdef cp_sdcckeywords[] = {
    {"__data", CP_K_KWATTR}, {"__code", CP_K_KWATTR}, {"__trap", CP_K_KWATTR},
    {"__bit", CP_K_BOOL},
};

static i32 cp_keyword(cp_map *kw, cp_str s)
{
    if (!s.len || (!cp_isalpha(s.data[0]) && s.data[0]!='_')) return CP_IDENT;
    cp_map *m = cp_upsert(&kw, s, 0);
    return m ? (i32)m->ival : CP_IDENT;
}

// ----------------------------------------------------------------------
// Lexer: turns one buffer into preprocessing tokens

typedef struct cp_hideset cp_hideset;
struct cp_hideset {
    cp_hideset *next;
    cp_str      name;
};

typedef struct cp_pptok cp_pptok;
struct cp_pptok {
    cp_pptok   *next;
    cp_str      text;
    i32         kind;     // CP_IDENT for all identifiers and keywords
    i32         flags;
    i32         line, col, file;
    i32         exp;      // 1 + expansion index
    cp_macro   *macro;
    cp_hideset *hs;
    cp_str      doc;
    i32         noexpand; // painted blue: never expand again
    i32         lmline, lmfile;  // CP_PP_LINE: the line and file it names
};

typedef struct {
    cp_str text;
    i32    kind;
} cp_punct;

// Longest first within each leading character. (CP_P, not S(): a
// compound literal is not a constant initializer in strict C99.)
#define CP_P(s) {s, sizeof(s)-1}
static const cp_punct cp_puncts[] = {
    {CP_P("%:%:"), CP_HASHHASH}, {CP_P("..."), CP_ELLIPSIS},
    {CP_P("<<="), CP_SHLEQ}, {CP_P(">>="), CP_SHREQ}, {CP_P("->"), CP_ARROW},
    {CP_P("++"), CP_INC}, {CP_P("--"), CP_DEC}, {CP_P("<<"), CP_SHL},
    {CP_P(">>"), CP_SHR}, {CP_P("<="), CP_LE}, {CP_P(">="), CP_GE},
    {CP_P("=="), CP_EQ}, {CP_P("!="), CP_NE}, {CP_P("&&"), CP_ANDAND},
    {CP_P("||"), CP_OROR}, {CP_P("*="), CP_MULEQ}, {CP_P("/="), CP_DIVEQ},
    {CP_P("%="), CP_MODEQ}, {CP_P("+="), CP_ADDEQ}, {CP_P("-="), CP_SUBEQ},
    {CP_P("&="), CP_ANDEQ}, {CP_P("^="), CP_XOREQ}, {CP_P("|="), CP_OREQ},
    {CP_P("##"), CP_HASHHASH}, {CP_P("<:"), CP_LBRACK},
    {CP_P(":>"), CP_RBRACK}, {CP_P("<%"), CP_LBRACE}, {CP_P("%>"), CP_RBRACE},
    {CP_P("%:"), CP_HASH}, {CP_P("["), CP_LBRACK}, {CP_P("]"), CP_RBRACK},
    {CP_P("("), CP_LPAREN}, {CP_P(")"), CP_RPAREN}, {CP_P("{"), CP_LBRACE},
    {CP_P("}"), CP_RBRACE}, {CP_P("."), CP_DOT}, {CP_P("&"), CP_AMP},
    {CP_P("*"), CP_STAR}, {CP_P("+"), CP_PLUS}, {CP_P("-"), CP_MINUS},
    {CP_P("~"), CP_TILDE}, {CP_P("!"), CP_NOT}, {CP_P("/"), CP_SLASH},
    {CP_P("%"), CP_PERCENT}, {CP_P("<"), CP_LT}, {CP_P(">"), CP_GT},
    {CP_P("^"), CP_XOR}, {CP_P("|"), CP_OR}, {CP_P("?"), CP_QUESTION},
    {CP_P(":"), CP_COLON}, {CP_P(";"), CP_SEMI}, {CP_P("="), CP_ASSIGN},
    {CP_P(","), CP_COMMA}, {CP_P("#"), CP_HASH},
};
#undef CP_P

// Token kinds only the preprocessor sees.
enum {
    CP_PP_FILEEND = CP_NTOKKINDS + 1,
    CP_PP_LINE,  // a line marker or #line, applied only if not skipped
};

typedef struct {
    cp_mem     *mem;
    cp_comment *comments;  // list built in reverse via a side chain
    i32         ncomments;
    struct cp_commentnode { struct cp_commentnode *next; cp_comment c; }
               *chead, **ctail;
    b32         keepcomments;
    i32       (*internfile)(void *ctx, cp_str name);
    void       *ctx;
} cp_lexstate;

// Scan the extent of a string or character literal starting at the quote.
// Returns one past the closing quote, or 0 if unterminated on this line.
static char *cp_scanquote(char *s, char *e)
{
    char q = *s++;
    while (s < e) {
        if (*s == q) return s + 1;
        if (*s == '\n') return 0;
        if (*s == '\\') {
            s++;
            if (s < e && *s == '\r') s++;
            if (s >= e) break;
        }
        s++;
    }
    return 0;
}

// Lex a whole file into a linked list of tokens ending in CP_EOF.
// Backslash-newline is treated as whitespace between tokens and spliced
// out of identifiers; it is rare enough elsewhere to ignore.
// Past any line splices (backslash-newline) at p: in the token text they
// are still there, but they are no part of the literal.
static char *cp_skipsplices(char *p, char *e)
{
    for (;;) {
        if (p+1 < e && p[0] == '\\' && p[1] == '\n') p += 2;
        else if (p+2 < e && p[0] == '\\' && p[1] == '\r' && p[2] == '\n') p += 3;
        else return p;
    }
}

static cp_pptok *cp_lexfile(cp_lexstate *ls, cp_str src, i32 file)
{
    cp_mem *m = ls->mem;
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    char *s = src.data;
    char *e = src.data + src.len;
    if (e - s >= 3 && (u8)s[0] == 0xef && (u8)s[1] == 0xbb && (u8)s[2] == 0xbf) {
        s += 3;  // UTF-8 byte order mark
    }
    char *linestart = s;
    i32 line = 1;
    i32 flags = CP_TF_BOL;
    cp_str doc = {0};
    i32 docend = -1;  // line the doc comment ended on

    for (;;) {
        // whitespace and comments
        for (;;) {
            if (s >= e) break;
            char c = *s;
            if (c == '\n') {
                s++; line++; linestart = s;
                flags = CP_TF_BOL | CP_TF_SPACE;
            } else if (c==' ' || c=='\t' || c=='\r' || c=='\f' || c=='\v') {
                s++; flags |= CP_TF_SPACE;
            } else if (c=='\\' && s+1<e && (s[1]=='\n' || (s[1]=='\r' && s+2<e && s[2]=='\n'))) {
                s += s[1]=='\r' ? 3 : 2;
                line++; linestart = s;
                flags |= CP_TF_SPACE;
            } else if (c=='/' && cp_skipsplices(s+1, e) < e && *cp_skipsplices(s+1, e)=='/') {
                char *beg = s;
                i32 bline = line, bcol = (i32)(s - linestart) + 1;
                while (s < e && *s != '\n') {
                    if (*s=='\\' && s+1<e && s[1]=='\n') { s++; line++; linestart = s+1; }
                    s++;
                }
                cp_str text = cp_span(beg, s);
                if (!(flags & CP_TF_BOL)) {
                    doc.data = 0; doc.len = 0; docend = -1;  // trailing
                } else if (docend >= bline-1 && doc.data) {
                    doc = cp_span(doc.data, s);
                    docend = line;
                } else {
                    doc = text;
                    docend = line;
                }
                if (ls->keepcomments) {
                    struct cp_commentnode *n = cp_new(m, 1, struct cp_commentnode);
                    n->c.file = file; n->c.line = bline; n->c.col = bcol;
                    n->c.text = text;
                    *ls->ctail = n; ls->ctail = &n->next; ls->ncomments++;
                }
                flags |= CP_TF_SPACE;
            } else if (c=='/' && cp_skipsplices(s+1, e) < e && *cp_skipsplices(s+1, e)=='*') {
                // the delimiters may be split by line splices: "/\<newline>*"
                char *beg = s;
                i32 bline = line, bcol = (i32)(s - linestart) + 1;
                char *open = cp_skipsplices(s+1, e) + 1;
                for (; s < open; s++) {
                    if (*s == '\n') { line++; linestart = s+1; }
                }
                for (;;) {
                    if (s >= e) break;
                    if (*s == '*') {
                        char *q = cp_skipsplices(s+1, e);
                        if (q < e && *q == '/') {
                            for (; s <= q; s++) {
                                if (*s == '\n') { line++; linestart = s+1; }
                            }
                            break;
                        }
                    }
                    if (*s == '\n') { line++; linestart = s+1; }
                    s++;
                }
                cp_str text = cp_span(beg, s);
                if (!(flags & CP_TF_BOL)) {
                    doc.data = 0; doc.len = 0; docend = -1;  // trailing
                } else if (docend >= bline-1 && doc.data) {
                    doc = cp_span(doc.data, s);
                    docend = line;
                } else {
                    doc = text;
                    docend = line;
                }
                if (ls->keepcomments) {
                    struct cp_commentnode *n = cp_new(m, 1, struct cp_commentnode);
                    n->c.file = file; n->c.line = bline; n->c.col = bcol;
                    n->c.text = text;
                    *ls->ctail = n; ls->ctail = &n->next; ls->ncomments++;
                }
                flags |= CP_TF_SPACE;
            } else {
                break;
            }
        }

        // Line markers ("# 12 "file" 1 3") and #line become a CP_PP_LINE
        // token; the preprocessor applies it unless it lies in a skipped
        // group (an amalgamation's "#ifdef LINES / #line 1 "x.c" / #endif").
        if ((flags & CP_TF_BOL) && s < e && *s == '#' && ls->internfile) {
            char *q = s + 1;
            while (q < e && (*q==' ' || *q=='\t')) q++;
            if (e-q > 4 && q[0]=='l' && q[1]=='i' && q[2]=='n' && q[3]=='e' &&
                (q[4]==' ' || q[4]=='\t')) {
                q += 4;
                while (q < e && (*q==' ' || *q=='\t')) q++;
            }
            if (q < e && cp_isdigit((u8)*q)) {
                i64 n = 0;
                for (; q < e && cp_isdigit((u8)*q); q++) {
                    n = n < 1<<28 ? n*10 + (*q - '0') : n;
                }
                while (q < e && (*q==' ' || *q=='\t')) q++;
                i32 lmfile = -1;
                if (q < e && *q == '"') {
                    char *ne = cp_scanquote(q, e);
                    if (ne) {
                        lmfile = ls->internfile(ls->ctx, cp_span(q+1, ne-1));
                        q = ne;
                    }
                }
                while (q < e && *q != '\n') q++;
                cp_pptok *lm = cp_new(m, 1, cp_pptok);
                lm->kind = CP_PP_LINE;
                lm->flags = CP_TF_BOL;
                lm->line = line;
                lm->col = (i32)(s - linestart) + 1;
                lm->file = file;
                lm->text = cp_span(s, q);
                lm->lmline = (i32)n;
                lm->lmfile = lmfile;
                tail = tail->next = lm;
                s = q;
                doc.data = 0; doc.len = 0; docend = -1;
                continue;
            }
        }

        cp_pptok *t = cp_new(m, 1, cp_pptok);
        t->line = line;
        t->col  = (i32)(s - linestart) + 1;
        t->file = file;
        t->flags = flags;
        if (doc.data && docend >= line-1) {
            t->doc = doc;
        }
        doc.data = 0; doc.len = 0; docend = -1;
        flags = 0;
        tail->next = t;
        tail = t;

        if (s >= e) {
            t->kind = CP_EOF;
            t->text = cp_span(e, e);
            return head.next;
        }

        char *beg = s;
        u8 c = (u8)*s;
        if (cp_isalpha(c) || c=='_' || c=='$' || c>=0x80 || (c=='\\' && s+1<e && (s[1]=='u' || s[1]=='U'))) {
            // identifier, possibly a string/char prefix
            for (;;) {
                if (s < e && cp_isident((u8)*s)) {
                    s++;
                } else if (s+1 < e && *s=='\\' && (s[1]=='u' || s[1]=='U')) {
                    s += 2;
                } else if (s+1 < e && *s=='\\' && s[1]=='\n') {
                    s += 2;  // rare splice, lines counted below
                } else {
                    break;
                }
            }
            iz n = s - beg;
            b32 prefix = (n==1 && (*beg=='L' || *beg=='u' || *beg=='U')) ||
                         (n==2 && beg[0]=='u' && beg[1]=='8');
            b32 raw = n >= 1 && beg[n-1] == 'R' &&
                      (n == 1 || (n == 2 && (*beg=='L' || *beg=='u' || *beg=='U')) ||
                       (n == 3 && beg[0]=='u' && beg[1]=='8'));
            if (raw && s < e && *s == '"') {
                // GNU raw string: R"delim( ... )delim"
                char *d = s + 1;
                char *q = d;
                while (q < e && q - d <= 16 && *q != '(' && *q != ')' && *q != '\\' &&
                       *q != ' ' && *q != '\n') q++;
                b32 ok = 0;
                if (q < e && *q == '(') {
                    cp_str delim = cp_span(d, q);
                    for (char *r = q + 1; r < e; r++) {
                        if (*r == ')' && e - r > delim.len + 1 &&
                            cp_equals(cp_span(r+1, r+1+delim.len), delim) && r[1+delim.len] == '"') {
                            s = r + delim.len + 2;
                            ok = 1;
                            break;
                        }
                    }
                }
                if (ok) {
                    t->kind = CP_STRLIT;
                } else {
                    t->kind = CP_OTHER;
                    s++;
                }
            } else if (prefix && s < e && (*s=='"' || *s=='\'')) {
                char *end = cp_scanquote(s, e);
                if (end) {
                    t->kind = *s=='"' ? CP_STRLIT : CP_CHARLIT;
                    s = end;
                } else {
                    t->kind = CP_OTHER;
                    while (s < e && *s != '\n') s++;
                }
            } else {
                t->kind = CP_IDENT;
            }
        } else if (cp_isdigit(c) || (c=='.' && s+1<e && cp_isdigit((u8)s[1]))) {
            s++;
            while (s < e) {
                u8 d = (u8)*s;
                if ((d|32)=='e' || (d|32)=='p') {
                    s++;
                    if (s < e && (*s=='+' || *s=='-')) s++;
                } else if (cp_isident(d) || d=='.') {
                    s++;
                } else if (d=='\'' && s+1<e && cp_isident((u8)s[1]) && cp_isxdigit((u8)s[-1])) {
                    s++;
                } else if (d=='\\' && s+1<e && s[1]=='\n') {
                    s += 2;  // a splice inside the number, removed below
                } else {
                    break;
                }
            }
            t->kind = CP_INTLIT;  // classified later
            iz n = s - beg;
            b32 hex = n>1 && beg[0]=='0' && (beg[1]|32)=='x';
            for (iz i = 0; i < n; i++) {
                char d = (char)(beg[i]|32);
                if (beg[i]=='.' || (!hex && d=='e') || (hex && d=='p')) {
                    t->kind = CP_FLOATLIT;
                    break;
                }
            }
        } else if (c=='"' || c=='\'') {
            char *end = cp_scanquote(s, e);
            if (end) {
                t->kind = c=='"' ? CP_STRLIT : CP_CHARLIT;
                s = end;
            } else {
                // Unterminated: an apostrophe in skipped text, usually.
                t->kind = CP_OTHER;
                s++;
            }
        } else {
            t->kind = CP_OTHER;
            for (iz i = 0; i < cp_countof(cp_puncts); i++) {
                cp_str p = cp_puncts[i].text;
                if (e - s >= p.len && cp_equals(cp_span(s, s+p.len), p)) {
                    // "<::" is not a digraph in C++, but it is in C
                    t->kind = cp_puncts[i].kind;
                    s += p.len;
                    break;
                }
            }
            if (t->kind == CP_OTHER) {
                s++;
                while (s < e && ((u8)*s & 0xc0) == 0x80) s++;  // whole UTF-8 seq
            }
        }
        t->text = cp_span(beg, s);
        b32 spliced = 0;
        for (char *q = beg; q < s; q++) {
            if (*q == '\n') {  // spliced lines inside the token
                line++;
                linestart = q + 1;
                spliced = 1;
            }
        }
        if (spliced && (t->kind == CP_IDENT || t->kind == CP_INTLIT || t->kind == CP_FLOATLIT)) {
            // a name or number is spelled without its line splices:
            // "#define NAME\" defines NAME
            char *buf = cp_new(ls->mem, s - beg, char);
            iz n = 0;
            for (char *q = beg; q < s; q++) {
                if (q+1 < s && q[0] == '\\' && q[1] == '\n') { q++; continue; }
                if (q+2 < s && q[0] == '\\' && q[1] == '\r' && q[2] == '\n') { q += 2; continue; }
                buf[n++] = *q;
            }
            t->text = cp_span(buf, buf+n);
        }
    }
}

// ----------------------------------------------------------------------
// Diagnostics

typedef struct {
    cp_unit  *u;
    cp_mem   *mem;
    cp_error *tail;
    i32       quiet;  // > 0: drop diagnostics (speculative parsing)
} cp_diags;

static void cp_diag(cp_diags *d, b32 warning, i32 tok, i32 file, i32 line, i32 col, cp_str msg)
{
    if (d->quiet) return;
    // count everything, but keep only the first CPARSE_MAXERRORS of each
    // kind, so a flood of warnings cannot hide the errors
    i32 n = warning ? ++d->u->nwarnings : ++d->u->nerrors;
    if (n > CPARSE_MAXERRORS) return;
    cp_error *e = cp_new(d->mem, 1, cp_error);
    e->warning = warning;
    e->tok  = tok;
    e->line = line;
    e->col  = col;
    if (file >= 0 && file < d->u->nfiles) e->file = d->u->files[file].path;
    e->msg  = msg;
    if (d->tail) d->tail->next = e; else d->u->errors = e;
    d->tail = e;
}

// ----------------------------------------------------------------------
// Preprocessor (Prosser's algorithm with hidesets, as in chibicc)


typedef struct cp_ppmacro cp_ppmacro;
struct cp_ppmacro {
    cp_macro    pub;
    cp_pptok   *body;
    i32         builtin;
};

enum {
    CP_BI_NONE, CP_BI_FILE, CP_BI_LINE, CP_BI_COUNTER, CP_BI_INCLUDE_LEVEL,
    CP_BI_BASE_FILE, CP_BI_DATE, CP_BI_TIME, CP_BI_TIMESTAMP, CP_BI_FILE_NAME,
    CP_BI_HAS,  // __has_include and friends: only meaningful in #if
};

typedef struct cp_cond cp_cond;
struct cp_cond {
    cp_cond *next;
    i32      ctx;       // 0 #if, 1 #elif, 2 #else
    b32      included;  // some branch already taken
    i32      depth;     // include depth where it opened
    i32      file, line;
};

typedef struct cp_macrostack cp_macrostack;
struct cp_macrostack {
    cp_macrostack *next;
    cp_ppmacro    *m;
};

typedef struct cp_listnode cp_listnode;
struct cp_listnode {
    cp_listnode *next;
    void        *item;
};

typedef struct {
    cp_mem      *mem;
    cp_config   *cfg;
    cp_unit     *u;
    cp_diags    *diags;
    cp_map      *kw;
    cp_map      *macros;   // name -> cp_ppmacro (val null when undefined)
    cp_map      *once;     // normalized paths not to enter again
    cp_map      *entered;  // normalized paths entered at least once
    i32         *oncefiles;  // files marked once, for the content check
    i32          nonce, caponce;
    cp_map      *guards;   // path -> guard macro name
    cp_map      *filemap;  // path -> 1 + file index
    cp_map      *pushed;   // name -> cp_macrostack
    cp_map      *asserts;  // #assert predicate -> cp_answer list
    cp_lexstate  ls;
    cp_file     *files;
    i32         *filedir;  // search directory index each file was found in
    struct cp_linemap { i32 on, phys, line, file; } *lmap;  // per file:
    i32          caplmap;  // after physical line phys, line numbers run
                           // from line in file (the last #line applied)
    i32          nfiles, capfiles;
    cp_macro    *mhead, *mtail;
    cp_cond     *cond;
    i32          depth;    // include depth
    i32          counter;
    i32          inargs;   // > 0 while pre-expanding macro arguments
    b32          predefs;  // processing cp_config.predefs
    b32          inif;     // expanding a #if line
    b32          nopp;     // input already preprocessed (cp_config.nopp)
    b32          noinclude;  // reading predefs for preprocessed input
    // output
    cp_pptok    *outhead, *outtail;
    i32          nout;
    cp_listnode *pragmas, **pragmatail; i32 npragmas;
    cp_listnode *includes, **includetail; i32 nincludes;
    cp_listnode *skips, **skiptail; i32 nskips;
    cp_listnode *exps, **exptail; i32 nexps;
    cp_expansion **exparr; i32 capexp;  // index -> record, for emission
    char        *numbers;  // "0\01\0...255\0" for #embed
} cp_pp;

static void cp_listpush(cp_mem *m, cp_listnode ***tail, i32 *count, void *item)
{
    cp_listnode *n = cp_new(m, 1, cp_listnode);
    n->item = item;
    **tail = n;
    *tail = &n->next;
    (*count)++;
}

// Translation phase 1: replace trigraphs. The copy is made only when
// there is something to replace, so columns stay exact in ordinary files.
// A lone carriage return ends a line, as in old Mac files and as GCC
// counts it. Each becomes "\n", which keeps every offset; the copy is
// made only if there is one.
static cp_str cp_lonecr(cp_mem *perm, cp_str src)
{
    char *buf = 0;
    for (iz i = 0; i < src.len; i++) {
        if (src.data[i] != '\r' || (i+1 < src.len && src.data[i+1] == '\n')) continue;
        if (!buf) {
            buf = cp_new(perm, src.len, char);
            cp_memcpy(buf, src.data, src.len);
        }
        buf[i] = '\n';
    }
    return buf ? cp_span(buf, buf+src.len) : src;
}

static cp_str cp_trigraphs(cp_mem *m, cp_str src)
{
    static const char from[] = "=/'()!<>-", to[] = "#\\^[]|{}~";
    iz i = 0;
    for (; i+2 < src.len; i++) {
        if (src.data[i] == '?' && src.data[i+1] == '?') {
            iz j = 0;
            while (j < 9 && from[j] != src.data[i+2]) j++;
            if (j < 9) break;
        }
    }
    if (i+2 >= src.len) return src;
    cp_str r = {cp_new(m, src.len, char), 0};
    for (i = 0; i < src.len; i++) {
        char c = src.data[i];
        if (c == '?' && i+2 < src.len && src.data[i+1] == '?') {
            iz j = 0;
            while (j < 9 && from[j] != src.data[i+2]) j++;
            if (j < 9) {
                c = to[j];
                i += 2;
            }
        }
        r.data[r.len++] = c;
    }
    return r;
}

static i32 cp_addfile(cp_pp *pp, cp_str path, cp_str src, i32 dir, b32 sys)
{
    if (pp->cfg && pp->cfg->trigraphs) src = cp_trigraphs(pp->mem, src);
    src = cp_lonecr(pp->mem, src);
    if (pp->nfiles == pp->capfiles) {
        i32 cap = pp->capfiles ? pp->capfiles*2 : 16;
        cp_file *f = cp_new(pp->mem, cap, cp_file);
        i32 *d = cp_new(pp->mem, cap, i32);
        if (pp->nfiles) {
            cp_memcpy(f, pp->files, pp->nfiles*(iz)sizeof(*f));
            cp_memcpy(d, pp->filedir, pp->nfiles*(iz)sizeof(*d));
        }
        pp->files = f; pp->filedir = d; pp->capfiles = cap;
    }
    i32 i = pp->nfiles++;
    pp->files[i].path = path;
    pp->files[i].src = src;
    pp->files[i].system = sys;
    pp->files[i].parent = -1;
    pp->filedir[i] = dir;
    pp->u->files = pp->files;
    pp->u->nfiles = pp->nfiles;
    return i;
}

static i32 cp_internfile(void *ctx, cp_str name)
{
    cp_pp *pp = (cp_pp *)ctx;
    cp_map *m = cp_upsert(&pp->filemap, name, pp->mem);
    if (!m->ival) {
        cp_str empty = {0};
        m->ival = 1 + cp_addfile(pp, cp_copy(pp->mem, name), empty, -1, 0);
    }
    return (i32)m->ival - 1;
}

static cp_str cp_msg2(cp_pp *pp, char *a, cp_str b)
{
    cp_str s = {a, 0};
    while (a[s.len]) s.len++;
    return cp_concat(pp->mem, s, b);
}

static cp_str cp_cstr(char *s)
{
    cp_str r = {s, 0};
    while (s[r.len]) r.len++;
    return r;
}

// Where a token is, after any #line that applies to its file.
static void cp_mappos(cp_pp *pp, i32 *file, i32 *line)
{
    i32 f = *file;
    if (f < 0 || f >= pp->caplmap || !pp->lmap[f].on) return;
    struct cp_linemap *m = &pp->lmap[f];
    *line = m->line + (*line - m->phys - 1);
    *file = m->file;
}

// Apply a line marker or #line in the code being processed.
static void cp_setline(cp_pp *pp, cp_pptok *t)
{
    i32 f = t->file;
    if (f < 0) return;
    if (f >= pp->caplmap) {
        i32 cap = pp->caplmap ? pp->caplmap : 16;
        while (cap <= f) cap *= 2;
        struct cp_linemap *n = cp_new(pp->mem, cap, struct cp_linemap);
        if (pp->caplmap) cp_memcpy(n, pp->lmap, pp->caplmap*(iz)sizeof(*n));
        pp->lmap = n;
        pp->caplmap = cap;
    }
    struct cp_linemap *m = &pp->lmap[f];
    i32 curfile = m->on ? m->file : f;
    m->on = 1;
    m->phys = t->line;
    m->line = t->lmline;
    m->file = t->lmfile >= 0 ? t->lmfile : curfile;
}

static void cp_pperror(cp_pp *pp, cp_pptok *t, b32 warning, cp_str msg)
{
    i32 file = t->file, line = t->line;
    cp_mappos(pp, &file, &line);
    cp_diag(pp->diags, warning, -1, file, line, t->col, msg);
}

static cp_pptok *cp_copytok(cp_pp *pp, cp_pptok *t)
{
    cp_pptok *r = cp_new(pp->mem, 1, cp_pptok);
    *r = *t;
    r->next = 0;
    return r;
}

static cp_pptok *cp_neweof(cp_pp *pp, cp_pptok *at)
{
    cp_pptok *r = cp_copytok(pp, at);
    r->kind = CP_EOF;
    r->text.len = 0;
    return r;
}

// Is t at the start of a new line (ends a directive)?
static b32 cp_atbol(cp_pptok *t)
{
    return (t->flags & CP_TF_BOL) || t->kind == CP_EOF || t->kind == CP_PP_FILEEND;
}

// Skip to the first token of the next line.
static cp_pptok *cp_skipline(cp_pptok *t)
{
    while (!cp_atbol(t)) t = t->next;
    return t;
}

// Copy the rest of the directive line into a fresh EOF-terminated list.
static cp_pptok *cp_copyline(cp_pp *pp, cp_pptok **rest, cp_pptok *t)
{
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    for (; !cp_atbol(t); t = t->next) {
        tail = tail->next = cp_copytok(pp, t);
    }
    tail->next = cp_neweof(pp, t);
    *rest = t;
    return head.next;
}

// --- hidesets

static cp_hideset *cp_hsnew(cp_pp *pp, cp_str name)
{
    cp_hideset *h = cp_new(pp->mem, 1, cp_hideset);
    h->name = name;
    return h;
}

static b32 cp_hscontains(cp_hideset *h, cp_str name)
{
    for (; h; h = h->next) {
        if (cp_equals(h->name, name)) return 1;
    }
    return 0;
}

static cp_hideset *cp_hsunion(cp_pp *pp, cp_hideset *a, cp_hideset *b)
{
    cp_hideset head = {0};
    cp_hideset *tail = &head;
    for (; a; a = a->next) {
        tail = tail->next = cp_hsnew(pp, a->name);
    }
    tail->next = b;
    return head.next;
}

static cp_hideset *cp_hsintersect(cp_pp *pp, cp_hideset *a, cp_hideset *b)
{
    cp_hideset head = {0};
    cp_hideset *tail = &head;
    for (; a; a = a->next) {
        if (cp_hscontains(b, a->name)) {
            tail = tail->next = cp_hsnew(pp, a->name);
        }
    }
    return head.next;
}

static cp_pptok *cp_addhideset(cp_pp *pp, cp_pptok *t, cp_hideset *hs)
{
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    for (; t; t = t->next) {
        cp_pptok *c = cp_copytok(pp, t);
        c->hs = cp_hsunion(pp, c->hs, hs);
        tail = tail->next = c;
    }
    return head.next;
}

// Append list a (EOF-terminated, EOF dropped) before b.
static cp_pptok *cp_append(cp_pp *pp, cp_pptok *a, cp_pptok *b)
{
    if (!a || a->kind == CP_EOF) return b;
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    for (; a && a->kind != CP_EOF; a = a->next) {
        tail = tail->next = cp_copytok(pp, a);
    }
    tail->next = b;
    return head.next;
}

static cp_ppmacro *cp_findmacro(cp_pp *pp, cp_str name)
{
    cp_map *m = cp_upsert(&pp->macros, name, 0);
    return m ? (cp_ppmacro *)m->val : 0;
}

// --- macro definitions

static cp_token *cp_totokens(cp_pp *pp, cp_pptok *t, i32 *n)
{
    i32 count = 0;
    for (cp_pptok *p = t; p && p->kind != CP_EOF; p = p->next) count++;
    cp_token *r = cp_new(pp->mem, count+1, cp_token);
    i32 i = 0;
    for (cp_pptok *p = t; p && p->kind != CP_EOF; p = p->next, i++) {
        r[i].text = p->text;
        r[i].kind = p->kind;
        r[i].flags = p->flags;
        r[i].line = p->line;
        r[i].col = p->col;
        r[i].file = p->file;
    }
    *n = count;
    return r;
}

static cp_ppmacro *cp_define(cp_pp *pp, cp_str name, i32 file, i32 line)
{
    cp_map *slot = cp_upsert(&pp->macros, name, pp->mem);
    cp_ppmacro *old = (cp_ppmacro *)slot->val;
    if (old) old->pub.flags |= CP_MF_UNDEF;
    cp_ppmacro *m = cp_new(pp->mem, 1, cp_ppmacro);
    m->pub.name = name;
    m->pub.file = file;
    m->pub.line = line;
    if (pp->predefs) m->pub.flags |= CP_MF_PREDEF;
    slot->val = m;
    if (pp->mtail) pp->mtail->next = &m->pub; else pp->mhead = &m->pub;
    pp->mtail = &m->pub;
    return m;
}

static void cp_defbuiltin(cp_pp *pp, char *name, i32 kind)
{
    cp_ppmacro *m = cp_define(pp, cp_cstr(name), -1, 0);
    m->builtin = kind;
    m->pub.flags |= CP_MF_PREDEF;
}

// A #define that failed to parse defines nothing.
static cp_pptok *cp_baddefine(cp_pp *pp, cp_ppmacro *m, cp_pptok *t, char *msg)
{
    cp_pperror(pp, t, 0, cp_cstr(msg));
    cp_map *slot = cp_upsert(&pp->macros, m->pub.name, 0);
    if (slot && slot->val == m) slot->val = 0;
    m->pub.flags |= CP_MF_UNDEF;
    m->body = cp_neweof(pp, t);
    return cp_skipline(t);
}

static cp_pptok *cp_dodefine(cp_pp *pp, cp_pptok *hash, cp_pptok *t)
{
    if (t->kind != CP_IDENT || cp_atbol(t)) {
        cp_pperror(pp, t, 0, S("macro name must be an identifier"));
        return cp_skipline(t);
    }
    cp_ppmacro *m = cp_define(pp, t->text, t->file, t->line);
    m->pub.doc = hash->doc;
    t = t->next;
    if (!cp_atbol(t) && t->kind == CP_LPAREN && !(t->flags & CP_TF_SPACE)) {
        m->pub.flags |= CP_MF_FUNCTION;
        t = t->next;
        cp_str params[256];
        i32 n = 0;
        for (;;) {
            if (cp_atbol(t)) {
                return cp_baddefine(pp, m, t, "unterminated macro parameter list");
            }
            if (t->kind == CP_RPAREN) { t = t->next; break; }
            if (n && t->kind == CP_COMMA) t = t->next;
            if (t->kind == CP_ELLIPSIS) {
                m->pub.flags |= CP_MF_VARIADIC;
                if (n < cp_countof(params)) params[n++] = S("__VA_ARGS__");
                t = t->next;
            } else if (t->kind == CP_IDENT) {
                if (n < cp_countof(params)) params[n++] = t->text;
                t = t->next;
                if (t->kind == CP_ELLIPSIS && !cp_atbol(t)) {  // GNU name...
                    m->pub.flags |= CP_MF_VARIADIC;
                    t = t->next;
                }
            } else {
                return cp_baddefine(pp, m, t, "bad macro parameter list");
            }
            if (!cp_atbol(t) && t->kind != CP_COMMA && t->kind != CP_RPAREN) {
                return cp_baddefine(pp, m, t, "bad macro parameter list");
            }
        }
        m->pub.params = cp_new(pp->mem, n, cp_str);
        if (n) cp_memcpy(m->pub.params, params, n*(iz)sizeof(cp_str));
        m->pub.nparams = n;
    }
    cp_pptok *rest;
    m->body = cp_copyline(pp, &rest, t);
    if (m->body->kind != CP_EOF) m->body->flags &= ~(CP_TF_BOL|CP_TF_SPACE);
    m->pub.body = cp_totokens(pp, m->body, &m->pub.nbody);
    return rest;
}

// --- expansion

typedef struct {
    cp_pptok *raw;
    cp_pptok *exp;
    b32       done;
} cp_macroarg;

static b32 cp_expand(cp_pp *pp, cp_pptok **rest, cp_pptok *tok);
static cp_pptok *cp_directive(cp_pp *pp, cp_pptok *hash);

// Fully macro-expand an EOF-terminated list in isolation.
static cp_pptok *cp_expandlist(cp_pp *pp, cp_pptok *t)
{
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    cp_pptok *prev = 0;  // token before tail
    pp->inargs++;
    while (t->kind != CP_EOF) {
        // In #if, the operand of a "defined" that a macro produced is not
        // expanded (GCC), as in mingw's (!defined(X)) && defined (Y)
        b32 operand = pp->inif && t->kind == CP_IDENT && tail != &head &&
                      ((tail->kind == CP_IDENT && cp_equals(tail->text, S("defined"))) ||
                       (tail->kind == CP_LPAREN && prev && prev->kind == CP_IDENT &&
                        cp_equals(prev->text, S("defined"))));
        if (!operand && cp_expand(pp, &t, t)) continue;
        prev = tail;
        tail = tail->next = cp_copytok(pp, t);
        t = t->next;
    }
    tail->next = t;
    pp->inargs--;
    return head.next;
}

static cp_str cp_stringize(cp_pp *pp, cp_pptok *t)
{
    iz len = 2;
    for (cp_pptok *p = t; p->kind != CP_EOF; p = p->next) {
        len += p->text.len*2 + 1;
    }
    char *buf = cp_new(pp->mem, len, char);
    iz n = 0;
    buf[n++] = '"';
    for (cp_pptok *p = t; p->kind != CP_EOF; p = p->next) {
        if (p != t && (p->flags & (CP_TF_SPACE|CP_TF_BOL))) buf[n++] = ' ';
        b32 lit = p->kind == CP_STRLIT || p->kind == CP_CHARLIT;
        // A stray quote is escaped too, and a stray backslash when it would
        // escape the closing quote, so the result is one valid literal.
        // Otherwise a stray backslash stays as it is: str(: @\n) is ": @\n".
        b32 stray = p->kind == CP_OTHER;
        b32 last = p->next->kind == CP_EOF;
        for (iz i = 0; i < p->text.len; i++) {
            char c = p->text.data[i];
            if ((lit && (c=='"' || c=='\\')) || (stray && (c=='"' || (c=='\\' && last)))) {
                buf[n++] = '\\';
            }
            if (lit && c == '\n') {
                // a raw string literal may span lines
                buf[n++] = '\\';
                c = 'n';
            }
            buf[n++] = c;
        }
    }
    buf[n++] = '"';
    return cp_span(buf, buf+n);
}

static cp_pptok *cp_lexstring(cp_pp *pp, cp_str s, cp_pptok *at)
{
    cp_lexstate ls = {0};
    ls.mem = pp->mem;
    cp_pptok *t = cp_lexfile(&ls, s, at->file);
    for (cp_pptok *p = t; p; p = p->next) {
        p->line = at->line;
        p->col = at->col;
        p->flags = at->flags;
        p->macro = at->macro;
        p->exp = at->exp;
    }
    return t;
}

// Paste two tokens. Returns the new token, or null if they do not form one.
static cp_pptok *cp_paste(cp_pp *pp, cp_pptok *a, cp_pptok *b)
{
    cp_str s = cp_concat(pp->mem, a->text, b->text);
    cp_pptok *t = cp_lexstring(pp, s, a);
    if (t->kind == CP_EOF || t->next->kind != CP_EOF) return 0;
    t->next = 0;
    t->flags = a->flags;
    return t;
}

static i32 cp_findparam(cp_ppmacro *m, cp_pptok *t)
{
    if (t->kind != CP_IDENT) return -1;
    for (i32 i = 0; i < m->pub.nparams; i++) {
        if (cp_equals(m->pub.params[i], t->text)) return i;
    }
    return -1;
}

static cp_pptok *cp_argexp(cp_pp *pp, cp_macroarg *a)
{
    if (!a->done) {
        a->exp = cp_expandlist(pp, a->raw);
        a->done = 1;
    }
    return a->exp;
}

// Find the token after the parenthesized group starting at "(" t.
static cp_pptok *cp_skipgroup(cp_pptok *t, cp_pptok **close)
{
    i32 depth = 0;
    for (; t->kind != CP_EOF; t = t->next) {
        if (t->kind == CP_LPAREN) depth++;
        if (t->kind == CP_RPAREN && --depth == 0) break;
    }
    *close = t;
    return t->kind == CP_EOF ? t : t->next;
}

static cp_pptok *cp_subst(cp_pp *pp, cp_ppmacro *m, cp_pptok *body, cp_macroarg *args, i32 nargs)
{
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    b32 fn = (m->pub.flags & CP_MF_FUNCTION) != 0;
    b32 va = (m->pub.flags & CP_MF_VARIADIC) != 0;
    cp_macroarg *vaarg = va && nargs ? &args[nargs-1] : 0;

    for (cp_pptok *t = body; t->kind != CP_EOF;) {
        // __VA_OPT__(content)
        if (va && t->kind == CP_IDENT && cp_equals(t->text, S("__VA_OPT__")) &&
            t->next->kind == CP_LPAREN) {
            cp_pptok *close;
            cp_pptok *after = cp_skipgroup(t->next, &close);
            if (vaarg && vaarg->raw->kind != CP_EOF) {
                // copy the content into its own list and substitute it
                cp_pptok ch = {0};
                cp_pptok *ct = &ch;
                for (cp_pptok *p = t->next->next; p != close; p = p->next) {
                    ct = ct->next = cp_copytok(pp, p);
                }
                ct->next = cp_neweof(pp, close);
                cp_pptok *sub = cp_subst(pp, m, ch.next, args, nargs);
                for (; sub->kind != CP_EOF; sub = sub->next) {
                    tail = tail->next = cp_copytok(pp, sub);
                }
            }
            t = after;
            continue;
        }

        // # param
        if (fn && t->kind == CP_HASH) {
            i32 i = cp_findparam(m, t->next);
            if (i >= 0 && i < nargs) {
                cp_pptok *s = cp_copytok(pp, t);
                s->kind = CP_STRLIT;
                s->text = cp_stringize(pp, args[i].raw);
                tail = tail->next = s;
                t = t->next->next;
                continue;
            }
        }

        // GNU: , ## __VA_ARGS__
        if (va && t->kind == CP_COMMA && t->next->kind == CP_HASHHASH) {
            i32 i = cp_findparam(m, t->next->next);
            if (i >= 0 && i == nargs-1) {
                if (args[i].raw->kind == CP_EOF) {
                    t = t->next->next->next;  // drop the comma
                } else {
                    tail = tail->next = cp_copytok(pp, t);
                    for (cp_pptok *p = args[i].raw; p->kind != CP_EOF; p = p->next) {
                        tail = tail->next = cp_copytok(pp, p);
                    }
                    t = t->next->next->next;
                }
                continue;
            }
        }

        // x ## y
        if (t->kind == CP_HASHHASH) {
            cp_pptok *rhs = t->next;
            if (rhs->kind == CP_EOF) { t = rhs; continue; }
            i32 i = cp_findparam(m, rhs);
            cp_pptok *list;
            if (i >= 0 && i < nargs) {
                list = args[i].raw;
            } else {
                list = cp_copytok(pp, rhs);
                list->next = cp_neweof(pp, rhs);
            }
            if (list->kind != CP_EOF) {
                if (tail == &head) {
                    // nothing on the left: just append
                    for (cp_pptok *p = list; p->kind != CP_EOF; p = p->next) {
                        tail = tail->next = cp_copytok(pp, p);
                    }
                } else {
                    cp_pptok *pasted = cp_paste(pp, tail, list);
                    if (pasted) {
                        // replace tail in place
                        cp_pptok *keepnext = tail->next;
                        *tail = *pasted;
                        tail->next = keepnext;
                        tail->hs = 0;
                    } else {
                        cp_pperror(pp, t, 1, S("pasting does not form a valid token"));
                        tail = tail->next = cp_copytok(pp, list);
                    }
                    for (cp_pptok *p = list->next; p->kind != CP_EOF; p = p->next) {
                        tail = tail->next = cp_copytok(pp, p);
                    }
                }
            }
            t = rhs->next;
            continue;
        }

        i32 i = cp_findparam(m, t);
        if (i >= 0 && i < nargs) {
            cp_pptok *list;
            if (t->next->kind == CP_HASHHASH) {
                list = args[i].raw;  // operand of ##: not expanded
            } else {
                list = cp_argexp(pp, &args[i]);
            }
            b32 first = 1;
            for (cp_pptok *p = list; p->kind != CP_EOF; p = p->next) {
                cp_pptok *c = cp_copytok(pp, p);
                if (first) {
                    c->flags = (c->flags & ~(CP_TF_SPACE|CP_TF_BOL)) | (t->flags & CP_TF_SPACE);
                    first = 0;
                }
                tail = tail->next = c;
            }
            if (first && list->kind == CP_EOF && t->next->kind == CP_HASHHASH) {
                // empty left operand of ##: the right side goes in as is
                cp_pptok *rhs = t->next->next;
                i32 j = cp_findparam(m, rhs);
                if (j >= 0 && j < nargs) {
                    for (cp_pptok *p = args[j].raw; p->kind != CP_EOF; p = p->next) {
                        tail = tail->next = cp_copytok(pp, p);
                    }
                    t = rhs->next;
                } else {
                    t = t->next->next;
                }
                continue;
            }
            t = t->next;
            continue;
        }

        tail = tail->next = cp_copytok(pp, t);
        t = t->next;
    }
    tail->next = cp_neweof(pp, body);
    return head.next;
}

// Read macro arguments starting after "(". Returns the closing paren via
// *close, or null when the argument list is unterminated.
static cp_macroarg *cp_readargs(cp_pp *pp, cp_ppmacro *m, cp_pptok *t, cp_pptok **close, i32 *nargs)
{
    i32 nparams = m->pub.nparams;
    b32 va = (m->pub.flags & CP_MF_VARIADIC) != 0;
    i32 cap = nparams > 0 ? nparams : 1;
    cp_macroarg *args = cp_new(pp->mem, cap, cp_macroarg);
    i32 n = 0;
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    i32 depth = 0;
    for (;;) {
        if (t->kind == CP_EOF || t->kind == CP_PP_FILEEND) {
            return 0;
        }
        if (t->kind == CP_HASH && (t->flags & CP_TF_BOL) && !t->macro && !pp->inargs) {
            t = cp_directive(pp, t);
            continue;
        }
        b32 last = n == nparams-1 && va;
        if (depth == 0 && (t->kind == CP_RPAREN || (t->kind == CP_COMMA && !last))) {
            tail->next = cp_neweof(pp, t);
            if (n < cap) args[n].raw = head.next;  // extras are dropped
            n++;
            head.next = 0;
            tail = &head;
            if (t->kind == CP_RPAREN) break;
            t = t->next;
            continue;
        }
        if (t->kind == CP_LPAREN) depth++;
        if (t->kind == CP_RPAREN) depth--;
        cp_pptok *c = cp_copytok(pp, t);
        if (head.next == 0) c->flags &= ~CP_TF_BOL;
        tail = tail->next = c;
        t = t->next;
    }
    *close = t;
    if (nparams == 0 && n == 1 && args[0].raw->kind == CP_EOF) {
        n = 0;
    }
    if (n != nparams) {
        if (n == nparams-1 && va) {
            // C23/GNU: variadic arguments omitted entirely
        } else {
            cp_pperror(pp, t, 1, cp_msg2(pp, "wrong number of macro arguments for ", m->pub.name));
        }
    }
    for (i32 i = 0; i < cap; i++) {
        if (!args[i].raw) args[i].raw = cp_neweof(pp, t);
    }
    *nargs = nparams;
    return args;
}

static cp_pptok *cp_newtok(cp_pp *pp, cp_pptok *at, i32 kind, cp_str text)
{
    cp_pptok *t = cp_copytok(pp, at);
    t->kind = kind;
    t->text = text;
    t->hs = 0;
    return t;
}

static cp_str cp_numstr(cp_pp *pp, i64 v)
{
    char buf[32];
    i32 n = 0;
    b32 neg = v < 0;
    u64 u = neg ? 0 - (u64)v : (u64)v;
    do { buf[n++] = (char)('0' + u%10); u /= 10; } while (u);
    if (neg) buf[n++] = '-';
    char *r = cp_new(pp->mem, n+1, char);
    for (i32 i = 0; i < n; i++) r[i] = buf[n-1-i];
    return cp_span(r, r+n);
}

static cp_str cp_quote(cp_pp *pp, cp_str s)
{
    char *r = cp_new(pp->mem, s.len*2+3, char);
    iz n = 0;
    r[n++] = '"';
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i]=='"' || s.data[i]=='\\') r[n++] = '\\';
        r[n++] = s.data[i];
    }
    r[n++] = '"';
    return cp_span(r, r+n);
}

static cp_str cp_basename(cp_str s)
{
    iz i = s.len;
    while (i > 0 && s.data[i-1] != '/' && s.data[i-1] != '\\') i--;
    return cp_span(s.data+i, s.data+s.len);
}

// Mark expansion output with its origin. Top-level expansions get a
// record; nested ones inherit the outer token's origin.
static void cp_markorigin(cp_pp *pp, cp_pptok *list, cp_pptok *name, cp_ppmacro *m)
{
    m->pub.nuses++;
    i32 exp = name->exp;
    cp_macro *macro = name->macro ? name->macro : &m->pub;
    if (!name->macro && !pp->inargs) {
        cp_expansion *e = cp_new(pp->mem, 1, cp_expansion);
        e->macro = &m->pub;
        e->file = name->file;
        e->line = name->line;
        e->col  = name->col;
        e->tok = e->endtok = -1;
        if (pp->nexps == pp->capexp) {
            i32 cap = pp->capexp ? pp->capexp*2 : 256;
            cp_expansion **a = cp_new(pp->mem, cap, cp_expansion *);
            if (pp->nexps) cp_memcpy(a, pp->exparr, pp->nexps*(iz)sizeof(*a));
            pp->exparr = a;
            pp->capexp = cap;
        }
        pp->exparr[pp->nexps] = e;
        cp_listpush(pp->mem, &pp->exptail, &pp->nexps, e);
        exp = pp->nexps;
        e->tok = e->endtok = pp->nout;
    }
    b32 first = 1;
    for (cp_pptok *t = list; t && t->kind != CP_EOF; t = t->next) {
        t->exp = exp;
        t->macro = macro;
        t->line = name->line;
        t->col = name->col;
        t->file = name->file;
        t->doc.data = 0; t->doc.len = 0;
        if (first) {
            t->flags = (t->flags & ~(CP_TF_BOL|CP_TF_SPACE)) |
                       (name->flags & (CP_TF_BOL|CP_TF_SPACE));
            t->doc = name->doc;
            first = 0;
        } else {
            t->flags &= ~CP_TF_BOL;
        }
    }
}

// __DATE__ "Mmm dd yyyy", __TIME__ "hh:mm:ss" and __TIMESTAMP__ "Www Mmm dd
// hh:mm:ss yyyy" for the configured time, in UTC. (GCC's __TIMESTAMP__ is
// the file's modification time, which a library without I/O cannot know.)
static cp_str cp_datestr(cp_pp *pp, i32 kind)
{
    i64 t = pp->cfg && pp->cfg->time ? pp->cfg->time : 946684800;  // 2000-01-01
    i64 days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    i32 wday = (i32)(((days + 4) % 7 + 7) % 7);  // 1970-01-01 was a Thursday
    // civil date from a day count (Howard Hinnant's algorithm)
    i64 z = days + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era*146097;
    i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
    i64 mp = (5*doy + 2) / 153;
    i32 day = (i32)(doy - (153*mp + 2)/5 + 1);
    i32 mon = (i32)(mp < 10 ? mp + 3 : mp - 9);
    i64 year = yoe + era*400 + (mon <= 2);
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const char wdays[] = "SunMonTueWedThuFriSat";
    char buf[32];
    i32 n = 0;
    #define CP_D2(v) (buf[n++] = (char)('0' + (v)/10), buf[n++] = (char)('0' + (v)%10))
    buf[n++] = '"';
    if (kind == CP_BI_TIMESTAMP) {
        for (i32 i = 0; i < 3; i++) buf[n++] = wdays[wday*3+i];
        buf[n++] = ' ';
    }
    if (kind != CP_BI_TIME) {
        for (i32 i = 0; i < 3; i++) buf[n++] = months[(mon-1)*3+i];
        buf[n++] = ' ';
        buf[n++] = (char)(day < 10 ? ' ' : '0' + day/10);
        buf[n++] = (char)('0' + day%10);
        buf[n++] = ' ';
    }
    if (kind != CP_BI_DATE) {
        CP_D2((i32)(secs/3600));
        buf[n++] = ':';
        CP_D2((i32)(secs/60%60));
        buf[n++] = ':';
        CP_D2((i32)(secs%60));
        if (kind == CP_BI_TIMESTAMP) buf[n++] = ' ';
    }
    if (kind != CP_BI_TIME) {
        i32 y = (i32)(year < 0 ? 0 : year > 9999 ? 9999 : year);
        CP_D2(y/100);
        CP_D2(y%100);
    }
    #undef CP_D2
    buf[n++] = '"';
    char *r = cp_new(pp->mem, n, char);
    cp_memcpy(r, buf, n);
    return cp_span(r, r+n);
}

static b32 cp_expand(cp_pp *pp, cp_pptok **rest, cp_pptok *tok)
{
    if (tok->kind != CP_IDENT || tok->noexpand) return 0;
    cp_ppmacro *m = cp_findmacro(pp, tok->text);
    if (!m) return 0;
    if (cp_hscontains(tok->hs, tok->text)) {
        tok->noexpand = 1;
        return 0;
    }

    if (m->builtin) {
        cp_pptok *t = 0;
        switch (m->builtin) {
        case CP_BI_FILE:
        case CP_BI_FILE_NAME:
        case CP_BI_BASE_FILE: {
            i32 f = m->builtin == CP_BI_BASE_FILE ? 0 : tok->file, l = tok->line;
            if (m->builtin != CP_BI_BASE_FILE) cp_mappos(pp, &f, &l);
            cp_str path = f>=0 && f<pp->nfiles ? pp->files[f].path : S("");
            if (m->builtin == CP_BI_FILE_NAME) path = cp_basename(path);
            t = cp_newtok(pp, tok, CP_STRLIT, cp_quote(pp, path));
        } break;
        case CP_BI_LINE: {
            i32 f = tok->file, l = tok->line;
            cp_mappos(pp, &f, &l);
            t = cp_newtok(pp, tok, CP_INTLIT, cp_numstr(pp, l));
        } break;
        case CP_BI_COUNTER:
            t = cp_newtok(pp, tok, CP_INTLIT, cp_numstr(pp, pp->counter++));
            break;
        case CP_BI_INCLUDE_LEVEL:
            t = cp_newtok(pp, tok, CP_INTLIT, cp_numstr(pp, pp->depth > 0 ? pp->depth-1 : 0));
            break;
        case CP_BI_DATE:
        case CP_BI_TIME:
        case CP_BI_TIMESTAMP:
            t = cp_newtok(pp, tok, CP_STRLIT, cp_datestr(pp, m->builtin));
            break;
        default:
            return 0;  // __has_include etc. outside #if
        }
        t->next = 0;
        t->noexpand = 1;
        cp_markorigin(pp, t, tok, m);
        t->next = tok->next;
        *rest = t;
        return 1;
    }

    if (!(m->pub.flags & CP_MF_FUNCTION)) {
        cp_hideset *hs = cp_hsunion(pp, tok->hs, cp_hsnew(pp, m->pub.name));
        cp_pptok *body = cp_addhideset(pp, m->body, hs);
        cp_markorigin(pp, body, tok, m);
        *rest = cp_append(pp, body, tok->next);
        return 1;
    }

    // Function-like: needs "(" next, possibly on a later line.
    cp_pptok *lp = tok->next;
    if (lp->kind != CP_LPAREN) return 0;
    cp_pptok *close;
    i32 nargs;
    cp_macroarg *args = cp_readargs(pp, m, lp->next, &close, &nargs);
    if (!args) {
        cp_pperror(pp, tok, 0, cp_msg2(pp, "unterminated call to macro ", m->pub.name));
        tok->noexpand = 1;
        return 0;
    }
    cp_hideset *hs = cp_hsintersect(pp, tok->hs, close->hs);
    hs = cp_hsunion(pp, hs, cp_hsnew(pp, m->pub.name));
    cp_pptok *body = cp_subst(pp, m, m->body, args, nargs);
    body = cp_addhideset(pp, body, hs);
    cp_markorigin(pp, body, tok, m);
    *rest = cp_append(pp, body, close->next);
    return 1;
}

// --- #if expressions

typedef struct {
    cp_pp    *pp;
    cp_pptok *t;
    b32       err;
    i32       skip;  // > 0 inside an unevaluated operand
} cp_ifparser;

typedef struct { i64 v; b32 u; } cp_ifval;

static i64 cp_parsecharval(cp_str s, b32 *ok);
static u64 cp_parseintval(cp_str s, i32 *flags, b32 *ok);

static cp_ifval cp_ifexpr(cp_ifparser *ip);

static cp_ifval cp_ifprimary(cp_ifparser *ip)
{
    cp_ifval r = {0, 0};
    cp_pptok *t = ip->t;
    switch (t->kind) {
    case CP_LPAREN:
        ip->t = t->next;
        r = cp_ifexpr(ip);
        if (ip->t->kind != CP_RPAREN) { ip->err = 1; return r; }
        ip->t = ip->t->next;
        return r;
    case CP_INTLIT: {
        i32 flags = 0;
        b32 ok = 1;
        r.v = (i64)cp_parseintval(t->text, &flags, &ok);
        if (!ok) ip->err = 1;
        r.u = (flags & CP_NF_UNSIGNED) || r.v < 0;
        ip->t = t->next;
        return r;
    }
    case CP_CHARLIT: {
        b32 ok = 1;
        r.v = cp_parsecharval(t->text, &ok);
        ip->t = t->next;
        return r;
    }
    case CP_PLUS: ip->t = t->next; return cp_ifprimary(ip);
    case CP_MINUS: ip->t = t->next; r = cp_ifprimary(ip); r.v = (i64)(0 - (u64)r.v); return r;
    case CP_TILDE: ip->t = t->next; r = cp_ifprimary(ip); r.v = ~r.v; return r;
    case CP_NOT: ip->t = t->next; r = cp_ifprimary(ip); r.v = !r.v; r.u = 0; return r;
    case CP_IDENT:  // remaining identifiers are 0 (true is 1)
        r.v = cp_equals(t->text, S("true"));
        ip->t = t->next;
        return r;
    default:
        ip->err = 1;
        return r;
    }
}

static i32 cp_ifprec(i32 kind)
{
    switch (kind) {
    case CP_STAR: case CP_SLASH: case CP_PERCENT: return 10;
    case CP_PLUS: case CP_MINUS: return 9;
    case CP_SHL: case CP_SHR: return 8;
    case CP_LT: case CP_GT: case CP_LE: case CP_GE: return 7;
    case CP_EQ: case CP_NE: return 6;
    case CP_AMP: return 5;
    case CP_XOR: return 4;
    case CP_OR: return 3;
    case CP_ANDAND: return 2;
    case CP_OROR: return 1;
    }
    return 0;
}

static cp_ifval cp_ifbinary(cp_ifparser *ip, i32 minprec)
{
    cp_ifval l = cp_ifprimary(ip);
    for (;;) {
        i32 op = ip->t->kind;
        i32 prec = cp_ifprec(op);
        if (!prec || prec < minprec || ip->err) return l;
        ip->t = ip->t->next;
        b32 shortcirc = (op == CP_ANDAND && !l.v) || (op == CP_OROR && l.v);
        if (shortcirc) ip->skip++;
        cp_ifval r = cp_ifbinary(ip, prec+1);
        if (shortcirc) ip->skip--;
        b32 u = l.u || r.u;
        u64 a = (u64)l.v, b = (u64)r.v;
        cp_ifval o = {0, u};
        switch (op) {
        case CP_STAR:  o.v = (i64)(a*b); break;
        case CP_SLASH:
        case CP_PERCENT:
            if (!b) {
                if (!ip->skip) ip->err = 1;
                o.v = 0;
            } else if (u) {
                o.v = (i64)(op==CP_SLASH ? a/b : a%b);
            } else if (l.v == INT64_MIN && r.v == -1) {
                o.v = op==CP_SLASH ? INT64_MIN : 0;
            } else {
                o.v = op==CP_SLASH ? l.v/r.v : l.v%r.v;
            }
            break;
        case CP_PLUS:  o.v = (i64)(a+b); break;
        case CP_MINUS: o.v = (i64)(a-b); break;
        case CP_SHL:   o.v = b > 63 ? 0 : (i64)(a << b); o.u = l.u; break;
        case CP_SHR:
            o.u = l.u;
            if (b > 63) o.v = l.u || l.v >= 0 ? 0 : -1;
            else o.v = l.u ? (i64)(a >> b) : (l.v < 0 ? ~(i64)(~a >> b) : (i64)(a >> b));
            break;
        case CP_LT: o.v = u ? a<b : l.v<r.v; o.u = 0; break;
        case CP_GT: o.v = u ? a>b : l.v>r.v; o.u = 0; break;
        case CP_LE: o.v = u ? a<=b : l.v<=r.v; o.u = 0; break;
        case CP_GE: o.v = u ? a>=b : l.v>=r.v; o.u = 0; break;
        case CP_EQ: o.v = a==b; o.u = 0; break;
        case CP_NE: o.v = a!=b; o.u = 0; break;
        case CP_AMP: o.v = (i64)(a&b); break;
        case CP_XOR: o.v = (i64)(a^b); break;
        case CP_OR:  o.v = (i64)(a|b); break;
        case CP_ANDAND: o.v = l.v && r.v; o.u = 0; break;
        case CP_OROR:   o.v = l.v || r.v; o.u = 0; break;
        }
        l = o;
    }
}

static cp_ifval cp_ifcond(cp_ifparser *ip)
{
    cp_ifval c = cp_ifbinary(ip, 1);
    if (ip->t->kind != CP_QUESTION || ip->err) return c;
    ip->t = ip->t->next;
    if (!c.v) ip->skip++;
    cp_ifval a = cp_ifexpr(ip);
    if (!c.v) ip->skip--;
    if (ip->t->kind != CP_COLON) { ip->err = 1; return c; }
    ip->t = ip->t->next;
    if (c.v) ip->skip++;
    cp_ifval b = cp_ifcond(ip);
    if (c.v) ip->skip--;
    cp_ifval r = c.v ? a : b;
    r.u = a.u || b.u;
    return r;
}

static cp_ifval cp_ifexpr(cp_ifparser *ip)
{
    cp_ifval r = cp_ifcond(ip);
    while (ip->t->kind == CP_COMMA && !ip->err) {
        ip->t = ip->t->next;
        r = cp_ifcond(ip);
    }
    return r;
}

static b32 cp_findinclude(cp_pp *pp, cp_str name, b32 angled, i32 curfile, b32 next,
                          cp_str *path, cp_str *src, i32 *dir, b32 *sys);

// Read a header name: "x.h", <x.h>, or macros expanding to one. On
// success *rest is after it.
static b32 cp_headername(cp_pp *pp, cp_pptok *t, cp_pptok **rest, cp_str *name, b32 *angled, b32 expand)
{
    if (t->kind == CP_STRLIT && t->text.len >= 2 && t->text.data[0] == '"') {
        *name = cp_span(t->text.data+1, t->text.data+t->text.len-1);
        *angled = 0;
        *rest = t->next;
        return 1;
    }
    if (t->kind == CP_LT) {
        // join the spellings up to ">"
        iz len = 0;
        cp_pptok *p = t->next;
        for (; p->kind != CP_GT && !cp_atbol(p); p = p->next) {
            len += p->text.len + 1;
        }
        if (p->kind != CP_GT) return 0;
        char *buf = cp_new(pp->mem, len+1, char);
        iz n = 0;
        for (cp_pptok *q = t->next; q != p; q = q->next) {
            if (q != t->next && (q->flags & CP_TF_SPACE)) buf[n++] = ' ';
            cp_memcpy(buf+n, q->text.data, q->text.len);
            n += q->text.len;
        }
        *name = cp_span(buf, buf+n);
        *angled = 1;
        *rest = p->next;
        return 1;
    }
    if (expand && t->kind == CP_IDENT) {
        cp_pptok *rest2;
        cp_pptok *line = cp_copyline(pp, &rest2, t);
        line = cp_expandlist(pp, line);
        cp_pptok *r2;
        if (cp_headername(pp, line, &r2, name, angled, 0)) {
            *rest = cp_skipline(t);
            return 1;
        }
    }
    return 0;
}

static b32 cp_inlist(cp_str s, const char *const *list, iz n)
{
    for (iz i = 0; i < n; i++) {
        if (cp_equals(s, cp_cstr((char *)list[i]))) return 1;
    }
    return 0;
}

// __has_builtin(name) as the configured compiler answers it. GCC and
// Clang share most builtins and each has thousands, so the rule is "yes"
// for __builtin_*, __sync_* and __atomic_*, less the ones real headers ask
// about that one compiler or both lack on x86 (surveyed against gcc 16 and
// clang 23 across /usr/include, mingw-w64, and the corpus).
static b32 cp_hasbuiltin(cp_pp *pp, cp_str s)
{
    static const char *const neither[] = {
        "__builtin_", "__builtin_sponentry", "__builtin_fclose",
        "__builtin_common_type", "__builtin_constexpr_diag", "__builtin_type_order",
        "__builtin_structured_binding_size", "__builtin_source_location",
    };
    static const char *const neitherpre[] = {
        "__builtin_arm_", "__builtin_aarch64_", "__builtin_spirv_", "__builtin_dx_",
        "__builtin_coro_", "__builtin_is_", "__builtin_amdgcn_", "__builtin_nvvm_",
        "__builtin_wasm_", "__builtin_riscv_", "__builtin_ppc_", "__builtin_mips_",
    };
    static const char *const clangonly[] = {
        "__builtin_addressof", "__builtin_assume", "__builtin_available",
        "__builtin_bit_cast", "__builtin_bitreverse8", "__builtin_bitreverse16",
        "__builtin_bitreverse32", "__builtin_bitreverse64", "__builtin_clzs",
        "__builtin_ctzs", "__builtin_debugtrap", "__builtin_launder",
        "__builtin_nondeterministic_value", "__builtin_nontemporal_store",
        "__builtin_nontemporal_load", "__builtin_operator_new",
        "__builtin_operator_delete", "__builtin_readcyclecounter",
        "__builtin_unpredictable", "__builtin_wmemchr", "__builtin_ia32_undef128",
        "__builtin_ia32_undef256", "__builtin_ia32_undef512",
    };
    static const char *const clangonlypre[] = {
        "__builtin_elementwise_", "__builtin_reduce_", "__builtin_rotateleft",
        "__builtin_rotateright", "__builtin_matrix_", "__builtin_ms_",
    };
    static const char *const gcconly[] = {
        "__builtin_shuffle", "__builtin_stack_save", "__builtin_stack_restore",
        "__builtin_va_arg_pack", "__builtin_va_arg_pack_len", "__builtin_toupper",
        "__builtin_nansq", "__builtin_tgmath", "__builtin_speculation_safe_value",
        "__builtin_observable_checkpoint",
    };
    if (cp_startswith(s, S("__sync_")) || cp_startswith(s, S("__atomic_"))) return 1;
    if (!cp_startswith(s, S("__builtin_"))) return 0;
    if (cp_inlist(s, neither, cp_countof(neither))) return 0;
    for (iz i = 0; i < cp_countof(neitherpre); i++) {
        if (cp_startswith(s, cp_cstr((char *)neitherpre[i]))) return 0;
    }
    b32 clang = cp_findmacro(pp, S("__clang__")) != 0;
    if (cp_inlist(s, clangonly, cp_countof(clangonly))) return clang;
    for (iz i = 0; i < cp_countof(clangonlypre); i++) {
        if (cp_startswith(s, cp_cstr((char *)clangonlypre[i]))) return clang;
    }
    if (cp_inlist(s, gcconly, cp_countof(gcconly))) return !clang;
    if (cp_equals(s, S("__builtin_bswap128"))) {
        return !clang && cp_findmacro(pp, S("__SIZEOF_INT128__")) != 0;
    }
    if (cp_startswith(s, S("__builtin_ia32_"))) {
        // BMI2 instructions exist only when the target enables them
        if (cp_startswith(s, S("__builtin_ia32_bzhi_")) ||
            cp_startswith(s, S("__builtin_ia32_pdep_")) ||
            cp_startswith(s, S("__builtin_ia32_pext_"))) {
            return cp_findmacro(pp, S("__BMI2__")) != 0;
        }
        return cp_findmacro(pp, S("__x86_64__")) || cp_findmacro(pp, S("__i386__"));
    }
    return 1;
}

// Replace defined X, defined(X), and __has_*(...) with 0/1 tokens.
// The leading decimal number in a one-token macro body (201710L gives
// 201710), or -1 when the macro is undefined or not a number.
static i64 cp_macronum(cp_pp *pp, char *name)
{
    cp_ppmacro *m = cp_findmacro(pp, cp_cstr(name));
    if (!m || m->pub.nbody != 1 || m->pub.body[0].kind != CP_INTLIT) return -1;
    cp_str t = m->pub.body[0].text;
    i64 v = 0;
    for (iz i = 0; i < t.len && cp_isdigit((u8)t.data[i]) && v < (i64)1 << 40; i++) {
        v = v*10 + (t.data[i] - '0');
    }
    return v;
}

// __has_feature(name) or, when ext, __has_extension(name), answered as
// the configured compiler would (GCC 14 or Clang) from its predefined
// macros: C11 features need -std=c11 or later unless asked as extensions.
static b32 cp_hasfeature(cp_pp *pp, cp_str name, b32 ext)
{
    static const char *c11[] = {
        "c_atomic", "c_alignas", "c_alignof", "c_static_assert",
        "c_generic_selections", "c_thread_local",
    };
    static const char *always[] = {
        "attribute_deprecated_with_message", "attribute_unavailable_with_message",
        "enumerator_attributes", "tls",
    };
    static const char *clang[] = {"attribute_availability", "nullability"};
    for (iz i = 0; i < cp_countof(c11); i++) {
        if (cp_equals(name, cp_cstr((char *)c11[i]))) {
            return ext || cp_macronum(pp, "__STDC_VERSION__") >= 201112;
        }
    }
    for (iz i = 0; i < cp_countof(always); i++) {
        if (cp_equals(name, cp_cstr((char *)always[i]))) return 1;
    }
    b32 isclang = cp_findmacro(pp, S("__clang__")) != 0;
    for (iz i = 0; i < cp_countof(clang); i++) {
        if (cp_equals(name, cp_cstr((char *)clang[i]))) return isclang;
    }
    if (cp_equals(name, S("c_fixed_enum"))) return isclang && ext;
    if (cp_equals(name, S("address_sanitizer"))) {
        return cp_findmacro(pp, S("__SANITIZE_ADDRESS__")) != 0;
    }
    if (cp_equals(name, S("thread_sanitizer"))) {
        return cp_findmacro(pp, S("__SANITIZE_THREAD__")) != 0;
    }
    return 0;
}

// Obsolete GCC assertions: "#assert machine(x86)" gives the predicate
// machine the answer x86, "#if #machine(x86)" or "#if #machine" tests it,
// and "#unassert machine" takes answers back. An answer is kept as its
// tokens joined by single spaces.
typedef struct cp_answer cp_answer;
struct cp_answer {
    cp_answer *next;
    cp_str     text;
};

// Read "(answer)" at t into *ans and point *rest past it. False when the
// parenthesis is never closed.
static b32 cp_readanswer(cp_pp *pp, cp_pptok *t, cp_str *ans, cp_pptok **rest)
{
    // first find the closing parenthesis and the joined length
    cp_pptok *close = t;
    iz len = 0;
    i32 depth = 0;
    for (; close->kind != CP_EOF; close = close->next) {
        if (close->kind == CP_LPAREN && depth++ == 0) continue;
        if (close->kind == CP_RPAREN && --depth == 0) break;
        len += close->text.len + 1;
    }
    *rest = close;
    if (close->kind == CP_EOF) return 0;
    *rest = close->next;
    cp_str r = {cp_new(pp->mem, len+1, char), 0};  // not null: "()" counts
    for (t = t->next; t != close; t = t->next) {
        if (r.len) r.data[r.len++] = ' ';
        cp_memcpy(r.data + r.len, t->text.data, t->text.len);
        r.len += t->text.len;
    }
    *ans = r;
    return 1;
}

static b32 cp_hasanswer(cp_answer *a, cp_str ans)
{
    for (; a; a = a->next) {
        if (cp_equals(a->text, ans)) return 1;
    }
    return 0;
}

// #assert and #unassert, given the rest of the line as an EOF-ended list.
static void cp_doassert(cp_pp *pp, cp_pptok *t, b32 un)
{
    if (t->kind != CP_IDENT) {
        cp_pperror(pp, t, 0, S("predicate must be an identifier"));
        return;
    }
    cp_map *m = cp_upsert(&pp->asserts, t->text, pp->mem);
    cp_str ans = {0};
    cp_pptok *after = t->next;
    if (after->kind == CP_LPAREN && !cp_readanswer(pp, after, &ans, &after)) {
        cp_pperror(pp, t, 0, S("missing ')' to complete answer"));
        return;
    }
    if (after->kind != CP_EOF) cp_pperror(pp, after, 1, S("extra tokens after assertion"));
    if (un) {
        // without an answer every answer goes
        cp_answer *keep = 0, **tail = &keep;
        for (cp_answer *a = (cp_answer *)m->val; a && ans.data; a = a->next) {
            if (!cp_equals(a->text, ans)) {
                *tail = a;
                tail = &a->next;
            }
        }
        *tail = 0;
        m->val = keep;
    } else if (!ans.data) {
        cp_pperror(pp, t, 0, S("missing '(' after predicate"));
    } else if (!cp_hasanswer((cp_answer *)m->val, ans)) {
        cp_answer *a = cp_new(pp->mem, 1, cp_answer);
        a->text = ans;
        a->next = (cp_answer *)m->val;
        m->val = a;
    }
}

// Add the dialect keywords for the compiler: the embedded ones unless it
// is GCC-like, and SDCC's words that elsewhere are names for SDCC.
static void cp_sdcckw(cp_pp *pp, cp_map **kw)
{
    if (!cp_findmacro(pp, S("__GNUC__"))) {
        for (iz i = 0; i < cp_countof(cp_embeddedkeywords); i++) {
            cp_map *m = cp_upsert(kw, cp_cstr(cp_embeddedkeywords[i].name), pp->mem);
            m->ival = cp_embeddedkeywords[i].kind;
        }
    }
    if (!cp_findmacro(pp, S("__SDCC")) && !cp_findmacro(pp, S("SDCC"))) return;
    for (iz i = 0; i < cp_countof(cp_sdcckeywords); i++) {
        cp_map *m = cp_upsert(kw, cp_cstr(cp_sdcckeywords[i].name), pp->mem);
        m->ival = cp_sdcckeywords[i].kind;
    }
}

static cp_pptok *cp_replacedefined(cp_pp *pp, cp_pptok *t, i32 curfile)
{
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    while (t->kind != CP_EOF) {
        if (t->kind == CP_IDENT && cp_equals(t->text, S("defined"))) {
            cp_pptok *start = t;
            t = t->next;
            b32 paren = t->kind == CP_LPAREN;
            if (paren) t = t->next;
            b32 v = 0;
            if (t->kind == CP_IDENT) {
                v = cp_findmacro(pp, t->text) != 0;
                t = t->next;
            } else {
                cp_pperror(pp, t, 0, S("macro name missing after defined"));
            }
            if (paren) {
                if (t->kind == CP_RPAREN) t = t->next;
                else cp_pperror(pp, t, 0, S("missing ) after defined"));
            }
            tail = tail->next = cp_newtok(pp, start, CP_INTLIT, v ? S("1") : S("0"));
            continue;
        }
        if (t->kind == CP_HASH && t->next->kind == CP_IDENT) {
            // assertion test: #pred or #pred(answer)
            cp_pptok *start = t;
            cp_map *m = cp_upsert(&pp->asserts, t->next->text, 0);
            cp_answer *answers = m ? (cp_answer *)m->val : 0;
            b32 v = answers != 0;
            t = t->next->next;
            if (t->kind == CP_LPAREN) {
                cp_str ans = {0};
                if (!cp_readanswer(pp, t, &ans, &t)) {
                    cp_pperror(pp, start, 0, S("missing ')' to complete answer"));
                }
                v = cp_hasanswer(answers, ans);
            }
            tail = tail->next = cp_newtok(pp, start, CP_INTLIT, v ? S("1") : S("0"));
            continue;
        }
        // feature queries, including Clang's __is_identifier,
        // __building_module and __is_target_* that its headers test
        if (t->kind == CP_IDENT && t->next->kind == CP_LPAREN &&
            (cp_startswith(t->text, S("__has_")) || cp_equals(t->text, S("__is_identifier")) ||
             cp_equals(t->text, S("__building_module")) ||
             cp_startswith(t->text, S("__is_target_")))) {
            cp_str w = t->text;
            cp_pptok *start = t;
            cp_pptok *close;
            cp_pptok *after = cp_skipgroup(t->next, &close);
            cp_pptok *arg = t->next->next;
            i64 v = 0;
            if (cp_equals(w, S("__has_include")) || cp_equals(w, S("__has_include_next")) ||
                cp_equals(w, S("__has_embed"))) {
                cp_str name;
                b32 angled;
                cp_pptok *r;
                if (cp_headername(pp, arg, &r, &name, &angled, 1)) {  // __has_include(MACRO) too
                    cp_str path, src;
                    i32 dir;
                    b32 sys;
                    b32 next = cp_equals(w, S("__has_include_next"));
                    v = cp_findinclude(pp, name, angled, curfile, next, &path, &src, &dir, &sys);
                    if (v && cp_equals(w, S("__has_embed"))) v = src.len ? 1 : 2;
                }
            } else if (cp_equals(w, S("__has_attribute")) ||
                       cp_equals(w, S("__has_c_attribute"))) {
                v = arg->kind == CP_IDENT || arg->kind >= CP_K_AUTO;
            } else if (cp_equals(w, S("__has_builtin"))) {
                v = arg->kind == CP_IDENT && cp_hasbuiltin(pp, arg->text);
            } else if (cp_equals(w, S("__is_identifier"))) {
                v = arg->kind == CP_IDENT && !cp_upsert(&pp->kw, arg->text, 0);
            } else if (cp_equals(w, S("__has_feature")) || cp_equals(w, S("__has_extension"))) {
                v = arg->kind == CP_IDENT && cp_hasfeature(pp, arg->text, w.data[6] == 'e');
            }
            // __has_cpp_attribute, __has_warning: report 0
            tail = tail->next = cp_newtok(pp, start, CP_INTLIT, v ? S("1") : S("0"));
            t = after;
            continue;
        }
        tail = tail->next = cp_copytok(pp, t);
        t = t->next;
    }
    tail->next = t;
    return head.next;
}

static b32 cp_evalif(cp_pp *pp, cp_pptok **rest, cp_pptok *t)
{
    cp_pptok *start = t;
    i32 curfile = t->file;
    cp_pptok *line = cp_copyline(pp, rest, t);
    if (line->kind == CP_EOF) {
        cp_pperror(pp, start, 0, S("#if with no expression"));
        return 0;
    }
    line = cp_replacedefined(pp, line, curfile);
    pp->inif = 1;
    line = cp_expandlist(pp, line);
    pp->inif = 0;
    line = cp_replacedefined(pp, line, curfile);
    cp_ifparser ip = {0};
    ip.pp = pp;
    ip.t = line;
    cp_ifval v = cp_ifexpr(&ip);
    if (ip.err || ip.t->kind != CP_EOF) {
        cp_pperror(pp, start, 0, S("invalid #if expression"));
        return 0;
    }
    return v.v != 0;
}

// --- includes

static cp_str cp_dirname(cp_str path)
{
    iz i = path.len;
    while (i > 0 && path.data[i-1] != '/' && path.data[i-1] != '\\') i--;
    return cp_span(path.data, path.data + (i ? i-1 : 0));
}

static b32 cp_tryread(cp_pp *pp, cp_str path, cp_str *src)
{
    cp_map *m = cp_upsert(&pp->filemap, path, 0);
    if (m && m->ival && pp->files[m->ival-1].src.data) {
        *src = pp->files[m->ival-1].src;
        return 1;
    }
    if (!pp->cfg || !pp->cfg->readfile) return 0;
    cp_str s = pp->cfg->readfile(pp->cfg->ctx, path, pp->mem->arena);
    if (!s.data) return 0;
    *src = s;
    return 1;
}

static cp_str cp_joinpath(cp_pp *pp, cp_str dir, cp_str name)
{
    if (!dir.len) return cp_copy(pp->mem, name);
    cp_str sep = S("/");
    if (dir.data[dir.len-1] == '/') sep.len = 0;
    return cp_concat(pp->mem, cp_concat(pp->mem, dir, sep), name);
}

// Directory indices: -2 current file's directory, then quote dirs,
// include dirs, system dirs numbered consecutively from 0.
static b32 cp_findinclude(cp_pp *pp, cp_str name, b32 angled, i32 curfile, b32 next,
                          cp_str *path, cp_str *src, i32 *dir, b32 *sys)
{
    cp_config *c = pp->cfg;
    i32 nq = c ? c->nquotedirs : 0;
    i32 ni = c ? c->nincludedirs : 0;
    i32 ns = c ? c->nsystemdirs : 0;
    *sys = 0;
    if (!name.len) return 0;
    if (name.data[0] == '/' || (name.len > 2 && name.data[1] == ':')) {
        *path = cp_copy(pp->mem, name);
        *dir = -1;
        return cp_tryread(pp, *path, src);
    }
    i32 start = angled ? nq : -2;
    if (next && curfile >= 0 && curfile < pp->nfiles && pp->filedir[curfile] >= 0) {
        start = pp->filedir[curfile] + 1;
    }
    if (start == -2) {
        cp_str d = curfile >= 0 && curfile < pp->nfiles ? cp_dirname(pp->files[curfile].path) : S("");
        cp_str p = cp_joinpath(pp, d, name);
        if (cp_tryread(pp, p, src)) {
            *path = p;
            *dir = -2;
            *sys = curfile >= 0 && curfile < pp->nfiles && pp->files[curfile].system;
            return 1;
        }
        start = 0;
    }
    for (i32 i = start < 0 ? 0 : start; i < nq+ni+ns; i++) {
        cp_str d = i < nq ? c->quotedirs[i] : i < nq+ni ? c->includedirs[i-nq] : c->systemdirs[i-nq-ni];
        cp_str p = cp_joinpath(pp, d, name);
        if (cp_tryread(pp, p, src)) {
            *path = p;
            *dir = i;
            *sys = i >= nq+ni;
            return 1;
        }
    }
    return 0;
}

// Detect "#ifndef X / #define X ... #endif" spanning the whole file.
static cp_str cp_detectguard(cp_pptok *t)
{
    cp_str none = {0};
    if (t->kind != CP_HASH || !cp_equals(t->next->text, S("ifndef"))) return none;
    cp_pptok *name = t->next->next;
    if (name->kind != CP_IDENT) return none;
    cp_pptok *d = name->next;
    if (d->kind != CP_HASH || !(d->flags & CP_TF_BOL) || !cp_equals(d->next->text, S("define")) ||
        !cp_equals(d->next->next->text, name->text)) {
        return none;
    }
    i32 depth = 0;
    for (cp_pptok *p = t; p->kind != CP_EOF; p = p->next) {
        if (p->kind != CP_HASH || !(p->flags & CP_TF_BOL)) continue;
        cp_str w = p->next->text;
        if (cp_equals(w, S("if")) || cp_equals(w, S("ifdef")) || cp_equals(w, S("ifndef"))) {
            depth++;
        } else if (depth == 1 && (cp_equals(w, S("else")) || cp_startswith(w, S("elif")))) {
            return none;  // gcc's limits.h re-enters for its #else branch
        } else if (cp_equals(w, S("endif"))) {
            if (--depth == 0) {
                cp_pptok *after = cp_skipline(p->next);
                return after->kind == CP_EOF ? name->text : none;
            }
        }
    }
    return none;
}

static cp_pptok *cp_enterfile(cp_pp *pp, i32 file, cp_pptok *rest)
{
    cp_pptok *t = cp_lexfile(&pp->ls, pp->files[file].src, file);
    cp_str guard = cp_detectguard(t);
    if (guard.len) {
        cp_map *g = cp_upsert(&pp->guards, pp->files[file].path, pp->mem);
        cp_str *gs = cp_new(pp->mem, 1, cp_str);
        *gs = guard;
        g->val = gs;
    }
    // replace EOF with a file-end marker
    cp_pptok head = {0};
    head.next = t;
    cp_pptok *p = &head;
    while (p->next->kind != CP_EOF) p = p->next;
    cp_pptok *end = p->next;
    end->kind = CP_PP_FILEEND;
    end->flags = CP_TF_BOL;
    end->line = pp->depth + 1;  // include depth this marker closes
    end->next = rest;
    pp->depth++;
    return head.next;
}

// A path with "./" and "dir/../" steps removed, so that one file reached
// two ways is known as one (for #pragma once and #import).
static cp_str cp_normpath(cp_pp *pp, cp_str path)
{
    // keep a stack of segments, then join them
    cp_str *seg = cp_new(pp->mem, path.len/2 + 1, cp_str);
    i32 n = 0;
    b32 absolute = path.len && path.data[0] == '/';
    for (iz i = 0; i < path.len;) {
        iz e = i;
        while (e < path.len && path.data[e] != '/') e++;
        cp_str s = cp_span(path.data+i, path.data+e);
        if (!s.len || cp_equals(s, S("."))) {
            // nothing
        } else if (cp_equals(s, S("..")) && n && !cp_equals(seg[n-1], S(".."))) {
            n--;
        } else if (!(cp_equals(s, S("..")) && absolute)) {
            seg[n++] = s;
        }
        i = e + 1;
    }
    char *buf = cp_new(pp->mem, path.len+2, char);
    iz len = 0;
    if (absolute) buf[len++] = '/';
    for (i32 k = 0; k < n; k++) {
        if (k) buf[len++] = '/';
        cp_memcpy(buf+len, seg[k].data, seg[k].len);
        len += seg[k].len;
    }
    return cp_span(buf, buf+len);
}

// Mark a file never to be entered again (#pragma once, #import).
static void cp_markonce(cp_pp *pp, i32 file)
{
    cp_upsert(&pp->once, cp_normpath(pp, pp->files[file].path), pp->mem);
    if (pp->nonce == pp->caponce) {
        i32 cap = pp->caponce ? pp->caponce*2 : 16;
        i32 *n = cp_new(pp->mem, cap, i32);
        if (pp->nonce) cp_memcpy(n, pp->oncefiles, pp->nonce*(iz)sizeof(i32));
        pp->oncefiles = n;
        pp->caponce = cap;
    }
    pp->oncefiles[pp->nonce++] = file;
}

// Is this file one marked once? By path, or else, as GCC does when it
// cannot tell, by the same name and contents: "../dir/x.h" is "x.h".
static b32 cp_isonce(cp_pp *pp, cp_str key, cp_str path, cp_str src)
{
    if (cp_upsert(&pp->once, key, 0)) return 1;
    cp_str base = cp_basename(path);
    for (i32 i = 0; i < pp->nonce; i++) {
        cp_file *f = &pp->files[pp->oncefiles[i]];
        if (cp_equals(cp_basename(f->path), base) && cp_equals(f->src, src)) return 1;
    }
    return 0;
}

static cp_pptok *cp_doinclude(cp_pp *pp, cp_pptok *hash, cp_pptok *t, b32 next, b32 import)
{
    if (pp->noinclude) return cp_skipline(t);  // preprocessed input has them
    cp_str name;
    b32 angled;
    cp_pptok *rest;
    cp_pptok *dir = hash->next;
    if (!cp_headername(pp, t, &rest, &name, &angled, 1)) {
        cp_pperror(pp, t, 0, S("expected \"file\" or <file>"));
        return cp_skipline(t);
    }
    if (!cp_atbol(rest)) {
        rest = cp_skipline(rest);
    }
    cp_include *inc = cp_new(pp->mem, 1, cp_include);
    inc->from = hash->file;
    inc->line = hash->line;
    inc->next = next;
    inc->file = -1;
    {
        cp_str q = angled ? S("<") : S("\"");
        cp_str qe = angled ? S(">") : S("\"");
        inc->spelled = cp_concat(pp->mem, cp_concat(pp->mem, q, name), qe);
    }
    cp_listpush(pp->mem, &pp->includetail, &pp->nincludes, inc);

    cp_str path, src;
    i32 d;
    b32 sys;
    if (!cp_findinclude(pp, name, angled, hash->file, next, &path, &src, &d, &sys)) {
        cp_pperror(pp, dir, 0, cp_msg2(pp, "file not found: ", inc->spelled));
        return rest;
    }
    cp_map *fm = cp_upsert(&pp->filemap, path, pp->mem);
    if (!fm->ival) {
        fm->ival = 1 + cp_addfile(pp, path, src, d, sys);
        pp->files[fm->ival-1].parent = hash->file;
    } else if (!pp->files[fm->ival-1].src.data) {
        pp->files[fm->ival-1].src = src;  // named by a line marker earlier
    }
    i32 file = (i32)fm->ival - 1;
    pp->filedir[file] = d;
    pp->files[file].system = sys;
    inc->file = file;

    cp_str key = cp_normpath(pp, path);
    if (cp_isonce(pp, key, path, src) || (import && cp_upsert(&pp->entered, key, 0))) {
        inc->skipped = 1;
        return rest;
    }
    if (import) cp_markonce(pp, file);  // #import: at most once
    cp_upsert(&pp->entered, key, pp->mem);
    cp_map *g = cp_upsert(&pp->guards, path, 0);
    if (g && g->val && cp_findmacro(pp, *(cp_str *)g->val)) {
        inc->skipped = 1;
        return rest;
    }
    if (pp->depth >= 200) {
        cp_pperror(pp, dir, 0, S("#include nested too deeply"));
        return rest;
    }
    return cp_enterfile(pp, file, rest);
}

// --- conditionals

// Skip a false group. Returns the "#" of the directive that ends it.
static cp_pptok *cp_skipcond(cp_pp *pp, cp_pptok *t, i32 begline)
{
    i32 depth = 0;
    i32 file = t->file;
    cp_pptok *last = t;
    while (t->kind != CP_EOF && t->kind != CP_PP_FILEEND) {
        if (t->kind == CP_HASH && (t->flags & CP_TF_BOL)) {
            cp_str w = t->next->text;
            if (cp_equals(w, S("if")) || cp_equals(w, S("ifdef")) || cp_equals(w, S("ifndef"))) {
                depth++;
            } else if (cp_equals(w, S("endif"))) {
                if (depth-- == 0) break;
            } else if (depth == 0 && (cp_equals(w, S("elif")) || cp_equals(w, S("else")) ||
                       cp_equals(w, S("elifdef")) || cp_equals(w, S("elifndef")))) {
                break;
            }
        }
        last = t;
        t = t->next;
    }
    cp_skip *sk = cp_new(pp->mem, 1, cp_skip);
    sk->file = file;
    sk->beg = begline;
    sk->end = t->kind == CP_HASH ? t->line - 1 : last->line;
    if (sk->end < sk->beg) sk->end = sk->beg;
    cp_listpush(pp->mem, &pp->skiptail, &pp->nskips, sk);
    return t;
}

static cp_cond *cp_pushcond(cp_pp *pp, cp_pptok *at, b32 included)
{
    cp_cond *c = cp_new(pp->mem, 1, cp_cond);
    c->included = included;
    c->depth = pp->depth;
    c->file = at->file;
    c->line = at->line;
    c->next = pp->cond;
    pp->cond = c;
    return c;
}

static void cp_addpragma(cp_pp *pp, cp_pptok *at, cp_str text, b32 isop)
{
    cp_pragma *p = cp_new(pp->mem, 1, cp_pragma);
    p->tok = pp->nout;
    p->isop = isop;
    p->line = at->line;
    p->file = at->file;
    p->text = text;
    cp_listpush(pp->mem, &pp->pragmatail, &pp->npragmas, p);
}

// Text of the rest of a line, from the source buffer.
static cp_str cp_linetext(cp_pptok *t)
{
    cp_str r = {0};
    if (cp_atbol(t)) return r;
    char *beg = t->text.data;
    char *end = beg;
    for (; !cp_atbol(t); t = t->next) {
        if (t->text.data >= beg) end = t->text.data + t->text.len;
    }
    return cp_span(beg, end);
}

static cp_pptok *cp_doembed(cp_pp *pp, cp_pptok *hash, cp_pptok *t)
{
    cp_str name;
    b32 angled;
    cp_pptok *rest;
    if (!cp_headername(pp, t, &rest, &name, &angled, 1)) {
        cp_pperror(pp, t, 0, S("expected \"file\" or <file>"));
        return cp_skipline(t);
    }
    i64 limit = -1;
    cp_pptok *prefix = 0, *suffix = 0, *ifempty = 0;
    // parameters
    cp_pptok *p = rest;
    while (!cp_atbol(p)) {
        cp_str w = p->text;
        if (p->kind == CP_IDENT && p->next->kind == CP_COLON && p->next->next->kind == CP_COLON) {
            p = p->next->next->next;  // vendor prefix
            w = p->text;
        }
        if (p->kind != CP_IDENT) { p = p->next; continue; }
        cp_pptok *open = p->next;
        if (open->kind != CP_LPAREN) { p = open; continue; }
        cp_pptok *close;
        cp_pptok *after = cp_skipgroup(open, &close);
        cp_pptok ch = {0};
        cp_pptok *ct = &ch;
        for (cp_pptok *q = open->next; q != close && q->kind != CP_EOF; q = q->next) {
            ct = ct->next = cp_copytok(pp, q);
        }
        ct->next = cp_neweof(pp, close);
        if (cp_equals(w, S("limit")) || cp_equals(w, S("__limit__"))) {
            cp_pptok *e = cp_expandlist(pp, cp_replacedefined(pp, ch.next, hash->file));
            cp_ifparser ip = {0};
            ip.pp = pp; ip.t = e;
            limit = cp_ifexpr(&ip).v;
        } else if (cp_equals(w, S("prefix")) || cp_equals(w, S("__prefix__"))) {
            prefix = ch.next;
        } else if (cp_equals(w, S("suffix")) || cp_equals(w, S("__suffix__"))) {
            suffix = ch.next;
        } else if (cp_equals(w, S("if_empty")) || cp_equals(w, S("__if_empty__"))) {
            ifempty = ch.next;
        }
        p = after;
    }
    rest = cp_skipline(p);
    cp_str path, src;
    i32 d;
    b32 sys;
    if (!cp_findinclude(pp, name, angled, hash->file, 0, &path, &src, &d, &sys)) {
        cp_pperror(pp, t, 0, cp_msg2(pp, "#embed file not found: ", name));
        return rest;
    }
    iz n = src.len;
    if (limit >= 0 && limit < n) n = (iz)limit;
    cp_pptok head = {0};
    cp_pptok *tail = &head;
    if (n == 0) {
        for (cp_pptok *q = ifempty; q && q->kind != CP_EOF; q = q->next) {
            tail = tail->next = cp_copytok(pp, q);
        }
    } else {
        for (cp_pptok *q = prefix; q && q->kind != CP_EOF; q = q->next) {
            tail = tail->next = cp_copytok(pp, q);
        }
        if (!pp->numbers) {
            pp->numbers = cp_new(pp->mem, 256*4, char);
            for (i32 i = 0; i < 256; i++) {
                char *s = pp->numbers + i*4;
                if (i >= 100) *s++ = (char)('0' + i/100);
                if (i >= 10) *s++ = (char)('0' + i/10%10);
                *s = (char)('0' + i%10);
            }
        }
        for (iz i = 0; i < n; i++) {
            u8 b = (u8)src.data[i];
            char *s = pp->numbers + b*4;
            cp_str txt = {s, b >= 100 ? 3 : b >= 10 ? 2 : 1};
            cp_pptok *num = cp_newtok(pp, hash, CP_INTLIT, txt);
            num->flags = i ? CP_TF_SPACE : CP_TF_BOL;
            tail = tail->next = num;
            if (i+1 < n) {
                tail = tail->next = cp_newtok(pp, hash, CP_COMMA, S(","));
                tail->flags = 0;
            }
        }
        for (cp_pptok *q = suffix; q && q->kind != CP_EOF; q = q->next) {
            tail = tail->next = cp_copytok(pp, q);
        }
    }
    tail->next = rest;
    return head.next;
}

// Handle one directive. Returns the token to continue from.
static cp_pptok *cp_directive(cp_pp *pp, cp_pptok *hash)
{
    cp_pptok *t = hash->next;
    if (cp_atbol(t)) return t;  // null directive
    cp_str w = t->text;
    cp_pptok *arg = t->next;

    if (pp->nopp) {
        if (cp_equals(w, S("pragma"))) {
            cp_addpragma(pp, hash, cp_linetext(arg), 0);
        }
        return cp_skipline(t);
    }

    if (cp_equals(w, S("include")) || cp_equals(w, S("import"))) {
        return cp_doinclude(pp, hash, arg, 0, w.data[1] == 'm');
    }
    if (cp_equals(w, S("include_next"))) {
        return cp_doinclude(pp, hash, arg, 1, 0);
    }
    if (cp_equals(w, S("define"))) {
        return cp_dodefine(pp, hash, arg);
    }
    if (cp_equals(w, S("undef"))) {
        if (arg->kind == CP_IDENT && !cp_atbol(arg)) {
            cp_map *m = cp_upsert(&pp->macros, arg->text, 0);
            if (m && m->val) {
                ((cp_ppmacro *)m->val)->pub.flags |= CP_MF_UNDEF;
                m->val = 0;
            }
        } else {
            cp_pperror(pp, arg, 0, S("macro name must be an identifier"));
        }
        return cp_skipline(arg);
    }
    if (cp_equals(w, S("if"))) {
        cp_pptok *rest;
        b32 v = cp_evalif(pp, &rest, arg);
        cp_pushcond(pp, hash, v);
        return v ? rest : cp_skipcond(pp, rest, hash->line);
    }
    if (cp_equals(w, S("ifdef")) || cp_equals(w, S("ifndef"))) {
        b32 v = 0;
        if (arg->kind == CP_IDENT && !cp_atbol(arg)) {
            v = cp_findmacro(pp, arg->text) != 0;
        } else {
            cp_pperror(pp, arg, 0, S("macro name must be an identifier"));
        }
        if (w.len == 6) v = !v;  // ifndef
        cp_pushcond(pp, hash, v);
        cp_pptok *rest = cp_skipline(arg);
        return v ? rest : cp_skipcond(pp, rest, hash->line);
    }
    if (cp_equals(w, S("elif")) || cp_equals(w, S("elifdef")) || cp_equals(w, S("elifndef"))) {
        cp_cond *c = pp->cond;
        if (!c || c->depth != pp->depth || c->ctx == 2) {
            cp_pperror(pp, hash, 0, S("#elif without #if"));
            return cp_skipline(arg);
        }
        c->ctx = 1;
        if (c->included) {
            return cp_skipcond(pp, cp_skipline(arg), hash->line);
        }
        b32 v;
        cp_pptok *rest;
        if (w.len == 4) {
            v = cp_evalif(pp, &rest, arg);
        } else {
            v = arg->kind == CP_IDENT && cp_findmacro(pp, arg->text);
            if (w.len == 8) v = !v;
            rest = cp_skipline(arg);
        }
        if (v) {
            c->included = 1;
            return rest;
        }
        return cp_skipcond(pp, rest, hash->line);
    }
    if (cp_equals(w, S("else"))) {
        cp_cond *c = pp->cond;
        if (!c || c->depth != pp->depth || c->ctx == 2) {
            cp_pperror(pp, hash, 0, S("#else without #if"));
            return cp_skipline(arg);
        }
        c->ctx = 2;
        cp_pptok *rest = cp_skipline(arg);
        if (c->included) {
            return cp_skipcond(pp, rest, hash->line);
        }
        c->included = 1;
        return rest;
    }
    if (cp_equals(w, S("endif"))) {
        cp_cond *c = pp->cond;
        if (!c || c->depth != pp->depth) {
            cp_pperror(pp, hash, 0, S("#endif without #if"));
        } else {
            pp->cond = c->next;
        }
        return cp_skipline(arg);
    }
    if (cp_equals(w, S("pragma"))) {
        cp_str text = cp_linetext(arg);
        if (arg->kind == CP_IDENT && cp_equals(arg->text, S("once"))) {
            cp_markonce(pp, hash->file);
        } else if (arg->kind == CP_IDENT && (cp_equals(arg->text, S("push_macro")) ||
                   cp_equals(arg->text, S("pop_macro")))) {
            cp_pptok *s = arg->next->next;
            if (arg->next->kind == CP_LPAREN && s->kind == CP_STRLIT && s->text.len >= 2) {
                cp_str name = cp_span(s->text.data+1, s->text.data+s->text.len-1);
                cp_map *st = cp_upsert(&pp->pushed, name, pp->mem);
                if (arg->text.data[1] == 'u') {
                    cp_macrostack *e = cp_new(pp->mem, 1, cp_macrostack);
                    e->m = cp_findmacro(pp, name);
                    e->next = (cp_macrostack *)st->val;
                    st->val = e;
                } else if (st->val) {
                    cp_macrostack *e = (cp_macrostack *)st->val;
                    st->val = e->next;
                    cp_map *slot = cp_upsert(&pp->macros, name, pp->mem);
                    slot->val = e->m;
                }
            }
        }
        // like gcc -E, #pragma once is the preprocessor's alone: not kept
        b32 once = arg->kind == CP_IDENT && cp_equals(arg->text, S("once"));
        if (!once) cp_addpragma(pp, hash, text, 0);
        return cp_skipline(arg);
    }
    if (cp_equals(w, S("error")) || cp_equals(w, S("warning"))) {
        cp_str text = cp_linetext(arg);
        cp_pperror(pp, hash, w.data[0] == 'w', cp_msg2(pp, w.data[0]=='w' ? "#warning " : "#error ", text));
        return cp_skipline(arg);
    }
    if (cp_equals(w, S("embed"))) {
        return cp_doembed(pp, hash, arg);
    }
    if (cp_equals(w, S("assert")) || cp_equals(w, S("unassert"))) {
        cp_pptok *rest;
        cp_doassert(pp, cp_copyline(pp, &rest, arg), w.data[0] == 'u');
        return rest;
    }
    if (cp_equals(w, S("line")) && !(pp->cfg && pp->cfg->nopp)) {
        // "#line X" with macros: the lexer took the plain forms already
        cp_pptok *rest;
        cp_pptok *l = cp_expandlist(pp, cp_copyline(pp, &rest, arg));
        if (l->kind == CP_INTLIT && l->text.len && cp_isdigit((u8)l->text.data[0])) {
            b32 ok = 1;
            i32 fl;
            cp_pptok lm = *hash;
            lm.lmline = (i32)cp_parseintval(l->text, &fl, &ok);
            lm.lmfile = -1;
            cp_pptok *f = l->next;
            if (f && f->kind == CP_STRLIT && f->text.len >= 2 && f->text.data[0] == '"') {
                lm.lmfile = cp_internfile(pp, cp_span(f->text.data+1, f->text.data+f->text.len-1));
            }
            lm.line = hash->line;  // the next line is lmline
            cp_setline(pp, &lm);
        }
        return rest;
    }
    if (cp_equals(w, S("line")) || cp_equals(w, S("ident")) || cp_equals(w, S("sccs")) ||
        t->kind == CP_INTLIT) {
        return cp_skipline(arg);
    }
    cp_pperror(pp, t, 1, cp_msg2(pp, "unknown directive #", w));
    return cp_skipline(arg);
}

static void cp_emit(cp_pp *pp, cp_pptok *t)
{
    if (t->kind == CP_PP_LINE) {  // one that rode along in macro arguments
        cp_setline(pp, t);
        return;
    }
    cp_pptok *c = cp_copytok(pp, t);
    cp_mappos(pp, &c->file, &c->line);
    if (pp->outtail) pp->outtail->next = c; else pp->outhead = c;
    pp->outtail = c;
    if (c->exp) {
        cp_expansion *e = pp->exparr[c->exp-1];
        if (e->tok < 0 || e->tok > pp->nout) e->tok = pp->nout;
        e->endtok = pp->nout + 1;
    }
    pp->nout++;
}

// Turn _Pragma("x") and __pragma(x) into pragma records.
static b32 cp_pragmaop(cp_pp *pp, cp_pptok **rest, cp_pptok *t)
{
    if (t->kind != CP_IDENT || t->next->kind != CP_LPAREN) return 0;
    if (cp_equals(t->text, S("_Pragma"))) {
        cp_pptok *s = t->next->next;
        cp_pptok *after = s->next->next;
        if (s->kind != CP_STRLIT || s->next->kind != CP_RPAREN) {
            // the operand may come out of a macro, as GCC allows:
            // _Pragma(STR(GCC diagnostic ignored "-Wx"))
            cp_pptok *close;
            after = cp_skipgroup(t->next, &close);
            if (close->kind != CP_RPAREN) return 0;
            cp_pptok head = {0};
            cp_pptok *tail = &head;
            for (cp_pptok *p = t->next->next; p != close; p = p->next) {
                tail = tail->next = cp_copytok(pp, p);
            }
            tail->next = cp_neweof(pp, close);
            s = cp_expandlist(pp, head.next);
            if (s->kind != CP_STRLIT || s->next->kind != CP_EOF) return 0;
        }
        // destringize: drop prefix and quotes, unescape \" and \\ .
        cp_str q = s->text;
        iz i = 0;
        while (i < q.len && q.data[i] != '"') i++;
        cp_str body = cp_span(q.data+i+1, q.data+q.len-1);
        char *buf = cp_new(pp->mem, body.len+1, char);
        iz n = 0;
        for (iz j = 0; j < body.len; j++) {
            if (body.data[j]=='\\' && j+1 < body.len && (body.data[j+1]=='"' || body.data[j+1]=='\\')) j++;
            buf[n++] = body.data[j];
        }
        cp_addpragma(pp, t, cp_span(buf, buf+n), 0);
        *rest = after;
        return 1;
    }
    if (cp_equals(t->text, S("__pragma"))) {
        cp_pptok *close;
        cp_pptok *after = cp_skipgroup(t->next, &close);
        iz len = 16;
        for (cp_pptok *p = t; p != after; p = p->next) len += p->text.len + 1;
        char *buf = cp_new(pp->mem, len, char);
        iz n = 0;
        for (cp_pptok *p = t; p != after && p->kind != CP_EOF; p = p->next) {
            if (p->kind == CP_OTHER && p->text.len && (p->text.data[0] == '"' || p->text.data[0] == '\'')) {
                // printed on one line, the stray quote would pair with
                // a later one and swallow the code after it
                cp_pperror(pp, p, 0, S("unterminated literal in __pragma"));
            }
            if (p != t && (p->flags & CP_TF_SPACE)) buf[n++] = ' ';
            cp_memcpy(buf+n, p->text.data, p->text.len);
            n += p->text.len;
        }
        cp_addpragma(pp, t, cp_span(buf, buf+n), 1);
        *rest = after;
        return 1;
    }
    return 0;
}

static void cp_ppmain(cp_pp *pp, cp_pptok *t)
{
    for (;;) {
        if (t->kind == CP_EOF) break;
        if (t->kind == CP_PP_FILEEND) {
            i32 d = t->line;
            while (pp->cond && pp->cond->depth >= d) {
                cp_cond *c = pp->cond;
                cp_diag(pp->diags, 0, -1, c->file, c->line, 1, S("unterminated conditional directive"));
                pp->cond = c->next;
            }
            pp->depth = d - 1;
            if (pp->predefs && d == 1) pp->predefs = 0;
            t = t->next;
            continue;
        }
        if (t->kind == CP_PP_LINE) {  // reached, so not in a skipped group
            cp_setline(pp, t);
            t = t->next;
            continue;
        }
        if (t->kind == CP_HASH && (t->flags & CP_TF_BOL) && !t->macro) {
            t = cp_directive(pp, t);
            continue;
        }
        if (!pp->nopp && cp_expand(pp, &t, t)) continue;
        if (cp_pragmaop(pp, &t, t)) continue;
        cp_emit(pp, t);
        t = t->next;
    }
}

// ----------------------------------------------------------------------
// Literals

static i32 cp_hexval(i32 c)
{
    if (cp_isdigit(c)) return c - '0';
    return (c|32) - 'a' + 10;
}

// Parse an integer literal's value and suffix. Value saturates on
// overflow (flag CP_NF_OVERFLOW).
static u64 cp_parseintval(cp_str s, i32 *flags, b32 *ok)
{
    char *p = s.data, *e = s.data + s.len;
    u64 v = 0;
    i32 base = 10;
    *flags = 0;
    if (e-p >= 2 && p[0]=='0' && (p[1]|32)=='x') { base = 16; p += 2; }
    else if (e-p >= 2 && p[0]=='0' && (p[1]|32)=='b') { base = 2; p += 2; }
    else if (e-p >= 2 && p[0]=='0' && (p[1]|32)=='o') { base = 8; p += 2; }  // C2y
    else if (e-p >= 1 && p[0]=='0') { base = 8; }
    b32 any = 0;
    for (; p < e; p++) {
        i32 c = (u8)*p;
        if (c == '\'') continue;
        i32 d;
        if (cp_isdigit(c)) d = c - '0';
        else if (base == 16 && cp_isxdigit(c)) d = cp_hexval(c);
        else break;
        if (d >= base) { *ok = 0; break; }
        any = 1;
        u64 nv = v*(u64)base + (u64)d;
        if ((nv - (u64)d)/(u64)base != v) *flags |= CP_NF_OVERFLOW;
        v = nv;
    }
    if (!any && base != 8) *ok = 0;
    // suffixes
    for (; p < e; p++) {
        i32 c = *p | 32;
        if (c == 'u') *flags |= CP_NF_UNSIGNED;
        else if (c == 'l') {
            if (p+1 < e && (p[1]|32) == 'l') { *flags |= CP_NF_LONGLONG; p++; }
            else *flags |= CP_NF_LONG;
        } else if (c == 'w' && p+1 < e && (p[1]|32) == 'b') { *flags |= CP_NF_BITINT; p++; }
        else if (c == 'z') *flags |= CP_NF_SIZE;
        else if (c == 'i' && p+1 < e && cp_isdigit((u8)p[1])) {
            // MSVC sized suffixes: i8 i16 i32 i64 i128
            i32 bits = 0;
            for (p++; p < e && cp_isdigit((u8)*p); p++) {
                if (bits < 1000) bits = bits*10 + (*p - '0');
            }
            p--;
            if (bits == 64) *flags |= CP_NF_LONGLONG;
            else if (bits != 8 && bits != 16 && bits != 32 && bits != 128) *ok = 0;
        }
        else if (c == 'i' || c == 'j') {}  // GNU imaginary
        else { *ok = 0; break; }
    }
    if (*flags & CP_NF_OVERFLOW) v = UINT64_MAX;
    return v;
}

// Decode one escape or character at *pp, advancing it. Returns a code
// point (or raw byte value for \x and octal escapes).
static u32 cp_decodechar(char **pp, char *e, b32 *raw)
{
    char *p = *pp;
    *raw = 0;
    u8 c = (u8)*p++;
    if (c != '\\') {
        if (c < 0x80) { *pp = p; return c; }
        // UTF-8 sequence
        i32 n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        u32 v = n==3 ? c&7u : n==2 ? c&15u : n==1 ? c&31u : c;
        for (i32 i = 0; i < n && p < e && ((u8)*p & 0xc0) == 0x80; i++) {
            v = v<<6 | ((u8)*p++ & 63u);
        }
        if (!n) *raw = 1;
        *pp = p;
        return v;
    }
    if (p >= e) { *pp = p; return '\\'; }
    c = (u8)*p++;
    u32 v;
    switch (c) {
    case 'n': v = '\n'; break;
    case 't': v = '\t'; break;
    case 'r': v = '\r'; break;
    case 'a': v = 7; break;
    case 'b': v = 8; break;
    case 'f': v = 12; break;
    case 'v': v = 11; break;
    case 'e': case 'E': v = 27; break;
    case 'x':
        v = 0;
        *raw = 1;
        while (p < e && cp_isxdigit((u8)*p)) v = v*16 + (u32)cp_hexval((u8)*p++);
        break;
    case 'u': case 'U': {
        i32 n = c == 'u' ? 4 : 8;
        v = 0;
        for (i32 i = 0; i < n && p < e && cp_isxdigit((u8)*p); i++) {
            v = v*16 + (u32)cp_hexval((u8)*p++);
        }
    } break;
    default:
        if (c >= '0' && c <= '7') {
            v = c - '0';
            for (i32 i = 0; i < 2 && p < e && *p >= '0' && *p <= '7'; i++) {
                v = v*8 + (u32)(*p++ - '0');
            }
            *raw = 1;
        } else {
            v = c;  // \\ \' \" \? and unknown escapes
        }
    }
    *pp = p;
    return v;
}

static i64 cp_parsecharval(cp_str s, b32 *ok)
{
    char *p = s.data, *e = s.data + s.len;
    while (p < e && *p != '\'') p++;
    b32 prefixed = p != s.data;
    if (p >= e) { *ok = 0; return 0; }
    p++;
    e--;  // closing quote
    b32 utf16 = p - s.data == 2 && s.data[0] == 'u';  // u'x', not u8'x'
    i64 v = 0;
    i32 n = 0;
    while ((p = cp_skipsplices(p, e)) < e) {
        b32 raw;
        u32 c;
        if (!prefixed && *p != '\\' && (u8)*p >= 0x80) {
            c = (u8)*p++;  // a plain constant holds UTF-8 bytes: 'é' is two
        } else {
            c = cp_decodechar(&p, e, &raw);
        }
        if (utf16 && c > 0xffff) {
            // GCC truncates a numeric escape, and keeps the low surrogate
            // of a character outside the BMP
            c = raw ? c & 0xffff : 0xdc00 | ((c - 0x10000) & 0x3ff);
        }
        if (prefixed) {
            v = c;
        } else {
            v = (i64)(i32)((u32)v << 8 | (c & 255));
        }
        n++;
    }
    if (!prefixed && n == 1) v = (i8)(u8)v;  // plain char is signed here
    return v;
}

// Parse a floating literal. Decimal digits are accumulated exactly while
// they fit in 64 bits; this is correctly rounded for typical literals.
static double cp_parsefloat(cp_str s)
{
    char *p = s.data, *e = s.data + s.len;
    if (e-p >= 2 && p[0]=='0' && (p[1]|32)=='x') {
        p += 2;
        double m = 0;
        i32 exp = 0;
        b32 dot = 0;
        for (; p < e; p++) {
            if (*p == '.') { dot = 1; continue; }
            if (*p == '\'') continue;
            if (!cp_isxdigit((u8)*p)) break;
            m = m*16 + cp_hexval((u8)*p);
            if (dot) exp -= 4;
        }
        if (p < e && (*p|32) == 'p') {
            p++;
            i32 sign = 1, x = 0;
            if (p < e && (*p=='+' || *p=='-')) sign = *p++ == '-' ? -1 : 1;
            for (; p < e && cp_isdigit((u8)*p); p++) x = x < 100000 ? x*10 + (*p - '0') : x;
            exp += sign*x;
        }
        for (; exp > 0; exp--) m *= 2;
        for (; exp < 0; exp++) m /= 2;
        return m;
    }
    u64 mant = 0;
    i32 exp = 0;
    b32 dot = 0;
    i32 digits = 0;
    for (; p < e; p++) {
        if (*p == '.') { dot = 1; continue; }
        if (*p == '\'') continue;
        if (!cp_isdigit((u8)*p)) break;
        if (digits < 19) {
            mant = mant*10 + (u64)(*p - '0');
            if (mant) digits++;
            if (dot) exp--;
        } else if (!dot) {
            exp++;
        }
    }
    if (p < e && (*p|32) == 'e') {
        p++;
        i32 sign = 1, x = 0;
        if (p < e && (*p=='+' || *p=='-')) sign = *p++ == '-' ? -1 : 1;
        for (; p < e && cp_isdigit((u8)*p); p++) x = x < 100000 ? x*10 + (*p - '0') : x;
        exp += sign*x;
    }
    // Scale by exact powers of ten where possible.
    static const double pow10[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };
    double v = (double)mant;
    if (!mant) return 0;
    if (exp >= 0) {
        while (exp > 22) { v *= 1e22; exp -= 22; if (v > 1e308) return v*10; }
        v *= pow10[exp];
    } else {
        exp = -exp;
        while (exp > 22) { v /= 1e22; exp -= 22; if (v == 0) return 0; }
        v /= pow10[exp];
    }
    return v;
}

// ----------------------------------------------------------------------
// Parser and semantic analysis

typedef struct cp_scope cp_scope;
struct cp_scope {
    cp_scope *parent;
    cp_map   *names;  // ordinary identifiers -> cp_sym
    cp_map   *tags;   // tags -> cp_sym (kind CP_SYM_TAG, ty the tag type)
    i32       depth;
    b32       params;  // a function's parameter list
};

typedef struct cp_packstack cp_packstack;
struct cp_packstack {
    cp_packstack *next;
    i32           pack;
};

typedef struct {
    cp_mem    *mem;
    cp_unit   *u;
    cp_diags  *diags;
    cp_token  *toks;
    i32        pos;
    i32        ntoks;
    cp_scope  *scope;
    cp_scope  *fnscope;   // parameter scope of the last function declarator
    i32        depth;
    jmp_buf   *err;
    cp_node   *dtail;
    cp_sym    *symtail;
    cp_sym    *curfunc;
    cp_map    *labels;
    cp_listnode *gotos, **gototail;
    i32        ngotos;
    i32        target;
    i32        ptrsize;
    cp_type   *bt[CP_NBASIC];
    cp_type   *cbt[CP_NBASIC];  // _Complex versions
    cp_type   *sizet, *ptrdifft, *wchart, *char16t, *char32t, *voidptr;
    cp_type   *errtype;  // stands in for unknown types (int)
    // #pragma pack
    i32        pack;
    cp_packstack *packstack;
    i32        nextpragma;
    i32        instruct;     // parsing struct members
    i32        inoffsetof;   // parsing a __builtin_offsetof designator
    b32        clang;        // the configured compiler is Clang
    b32        c23;          // compiling C23 or later: () means (void)
    i32        unevaluated;  // inside sizeof, _Alignof, typeof or a
                             //   _Generic selector: uses read nothing
    cp_attr   *nameattrs;    // [[...]] after the last declarator name
} cp_parser;

#define cp_tok(p)      (&(p)->toks[(p)->pos])
#define cp_peekk(p, n) ((p)->toks[(p)->pos + (n) < (p)->ntoks ? (p)->pos + (n) : (p)->ntoks-1].kind)
#define cp_kind(p)     ((p)->toks[(p)->pos].kind)

static cp_str cp_tokdesc(cp_parser *p, cp_token *t)
{
    if (t->kind == CP_EOF) return S("end of file");
    cp_str q = S("'");
    return cp_concat(p->mem, cp_concat(p->mem, q, t->text), q);
}

#if defined(__GNUC__)
__attribute__((noreturn))
#elif defined(_MSC_VER)
__declspec(noreturn)
#endif
static void cp_fail(cp_parser *p, char *msg)
{
    cp_token *t = cp_tok(p);
    cp_str m = cp_concat(p->mem, cp_cstr(msg), S(" near "));
    m = cp_concat(p->mem, m, cp_tokdesc(p, t));
    cp_diag(p->diags, 0, p->pos, t->file, t->line, t->col, m);
    longjmp(*p->err, 1);
}

static void cp_warn(cp_parser *p, i32 tok, cp_str msg)
{
    if (tok < 0 || tok >= p->ntoks) tok = p->pos;
    cp_token *t = &p->toks[tok];
    cp_diag(p->diags, 1, tok, t->file, t->line, t->col, msg);
}

static void cp_next(cp_parser *p)
{
    if (p->pos < p->ntoks-1) p->pos++;
}

static b32 cp_accept(cp_parser *p, i32 kind)
{
    if (cp_kind(p) == kind) {
        cp_next(p);
        return 1;
    }
    return 0;
}

static void cp_expect(cp_parser *p, i32 kind, char *msg)
{
    if (!cp_accept(p, kind)) cp_fail(p, msg);
}

static void cp_enter(cp_parser *p)
{
    if (++p->depth > CPARSE_MAXDEPTH) cp_fail(p, "nesting too deep");
}

static cp_node *cp_newnode(cp_parser *p, i32 kind, i32 tok)
{
    cp_node *n = cp_new(p->mem, 1, cp_node);
    n->kind = kind;
    n->tok = tok;
    n->endtok = tok + 1;
    n->bitwidth = -1;
    return n;
}

static void cp_finish(cp_parser *p, cp_node *n)
{
    n->endtok = p->pos > n->tok ? p->pos : n->tok + 1;
}

// --- semantic types

static cp_type *cp_mktype(cp_parser *p, i32 kind)
{
    cp_type *t = cp_new(p->mem, 1, cp_type);
    t->kind = kind;
    t->sem = t;
    t->unqual = t;
    t->len = -1;
    t->tok = -1;
    return t;
}

static cp_type *cp_arrayof(cp_parser *p, cp_type *base, i64 len);

static cp_type *cp_qualify(cp_parser *p, cp_type *t, i32 quals)
{
    if (!t) return t;
    if (t->kind == CP_T_ARRAY && quals && (t->base ? (t->base->quals | quals) != t->base->quals : 0)) {
        // qualifiers on an array type apply to its elements
        cp_type *a = cp_arrayof(p, cp_qualify(p, t->base, quals), t->len);
        a->flags |= t->flags & CP_TF_VLA;
        return a;
    }
    if (t->kind == CP_T_ARRAY) return t;
    quals |= t->quals;
    if (quals == t->quals) return t;
    cp_type *u = t->unqual;
    if (!u->variants_) u->variants_ = cp_new(p->mem, 16, cp_type *);
    cp_type **slot = &u->variants_[quals & 15];
    if (!*slot) {
        cp_type *c = cp_new(p->mem, 1, cp_type);
        *c = *u;
        c->quals = quals;
        c->sem = c;
        c->unqual = u;
        c->variants_ = 0;
        *slot = c;
    }
    return *slot;
}

static cp_type *cp_unqualified(cp_type *t)
{
    return t ? t->unqual : t;
}

// After a tag is completed, refresh its qualified copies.
static void cp_synctag(cp_type *t)
{
    if (!t->variants_) return;
    for (i32 i = 0; i < 16; i++) {
        cp_type *v = t->variants_[i];
        if (!v) continue;
        i32 q = v->quals;
        *v = *t;
        v->quals = q;
        v->sem = v;
        v->unqual = t;
        v->variants_ = 0;
    }
}

static cp_type *cp_ptrto(cp_parser *p, cp_type *base)
{
    cp_type *t = cp_mktype(p, CP_T_POINTER);
    t->base = base;
    t->size = t->align = p->ptrsize;
    return t;
}

static cp_type *cp_arrayof(cp_parser *p, cp_type *base, i64 len)
{
    cp_type *t = cp_mktype(p, CP_T_ARRAY);
    t->base = base;
    t->len = len;
    t->align = base ? base->align : 1;
    if (len < 0 || !base || base->size < 0 ||
        (base->size && len > CP_MAXSIZE/base->size)) {
        t->size = -1;
        t->flags |= CP_TF_INCOMPLETE;
    } else {
        t->size = base->size * len;
    }
    return t;
}

static void cp_setbt(cp_parser *p, i32 basic, i64 size, i64 align)
{
    cp_type *t = cp_mktype(p, CP_T_BUILTIN);
    t->basic = basic;
    t->size = size;
    t->align = align;
    p->bt[basic] = t;
    cp_type *c = cp_mktype(p, CP_T_BUILTIN);
    c->basic = basic;
    c->flags = CP_TF_COMPLEX;
    c->size = size*2;
    c->align = align;
    p->cbt[basic] = c;
}

// The value of a predefined integer macro such as __SIZEOF_LONG_DOUBLE__
// or __STDC_VERSION__, or -1 when it is not a plain decimal number.
static i64 cp_predefint(cp_parser *p, cp_str name)
{
    i64 v = -1;
    for (cp_macro *m = p->u->macros; m; m = m->next) {
        if (!cp_equals(m->name, name) || m->nbody != 1 || m->body[0].kind != CP_INTLIT) continue;
        cp_str t = m->body[0].text;
        v = 0;
        iz i = 0;
        for (; i < t.len && cp_isdigit((u8)t.data[i]) && v < (i64)1 << 40; i++) {
            v = v*10 + (t.data[i] - '0');
        }
        for (; i < t.len; i++) {  // only an integer suffix may follow
            char c = t.data[i] | 32;
            if (c != 'l' && c != 'u') v = -1;
        }
        if (!t.len) v = -1;
    }
    return v;
}

static void cp_inittypes(cp_parser *p)
{
    i32 tg = p->target;
    p->clang = cp_predefint(p, S("__clang__")) >= 0;
    p->c23 = cp_predefint(p, S("__STDC_VERSION__")) >= 202311;
    p->ptrsize = tg == CP_TARGET_ILP32 ? 4 : 8;
    i64 lsize = tg == CP_TARGET_LP64 ? 8 : 4;
    cp_setbt(p, CP_BT_VOID, 1, 1);  // GNU: sizeof(void) == 1
    cp_setbt(p, CP_BT_BOOL, 1, 1);
    cp_setbt(p, CP_BT_CHAR, 1, 1);
    cp_setbt(p, CP_BT_SCHAR, 1, 1);
    cp_setbt(p, CP_BT_UCHAR, 1, 1);
    cp_setbt(p, CP_BT_SHORT, 2, 2);
    cp_setbt(p, CP_BT_USHORT, 2, 2);
    cp_setbt(p, CP_BT_INT, 4, 4);
    cp_setbt(p, CP_BT_UINT, 4, 4);
    cp_setbt(p, CP_BT_LONG, lsize, lsize);
    cp_setbt(p, CP_BT_ULONG, lsize, lsize);
    i64 llalign = tg == CP_TARGET_ILP32 ? 4 : 8;
    cp_setbt(p, CP_BT_LLONG, 8, llalign);
    cp_setbt(p, CP_BT_ULLONG, 8, llalign);
    cp_setbt(p, CP_BT_INT128, 16, 16);
    cp_setbt(p, CP_BT_UINT128, 16, 16);
    cp_setbt(p, CP_BT_FLOAT16, 2, 2);
    cp_setbt(p, CP_BT_BF16, 2, 2);
    cp_setbt(p, CP_BT_FLOAT, 4, 4);
    cp_setbt(p, CP_BT_DOUBLE, 8, llalign);
    if (tg == CP_TARGET_LLP64) cp_setbt(p, CP_BT_LDOUBLE, 8, 8);
    else if (tg == CP_TARGET_ILP32) cp_setbt(p, CP_BT_LDOUBLE, 12, 4);
    else cp_setbt(p, CP_BT_LDOUBLE, 16, 16);
    // The compiler's own word, where it gives one: compilers for one
    // target model disagree (MSVC 8 bytes, mingw-w64 16)
    i64 ld = cp_predefint(p, S("__SIZEOF_LONG_DOUBLE__"));
    if (ld == 8 || ld == 16) cp_setbt(p, CP_BT_LDOUBLE, ld, ld);
    if (ld == 12) cp_setbt(p, CP_BT_LDOUBLE, 12, 4);
    cp_setbt(p, CP_BT_FLOAT32, 4, 4);
    cp_setbt(p, CP_BT_FLOAT64, 8, llalign);
    cp_setbt(p, CP_BT_FLOAT32X, 8, llalign);
    cp_setbt(p, CP_BT_FLOAT64X, p->bt[CP_BT_LDOUBLE]->size, p->bt[CP_BT_LDOUBLE]->align);
    cp_setbt(p, CP_BT_FLOAT128, 16, 16);
    cp_setbt(p, CP_BT_IBM128, 16, 16);
    cp_setbt(p, CP_BT_DEC32, 4, 4);
    cp_setbt(p, CP_BT_DEC64, 8, 8);
    cp_setbt(p, CP_BT_DEC128, 16, 16);
    cp_setbt(p, CP_BT_NULLPTR, p->ptrsize, p->ptrsize);
    p->sizet    = p->bt[tg == CP_TARGET_LLP64 ? CP_BT_ULLONG : CP_BT_ULONG];
    p->ptrdifft = p->bt[tg == CP_TARGET_LLP64 ? CP_BT_LLONG : CP_BT_LONG];
    if (tg == CP_TARGET_ILP32) {
        p->sizet = p->bt[CP_BT_UINT];
        p->ptrdifft = p->bt[CP_BT_INT];
    }
    p->wchart   = p->bt[tg == CP_TARGET_LLP64 ? CP_BT_USHORT : CP_BT_INT];
    p->char16t  = p->bt[CP_BT_USHORT];
    p->char32t  = p->bt[CP_BT_UINT];
    p->voidptr  = cp_ptrto(p, p->bt[CP_BT_VOID]);
    p->errtype  = p->bt[CP_BT_INT];
}

static b32 cp_isinteger(cp_type *t)
{
    if (!t) return 0;
    if (t->kind == CP_T_ENUM || t->kind == CP_T_BITINT) return 1;
    return t->kind == CP_T_BUILTIN && !(t->flags & CP_TF_COMPLEX) &&
           t->basic >= CP_BT_BOOL && t->basic <= CP_BT_UINT128;
}

static b32 cp_isfloating(cp_type *t)
{
    return t && t->kind == CP_T_BUILTIN && ((t->basic >= CP_BT_FLOAT16 && t->basic <= CP_BT_DEC128) || (t->flags & CP_TF_COMPLEX));
}

static b32 cp_isarith(cp_type *t)  { return cp_isinteger(t) || cp_isfloating(t); }
static b32 cp_isptr(cp_type *t)    { return t && t->kind == CP_T_POINTER; }
static b32 cp_isscalar(cp_type *t)
{
    return cp_isarith(t) || cp_isptr(t) || (t && t->kind == CP_T_BLOCKPTR);
}

static b32 cp_isunsigned(cp_parser *p, cp_type *t)
{
    if (!t) return 0;
    if (t->kind == CP_T_ENUM) return t->base ? cp_isunsigned(p, t->base) : 0;
    if (t->kind == CP_T_BITINT) return (t->bits & CP_B_UNSIGNED) != 0;
    if (t->kind == CP_T_POINTER) return 1;
    if (t->kind != CP_T_BUILTIN) return 0;
    switch (t->basic) {
    case CP_BT_BOOL: case CP_BT_UCHAR: case CP_BT_USHORT: case CP_BT_UINT:
    case CP_BT_ULONG: case CP_BT_ULLONG: case CP_BT_UINT128:
        return 1;
    }
    return 0;
}

static i32 cp_rank(cp_type *t)
{
    if (t->kind == CP_T_BITINT) return 0;
    switch (t->basic) {
    case CP_BT_BOOL: return 1;
    case CP_BT_CHAR: case CP_BT_SCHAR: case CP_BT_UCHAR: return 2;
    case CP_BT_SHORT: case CP_BT_USHORT: return 3;
    case CP_BT_INT: case CP_BT_UINT: return 4;
    case CP_BT_LONG: case CP_BT_ULONG: return 5;
    case CP_BT_LLONG: case CP_BT_ULLONG: return 6;
    case CP_BT_INT128: case CP_BT_UINT128: return 7;
    }
    return 4;
}

static cp_type *cp_promote(cp_parser *p, cp_type *t)
{
    if (!t) return t;
    t = cp_unqualified(t);
    if (t->kind == CP_T_ENUM) t = t->base ? cp_unqualified(t->base) : p->bt[CP_BT_INT];
    if (t->kind == CP_T_BUILTIN && !(t->flags & CP_TF_COMPLEX) && cp_rank(t) < 4 && cp_isinteger(t)) {
        return p->bt[CP_BT_INT];
    }
    return t;
}

static cp_type *cp_tosigned(cp_parser *p, cp_type *t, b32 uns)
{
    i32 b = t->basic;
    switch (cp_rank(t)) {
    case 4: b = uns ? CP_BT_UINT : CP_BT_INT; break;
    case 5: b = uns ? CP_BT_ULONG : CP_BT_LONG; break;
    case 6: b = uns ? CP_BT_ULLONG : CP_BT_LLONG; break;
    case 7: b = uns ? CP_BT_UINT128 : CP_BT_INT128; break;
    }
    return p->bt[b];
}

static i32 cp_floatrank(cp_type *t)
{
    switch (t->basic) {
    case CP_BT_FLOAT16: case CP_BT_BF16: return 1;
    case CP_BT_FLOAT: case CP_BT_FLOAT32: return 2;
    case CP_BT_DOUBLE: case CP_BT_FLOAT64: case CP_BT_FLOAT32X: return 3;
    case CP_BT_LDOUBLE: case CP_BT_FLOAT64X: return 4;
    case CP_BT_FLOAT128: case CP_BT_IBM128: return 5;
    case CP_BT_DEC32: return 6;
    case CP_BT_DEC64: return 7;
    case CP_BT_DEC128: return 8;
    }
    return 0;  // integer (complex int)
}

// Usual arithmetic conversions.
static cp_type *cp_uac(cp_parser *p, cp_type *a, cp_type *b)
{
    if (!a || !b) return a ? cp_unqualified(a) : b ? cp_unqualified(b) : 0;
    a = cp_unqualified(a);
    b = cp_unqualified(b);
    if (a->kind == CP_T_VECTOR) return a;
    if (b->kind == CP_T_VECTOR) return b;
    if (cp_isfloating(a) || cp_isfloating(b)) {
        b32 cx = (a->flags & CP_TF_COMPLEX) || (b->flags & CP_TF_COMPLEX);
        i32 ra = cp_isfloating(a) ? cp_floatrank(a) : 0;
        i32 rb = cp_isfloating(b) ? cp_floatrank(b) : 0;
        cp_type *w = ra >= rb ? a : b;
        if (!cp_floatrank(w)) {
            // complex integer types
            cp_type *ia = (a->flags & CP_TF_COMPLEX) ? p->bt[a->basic] : a;
            cp_type *ib = (b->flags & CP_TF_COMPLEX) ? p->bt[b->basic] : b;
            cp_type *r = cp_uac(p, ia, ib);
            return r && r->kind == CP_T_BUILTIN ? p->cbt[r->basic] : r;
        }
        return cx ? p->cbt[w->basic] : p->bt[w->basic];
    }
    a = cp_promote(p, a);
    b = cp_promote(p, b);
    if (a->kind == CP_T_BITINT || b->kind == CP_T_BITINT) {
        i64 wa = a->kind == CP_T_BITINT ? a->len : a->size*8;
        i64 wb = b->kind == CP_T_BITINT ? b->len : b->size*8;
        if (wa != wb) return wa > wb ? a : b;
        b32 uns = cp_isunsigned(p, a) || cp_isunsigned(p, b);
        if (a->kind == CP_T_BITINT && b->kind == CP_T_BITINT) return cp_isunsigned(p, a) || !uns ? a : b;
        // a standard type outranks a bit-precise one of its width, so the
        // result is the standard type, unsigned if either operand is
        cp_type *std = a->kind == CP_T_BITINT ? b : a;
        return uns ? cp_tosigned(p, std, 1) : std;
    }
    if (a == b) return a;
    b32 ua = cp_isunsigned(p, a), ub = cp_isunsigned(p, b);
    if (ua == ub) return cp_rank(a) >= cp_rank(b) ? a : b;
    cp_type *u = ua ? a : b;
    cp_type *s = ua ? b : a;
    if (cp_rank(u) >= cp_rank(s)) return u;
    if (s->size > u->size) return s;
    return cp_tosigned(p, s, 1);
}

static cp_type *cp_decay(cp_parser *p, cp_type *t)
{
    if (!t) return t;
    if (t->kind == CP_T_ARRAY) return cp_ptrto(p, t->base);
    if (t->kind == CP_T_FUNC) return cp_ptrto(p, t);
    return cp_unqualified(t);
}

static b32 cp_compatible(cp_type *a, cp_type *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->quals != b->quals) return 0;
    a = a->unqual;
    b = b->unqual;
    if (a == b) return 1;
    if (a->kind == CP_T_ENUM && b->kind == CP_T_BUILTIN) return a->base && cp_compatible(a->base, b);
    if (b->kind == CP_T_ENUM && a->kind == CP_T_BUILTIN) return b->base && cp_compatible(b->base, a);
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case CP_T_BUILTIN:
        return a->basic == b->basic && (a->flags & CP_TF_COMPLEX) == (b->flags & CP_TF_COMPLEX);
    case CP_T_POINTER:
    case CP_T_BLOCKPTR:
        return cp_compatible(a->base, b->base);
    case CP_T_ARRAY:
        if (a->len >= 0 && b->len >= 0 && a->len != b->len) return 0;
        return cp_compatible(a->base, b->base);
    case CP_T_VECTOR:
        return a->size == b->size && cp_compatible(a->base, b->base);
    case CP_T_BITINT:
        return a->len == b->len && (a->bits & CP_B_UNSIGNED) == (b->bits & CP_B_UNSIGNED);
    case CP_T_FUNC:
        if (!cp_compatible(a->base, b->base)) return 0;
        if ((a->flags & CP_TF_NOPROTO) || (b->flags & CP_TF_NOPROTO)) return 1;
        if (a->nargs != b->nargs) return 0;
        if ((a->flags & CP_TF_VARIADIC) != (b->flags & CP_TF_VARIADIC)) return 0;
        for (i32 i = 0; i < a->nargs; i++) {
            if (!cp_compatible(cp_unqualified(a->args[i]), cp_unqualified(b->args[i]))) return 0;
        }
        return 1;
    }
    return 0;  // distinct tags
}

// Truncate a constant to the width and signedness of t.
static i64 cp_truncate(cp_parser *p, cp_type *t, i64 v)
{
    if (!t) return v;
    if (t->kind == CP_T_BUILTIN && t->basic == CP_BT_BOOL) return v != 0;
    i64 bits = t->kind == CP_T_BITINT ? t->len : t->size*8;
    if (t->kind == CP_T_ENUM && t->base) return cp_truncate(p, t->base, v);
    if (bits <= 0 || bits >= 64) return v;
    u64 mask = ((u64)1 << bits) - 1;
    u64 u = (u64)v & mask;
    if (!cp_isunsigned(p, t) && (u >> (bits-1)) & 1) u |= ~mask;
    return (i64)u;
}

// --- scopes and symbols

static void cp_pushscope(cp_parser *p)
{
    cp_scope *s = cp_new(p->mem, 1, cp_scope);
    s->parent = p->scope;
    s->depth = p->scope ? p->scope->depth + 1 : 0;
    p->scope = s;
}

static void cp_popscope(cp_parser *p)
{
    if (p->scope->parent) p->scope = p->scope->parent;
}

static cp_sym *cp_lookup(cp_parser *p, cp_str name)
{
    for (cp_scope *s = p->scope; s; s = s->parent) {
        cp_map *m = cp_upsert(&s->names, name, 0);
        if (m && m->val) return (cp_sym *)m->val;
    }
    return 0;
}

static cp_sym *cp_lookuptag(cp_parser *p, cp_str name, b32 local)
{
    for (cp_scope *s = p->scope; s; s = local ? 0 : s->parent) {
        cp_map *m = cp_upsert(&s->tags, name, 0);
        if (m && m->val) return (cp_sym *)m->val;
    }
    return 0;
}

static b32 cp_istypedef(cp_parser *p, cp_str name)
{
    cp_sym *s = cp_lookup(p, name);
    return s && s->kind == CP_SYM_TYPEDEF;
}

static cp_sym *cp_newsym(cp_parser *p, cp_str name, i32 kind)
{
    cp_sym *s = cp_new(p->mem, 1, cp_sym);
    s->name = name;
    s->kind = kind;
    s->depth = p->scope->depth;
    if (p->scope->depth > 0) s->func = p->curfunc;
    if (p->symtail) p->symtail->next = s; else p->u->syms = s;
    p->symtail = s;
    return s;
}

static cp_symref *cp_addref(cp_parser *p, cp_symref **head, cp_symref **tail, cp_node *n, cp_sym *other, i32 flags)
{
    cp_symref *r = cp_new(p->mem, 1, cp_symref);
    r->node = n;
    r->sym = other;
    r->flags = flags;
    if (*tail) (*tail)->next = r; else *head = r;
    *tail = r;
    return r;
}

static void cp_bindname(cp_parser *p, cp_str name, cp_sym *s)
{
    cp_map *m = cp_upsert(&p->scope->names, name, p->mem);
    m->val = s;
}

// Mark how an identifier expression is used (through member access).
static void cp_markuse(cp_node *n, i32 flags)
{
    for (;;) {
        if (!n) return;
        if (n->kind == CP_E_MEMBER && n->op == CP_DOT) { n = n->a; continue; }
        if (n->kind == CP_E_INDEX && n->a && n->a->ty && n->a->ty->kind == CP_T_ARRAY) { n = n->a; continue; }
        break;
    }
    if (n->kind != CP_E_IDENT || !n->sym || !n->useref_) return;
    n->useref_->flags = (n->useref_->flags & ~CP_RF_READ) | flags;
    if (flags & CP_RF_READ) n->sym->flags |= CP_YF_READ;
    if (flags & CP_RF_WRITE) n->sym->flags |= CP_YF_WRITTEN;
    if (flags & CP_RF_ADDR) n->sym->flags |= CP_YF_ADDRTAKEN;
}

// --- attributes

typedef struct {
    i64 aligned;     // 0 if none
    b32 packed;
    i64 vecsize;     // vector_size bytes, 0 if none
    i32 mode;        // CP_BT_* from mode(), -1 if none
    b32 transparent;
    i32 ms;          // 1 ms_struct, -1 gcc_struct, 0 unspecified
    b32 noreturn;
    b32 abi;         // ms_abi or sysv_abi: GCC makes a distinct type
} cp_attrinfo;

static cp_str cp_stripunders(cp_str s)
{
    if (s.len > 4 && s.data[0]=='_' && s.data[1]=='_' &&
        s.data[s.len-1]=='_' && s.data[s.len-2]=='_') {
        s.data += 2;
        s.len -= 4;
    }
    return s;
}

static cp_node *cp_parseexprat(cp_parser *p, i32 beg, i32 end);

// The lowest-ranked standard signed type of 64 bits on this target.
static i32 cp_int64bt(cp_parser *p)
{
    return p->bt[CP_BT_LONG]->size == 8 ? CP_BT_LONG : CP_BT_LLONG;
}

static void cp_scanattrs(cp_parser *p, cp_attr *a, cp_attrinfo *info)
{
    for (; a; a = a->next) {
        if (a->kind == CP_A_ALIGNAS) {
            // _Alignas(type or expr): tokens beg+2 .. end-2
            cp_node *e = cp_parseexprat(p, a->beg+2, a->end-1);
            if (e && e->isconst == 1 && e->cval > info->aligned && e->cval <= CP_MAXALIGN) {
                info->aligned = e->cval;
            }
            if (e && e->kind == CP_E_TYPEARG && e->ty && e->ty->align > info->aligned) info->aligned = e->ty->align;
            continue;
        }
        if (a->kind != CP_A_GNU && a->kind != CP_A_DECLSPEC && a->kind != CP_A_STD) continue;
        // Walk the attribute list: name [ (args) ] , ...
        i32 i = a->beg + (a->kind == CP_A_STD ? 2 : a->kind == CP_A_GNU ? 3 : 2);
        i32 end = a->end - (a->kind == CP_A_DECLSPEC ? 1 : 2);
        while (i < end) {
            cp_token *t = &p->toks[i];
            if (t->kind == CP_COMMA) { i++; continue; }
            if (a->kind == CP_A_STD && i+2 < end && p->toks[i+1].kind == CP_COLON && p->toks[i+2].kind == CP_COLON) {
                i += 3;
                continue;
            }
            cp_str name = cp_stripunders(t->text);
            i32 argbeg = -1, argend = -1;
            i++;
            if (i < end && p->toks[i].kind == CP_LPAREN) {
                i32 depth = 0;
                argbeg = i+1;
                for (; i < end; i++) {
                    if (p->toks[i].kind == CP_LPAREN) depth++;
                    if (p->toks[i].kind == CP_RPAREN && --depth == 0) break;
                }
                argend = i;
                i++;
            }
            if (cp_equals(name, S("aligned")) || cp_equals(name, S("align"))) {
                i64 v = 16;  // the target's biggest alignment
                if (argbeg >= 0) {
                    cp_node *e = cp_parseexprat(p, argbeg, argend);
                    v = e && e->isconst == 1 ? e->cval : 0;
                }
                if (v > info->aligned && v <= CP_MAXALIGN) info->aligned = v;
            } else if (cp_equals(name, S("noreturn"))) {
                info->noreturn = 1;
            } else if (cp_equals(name, S("ms_abi")) || cp_equals(name, S("sysv_abi"))) {
                info->abi = 1;
            } else if (cp_equals(name, S("packed"))) {
                info->packed = 1;
            } else if (cp_equals(name, S("vector_size")) && argbeg >= 0) {
                cp_node *e = cp_parseexprat(p, argbeg, argend);
                if (e && e->isconst == 1 && e->cval <= CP_MAXALIGN) info->vecsize = e->cval;
            } else if (cp_equals(name, S("mode")) && argbeg >= 0) {
                cp_str m = cp_stripunders(p->toks[argbeg].text);
                if (cp_equals(m, S("QI")) || cp_equals(m, S("byte"))) info->mode = CP_BT_SCHAR;
                else if (cp_equals(m, S("HI"))) info->mode = CP_BT_SHORT;
                else if (cp_equals(m, S("SI"))) info->mode = CP_BT_INT;
                // GCC takes the lowest-ranked type of the mode's size:
                // DImode is long on LP64, long long elsewhere
                else if (cp_equals(m, S("DI"))) info->mode = cp_int64bt(p);
                else if (cp_equals(m, S("TI"))) info->mode = CP_BT_INT128;
                else if (cp_equals(m, S("word")) || cp_equals(m, S("pointer")) ||
                         cp_equals(m, S("unwind_word")) || cp_equals(m, S("libgcc_cmp_return")) ||
                         cp_equals(m, S("libgcc_shift_count"))) {
                    info->mode = p->ptrsize == 4 ? CP_BT_INT : cp_int64bt(p);
                }
                else if (cp_equals(m, S("SF"))) info->mode = CP_BT_FLOAT;
                else if (cp_equals(m, S("DF"))) info->mode = CP_BT_DOUBLE;
                else if (cp_equals(m, S("XF"))) info->mode = CP_BT_LDOUBLE;
                else if (cp_equals(m, S("TF"))) info->mode = CP_BT_FLOAT128;
                else if (cp_equals(m, S("HF"))) info->mode = CP_BT_FLOAT16;
                // complex modes: the complex base type makes them complex
                else if (cp_equals(m, S("SC"))) info->mode = CP_BT_FLOAT;
                else if (cp_equals(m, S("DC"))) info->mode = CP_BT_DOUBLE;
                else if (cp_equals(m, S("XC"))) info->mode = CP_BT_LDOUBLE;
                else if (cp_equals(m, S("TC"))) info->mode = CP_BT_FLOAT128;
                else if (cp_equals(m, S("HC"))) info->mode = CP_BT_FLOAT16;
            } else if (cp_equals(name, S("transparent_union"))) {
                info->transparent = 1;
            } else if (cp_equals(name, S("ms_struct"))) {
                info->ms = 1;
            } else if (cp_equals(name, S("gcc_struct"))) {
                info->ms = -1;
            }
        }
    }
}

// ms_abi written inside a declarator, "int (__attribute__((ms_abi)) *f)()",
// sits on its pointer: find it there too.
static void cp_scanabi(cp_parser *p, cp_type *syn, cp_attrinfo *ai)
{
    for (i32 depth = 0; syn && depth < 64; syn = syn->base, depth++) {
        if (!syn->attrs) continue;
        cp_attrinfo t = {0};
        t.mode = -1;
        cp_scanattrs(p, syn->attrs, &t);
        ai->abi |= t.abi;
    }
}

// Apply mode() and vector_size() to a type.
static cp_type *cp_applytypeattrs(cp_parser *p, cp_type *t, cp_attrinfo *ai)
{
    if (!t) return t;
    if (ai->noreturn && t->kind == CP_T_POINTER && t->base && t->base->kind == CP_T_FUNC &&
        !(t->base->flags & CP_TF_NORETURN)) {
        // GCC's "volatile function": a distinct type, which only
        // --typecheck needs to know about
        cp_type *f = cp_new(p->mem, 1, cp_type);
        *f = *t->base;
        f->sem = f;
        f->unqual = f;
        f->variants_ = 0;
        f->flags |= CP_TF_NORETURN;
        t = cp_qualify(p, cp_ptrto(p, f), t->quals);
    }
    if (ai->abi && p->target == CP_TARGET_LP64) {
        // like noreturn above: only --typecheck needs to know
        cp_type **fp = t->kind == CP_T_FUNC ? &t :
                       t->kind == CP_T_POINTER && t->base && t->base->kind == CP_T_FUNC ? &t->base : 0;
        if (fp && !((*fp)->flags & CP_TF_ABI)) {
            cp_type *f = cp_new(p->mem, 1, cp_type);
            *f = **fp;
            f->sem = f;
            f->unqual = f;
            f->variants_ = 0;
            f->flags |= CP_TF_ABI;
            t = fp == &t ? f : cp_qualify(p, cp_ptrto(p, f), t->quals);
        }
    }
    if (ai->mode >= 0) {
        i32 m = ai->mode;
        if (t->kind == CP_T_BUILTIN || t->kind == CP_T_ENUM) {
            b32 uns = cp_isunsigned(p, t);
            if (m == CP_BT_LLONG && p->target == CP_TARGET_LP64) m = CP_BT_LONG;
            if (uns) {
                switch (m) {
                case CP_BT_SCHAR: m = CP_BT_UCHAR; break;
                case CP_BT_SHORT: m = CP_BT_USHORT; break;
                case CP_BT_INT: m = CP_BT_UINT; break;
                case CP_BT_LONG: m = CP_BT_ULONG; break;
                case CP_BT_LLONG: m = CP_BT_ULLONG; break;
                case CP_BT_INT128: m = CP_BT_UINT128; break;
                }
            }
            if ((t->flags & CP_TF_COMPLEX) && cp_floatrank(p->bt[m])) {
                t = cp_qualify(p, p->cbt[m], t->quals);
            } else {
                t = cp_qualify(p, p->bt[m], t->quals);
            }
        }
    }
    if (ai->vecsize > 0 && t->kind != CP_T_FUNC) {
        cp_type *v = cp_mktype(p, CP_T_VECTOR);
        v->base = cp_unqualified(t);
        v->size = ai->vecsize;
        v->align = ai->vecsize;
        if (v->align > 64 || (v->align & (v->align-1))) v->align = 16;
        v->len = t->size > 0 ? ai->vecsize / t->size : 0;
        t = cp_qualify(p, v, t->quals);
    }
    return t;
}

// A copy of t with a different alignment (typedef with aligned()).
static cp_type *cp_realign(cp_parser *p, cp_type *t, i64 align)
{
    if (!t || align <= 0 || align == t->align) return t;
    cp_type *c = cp_new(p->mem, 1, cp_type);
    *c = *t;
    c->align = align;
    c->sem = c;
    c->variants_ = 0;
    if (!c->quals) c->unqual = c;
    return c;
}

// --- token classification

static b32 cp_isqualkw(i32 k)
{
    return k == CP_K_CONST || k == CP_K_VOLATILE || k == CP_K_RESTRICT;
}

static b32 cp_isstoragekw(i32 k)
{
    switch (k) {
    case CP_K_TYPEDEF: case CP_K_EXTERN: case CP_K_STATIC: case CP_K_AUTO:
    case CP_K_REGISTER: case CP_K_THREAD_LOCAL: case CP_K_CONSTEXPR:
    case CP_K_INLINE: case CP_K_NORETURN: case CP_K_FORCEINLINE:
        return 1;
    }
    return 0;
}

static b32 cp_istypespeckw(i32 k)
{
    switch (k) {
    case CP_K_VOID: case CP_K_CHAR: case CP_K_SHORT: case CP_K_INT:
    case CP_K_LONG: case CP_K_SIGNED: case CP_K_UNSIGNED: case CP_K_FLOAT:
    case CP_K_DOUBLE: case CP_K_BOOL: case CP_K_COMPLEX: case CP_K_IMAGINARY:
    case CP_K_STRUCT: case CP_K_UNION: case CP_K_ENUM: case CP_K_TYPEOF:
    case CP_K_TYPEOF_UNQUAL: case CP_K_BITINT: case CP_K_DECIMAL32:
    case CP_K_DECIMAL64: case CP_K_DECIMAL128: case CP_K_INT128:
    case CP_K_FLOAT128: case CP_K_FLOAT80: case CP_K_IBM128: case CP_K_FLOAT16:
    case CP_K_FLOAT32: case CP_K_FLOAT64: case CP_K_FLOAT32X:
    case CP_K_FLOAT64X: case CP_K_FLOAT128X: case CP_K_BF16: case CP_K_FP16:
    case CP_K_INT8: case CP_K_INT16: case CP_K_INT32: case CP_K_INT64:
    case CP_K_AUTO_TYPE: case CP_K_ATOMIC:
        return 1;
    }
    return 0;
}

// Attribute-like words double as ordinary names in code that does not
// know them as keywords ("int __far;"). Where only a name fits, turn the
// token into an identifier.
static b32 cp_softname(cp_parser *p, i32 i)
{
    i32 k = p->toks[i].kind;
    if (k != CP_K_KWATTR && k != CP_K_KWATTRARGS) return 0;
    p->toks[i].kind = CP_IDENT;
    return 1;
}

// The newer floating types, which some compilers lack and their headers
// then define as typedef names.
static b32 cp_isfloatkw(i32 k)
{
    switch (k) {
    case CP_K_FLOAT128: case CP_K_FLOAT80: case CP_K_IBM128: case CP_K_FLOAT16:
    case CP_K_FLOAT32: case CP_K_FLOAT64: case CP_K_FLOAT32X: case CP_K_FLOAT64X:
    case CP_K_FLOAT128X: case CP_K_BF16: case CP_K_FP16:
        return 1;
    }
    return 0;
}

static b32 cp_namefollows(i32 k)
{
    return k == CP_SEMI || k == CP_COMMA || k == CP_RPAREN || k == CP_LBRACK ||
           k == CP_ASSIGN || k == CP_COLON;
}

static b32 cp_isattrkw(cp_parser *p, i32 i)
{
    i32 k = p->toks[i].kind;
    return k == CP_K_ATTRIBUTE || k == CP_K_DECLSPEC || k == CP_K_KWATTR ||
           k == CP_K_KWATTRARGS ||
           (k == CP_LBRACK && i+1 < p->ntoks && p->toks[i+1].kind == CP_LBRACK);
}

// Code whose headers are missing still uses their annotation macros:
// "EXPORT int f(void);", "int WINAPI f(void);", "void f(void) NORETURN;",
// "f(EV_P_ struct ev_io *w)". An undeclared name standing where only an
// annotation fits is taken for one, judged by the token after it:
//   CP_MA_LEAD   before the specifiers: more specifiers follow
//   CP_MA_MID    after a complete type: the declarator follows
//   CP_MA_TRAIL  after a declarator: what ends a declarator follows
//   CP_MA_MORE   like LEAD but after a type, so without arguments: in K&R
//                "int f(a, b) int a;" f(a, b) is the declarator
enum { CP_MA_LEAD, CP_MA_MID, CP_MA_TRAIL, CP_MA_MORE };

// Index just past an annotation macro at i: its name and, if any, its
// parenthesized arguments ("ALIGN(16)").
static i32 cp_skipgroupat(cp_parser *p, i32 i);
static i32 cp_macroattrend(cp_parser *p, i32 i)
{
    i++;
    return p->toks[i].kind == CP_LPAREN ? cp_skipgroupat(p, i) : i;
}

// Does an implicit-int K&R definition start at i: "f(a, b) int a; {"?
// It looks like an annotation macro with arguments, but the names it then
// declares are the ones in its parentheses.
static b32 cp_iskrdef(cp_parser *p, i32 i)
{
    i32 j = i + 2, end = cp_skipgroupat(p, i+1) - 1;  // ")" at end
    for (; j < end; j += 2) {
        if (p->toks[j].kind != CP_IDENT) return 0;
        if (j+1 < end && p->toks[j+1].kind != CP_COMMA) return 0;
    }
    j = end + 1;
    while (j < p->ntoks-1 && (cp_istypespeckw(p->toks[j].kind) || cp_isqualkw(p->toks[j].kind) ||
                             cp_isstoragekw(p->toks[j].kind) || p->toks[j].kind == CP_STAR)) {
        j++;
    }
    if (p->toks[j].kind != CP_IDENT) return 0;
    for (i32 k = i + 2; k < end; k += 2) {
        if (cp_equals(p->toks[k].text, p->toks[j].text)) return 1;
    }
    return 0;
}

static i32 cp_skipattrsat(cp_parser *p, i32 i);
static b32 cp_ismacroattr(cp_parser *p, i32 i, i32 where)
{
    if (p->toks[i].kind != CP_IDENT || cp_lookup(p, p->toks[i].text)) return 0;
    if (i+2 >= p->ntoks) return 0;
    i32 j = cp_macroattrend(p, i);
    i32 k = p->toks[j].kind;
    if (where == CP_MA_MORE) {
        if (p->toks[i+1].kind == CP_LPAREN) return 0;
        where = CP_MA_LEAD;
    }
    if (where == CP_MA_LEAD && p->toks[i+1].kind == CP_LPAREN && cp_iskrdef(p, i)) return 0;
    switch (where) {
    case CP_MA_LEAD:
        // maybe a short run of them, with qualifiers and attributes:
        // "DECLSPEC SDLCALL int f(void);", "API const __attribute__((x)) int"
        for (i32 n = 0; n < 8 && j < p->ntoks-1; n++) {
            if (k == CP_IDENT && !cp_lookup(p, p->toks[j].text)) {
                j = cp_macroattrend(p, j);
            } else if (cp_isqualkw(k) && k != CP_K_ATOMIC) {
                j++;
            } else if (cp_isattrkw(p, j)) {
                j = cp_skipattrsat(p, j);
            } else {
                break;
            }
            k = p->toks[j].kind;
        }
        if (k == CP_IDENT && cp_istypedef(p, p->toks[j].text)) {
            // "EXPORT T f(void);" with T a typedef, but not "unknown_t T;"
            i32 k2 = p->toks[j+1 < p->ntoks ? j+1 : j].kind;
            return k2 == CP_IDENT || k2 == CP_STAR || k2 == CP_LPAREN || cp_isqualkw(k2);
        }
        return (cp_istypespeckw(k) && k != CP_K_ATOMIC) || cp_isstoragekw(k) ||
               k == CP_K_EXTENSION;
    case CP_MA_MID: {
        // "int WINAPI f(" but not "int a UNUSED)", where a is the name.
        // No arguments here: in K&R "int f(a) T a;" f(a) is the declarator.
        k = p->toks[i+1].kind;
        i32 k2 = p->toks[i+2].kind;
        if (k == CP_IDENT && k2 == CP_LPAREN) {
            // "int WINAPI f(void)" has parameters in the parentheses;
            // "int a UNUSED(a)" has macro arguments, and a is the name
            cp_token *t = &p->toks[i+3 < p->ntoks ? i+3 : i+2];
            return t->kind == CP_RPAREN || t->kind == CP_ELLIPSIS || cp_istypespeckw(t->kind) ||
                   cp_isqualkw(t->kind) || cp_isstoragekw(t->kind) ||
                   (t->kind == CP_IDENT && cp_istypedef(p, t->text));
        }
        return (k == CP_IDENT && (k2 == CP_IDENT || k2 == CP_LBRACK)) || k == CP_STAR;
    }
    case CP_MA_TRAIL:
        // maybe a short run of them: "void f(void) NORETURN PRINTF(1, 2);"
        for (i32 n = 0; n < 4 && k == CP_IDENT && !cp_lookup(p, p->toks[j].text); n++) {
            j = cp_macroattrend(p, j);
            k = p->toks[j].kind;
        }
        return k == CP_SEMI || k == CP_COMMA || k == CP_ASSIGN || k == CP_LBRACE ||
               k == CP_RPAREN || k == CP_COLON || cp_isattrkw(p, j);
    }
    return 0;
}

static cp_attr *cp_macroattr(cp_parser *p);

// Index just past a balanced group starting at token i ("(" or "[").
static i32 cp_skipgroupat(cp_parser *p, i32 i)
{
    i32 depth = 0;
    for (; i < p->ntoks-1; i++) {
        i32 k = p->toks[i].kind;
        if (k == CP_LPAREN || k == CP_LBRACK || k == CP_LBRACE) depth++;
        if (k == CP_RPAREN || k == CP_RBRACK || k == CP_RBRACE) {
            if (--depth == 0) return i + 1;
        }
    }
    return i;
}

static i32 cp_skipattrsat(cp_parser *p, i32 i)
{
    while (i < p->ntoks-1 && cp_isattrkw(p, i)) {
        i32 k = p->toks[i].kind;
        if (k == CP_LBRACK) {
            i = cp_skipgroupat(p, i);
        } else if (k == CP_K_KWATTR) {
            i++;
        } else {
            i++;
            if (p->toks[i].kind == CP_LPAREN) i = cp_skipgroupat(p, i);
        }
    }
    return i;
}

// An undeclared identifier at token i used where only a type fits?
static b32 cp_guesstype(cp_parser *p, i32 i)
{
    cp_token *t = &p->toks[i];
    if (t->kind != CP_IDENT || cp_lookup(p, t->text)) return 0;
    if (cp_equals(t->text, S("nullptr")) || cp_equals(t->text, S("true")) ||
        cp_equals(t->text, S("false")) || cp_equals(t->text, S("__func__")) ||
        cp_equals(t->text, S("__FUNCTION__")) ||
        cp_equals(t->text, S("__PRETTY_FUNCTION__"))) {
        return 0;  // predefined values, never types
    }
    i32 k1 = p->toks[i+1 < p->ntoks ? i+1 : i].kind;
    if (k1 == CP_IDENT || cp_isqualkw(k1) || k1 == CP_K_KWATTR) return 1;
    if (k1 == CP_RPAREN && i > 0 && p->toks[i-1].kind == CP_LPAREN) {
        // "(name)" followed by an operand: a cast
        i32 k = p->toks[i+2 < p->ntoks ? i+2 : i+1].kind;
        return k == CP_IDENT || k == CP_INTLIT || k == CP_FLOATLIT || k == CP_CHARLIT ||
               k == CP_STRLIT;
    }
    if (k1 == CP_STAR) {
        i32 j = i + 1;
        while (j < p->ntoks-1 && (p->toks[j].kind == CP_STAR || cp_isqualkw(p->toks[j].kind))) j++;
        if (p->toks[j].kind == CP_RPAREN) return 1;  // (foo *)
        if (p->toks[j].kind != CP_IDENT) return 0;
        i32 k = p->toks[j+1 < p->ntoks ? j+1 : j].kind;
        return k == CP_SEMI || k == CP_ASSIGN || k == CP_COMMA || k == CP_LBRACK ||
               k == CP_RPAREN || k == CP_LPAREN;
    }
    return 0;
}

// Remember a guessed type name as an implicit typedef (of int) at file
// scope, so later uses agree.
static void cp_guessedtype(cp_parser *p, cp_str name)
{
    cp_scope *file = p->scope;
    while (file->depth > 0 && file->parent) file = file->parent;
    cp_map *m = cp_upsert(&file->names, name, p->mem);
    if (m->val) return;
    cp_sym *s = cp_new(p->mem, 1, cp_sym);
    s->name = name;
    s->kind = CP_SYM_TYPEDEF;
    s->ty = p->errtype;
    s->flags = CP_YF_IMPLICIT;
    m->val = s;
}

// Can token i begin a type name (in a cast, sizeof, or parameter)?
static b32 cp_istypestart(cp_parser *p, i32 i)
{
    cp_token *t = &p->toks[i];
    if (cp_istypespeckw(t->kind) || cp_isqualkw(t->kind)) return 1;
    if (t->kind == CP_K_ATTRIBUTE || t->kind == CP_K_KWATTR || t->kind == CP_K_KWATTRARGS) {
        i32 j = cp_skipattrsat(p, i);
        return j != i && cp_istypestart(p, j);
    }
    if (t->kind == CP_K_EXTENSION) return cp_istypestart(p, i+1);
    if (t->kind == CP_IDENT) {
        return cp_istypedef(p, t->text) || cp_guesstype(p, i) ||
               cp_ismacroattr(p, i, CP_MA_LEAD);
    }
    return 0;
}

// Can token i begin a type argument of a macro-like call, as in
// va_arg(ap, foo_t *)? An undeclared name followed by '*' only counts when
// the declarator is abstract: "foo * p" there is a multiplication.
static b32 cp_istypearg(cp_parser *p, i32 i)
{
    cp_token *t = &p->toks[i];
    if (t->kind != CP_IDENT || cp_istypedef(p, t->text)) return cp_istypestart(p, i);
    if (!cp_guesstype(p, i)) return 0;
    i32 j = i + 1;
    if (p->toks[j].kind != CP_STAR) return 1;
    while (j < p->ntoks-1 && (p->toks[j].kind == CP_STAR || cp_isqualkw(p->toks[j].kind))) j++;
    return p->toks[j].kind == CP_COMMA || p->toks[j].kind == CP_RPAREN;
}

// Can token i begin a declaration?
static b32 cp_isdeclstart(cp_parser *p, i32 i)
{
    cp_token *t = &p->toks[i];
    if (cp_isstoragekw(t->kind) || t->kind == CP_K_STATIC_ASSERT ||
        t->kind == CP_K_ALIGNAS || t->kind == CP_K_DECLSPEC || t->kind == CP_K_LABEL) {
        return 1;
    }
    if (t->kind == CP_K_EXTENSION) return cp_isdeclstart(p, i+1);
    if (cp_isattrkw(p, i)) {
        i32 j = cp_skipattrsat(p, i);
        return p->toks[j].kind != CP_SEMI && cp_isdeclstart(p, j);
    }
    return cp_istypestart(p, i);
}

// --- attribute capture

static cp_attr *cp_attrone(cp_parser *p)
{
    cp_attr *a = cp_new(p->mem, 1, cp_attr);
    a->beg = p->pos;
    i32 k = cp_kind(p);
    switch (k) {
    case CP_K_ATTRIBUTE: a->kind = CP_A_GNU; break;
    case CP_K_DECLSPEC:  a->kind = CP_A_DECLSPEC; break;
    case CP_K_ALIGNAS:   a->kind = CP_A_ALIGNAS; break;
    case CP_K_ASM:       a->kind = CP_A_ASMLABEL; break;
    case CP_LBRACK:      a->kind = CP_A_STD; break;
    default:             a->kind = CP_A_KEYWORD; break;
    }
    if (k == CP_LBRACK) {
        p->pos = cp_skipgroupat(p, p->pos);
    } else {
        cp_next(p);
        if (k != CP_K_KWATTR && cp_kind(p) == CP_LPAREN) {
            p->pos = cp_skipgroupat(p, p->pos);
        } else if (k == CP_K_KWATTRARGS && cp_equals(p->toks[a->beg].text, S("__at")) &&
                   (cp_kind(p) == CP_INTLIT || cp_kind(p) == CP_IDENT)) {
            cp_next(p);  // SDCC also writes the address bare: __at 0x80
        } else if (k == CP_K_ATTRIBUTE || k == CP_K_DECLSPEC || k == CP_K_ALIGNAS || k == CP_K_ASM) {
            cp_fail(p, "expected '('");
        }
    }
    if (p->pos >= p->ntoks-1 && a->kind != CP_A_KEYWORD) {
        p->pos = a->beg;
        cp_fail(p, "unbalanced attribute");
    }
    a->end = p->pos;
    return a;
}

static cp_attr *cp_macroattr(cp_parser *p)
{
    cp_attr *a = cp_new(p->mem, 1, cp_attr);
    a->kind = CP_A_MACRO;
    a->beg = p->pos;
    cp_warn(p, p->pos, cp_concat(p->mem, S("unknown name taken for an attribute macro: "),
                                 cp_tok(p)->text));
    p->pos = cp_macroattrend(p, p->pos);
    a->end = p->pos;
    return a;
}

static void cp_attrappend(cp_attr **list, cp_attr *a)
{
    while (*list) list = &(*list)->next;
    *list = a;
}

// Parse a run of attributes, appending to *list.
static b32 cp_attrs(cp_parser *p, cp_attr **list)
{
    b32 any = 0;
    while (cp_isattrkw(p, p->pos)) {
        cp_attrappend(list, cp_attrone(p));
        any = 1;
    }
    return any;
}

// Attributes after a declarator, including annotation macros.
static b32 cp_trailattrs(cp_parser *p, cp_attr **list)
{
    b32 any = 0;
    for (;;) {
        if (cp_isattrkw(p, p->pos)) {
            cp_attrappend(list, cp_attrone(p));
        } else if (cp_ismacroattr(p, p->pos, CP_MA_TRAIL)) {
            cp_attrappend(list, cp_macroattr(p));
        } else {
            return any;
        }
        any = 1;
    }
}

// --- specifiers

typedef struct {
    i32      storage;
    cp_type *type;      // syntactic base type
    cp_attr *attrs;
    b32      any;
    b32      hastype;
    i32      tok;
} cp_specs;

static cp_type *cp_syntype(cp_parser *p, i32 kind, i32 tok)
{
    cp_type *t = cp_new(p->mem, 1, cp_type);
    t->kind = kind;
    t->tok = tok;
    t->len = -1;
    return t;
}

static cp_type *cp_resolve(cp_parser *p, cp_type *t);
static cp_node *cp_assignexpr(cp_parser *p);
static cp_node *cp_condexpr(cp_parser *p);
static cp_node *cp_expr(cp_parser *p);
static cp_node *cp_parsetypename(cp_parser *p);
static cp_node *cp_declaration(cp_parser *p, i32 ctx);
static cp_node *cp_staticassert(cp_parser *p);
static void cp_resolvebase(cp_parser *p, cp_type *t);

static cp_type *cp_structspec(cp_parser *p);
static cp_type *cp_enumspec(cp_parser *p);

static cp_specs cp_parsespecs(cp_parser *p)
{
    cp_specs s = {0};
    s.tok = p->pos;
    i32 bits = 0;
    i32 quals = 0;
    b32 sdccbit = 0;
    cp_type *base = 0;
    for (;;) {
        i32 k = cp_kind(p);
        i32 tok = p->pos;
        if ((k == CP_K_KWATTR || k == CP_K_KWATTRARGS) && (base || bits) &&
            cp_namefollows(cp_peekk(p, 1))) {
            cp_softname(p, p->pos);  // "int __far;" names a member
            goto done;
        }
        if (cp_isfloatkw(k) && (base || bits) && cp_namefollows(cp_peekk(p, 1))) {
            // headers for compilers without the keyword declare it:
            // glibc under clang has "typedef __float128 _Float128;"
            p->toks[p->pos].kind = CP_IDENT;
            goto done;
        }
        if (cp_isattrkw(p, p->pos) || k == CP_K_ALIGNAS) {
            cp_attr *a = cp_attrone(p);
            // "int [[x]] v", "vpid __ptr64 *p": a standard or keyword attribute
            // after the type is printed there too
            if (a) a->aftertype = (a->kind == CP_A_STD || a->kind == CP_A_KEYWORD) && (base || bits);
            cp_attrappend(&s.attrs, a);
            s.any = 1;
            continue;
        }
        switch (k) {
        case CP_K_TYPEDEF:      s.storage |= CP_SC_TYPEDEF; break;
        case CP_K_EXTERN:       s.storage |= CP_SC_EXTERN; break;
        case CP_K_STATIC:       s.storage |= CP_SC_STATIC; break;
        case CP_K_AUTO:         s.storage |= CP_SC_AUTO; break;
        case CP_K_REGISTER:     s.storage |= CP_SC_REGISTER; break;
        case CP_K_THREAD_LOCAL: s.storage |= CP_SC_THREAD_LOCAL; break;
        case CP_K_CONSTEXPR:    s.storage |= CP_SC_CONSTEXPR; break;
        case CP_K_INLINE:       s.storage |= CP_SC_INLINE; break;
        case CP_K_NORETURN:     s.storage |= CP_SC_NORETURN; break;
        case CP_K_FORCEINLINE:  s.storage |= CP_SC_FORCEINLINE; break;
        case CP_K_EXTENSION:    s.storage |= CP_SC_EXTENSION; break;
        case CP_K_CONST:        quals |= CP_Q_CONST; break;
        case CP_K_VOLATILE:     quals |= CP_Q_VOLATILE; break;
        case CP_K_RESTRICT:     quals |= CP_Q_RESTRICT; break;
        case CP_K_ATOMIC:
            if (cp_peekk(p, 1) == CP_LPAREN) {
                if (base || bits) goto done;
                cp_next(p);
                cp_next(p);
                cp_node *tn = cp_parsetypename(p);
                cp_expect(p, CP_RPAREN, "expected ')'");
                base = cp_syntype(p, CP_T_ATOMIC, tok);
                base->base = tn->type;
                s.any = s.hastype = 1;
                continue;
            }
            quals |= CP_Q_ATOMIC;
            break;
        case CP_K_VOID:      bits |= CP_B_VOID; break;
        case CP_K_CHAR:      bits |= CP_B_CHAR; break;
        case CP_K_SHORT:     bits |= CP_B_SHORT; break;
        case CP_K_INT:       bits |= CP_B_INT; break;
        case CP_K_LONG:      bits |= (bits & CP_B_LONG) ? CP_B_LONGLONG : CP_B_LONG; break;
        case CP_K_SIGNED:    bits |= CP_B_SIGNED; break;
        case CP_K_UNSIGNED:  bits |= CP_B_UNSIGNED; break;
        case CP_K_FLOAT:     bits |= CP_B_FLOAT; break;
        case CP_K_DOUBLE:    bits |= CP_B_DOUBLE; break;
        case CP_K_BOOL:
            bits |= CP_B_BOOL;
            if (cp_equals(cp_tok(p)->text, S("__bit"))) sdccbit = 1;
            break;
        case CP_K_COMPLEX:   bits |= CP_B_COMPLEX; break;
        case CP_K_IMAGINARY: bits |= CP_B_IMAGINARY; break;
        case CP_K_INT128:    bits |= CP_B_INT128; break;
        case CP_K_FLOAT128:  bits |= CP_B_FLOAT128; break;
        case CP_K_FLOAT80:   bits |= CP_B_FLOAT80; break;
        case CP_K_IBM128:    bits |= CP_B_IBM128; break;
        case CP_K_FLOAT16:   bits |= CP_B_FLOAT16; break;
        case CP_K_FLOAT32:   bits |= CP_B_FLOAT32; break;
        case CP_K_FLOAT64:   bits |= CP_B_FLOAT64; break;
        case CP_K_FLOAT32X:  bits |= CP_B_FLOAT32X; break;
        case CP_K_FLOAT64X:  bits |= CP_B_FLOAT64X; break;
        case CP_K_FLOAT128X: bits |= CP_B_FLOAT128X; break;
        case CP_K_BF16:      bits |= CP_B_BF16; break;
        case CP_K_FP16:      bits |= CP_B_FP16; break;
        case CP_K_DECIMAL32: bits |= CP_B_DEC32; break;
        case CP_K_DECIMAL64: bits |= CP_B_DEC64; break;
        case CP_K_DECIMAL128:bits |= CP_B_DEC128; break;
        case CP_K_INT8:      bits |= CP_B_INT8; break;
        case CP_K_INT16:     bits |= CP_B_INT16; break;
        case CP_K_INT32:     bits |= CP_B_INT32; break;
        case CP_K_INT64:     bits |= CP_B_INT64; break;
        case CP_K_STRUCT:
        case CP_K_UNION:
            if (base) goto done;
            base = cp_structspec(p);
            s.any = s.hastype = 1;
            continue;
        case CP_K_ENUM:
            if (base) goto done;
            base = cp_enumspec(p);
            s.any = s.hastype = 1;
            continue;
        case CP_K_TYPEOF:
        case CP_K_TYPEOF_UNQUAL: {
            if (base) goto done;
            cp_next(p);
            cp_expect(p, CP_LPAREN, "expected '(' after typeof");
            base = cp_syntype(p, CP_T_TYPEOF, tok);
            if (k == CP_K_TYPEOF_UNQUAL) base->flags |= CP_TF_UNQUAL;
            if (cp_istypestart(p, p->pos)) {
                base->base = cp_parsetypename(p)->type;
            } else {
                p->unevaluated++;
                base->expr = cp_expr(p);
                p->unevaluated--;
            }
            cp_expect(p, CP_RPAREN, "expected ')'");
            s.any = s.hastype = 1;
            continue;
        }
        case CP_K_BITINT:
            if (base) goto done;
            cp_next(p);
            cp_expect(p, CP_LPAREN, "expected '(' after _BitInt");
            base = cp_syntype(p, CP_T_BITINT, tok);
            base->expr = cp_condexpr(p);
            cp_expect(p, CP_RPAREN, "expected ')'");
            s.any = s.hastype = 1;
            continue;
        case CP_K_AUTO_TYPE:
            if (base) goto done;
            base = cp_syntype(p, CP_T_AUTOTYPE, tok);
            break;
        case CP_IDENT: {
            // After a type only "unsigned X long" (more keywords) and
            // "int X f" (the declarator next) are taken: the printer moves
            // storage classes first and keeps keywords together, so it
            // could not keep X in "T X int" or "int X static"
            i32 nk = p->toks[cp_macroattrend(p, p->pos)].kind;
            b32 more = !base && cp_istypespeckw(nk) && nk != CP_K_STRUCT && nk != CP_K_UNION &&
                       nk != CP_K_ENUM && nk != CP_K_TYPEOF && nk != CP_K_TYPEOF_UNQUAL &&
                       nk != CP_K_BITINT && nk != CP_K_AUTO_TYPE && nk != CP_K_ATOMIC;
            if ((!base && !bits && cp_ismacroattr(p, p->pos, CP_MA_LEAD)) ||
                ((base || bits) && ((more && cp_ismacroattr(p, p->pos, CP_MA_MORE)) ||
                                    cp_ismacroattr(p, p->pos, CP_MA_MID)))) {
                cp_attr *a = cp_macroattr(p);
                a->aftertype = base || bits;  // printed there, or "A B *p" swaps
                a->leading = !base && !bits;  // printed before storage classes
                cp_attrappend(&s.attrs, a);
                s.any = 1;
                continue;
            }
            if (base || (bits & ~(CP_B_COMPLEX|CP_B_IMAGINARY))) goto done;
            if (cp_istypedef(p, cp_tok(p)->text) || cp_guesstype(p, p->pos)) {
                if (!cp_istypedef(p, cp_tok(p)->text)) {
                    cp_warn(p, p->pos, cp_concat(p->mem, S("unknown type name "), cp_tok(p)->text));
                    cp_guessedtype(p, cp_tok(p)->text);
                }
                base = cp_syntype(p, CP_T_NAME, tok);
                base->name = cp_tok(p)->text;
                break;
            }
            goto done;
        }
        default:
            goto done;
        }
        if (cp_istypespeckw(k) || k == CP_IDENT) s.hastype = 1;
        s.any = 1;
        cp_next(p);
    }
done:
    if (bits && !base) {
        base = cp_syntype(p, CP_T_BUILTIN, s.tok);
        base->bits = bits;
        if (sdccbit) base->flags |= CP_TF_SDCCBIT;
    } else if (bits && base) {
        base->bits |= bits;  // tolerated: e.g. "unsigned T" in odd code
    }
    if (!base) {
        base = cp_syntype(p, CP_T_IMPLICIT, s.tok);
    }
    base->quals |= quals;
    if (quals) s.any = 1;
    s.type = base;
    cp_resolvebase(p, base);
    return s;
}

// Semantic type of the base type from declaration specifiers.
static void cp_resolvebase(cp_parser *p, cp_type *t)
{
    cp_type *r = 0;
    switch (t->kind) {
    case CP_T_BUILTIN:
    case CP_T_IMPLICIT: {
        i32 b = t->bits;
        i32 bt = CP_BT_INT;
        b32 uns = (b & CP_B_UNSIGNED) != 0;
        if (b & CP_B_VOID) bt = CP_BT_VOID;
        else if (b & CP_B_BOOL) bt = CP_BT_BOOL;
        else if (b & CP_B_CHAR) bt = uns ? CP_BT_UCHAR : (b & CP_B_SIGNED) ? CP_BT_SCHAR : CP_BT_CHAR;
        else if (b & CP_B_INT8) bt = uns ? CP_BT_UCHAR : CP_BT_CHAR;
        else if (b & (CP_B_SHORT|CP_B_INT16)) bt = uns ? CP_BT_USHORT : CP_BT_SHORT;
        else if (b & CP_B_INT32) bt = uns ? CP_BT_UINT : CP_BT_INT;
        else if (b & (CP_B_LONGLONG|CP_B_INT64)) bt = uns ? CP_BT_ULLONG : CP_BT_LLONG;
        else if (b & CP_B_INT128) bt = uns ? CP_BT_UINT128 : CP_BT_INT128;
        else if (b & CP_B_DOUBLE) bt = (b & CP_B_LONG) ? CP_BT_LDOUBLE : CP_BT_DOUBLE;
        else if (b & CP_B_LONG) bt = uns ? CP_BT_ULONG : CP_BT_LONG;
        else if (b & CP_B_FLOAT) bt = CP_BT_FLOAT;
        else if (b & (CP_B_FLOAT16|CP_B_FP16)) bt = CP_BT_FLOAT16;
        else if (b & CP_B_BF16) bt = CP_BT_BF16;
        else if (b & CP_B_FLOAT32) bt = CP_BT_FLOAT32;
        else if (b & CP_B_FLOAT64) bt = CP_BT_FLOAT64;
        else if (b & CP_B_FLOAT32X) bt = CP_BT_FLOAT32X;
        else if (b & CP_B_FLOAT64X) bt = CP_BT_FLOAT64X;
        else if (b & (CP_B_FLOAT128|CP_B_FLOAT128X)) bt = CP_BT_FLOAT128;
        else if (b & CP_B_FLOAT80) bt = CP_BT_LDOUBLE;
        else if (b & CP_B_IBM128) bt = CP_BT_IBM128;
        else if (b & CP_B_DEC32) bt = CP_BT_DEC32;
        else if (b & CP_B_DEC64) bt = CP_BT_DEC64;
        else if (b & CP_B_DEC128) bt = CP_BT_DEC128;
        else if ((b & (CP_B_COMPLEX|CP_B_IMAGINARY)) && !(b & ~(CP_B_COMPLEX|CP_B_IMAGINARY))) bt = CP_BT_DOUBLE;
        else bt = uns ? CP_BT_UINT : CP_BT_INT;
        r = (b & CP_B_COMPLEX) ? p->cbt[bt] : p->bt[bt];
    } break;
    case CP_T_NAME: {
        cp_sym *s = cp_lookup(p, t->name);
        r = s && s->kind == CP_SYM_TYPEDEF && s->ty ? s->ty : p->errtype;
        if (s && s->kind == CP_SYM_TYPEDEF) {
            cp_addref(p, &s->refs, &s->lastref_, 0, 0, CP_RF_READ);
            s->nrefs++;
            s->flags |= CP_YF_READ;
        }
    } break;
    case CP_T_STRUCT:
    case CP_T_UNION:
    case CP_T_ENUM:
        r = t->sem;  // set by the tag parser
        break;
    case CP_T_TYPEOF:
        if (t->base) r = t->base->sem;
        else r = t->expr ? t->expr->ty : 0;
        if (!r) r = p->errtype;
        if (t->flags & CP_TF_UNQUAL) r = cp_unqualified(r);
        break;
    case CP_T_ATOMIC:
        r = t->base && t->base->sem ? cp_qualify(p, t->base->sem, CP_Q_ATOMIC) : p->errtype;
        break;
    case CP_T_BITINT:
        r = cp_mktype(p, CP_T_BITINT);
        r->len = t->expr && t->expr->isconst == 1 ? t->expr->cval : 32;
        if (r->len < 1 || r->len > 65535) r->len = 32;
        r->bits = t->bits & CP_B_UNSIGNED;
        r->size = r->len <= 8 ? 1 : r->len <= 16 ? 2 : r->len <= 32 ? 4 : (r->len + 63)/64*8;
        r->align = r->size > 8 ? 8 : r->size;
        break;
    case CP_T_AUTOTYPE:
        r = p->errtype;  // replaced by the initializer's type
        break;
    }
    t->sem = cp_qualify(p, r, t->quals);
}

// Semantic type of a whole syntactic declarator chain.
static cp_type *cp_resolve(cp_parser *p, cp_type *t)
{
    if (!t) return p->errtype;
    if (t->sem) return t->sem;
    cp_type *r = 0;
    switch (t->kind) {
    case CP_T_POINTER:
        r = cp_qualify(p, cp_ptrto(p, cp_resolve(p, t->base)), t->quals);
        break;
    case CP_T_BLOCKPTR:
        r = cp_mktype(p, CP_T_BLOCKPTR);
        r->base = cp_resolve(p, t->base);
        r->size = r->align = p->ptrsize;
        r = cp_qualify(p, r, t->quals);
        break;
    case CP_T_ARRAY: {
        cp_type *elem = cp_resolve(p, t->base);
        i64 len = -1;
        b32 vla = 0;
        if (t->expr) {
            if (t->expr->isconst == 1) len = t->expr->cval < 0 ? 0 : t->expr->cval;
            else vla = 1;
        } else if (t->flags & CP_TF_VLASTAR) {
            vla = 1;
        }
        r = cp_arrayof(p, elem, len);
        if (vla) r->flags |= CP_TF_VLA;
    } break;
    case CP_T_FUNC: {
        r = cp_mktype(p, CP_T_FUNC);
        // the return type's qualifiers go: const int f(void) is int(void)
        r->base = cp_unqualified(cp_resolve(p, t->base));
        r->size = 1;
        r->align = 1;
        r->flags = t->flags & CP_TF_VARIADIC;
        if ((t->flags & CP_TF_KR) || (!t->params && !p->c23)) {
            r->flags |= CP_TF_NOPROTO;  // in C23, () means (void)
            break;
        }
        i32 n = 0;
        for (cp_node *d = t->params; d; d = d->next) n++;
        // (void)
        cp_node *d0 = t->params;
        if (n == 1 && d0->list && !d0->list->text.len && d0->list->ty &&
            d0->list->ty->kind == CP_T_BUILTIN && d0->list->ty->basic == CP_BT_VOID) {
            n = 0;
        }
        r->args = cp_new(p->mem, n, cp_type *);
        r->nargs = n;
        i32 i = 0;
        for (cp_node *d = t->params; d && i < n; d = d->next, i++) {
            r->args[i] = d->list && d->list->ty ? d->list->ty : p->errtype;
        }
    } break;
    case CP_T_NONE:
        r = p->errtype;
        break;
    default:
        r = t->sem ? t->sem : p->errtype;
    }
    t->sem = r;
    return r;
}

// --- declarators

static void cp_params(cp_parser *p, cp_type *fn);

static cp_type *cp_suffixes(cp_parser *p, cp_type *base, b32 *sawname)
{
    if (cp_kind(p) == CP_LBRACK && cp_peekk(p, 1) != CP_LBRACK) {
        cp_type *t = cp_syntype(p, CP_T_ARRAY, p->pos);
        cp_next(p);
        for (;;) {
            i32 k = cp_kind(p);
            if (k == CP_K_STATIC) { t->flags |= CP_TF_STATIC; cp_next(p); }
            else if (k == CP_K_CONST) { t->quals |= CP_Q_CONST; cp_next(p); }
            else if (k == CP_K_VOLATILE) { t->quals |= CP_Q_VOLATILE; cp_next(p); }
            else if (k == CP_K_RESTRICT) { t->quals |= CP_Q_RESTRICT; cp_next(p); }
            else if (k == CP_K_ATOMIC) { t->quals |= CP_Q_ATOMIC; cp_next(p); }
            else break;
        }
        if (cp_kind(p) == CP_STAR && cp_peekk(p, 1) == CP_RBRACK) {
            t->flags |= CP_TF_VLASTAR;
            cp_next(p);
        } else if (cp_kind(p) != CP_RBRACK) {
            t->expr = cp_assignexpr(p);
        }
        cp_expect(p, CP_RBRACK, "expected ']'");
        cp_enter(p);
        t->base = cp_suffixes(p, base, sawname);
        p->depth--;
        return t;
    }
    if (cp_kind(p) == CP_LPAREN) {
        cp_type *t = cp_syntype(p, CP_T_FUNC, p->pos);
        cp_params(p, t);
        cp_enter(p);
        t->base = cp_suffixes(p, base, sawname);
        p->depth--;
        return t;
    }
    return base;
}

// At "(": does it open a nested declarator rather than parameters?
static b32 cp_isnested(cp_parser *p)
{
    i32 i = cp_skipattrsat(p, p->pos + 1);
    i32 k = p->toks[i].kind;
    if (k == CP_STAR || k == CP_XOR || k == CP_LPAREN) return 1;
    if (k == CP_LBRACK) return p->toks[i+1].kind != CP_LBRACK;
    if (k == CP_IDENT) return !cp_istypedef(p, p->toks[i].text) && !cp_guesstype(p, i);
    return 0;
}

static cp_type *cp_declarator(cp_parser *p, cp_type *base, cp_str *name, i32 *nametok, b32 *sawname)
{
    cp_enter(p);
    while (cp_kind(p) == CP_STAR || cp_kind(p) == CP_XOR) {
        cp_type *t = cp_syntype(p, cp_kind(p) == CP_STAR ? CP_T_POINTER : CP_T_BLOCKPTR, p->pos);
        cp_next(p);
        t->base = base;
        for (;;) {
            i32 k = cp_kind(p);
            if (k == CP_K_CONST) t->quals |= CP_Q_CONST;
            else if (k == CP_K_VOLATILE) t->quals |= CP_Q_VOLATILE;
            else if (k == CP_K_RESTRICT) t->quals |= CP_Q_RESTRICT;
            else if (k == CP_K_ATOMIC && cp_peekk(p, 1) != CP_LPAREN) t->quals |= CP_Q_ATOMIC;
            else if (cp_isattrkw(p, p->pos)) { cp_attrappend(&t->attrs, cp_attrone(p)); continue; }
            else break;
            cp_next(p);
        }
        base = t;
    }
    cp_type *r;
    if (cp_kind(p) == CP_LPAREN && cp_isnested(p)) {
        cp_next(p);
        cp_attr *inattrs = 0;
        cp_attrs(p, &inattrs);
        cp_type *hole = cp_syntype(p, CP_T_NONE, p->pos);
        cp_type *inner = cp_declarator(p, hole, name, nametok, sawname);
        cp_expect(p, CP_RPAREN, "expected ')' in declarator");
        cp_type *outer = cp_suffixes(p, base, sawname);
        if (inner == hole) {
            r = outer;
        } else {
            cp_type *n = inner;
            while (n->base && n->base != hole) n = n->base;
            n->base = outer;
            r = inner;
        }
        for (cp_attr *a = inattrs; a; a = a->next) a->before = 1;
        if (inattrs) {
            if (r == outer) cp_attrappend(&base->attrs, inattrs);
            else cp_attrappend(&inner->attrs, inattrs);
        }
    } else {
        cp_attr *at = 0;
        if (cp_kind(p) == CP_IDENT) {
            *name = cp_tok(p)->text;
            *nametok = p->pos;
            *sawname = 1;
            cp_next(p);
            // C23: "f [[noreturn]] (void)" gives the attributes to f
            if (cp_kind(p) == CP_LBRACK && cp_peekk(p, 1) == CP_LBRACK) {
                cp_attrs(p, &at);
                for (cp_attr *a = at; a; a = a->next) a->aftername = 1;
            }
        }
        r = cp_suffixes(p, base, sawname);
        cp_attrappend(&p->nameattrs, at);
    }
    p->depth--;
    return r;
}

static cp_node *cp_newdeclarator(cp_parser *p, cp_type *base)
{
    cp_node *d = cp_newnode(p, CP_D_DECLARATOR, p->pos);
    i32 nametok = -1;
    b32 sawname = 0;
    p->fnscope = 0;
    cp_attr *outer = p->nameattrs;  // nested: a parameter's declarator
    p->nameattrs = 0;
    d->type = cp_declarator(p, base, &d->text, &nametok, &sawname);
    if (nametok >= 0) d->tok = nametok;
    d->attrs = p->nameattrs;
    p->nameattrs = outer;
    return d;
}

// Parameter list for a function declarator, at "(".
static void cp_params(cp_parser *p, cp_type *fn)
{
    cp_expect(p, CP_LPAREN, "expected '('");
    cp_scope *outer = p->scope;
    cp_pushscope(p);
    cp_scope *scope = p->scope;
    scope->params = 1;
    cp_node head = {0};
    cp_node *tail = &head;
    i32 saveinstruct = p->instruct;
    p->instruct = 0;
    if (cp_kind(p) == CP_RPAREN) {
        // unprototyped ()
    } else if (cp_kind(p) == CP_IDENT && !cp_istypedef(p, cp_tok(p)->text) &&
               (cp_peekk(p, 1) == CP_COMMA || cp_peekk(p, 1) == CP_RPAREN)) {
        // an identifier list (the cast guess does not apply here: in
        // "int g(x) foo_t x;" the x is a name, not a type)
        fn->flags |= CP_TF_KR;
        for (;;) {
            if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected identifier");
            cp_node *n = cp_newnode(p, CP_E_IDENT, p->pos);
            n->text = cp_tok(p)->text;
            cp_next(p);
            cp_sym *s = cp_newsym(p, n->text, CP_SYM_PARAM);
            s->ty = p->bt[CP_BT_INT];
            s->decl = n;
            cp_addref(p, &s->decls, &s->lastdecl_, n, 0, CP_RF_DECL);
            cp_bindname(p, n->text, s);
            n->sym = s;
            n->ty = s->ty;
            tail = tail->next = n;
            if (!cp_accept(p, CP_COMMA)) break;
        }
    } else {
        for (;;) {
            if (cp_accept(p, CP_ELLIPSIS)) {
                fn->flags |= CP_TF_VARIADIC;
                break;
            }
            cp_node *decl = cp_newnode(p, CP_D_DECL, p->pos);
            if (cp_kind(p) == CP_IDENT && !cp_lookup(p, cp_tok(p)->text) &&
                (cp_peekk(p, 1) == CP_COMMA || cp_peekk(p, 1) == CP_RPAREN)) {
                // a lone unknown name in a prototype is a type: f(int, va_list)
                cp_warn(p, p->pos, cp_concat(p->mem, S("unknown type name "), cp_tok(p)->text));
                cp_guessedtype(p, cp_tok(p)->text);
            }
            cp_specs sp = cp_parsespecs(p);
            if (!sp.any) cp_fail(p, "expected parameter declaration");
            decl->flags = sp.storage;
            decl->type = sp.type;
            decl->attrs = sp.attrs;
            cp_scope *savefn = p->fnscope;
            cp_node *d = cp_newdeclarator(p, sp.type);
            p->fnscope = savefn;
            cp_trailattrs(p, &d->attrs);
            cp_type *ty = cp_resolve(p, d->type);
            cp_attrinfo ai = {0};
            ai.mode = -1;
            cp_scanattrs(p, sp.attrs, &ai);
            cp_scanattrs(p, d->attrs, &ai);
            ty = cp_applytypeattrs(p, ty, &ai);
            // parameter adjustment
            if (ty->kind == CP_T_ARRAY) {
                i32 q = d->type && d->type->kind == CP_T_ARRAY ? d->type->quals : 0;
                ty = cp_qualify(p, cp_ptrto(p, ty->base), q);
            } else if (ty->kind == CP_T_FUNC) {
                ty = cp_ptrto(p, ty);
            }
            d->ty = ty;
            if (d->text.len) {
                cp_sym *s = cp_newsym(p, d->text, CP_SYM_PARAM);
                s->ty = ty;
                s->decl = d;
                s->def = d;
                s->flags |= CP_YF_DEFINED;
                cp_addref(p, &s->decls, &s->lastdecl_, d, 0, CP_RF_DECL);
                cp_bindname(p, d->text, s);
                d->sym = s;
            }
            cp_finish(p, d);
            decl->list = d;
            cp_finish(p, decl);
            tail = tail->next = decl;
            if (!cp_accept(p, CP_COMMA)) break;
        }
    }
    p->instruct = saveinstruct;
    cp_expect(p, CP_RPAREN, "expected ')' after parameters");
    fn->params = head.next;
    p->scope = outer;
    if (!p->fnscope) p->fnscope = scope;
}

// --- struct, union, enum

static cp_sym *cp_tagsym(cp_parser *p, i32 kind, cp_str name, b32 local, i32 tok)
{
    cp_sym *s = cp_lookuptag(p, name, local);
    if (s && s->ty && s->ty->kind == kind) return s;
    if (s && !local) {
        cp_warn(p, tok, cp_concat(p->mem, S("tag used with the wrong kind: "), name));
    }
    s = cp_newsym(p, name, CP_SYM_TAG);
    cp_type *t = cp_mktype(p, kind);
    t->name = name;
    t->tok = tok;
    t->size = -1;
    t->align = 1;
    t->flags |= CP_TF_INCOMPLETE;
    if (p->scope->params) t->flags |= CP_TF_PROTOTAG;
    s->ty = t;
    cp_map *m = cp_upsert(&p->scope->tags, name, p->mem);
    m->val = s;
    return s;
}

static i64 cp_roundup(i64 v, i64 a)
{
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

// Is this member declaration, which has no declarators, an anonymous
// struct or union member? In C11 only an untagged definition is. Microsoft
// C, and so mingw's gcc, also takes a tagged or typedef'd one: mingw's
// headers lean on it ("struct _STGMEDIUM_UNION { ... } DUMMYUNIONNAME;"
// with DUMMYUNIONNAME empty). The answer is kept in decl->eflags.
static b32 cp_markanon(cp_parser *p, cp_node *decl)
{
    decl->eflags &= ~CP_EF_ANONMEMBER;
    if (decl->list || !decl->type || !decl->type->sem) return 0;
    cp_type *bt = decl->type->sem;
    if (bt->kind != CP_T_STRUCT && bt->kind != CP_T_UNION) return 0;
    b32 c11 = decl->type->kind != CP_T_NAME && !decl->type->name.len;
    b32 ms = p->target == CP_TARGET_LLP64 && !(bt->flags & CP_TF_INCOMPLETE);
    if (!c11 && !ms) return 0;
    decl->eflags |= CP_EF_ANONMEMBER;
    return 1;
}

// Microsoft record layout (ms_struct, or the LLP64 target), a transcription
// of GCC's place_field, update_alignment_for_field and layout_decl for
// targets where ms_bitfield_layout_p holds. All positions are in bits.
// Adjacent bit-fields share a unit only while their types have the same
// size; a unit, once started, is used up in full.
typedef struct {
    cp_node *target;  // receives offset, bitoffset (and bitwidth)
    cp_type *t;
    i64 size;         // type size in bits, 0 if unknown
    i64 talign;       // type alignment in bits
    i64 declsize;     // bits occupied: width, or the type size
    i64 ualign;       // aligned() on the member, in bits, 0 if none
    b32 bitfield;
    b32 packed;       // DECL_PACKED
    b32 flex;         // flexible array member: no size at all
} cp_msfield;

static void cp_layoutms(cp_parser *p, cp_type *obj, cp_attrinfo *sa)
{
    b32 isunion = obj->kind == CP_T_UNION;
    i64 mfa = p->pack * 8;  // maximum_field_alignment
    i64 recalign = 8;
    if (sa->aligned > 1) recalign = sa->aligned * 8;

    // Gather the fields first: a trailing bit-field pads to its unit.
    i32 n = 0;
    for (cp_node *decl = obj->members; decl; decl = decl->next) {
        if (decl->kind != CP_D_DECL) continue;
        n += decl->list ? 0 : 1;
        for (cp_node *d = decl->list; d; d = d->next) n++;
    }
    cp_msfield *fs = n ? cp_new(p->mem, n, cp_msfield) : 0;
    n = 0;
    for (cp_node *decl = obj->members; decl; decl = decl->next) {
        if (decl->kind != CP_D_DECL) continue;
        cp_attrinfo da = {0};
        da.mode = -1;
        cp_scanattrs(p, decl->attrs, &da);
        cp_node *d = decl->list;
        if (!d && !cp_markanon(p, decl)) continue;  // declares nothing
        for (;;) {
            cp_msfield *f = fs + n++;
            cp_attrinfo ma = da;
            if (d) cp_scanattrs(p, d->attrs, &ma);
            f->target = d ? d : decl;
            f->t = d ? d->ty : decl->type->sem;
            if (!f->t) f->t = p->errtype;
            f->size = f->t->size > 0 ? f->t->size*8 : 0;
            f->talign = (f->t->align < 1 ? 1 : f->t->align) * 8;
            f->ualign = ma.aligned * 8;
            f->bitfield = d && d->b;
            f->flex = f->t->kind == CP_T_ARRAY && f->t->len < 0;
            if (f->flex) obj->flags |= CP_TF_FLEX;
            if (f->bitfield) {
                i64 w = d->b->isconst == 1 ? d->b->cval : 0;
                if (w < 0) w = 0;
                if (w > (f->size ? f->size : 128)) w = f->size ? f->size : 128;
                d->bitwidth = (i32)w;
                f->declsize = w;
            } else {
                f->declsize = f->flex ? 0 : f->size;
            }
            // c-decl.cc finish_struct and handle_packed_attribute
            f->packed = (sa->packed || ma.packed) && (f->bitfield || f->talign > 8);
            if (!d || !(d = d->next)) break;
        }
    }

    i64 bits = 0, maxsize = 0, remaining = 0;
    cp_msfield *prev = 0;  // first bit-field of the current run
    for (i32 i = 0; i < n; i++) {
        cp_msfield *f = fs + i;
        i64 known = bits ? bits & -bits : 0;

        // layout_decl: the alignment this field asks for
        i64 desired;
        if (f->bitfield) {
            desired = f->ualign ? f->ualign : 1;
            i64 w = f->declsize;
            if ((w == 8 || w == 16 || w == 32 || w == 64 || w == 128) &&
                !(w > 8 && f->packed) && (isunion || !known || known >= w)) {
                desired = desired > w ? desired : w;  // fits an integer mode
            }
        } else if (f->packed && f->ualign) {
            desired = f->ualign;
        } else {
            desired = f->ualign > f->talign ? f->ualign : f->talign;
        }
        if (f->packed && !f->ualign && desired > 8) desired = 8;
        if (mfa && desired > mfa) desired = mfa;

        // update_alignment_for_field: what the record must align to
        b32 isbf = f->bitfield && f->size;
        b32 counts = !isbf || (f->declsize ? !f->packed : prev && prev->declsize);
        if (counts) {
            i64 a = !isbf && f->packed ? desired :
                    f->talign > desired ? f->talign : desired;
            if (mfa && a > mfa) a = mfa;
            if (a > recalign) recalign = a;
        }

        if (isunion) {
            f->target->offset = 0;
            f->target->bitoffset = 0;
            i64 sz = (f->declsize + 7) / 8;
            if (sz > maxsize) maxsize = sz;
            continue;
        }

        // place_field
        if (!known) known = recalign > 128 ? recalign : 128;
        if (known < desired && !prev) bits = cp_roundup(bits, desired);
        cp_msfield *saved = prev;
        if (prev) {
            b32 realign = known < desired;
            if (f->bitfield && f->declsize && prev->declsize && f->size == prev->size) {
                if (remaining < f->declsize) {
                    bits += remaining;  // out of bits: next unit
                    prev = f;
                    remaining = f->size < f->declsize ? 0 : f->size - f->declsize;
                } else {
                    remaining -= f->declsize;
                    realign = 0;
                }
            } else {
                if (prev->declsize) bits += remaining;  // use up the unit
                else saved = 0;
                if (!f->bitfield || !f->declsize) prev = 0;
            }
            if (realign) bits = cp_roundup(bits, desired);
        }
        if (!f->bitfield || (saved ? f->size != saved->size : f->declsize != 0)) {
            // start a run
            if (!f->flex) {
                remaining = f->size < f->declsize ? 0 : f->size - f->declsize;
            }
            i64 a = f->packed ? 8 : f->talign;
            if (mfa && a > mfa) a = mfa;
            bits = cp_roundup(bits, a);
            prev = 0;
        }
        f->target->bitoffset = bits;
        f->target->offset = bits / 8;
        if (!prev && f->bitfield) prev = f;
        bits += f->declsize;
        if (f->bitfield && f->declsize && i == n-1) bits += remaining;
        if (bits > CP_MAXSIZE*8) bits = CP_MAXSIZE*8;
    }

    // finalize_record_size
    i64 align = recalign / 8;
    i64 size = isunion ? maxsize : (bits + 7) / 8;
    size = cp_roundup(size, align);
    obj->size = size > CP_MAXSIZE ? CP_MAXSIZE : size;
    obj->align = align;
    obj->flags &= ~CP_TF_INCOMPLETE;
}

static void cp_layout(cp_parser *p, cp_type *obj, cp_attr *structattrs)
{
    cp_attrinfo sa = {0};
    sa.mode = -1;
    cp_scanattrs(p, structattrs, &sa);
    b32 isunion = obj->kind == CP_T_UNION;
    i64 bits = 0;      // current offset in bits (struct)
    i64 maxsize = 0;   // union size in bytes
    i64 align = 1;
    i64 pack = p->pack;
    obj->flags &= ~(CP_TF_FLEX|CP_TF_PACKED);
    if (sa.packed) obj->flags |= CP_TF_PACKED;
    b32 ms = sa.ms > 0 || (sa.ms == 0 && p->target == CP_TARGET_LLP64);
    if (ms) {
        cp_layoutms(p, obj, &sa);
        return;
    }
    for (cp_node *decl = obj->members; decl; decl = decl->next) {
        if (decl->kind != CP_D_DECL) continue;
        cp_attrinfo da = {0};
        da.mode = -1;
        cp_scanattrs(p, decl->attrs, &da);
        cp_node *d = decl->list;
        b32 anon = 0;
        if (!d) {
            if (cp_markanon(p, decl)) {
                anon = 1;
            } else {
                continue;
            }
        }
        for (;;) {
            if (bits > CP_MAXSIZE*8) bits = CP_MAXSIZE*8;
            if (maxsize > CP_MAXSIZE) maxsize = CP_MAXSIZE;
            cp_type *t = anon ? decl->type->sem : d->ty;
            cp_attrinfo ma = da;
            if (!anon) cp_scanattrs(p, d->attrs, &ma);
            if (!t) t = p->errtype;
            i64 tsize = t->size < 0 ? 0 : t->size;
            i64 talign = t->align < 1 ? 1 : t->align;
            b32 packed = sa.packed || ma.packed;
            cp_node *target = anon ? decl : d;
            if (!anon && d->b) {
                // bit-field
                i64 w = d->b->isconst == 1 ? d->b->cval : 0;
                if (w < 0) w = 0;
                if (w > (tsize ? tsize*8 : 128)) w = tsize ? tsize*8 : 128;
                d->bitwidth = (i32)w;
                // GCC: under #pragma pack the packed attribute does not
                // apply to bit-fields, and pack caps every alignment
                b32 bpacked = packed && !pack;
                i64 ta = bpacked ? 1 : talign;
                if (pack && ta > pack) ta = pack;
                i64 fa = ta;
                if (ma.aligned > fa) fa = ma.aligned;
                if (pack && fa > pack) fa = pack;
                if (isunion) {
                    target->bitoffset = 0;
                    target->offset = 0;
                    i64 sz = (w + 7)/8;
                    if (sz > maxsize) maxsize = sz;
                } else if (w == 0) {
                    // ":0" aligns to its type even under #pragma pack
                    bits = cp_roundup(bits, talign*8);
                    target->bitoffset = bits;
                    target->offset = bits/8;
                } else {
                    // a bit-field may not straddle a unit of its type's
                    // alignment (not applied under #pragma pack)
                    if (ma.aligned) {
                        i64 aa = pack && ma.aligned > pack ? pack : ma.aligned;
                        bits = cp_roundup(bits, aa*8);
                    }
                    if (!bpacked && !pack && tsize && (bits % (ta*8)) + w > tsize*8) {
                        bits = cp_roundup(bits, ta*8);
                    }
                    target->bitoffset = bits;
                    target->offset = bits/8;
                    bits += w;
                }
                if (d->text.len || ma.aligned) {
                    if (fa > align) align = fa;
                }
                // i386 GCC: a full-width bit-field with an aligned
                // attribute that lands on its type's natural boundary is
                // laid out as a plain field, of the type's natural
                // alignment (8 for long long, not the 4 of i386 fields)
                if (p->target == CP_TARGET_ILP32 && ma.aligned && !bpacked && !pack &&
                    tsize == 8 && w == 64 && (isunion || (bits - w) % 64 == 0) && align < 8) {
                    align = 8;
                }
            } else {
                i64 ma_ = packed ? 1 : talign;
                if (pack && ma_ > pack) ma_ = pack;
                if (ma.aligned) {
                    i64 want = ma.aligned;
                    if (pack && want > pack) want = pack;
                    if (want > ma_) ma_ = want;
                }
                if (t->kind == CP_T_ARRAY && t->len < 0) {
                    obj->flags |= CP_TF_FLEX;
                    tsize = 0;
                }
                if (isunion) {
                    target->offset = 0;
                    target->bitoffset = 0;
                    if (tsize > maxsize) maxsize = tsize;
                } else {
                    bits = cp_roundup(bits, ma_*8);
                    target->offset = bits/8;
                    target->bitoffset = bits;
                    bits += tsize*8;
                }
                if (ma_ > align) align = ma_;
            }
            if (anon || !(d = d->next)) break;
        }
    }
    if (sa.aligned > align) align = sa.aligned;
    i64 size = isunion ? maxsize : (bits + 7)/8;
    size = cp_roundup(size, align);
    obj->size = size > CP_MAXSIZE ? CP_MAXSIZE : size;
    obj->align = align;
    obj->flags &= ~CP_TF_INCOMPLETE;
}

static cp_node *cp_memberdecl(cp_parser *p)
{
    if (cp_kind(p) == CP_K_STATIC_ASSERT) return cp_staticassert(p);
    cp_node *decl = cp_newnode(p, CP_D_DECL, p->pos);
    cp_specs sp = cp_parsespecs(p);
    if (!sp.any) cp_fail(p, "expected member declaration");
    decl->flags = sp.storage;
    decl->type = sp.type;
    decl->attrs = sp.attrs;
    cp_node head = {0};
    cp_node *tail = &head;
    if (cp_kind(p) != CP_SEMI) {
        for (;;) {
            cp_node *d;
            cp_attr *lead = 0;
            if (tail != &head) cp_attrs(p, &lead);  // "int a, __attribute__((x)) b;"
            if (cp_kind(p) == CP_COLON) {
                d = cp_newnode(p, CP_D_DECLARATOR, p->pos);
                d->type = sp.type;
            } else {
                d = cp_newdeclarator(p, sp.type);
            }
            cp_attrappend(&lead, d->attrs);
            d->attrs = lead;
            cp_trailattrs(p, &d->attrs);
            if (cp_accept(p, CP_COLON)) {
                d->b = cp_condexpr(p);
            }
            cp_attr *aw = 0;
            cp_trailattrs(p, &aw);
            for (cp_attr *a = aw; a; a = a->next) a->afterwidth = 1;
            cp_attrappend(&d->attrs, aw);
            cp_type *ty = cp_resolve(p, d->type);
            cp_attrinfo ai = {0};
            ai.mode = -1;
            cp_scanattrs(p, sp.attrs, &ai);
            cp_scanattrs(p, d->attrs, &ai);
            d->ty = cp_applytypeattrs(p, ty, &ai);
            cp_finish(p, d);
            tail = tail->next = d;
            if (!cp_accept(p, CP_COMMA)) break;
        }
    }
    decl->list = head.next;
    cp_expect(p, CP_SEMI, "expected ';' after member");
    cp_finish(p, decl);
    return decl;
}

static void cp_applypragmas(cp_parser *p);

static cp_type *cp_structspec(cp_parser *p)
{
    i32 tok = p->pos;
    i32 kind = cp_kind(p) == CP_K_STRUCT ? CP_T_STRUCT : CP_T_UNION;
    cp_next(p);
    cp_type *syn = cp_syntype(p, kind, tok);
    cp_attrs(p, &syn->attrs);
    i32 nametok = p->pos;
    if (cp_kind(p) == CP_IDENT) {
        syn->name = cp_tok(p)->text;
        cp_next(p);
    }
    // attributes after the tag belong to the type only in a definition;
    // in "struct s __attribute__((cleanup(f))) x;" they are x's
    if (p->toks[cp_skipattrsat(p, p->pos)].kind == CP_LBRACE) cp_attrs(p, &syn->attrs);
    if (cp_kind(p) == CP_LBRACE) {
        cp_type *obj;
        cp_sym *s = 0;
        if (syn->name.len) {
            s = cp_tagsym(p, kind, syn->name, 1, nametok);
            obj = s->ty;
            if (!(obj->flags & CP_TF_INCOMPLETE) && obj->members) {
                // redefinition in the same scope: make a fresh one
                s = cp_newsym(p, syn->name, CP_SYM_TAG);
                obj = cp_mktype(p, kind);
                obj->name = syn->name;
                s->ty = obj;
                cp_map *m = cp_upsert(&p->scope->tags, syn->name, p->mem);
                m->val = s;
            }
        } else {
            obj = cp_mktype(p, kind);
            obj->size = -1;
            obj->flags |= CP_TF_INCOMPLETE;
        }
        syn->flags |= CP_TF_BODY;
        syn->sem = obj;
        cp_next(p);
        cp_node head = {0};
        cp_node *tail = &head;
        p->instruct++;
        cp_enter(p);
        while (cp_kind(p) != CP_RBRACE) {
            if (cp_kind(p) == CP_EOF) cp_fail(p, "unterminated struct");
            cp_applypragmas(p);
            if (cp_accept(p, CP_SEMI)) continue;
            tail = tail->next = cp_memberdecl(p);
        }
        p->depth--;
        p->instruct--;
        cp_applypragmas(p);
        cp_next(p);
        cp_attrs(p, &syn->attrs);
        syn->members = head.next;
        obj->members = head.next;
        obj->attrs = syn->attrs;
        cp_layout(p, obj, syn->attrs);
        cp_synctag(obj);
        if (s) {
            s->def = 0;
            s->flags |= CP_YF_DEFINED;
        }
        return syn;
    }
    if (!syn->name.len) cp_fail(p, "expected struct name or '{'");
    b32 forward = cp_kind(p) == CP_SEMI;
    cp_sym *s = forward ? cp_tagsym(p, kind, syn->name, 1, nametok) : cp_lookuptag(p, syn->name, 0);
    if (!s) s = cp_tagsym(p, kind, syn->name, 1, nametok);
    s->nrefs++;
    syn->sem = s->ty;
    return syn;
}

static cp_type *cp_enumspec(cp_parser *p)
{
    i32 tok = p->pos;
    cp_next(p);
    cp_type *syn = cp_syntype(p, CP_T_ENUM, tok);
    cp_attrs(p, &syn->attrs);
    i32 nametok = p->pos;
    if (cp_kind(p) == CP_IDENT) {
        syn->name = cp_tok(p)->text;
        cp_next(p);
    }
    {
        // as for struct: the type's attributes only before a body or ":"
        i32 k = p->toks[cp_skipattrsat(p, p->pos)].kind;
        if (k == CP_LBRACE || k == CP_COLON) cp_attrs(p, &syn->attrs);
    }
    cp_type *fixed = 0;
    if (cp_kind(p) == CP_COLON && (cp_istypestart(p, p->pos+1))) {
        cp_next(p);
        cp_node *tn = cp_parsetypename(p);
        syn->base = tn->type;
        fixed = tn->ty;
    }
    if (cp_kind(p) != CP_LBRACE) {
        if (!syn->name.len) cp_fail(p, "expected enum name or '{'");
        cp_sym *s = cp_kind(p) == CP_SEMI ? cp_tagsym(p, CP_T_ENUM, syn->name, 1, nametok)
                                         : cp_lookuptag(p, syn->name, 0);
        if (!s) s = cp_tagsym(p, CP_T_ENUM, syn->name, 1, nametok);
        if (fixed && !s->ty->base) {
            s->ty->base = cp_unqualified(fixed);
            s->ty->size = fixed->size;
            s->ty->align = fixed->align;
            s->ty->flags &= ~CP_TF_INCOMPLETE;
        }
        s->nrefs++;
        syn->sem = s->ty;
        return syn;
    }
    cp_type *obj;
    cp_sym *tagsym = 0;
    if (syn->name.len) {
        tagsym = cp_tagsym(p, CP_T_ENUM, syn->name, 1, nametok);
        obj = tagsym->ty;
        tagsym->flags |= CP_YF_DEFINED;
    } else {
        obj = cp_mktype(p, CP_T_ENUM);
    }
    syn->flags |= CP_TF_BODY;
    syn->sem = obj;
    if (fixed) {
        obj->base = cp_unqualified(fixed);
        obj->size = fixed->size;
        obj->align = fixed->align;
        obj->flags &= ~CP_TF_INCOMPLETE;
    }
    cp_next(p);
    cp_node head = {0};
    cp_node *tail = &head;
    i64 next = 0, lo = 0, hi = 0;
    b32 first = 1, big = 0;  // big: a value of 2^63 or more (held negative)
    while (cp_kind(p) != CP_RBRACE) {
        cp_applypragmas(p);
        if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected enumerator name");
        cp_node *e = cp_newnode(p, CP_D_ENUMERATOR, p->pos);
        e->text = cp_tok(p)->text;
        cp_next(p);
        cp_attrs(p, &e->attrs);
        b32 thisbig = 0;
        if (cp_accept(p, CP_ASSIGN)) {
            e->a = cp_condexpr(p);
            if (e->a->isconst == 1) {
                next = e->a->cval;
                thisbig = next < 0 && e->a->ty && cp_isinteger(e->a->ty) && e->a->ty->size >= 8 &&
                          cp_isunsigned(p, e->a->ty);
            } else {
                cp_warn(p, e->a->tok, S("enumerator value is not an integer constant"));
            }
        }
        e->cval = next;
        e->isconst = 1;
        big |= thisbig;
        if (!thisbig && (first || next < lo)) lo = next;
        if (!thisbig && (first || next > hi)) hi = next;
        first = 0;
        next++;
        cp_sym *s = cp_newsym(p, e->text, CP_SYM_ENUMCONST);
        s->decl = e;
        s->def = e;
        s->flags |= CP_YF_DEFINED;
        cp_addref(p, &s->decls, &s->lastdecl_, e, 0, CP_RF_DECL);
        // C23: the constant has type int while its value fits
        b32 fitsint = !thisbig && e->cval >= INT32_MIN && e->cval <= INT32_MAX;
        s->ty = fixed ? obj : fitsint ? p->bt[CP_BT_INT] : obj;
        e->ty = s->ty;
        cp_bindname(p, e->text, s);
        e->sym = s;
        cp_finish(p, e);
        tail = tail->next = e;
        if (!cp_accept(p, CP_COMMA)) break;
    }
    cp_applypragmas(p);
    cp_expect(p, CP_RBRACE, "expected '}' after enumerators");
    cp_attrs(p, &syn->attrs);
    syn->members = head.next;
    obj->members = head.next;
    if (!fixed) {
        cp_attrinfo ai = {0};
        ai.mode = -1;
        cp_scanattrs(p, syn->attrs, &ai);
        cp_type *u;
        if (ai.packed) {
            if (lo >= 0) u = hi <= 255 ? p->bt[CP_BT_UCHAR] : hi <= 65535 ? p->bt[CP_BT_USHORT] :
                             hi <= (i64)UINT32_MAX ? p->bt[CP_BT_UINT] : p->bt[CP_BT_ULLONG];
            else u = lo >= -128 && hi <= 127 ? p->bt[CP_BT_SCHAR] :
                     lo >= -32768 && hi <= 32767 ? p->bt[CP_BT_SHORT] :
                     lo >= INT32_MIN && hi <= INT32_MAX ? p->bt[CP_BT_INT] : p->bt[CP_BT_LLONG];
        } else if (big) {
            u = p->bt[p->target == CP_TARGET_LP64 ? CP_BT_ULONG : CP_BT_ULLONG];
        } else if (lo >= 0) {
            u = hi <= (i64)UINT32_MAX ? p->bt[CP_BT_UINT] : p->bt[CP_BT_ULONG];
            if (hi > (i64)UINT32_MAX && p->target != CP_TARGET_LP64) u = p->bt[CP_BT_ULLONG];
        } else {
            u = lo >= INT32_MIN && hi <= INT32_MAX ? p->bt[CP_BT_INT] : p->bt[CP_BT_LONG];
            if (u == p->bt[CP_BT_LONG] && p->target != CP_TARGET_LP64) u = p->bt[CP_BT_LLONG];
        }
        if (ai.mode >= 0) u = cp_applytypeattrs(p, u, &ai);
        obj->base = u;
        obj->size = u->size;
        obj->align = u->align;
        if (ai.aligned > obj->align) obj->align = ai.aligned;
        obj->flags &= ~CP_TF_INCOMPLETE;
    }
    // C23: if any value does not fit in int, every constant has the
    // enumerated type once the list is complete
    if (!fixed && (big || lo < INT32_MIN || hi > INT32_MAX)) {
        for (cp_node *e = head.next; e; e = e->next) e->ty = obj;
    }
    for (cp_node *e = head.next; e; e = e->next) {
        e->cval = cp_truncate(p, e->ty, e->cval);
        if (e->sym) e->sym->ty = e->ty;
    }
    cp_synctag(obj);
    return syn;
}

// --- symbols for declarations

static void cp_declare(cp_parser *p, cp_node *d, i32 storage, cp_node *def, b32 kr)
{
    if (!d->text.len) return;
    cp_type *ty = d->ty;
    b32 file = p->scope->depth == 0;
    i32 kind = (storage & CP_SC_TYPEDEF) ? CP_SYM_TYPEDEF :
               ty && ty->kind == CP_T_FUNC ? CP_SYM_FUNC : CP_SYM_VAR;
    cp_map *slot = cp_upsert(&p->scope->names, d->text, p->mem);
    cp_sym *s = (cp_sym *)slot->val;
    if (s && s->kind == CP_SYM_PARAM && kr) {
        // a K&R parameter's type is adjusted as a prototype's is
        if (ty && ty->kind == CP_T_ARRAY) {
            i32 q = d->type && d->type->kind == CP_T_ARRAY ? d->type->quals : 0;
            ty = cp_qualify(p, cp_ptrto(p, ty->base), q);
        } else if (ty && ty->kind == CP_T_FUNC) {
            ty = cp_ptrto(p, ty);
        }
        d->ty = ty;
        s->ty = ty;
        s->decl = d;
        s->def = d;
        cp_addref(p, &s->decls, &s->lastdecl_, d, 0, CP_RF_DECL);
        d->sym = s;
        return;
    }
    b32 merge = s && s->kind == kind && (kind == CP_SYM_TYPEDEF || s->linkage != CP_LINK_NONE);
    if (!s && !file && kind != CP_SYM_TYPEDEF && ((storage & CP_SC_EXTERN) || kind == CP_SYM_FUNC)) {
        for (cp_scope *sc = p->scope->parent; sc; sc = sc->parent) {
            cp_map *m = cp_upsert(&sc->names, d->text, 0);
            if (m && m->val) {
                cp_sym *o = (cp_sym *)m->val;
                if (o->linkage != CP_LINK_NONE && o->kind == kind) {
                    s = o;
                    merge = 1;
                }
                break;
            }
        }
    }
    if (!merge) {
        s = cp_newsym(p, d->text, kind);
        if (kind == CP_SYM_TYPEDEF) {
            s->linkage = CP_LINK_NONE;
        } else if (file) {
            s->linkage = (storage & CP_SC_STATIC) ? CP_LINK_INTERNAL : CP_LINK_EXTERNAL;
        } else {
            s->linkage = (storage & CP_SC_EXTERN) || kind == CP_SYM_FUNC ? CP_LINK_EXTERNAL : CP_LINK_NONE;
        }
        s->decl = d;
    }
    slot->val = s;
    // composite type: prefer complete arrays and prototypes
    if (!s->ty) {
        s->ty = ty;
    } else if (ty) {
        cp_type *o = s->ty;
        if ((o->kind == CP_T_ARRAY && o->len < 0 && ty->kind == CP_T_ARRAY && ty->len >= 0) ||
            (o->kind == CP_T_FUNC && (o->flags & CP_TF_NOPROTO) && ty->kind == CP_T_FUNC &&
             !(ty->flags & CP_TF_NOPROTO))) {
            s->ty = ty;
        }
    }
    if (kind != CP_SYM_TYPEDEF) {
        if (file || (storage & (CP_SC_STATIC|CP_SC_EXTERN))) s->flags |= CP_YF_STATICDUR;
        if (storage & CP_SC_THREAD_LOCAL) s->flags |= CP_YF_THREADDUR;
        if (storage & (CP_SC_INLINE|CP_SC_FORCEINLINE)) s->flags |= CP_YF_INLINE;
    }
    b32 defines = def != 0 || kind == CP_SYM_TYPEDEF ||
                  (kind == CP_SYM_VAR && !(storage & CP_SC_EXTERN));
    if (defines && !s->def) {
        s->def = def ? def : d;
        s->flags |= CP_YF_DEFINED;
    } else if (def) {
        s->def = def;
        s->flags |= CP_YF_DEFINED;
    }
    cp_addref(p, &s->decls, &s->lastdecl_, d, 0, CP_RF_DECL);
    d->sym = s;
}

// --- initializers

static cp_node *cp_initializer(cp_parser *p);

static cp_node *cp_initlist(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_E_INITLIST, p->pos);
    cp_expect(p, CP_LBRACE, "expected '{'");
    cp_enter(p);
    cp_node head = {0};
    cp_node *tail = &head;
    while (cp_kind(p) != CP_RBRACE) {
        cp_node *item = cp_newnode(p, CP_E_INIT, p->pos);
        cp_node dh = {0};
        cp_node *dt = &dh;
        if (cp_kind(p) == CP_IDENT && cp_peekk(p, 1) == CP_COLON) {
            cp_node *f = cp_newnode(p, CP_E_DFIELD, p->pos);
            f->text = cp_tok(p)->text;
            f->flags = 1;  // GNU old-style "field:"
            cp_next(p);
            cp_next(p);
            dt = dt->next = f;
        } else {
            b32 any = 0;
            for (;;) {
                if (cp_kind(p) == CP_DOT) {
                    cp_node *f = cp_newnode(p, CP_E_DFIELD, p->pos);
                    cp_next(p);
                    cp_softname(p, p->pos);
                    if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected field name");
                    f->text = cp_tok(p)->text;
                    cp_next(p);
                    dt = dt->next = f;
                    any = 1;
                } else if (cp_kind(p) == CP_LBRACK) {
                    cp_node *x = cp_newnode(p, CP_E_DINDEX, p->pos);
                    cp_next(p);
                    x->a = cp_condexpr(p);
                    if (cp_accept(p, CP_ELLIPSIS)) x->b = cp_condexpr(p);
                    cp_expect(p, CP_RBRACK, "expected ']'");
                    dt = dt->next = x;
                    any = 1;
                } else {
                    break;
                }
            }
            if (any && !cp_accept(p, CP_ASSIGN)) {
                // GNU: [i] value without '='
                if (dh.next->kind != CP_E_DINDEX) cp_fail(p, "expected '='");
                item->flags = 1;
            }
        }
        item->b = dh.next;
        item->a = cp_initializer(p);
        cp_finish(p, item);
        tail = tail->next = item;
        if (!cp_accept(p, CP_COMMA)) break;
    }
    p->depth--;
    cp_expect(p, CP_RBRACE, "expected '}'");
    n->list = head.next;
    cp_finish(p, n);
    return n;
}

static cp_node *cp_initializer(cp_parser *p)
{
    if (cp_kind(p) == CP_LBRACE) return cp_initlist(p);
    return cp_assignexpr(p);
}

static b32 cp_ischarish(cp_type *t)
{
    if (!t) return 0;
    t = t->unqual;
    if (t->kind != CP_T_BUILTIN) return 0;
    switch (t->basic) {
    case CP_BT_CHAR: case CP_BT_SCHAR: case CP_BT_UCHAR: case CP_BT_SHORT:
    case CP_BT_USHORT: case CP_BT_INT: case CP_BT_UINT: case CP_BT_LONG:
    case CP_BT_ULONG:
        return 1;
    }
    return 0;
}

// Does member-designator name pick out this member declaration, directly
// or inside an anonymous member?
static b32 cp_hasmember(cp_node *decl, cp_str name, i32 depth)
{
    if (depth > 64) return 0;
    if (decl->eflags & CP_EF_ANONMEMBER) {
        cp_type *at = decl->type ? decl->type->sem : 0;
        for (cp_node *m = at ? at->members : 0; m; m = m->next) {
            if (m->kind == CP_D_DECL && cp_hasmember(m, name, depth+1)) return 1;
        }
        return 0;
    }
    for (cp_node *d = decl->list; d; d = d->next) {
        if (cp_equals(d->text, name)) return 1;
    }
    return 0;
}

// Consume initializer items for one object of type t with braces elided
// (C 6.7.9p20). The first item may carry designators (first is set); desig
// is the part of its designator chain still to follow inside t, as in
// "[0].x[2].b = 10, 11", where 11 goes after x[2].b.
static void cp_consumeinit(cp_type *t, cp_node **it, cp_node *desig, b32 first, i32 depth)
{
    cp_node *item = *it;
    if (!item || depth > 64) return;
    if (!first && item->b) return;
    if (!desig && item->a && item->a->kind == CP_E_INITLIST) { *it = item->next; return; }
    if (!t) { *it = item->next; return; }
    t = t->unqual ? t->unqual : t;
    if (t->kind == CP_T_ARRAY) {
        if (!desig && item->a && item->a->kind == CP_E_STRING && cp_ischarish(t->base)) {
            *it = item->next;
            return;
        }
        i64 k0 = 0;
        cp_node *rest = 0;
        if (desig && desig->kind == CP_E_DINDEX && desig->a && desig->a->isconst == 1) {
            k0 = desig->b && desig->b->isconst == 1 ? desig->b->cval : desig->a->cval;
            rest = desig->next;
        }
        for (i64 k = k0; (t->len < 0 || k < t->len) && *it; k++) {
            b32 lead = k == k0;
            if (!(first && lead) && (*it)->b) break;
            cp_consumeinit(t->base, it, lead ? rest : 0, first && lead, depth+1);
        }
        return;
    }
    if (t->kind == CP_T_STRUCT || t->kind == CP_T_UNION) {
        // an expression of the same struct type initializes all of it:
        // "struct S c[] = {a, b};"
        if (!desig && item->a && item->a->ty) {
            cp_type *v = item->a->ty->sem ? item->a->ty->sem : item->a->ty;
            if ((v->unqual ? v->unqual : v) == t) { *it = item->next; return; }
        }
        // start at the designated member, if any
        cp_node *start = t->members;
        b32 named = desig && desig->kind == CP_E_DFIELD;
        if (named) {
            for (cp_node *m = t->members; m; m = m->next) {
                if (m->kind == CP_D_DECL && cp_hasmember(m, desig->text, 0)) { start = m; break; }
            }
        }
        b32 any = 0;
        for (cp_node *decl = start; decl && *it; decl = decl->next) {
            if (decl->kind != CP_D_DECL) continue;
            if (!decl->list) {
                cp_type *at = decl->type ? decl->type->sem : 0;
                if (at && (decl->eflags & CP_EF_ANONMEMBER)) {
                    if (any && (*it)->b) return;
                    // an anonymous member finds the designated name itself
                    cp_consumeinit(at, it, !any && named ? desig : 0, first && !any, depth+1);
                    any = 1;
                    if (t->kind == CP_T_UNION) return;
                }
                continue;
            }
            cp_node *d = decl->list;
            if (!any && named) {
                while (d && !cp_equals(d->text, desig->text)) d = d->next;
                if (!d) d = decl->list;
            }
            for (; d && *it; d = d->next) {
                if (!d->text.len) continue;  // unnamed bit-fields take nothing
                if (any && (*it)->b) return;
                cp_node *rest = !any && named ? desig->next : 0;
                cp_consumeinit(d->ty, it, rest, first && !any, depth+1);
                any = 1;
                if (t->kind == CP_T_UNION) return;
            }
        }
        if (!any) *it = item->next;
        return;
    }
    *it = item->next;
}

// Complete "T a[] = init" from the initializer.
static cp_type *cp_completearray(cp_parser *p, cp_type *ty, cp_node *init)
{
    if (!ty || ty->kind != CP_T_ARRAY || ty->len >= 0 || !init) return ty;
    i64 n = -1;
    if (init->kind == CP_E_STRING && init->ty && init->ty->kind == CP_T_ARRAY) {
        n = init->ty->len;
    } else if (init->kind == CP_E_INITLIST) {
        cp_node *only = init->list;
        if (only && !only->next && !only->b && only->a && only->a->kind == CP_E_STRING &&
            cp_ischarish(ty->base) && only->a->ty) {
            n = only->a->ty->len;
        } else {
            i64 idx = 0, max = 0;
            cp_node *it = init->list;
            while (it) {
                if (it->b && it->b->kind == CP_E_DINDEX && it->b->a && it->b->a->isconst == 1) {
                    idx = it->b->a->cval;
                    if (it->b->b && it->b->b->isconst == 1) idx = it->b->b->cval;
                }
                cp_node *before = it;
                cp_consumeinit(ty->base, &it, it->b ? it->b->next : 0, 1, 0);
                if (it == before) it = it->next;  // always make progress
                idx++;
                if (idx > max) max = idx;
            }
            n = max;
        }
    }
    if (n < 0) return ty;
    cp_type *r = cp_arrayof(p, ty->base, n);
    if (n == 0) {
        // "int a[] = {}" (GNU, C23) stays int[], but with size 0: GCC and
        // Clang alike, and unlike int[0]
        r = cp_arrayof(p, ty->base, -1);
        r->size = 0;
        r->flags &= ~CP_TF_INCOMPLETE;
    }
    return cp_qualify(p, r, ty->quals);
}

// --- expressions

static void cp_sidefx(cp_node *n)
{
    i32 f = 0;
    if (n->a) f |= n->a->eflags;
    if (n->b) f |= n->b->eflags;
    if (n->c) f |= n->c->eflags;
    for (cp_node *x = n->list; x; x = x->next) f |= x->eflags;
    n->eflags |= f & CP_EF_SIDEEFFECT;
}

// keepfn leaves p->fnscope at the type's parameters: ^ int (int v) {...}
static cp_node *cp_parsetypename2(cp_parser *p, b32 keepfn)
{
    cp_node *n = cp_newnode(p, CP_E_TYPEARG, p->pos);
    cp_specs sp = cp_parsespecs(p);
    if (!sp.any) cp_fail(p, "expected type name");
    cp_scope *savefn = p->fnscope;
    cp_node *d = cp_newdeclarator(p, sp.type);
    if (!keepfn) p->fnscope = savefn;
    if (d->text.len) cp_fail(p, "unexpected name in type name");
    cp_attrs(p, &d->attrs);
    n->type = d->type;
    cp_type *ty = cp_resolve(p, d->type);
    cp_attrinfo ai = {0};
    ai.mode = -1;
    cp_scanattrs(p, sp.attrs, &ai);
    cp_scanattrs(p, d->attrs, &ai);
    n->ty = cp_applytypeattrs(p, ty, &ai);
    n->attrs = sp.attrs;
    cp_finish(p, n);
    return n;
}

typedef struct { char *name; char code; } cp_builtindef;

static const cp_builtindef cp_builtins[] = {
    {"expect", 'l'}, {"expect_with_probability", 'l'}, {"unreachable", 'v'},
    {"trap", 'v'}, {"abort", 'v'}, {"constant_p", 'i'},
    {"types_compatible_p", 'i'}, {"offsetof", 'z'}, {"va_start", 'v'},
    {"va_end", 'v'}, {"va_copy", 'v'}, {"c23_va_start", 'v'},
    {"alloca", 'p'}, {"alloca_with_align", 'p'}, {"memcpy", 'p'},
    {"memmove", 'p'}, {"memset", 'p'}, {"memcmp", 'i'}, {"strlen", 'z'},
    {"strcmp", 'i'}, {"strncmp", 'i'}, {"strcpy", 'c'}, {"strncpy", 'c'},
    {"strchr", 'c'}, {"strrchr", 'c'}, {"strstr", 'c'}, {"strcat", 'c'},
    {"object_size", 'z'}, {"dynamic_object_size", 'z'},
    {"__memcpy_chk", 'p'}, {"__memset_chk", 'p'}, {"__memmove_chk", 'p'},
    {"__mempcpy_chk", 'p'}, {"mempcpy", 'p'},
    {"__strcpy_chk", 'c'}, {"__strncpy_chk", 'c'}, {"__strcat_chk", 'c'},
    {"__strncat_chk", 'c'}, {"__stpcpy_chk", 'c'}, {"__stpncpy_chk", 'c'},
    {"__sprintf_chk", 'i'}, {"__snprintf_chk", 'i'},
    {"__vsprintf_chk", 'i'}, {"__vsnprintf_chk", 'i'}, {"__printf_chk", 'i'},
    {"__fprintf_chk", 'i'}, {"printf", 'i'}, {"sprintf", 'i'},
    {"snprintf", 'i'}, {"puts", 'i'}, {"putchar", 'i'}, {"malloc", 'p'},
    {"calloc", 'p'}, {"realloc", 'p'}, {"free", 'v'}, {"abs", 'i'},
    {"labs", 'l'}, {"llabs", 'q'}, {"fabs", 'd'}, {"fabsf", 'f'},
    {"fabsl", 'D'}, {"sqrt", 'd'}, {"sqrtf", 'f'}, {"sqrtl", 'D'},
    {"inf", 'd'}, {"inff", 'f'}, {"infl", 'D'}, {"huge_val", 'd'},
    {"huge_valf", 'f'}, {"huge_vall", 'D'}, {"nan", 'd'}, {"nanf", 'f'},
    {"nanl", 'D'}, {"nans", 'd'}, {"nansf", 'f'}, {"nansl", 'D'},
    {"copysign", 'd'}, {"copysignf", 'f'}, {"copysignl", 'D'},
    {"isnan", 'i'}, {"isinf", 'i'}, {"isinf_sign", 'i'}, {"isfinite", 'i'},
    {"isnormal", 'i'}, {"signbit", 'i'}, {"signbitf", 'i'}, {"signbitl", 'i'},
    {"fpclassify", 'i'}, {"isgreater", 'i'}, {"isgreaterequal", 'i'},
    {"isless", 'i'}, {"islessequal", 'i'}, {"islessgreater", 'i'},
    {"isunordered", 'i'}, {"iseqsig", 'i'}, {"issignaling", 'i'},
    {"clz", 'i'}, {"clzl", 'i'}, {"clzll", 'i'}, {"ctz", 'i'}, {"ctzl", 'i'},
    {"ctzll", 'i'}, {"popcount", 'i'}, {"popcountl", 'i'}, {"popcountll", 'i'},
    {"parity", 'i'}, {"parityl", 'i'}, {"parityll", 'i'}, {"ffs", 'i'},
    {"ffsl", 'i'}, {"ffsll", 'i'}, {"clrsb", 'i'}, {"clrsbl", 'i'},
    {"clrsbll", 'i'}, {"bswap16", 'h'}, {"bswap32", 'u'}, {"bswap64", 'W'},
    {"return_address", 'p'}, {"frame_address", 'p'},
    {"extract_return_addr", 'p'}, {"prefetch", 'v'},
    {"assume_aligned", 'p'}, {"classify_type", 'i'},
    {"add_overflow", 'b'}, {"sub_overflow", 'b'}, {"mul_overflow", 'b'},
    {"add_overflow_p", 'b'}, {"sub_overflow_p", 'b'}, {"mul_overflow_p", 'b'},
    {"sadd_overflow", 'b'}, {"saddl_overflow", 'b'}, {"saddll_overflow", 'b'},
    {"uadd_overflow", 'b'}, {"uaddl_overflow", 'b'}, {"uaddll_overflow", 'b'},
    {"ssub_overflow", 'b'}, {"ssubl_overflow", 'b'}, {"ssubll_overflow", 'b'},
    {"usub_overflow", 'b'}, {"usubl_overflow", 'b'}, {"usubll_overflow", 'b'},
    {"smul_overflow", 'b'}, {"smull_overflow", 'b'}, {"smulll_overflow", 'b'},
    {"umul_overflow", 'b'}, {"umull_overflow", 'b'}, {"umulll_overflow", 'b'},
    {"speculation_safe_value", 'a'}, {"LINE", 'i'}, {"FILE", 'C'},
    {"FUNCTION", 'C'}, {"launder", 'a'}, {"assume", 'v'},
    {"clear_padding", 'v'}, {"has_attribute", 'b'}, {"shuffle", 'T'}, {"shufflevector", 'S'},
    {"bit_cast", 'T'}, {"va_arg", 'U'}, {"convertvector", 'U'},
    {"complex", 'K'}, {"choose_expr", 'X'}, {"tgmath", 'a'},
    {"isinf_sign", 'i'}, {"stack_save", 'p'}, {"stack_restore", 'v'},
    {"cpu_init", 'v'}, {"cpu_is", 'i'}, {"cpu_supports", 'i'},
    {"ia32_pause", 'v'}, {"setjmp", 'i'}, {"longjmp", 'v'},
    {"exit", 'v'}, {"_exit", 'v'}, {"fprintf", 'i'}, {"vprintf", 'i'},
    {"vfprintf", 'i'}, {"vsnprintf", 'i'}, {"vsprintf", 'i'},
    {"fputs", 'i'}, {"fputc", 'i'}, {"fwrite", 'z'},
};

static cp_type *cp_typecode(cp_parser *p, char code)
{
    switch (code) {
    case 'v': return p->bt[CP_BT_VOID];
    case 'i': return p->bt[CP_BT_INT];
    case 'u': return p->bt[CP_BT_UINT];
    case 'h': return p->bt[CP_BT_USHORT];
    case 'l': return p->bt[CP_BT_LONG];
    case 'q': return p->bt[CP_BT_LLONG];
    case 'W': return p->bt[p->target == CP_TARGET_LP64 ? CP_BT_ULONG : CP_BT_ULLONG];
    case 'd': return p->bt[CP_BT_DOUBLE];
    case 'f': return p->bt[CP_BT_FLOAT];
    case 'D': return p->bt[CP_BT_LDOUBLE];
    case 'p': return p->voidptr;
    case 'z': return p->sizet;
    case 'b': return p->bt[CP_BT_BOOL];
    case 'c': return cp_ptrto(p, p->bt[CP_BT_CHAR]);
    case 'C': return cp_ptrto(p, cp_qualify(p, p->bt[CP_BT_CHAR], CP_Q_CONST));
    }
    return 0;
}

// The type code of __builtin_<name>: the table, then the C library's
// math functions, whose f and l forms return float and long double.
static char cp_builtincode(cp_str name)
{
    for (iz i = 0; i < cp_countof(cp_builtins); i++) {
        if (cp_equals(name, cp_cstr(cp_builtins[i].name))) return cp_builtins[i].code;
    }
    static const char *const math[] = {
        "acos", "asin", "atan", "atan2", "cos", "sin", "tan", "cosh", "sinh",
        "tanh", "acosh", "asinh", "atanh", "exp", "exp2", "expm1", "exp10",
        "log", "log10", "log2", "log1p", "logb", "pow", "cbrt", "hypot",
        "fmod", "remainder", "remquo", "floor", "ceil", "round", "roundeven",
        "trunc", "rint", "nearbyint", "fmin", "fmax", "fdim", "fma", "ldexp",
        "frexp", "scalbn", "scalbln", "modf", "erf", "erfc", "tgamma",
        "lgamma", "nextafter", "nexttoward", "significand", "drem",
    };
    static const struct { char *name; char code; } ints[] = {
        {"ilogb", 'i'}, {"lround", 'l'}, {"llround", 'q'}, {"lrint", 'l'},
        {"llrint", 'q'},
    };
    for (iz i = 0; i < cp_countof(math); i++) {
        cp_str m = cp_cstr((char *)math[i]);
        if (!cp_startswith(name, m)) continue;
        if (name.len == m.len) return 'd';
        if (name.len == m.len+1 && name.data[m.len] == 'f') return 'f';
        if (name.len == m.len+1 && name.data[m.len] == 'l') return 'D';
    }
    for (iz i = 0; i < cp_countof(ints); i++) {
        cp_str m = cp_cstr(ints[i].name);
        if (cp_startswith(name, m) && name.len <= m.len+1 &&
            (name.len == m.len || name.data[m.len] == 'f' || name.data[m.len] == 'l')) {
            return ints[i].code;
        }
    }
    return 0;
}

static cp_sym *cp_implicitfunc(cp_parser *p, cp_node *id)
{
    cp_str name = id->text;
    cp_type *ret = p->bt[CP_BT_INT];
    b32 builtin = cp_startswith(name, S("__builtin_")) || cp_startswith(name, S("__sync_")) ||
                  cp_startswith(name, S("__atomic_"));
    if (cp_startswith(name, S("__builtin_"))) {
        cp_str rest = cp_span(name.data+10, name.data+name.len);
        char code = cp_builtincode(rest);
        cp_type *t = code ? cp_typecode(p, code) : 0;
        if (t) ret = t;
    } else if (!builtin) {
        cp_warn(p, id->tok, cp_concat(p->mem, S("implicit declaration of function "), name));
    }
    // declare at file scope
    cp_scope *file = p->scope;
    while (file->depth > 0) file = file->parent;
    cp_scope *save = p->scope;
    p->scope = file;
    cp_sym *s = cp_newsym(p, name, CP_SYM_FUNC);
    p->scope = save;
    cp_type *ft = cp_mktype(p, CP_T_FUNC);
    ft->base = ret;
    ft->flags = CP_TF_NOPROTO;
    ft->size = ft->align = 1;
    s->ty = ft;
    s->linkage = CP_LINK_EXTERNAL;
    s->flags |= CP_YF_IMPLICIT | CP_YF_STATICDUR;
    s->decl = id;
    s->depth = 0;
    s->func = 0;
    cp_map *m = cp_upsert(&file->names, name, p->mem);
    m->val = s;
    return s;
}

static cp_node *cp_strlit(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_E_STRING, p->pos);
    i32 first = p->pos;
    i32 width = 1;     // element size
    i32 elem = 0;      // 0 char, 1 wchar, 2 char16, 3 char32, 4 u8
    while (cp_kind(p) == CP_STRLIT) {
        cp_str t = cp_tok(p)->text;
        if (t.data[0] == 'L') { elem = 1; }
        else if (t.data[0] == 'u' && t.data[1] == '8') { if (!elem) elem = 4; }
        else if (t.data[0] == 'u') { elem = 2; }
        else if (t.data[0] == 'U') { elem = 3; }
        cp_next(p);
    }
    i32 last = p->pos;
    n->ival = last - first;
    cp_type *et = elem == 1 ? p->wchart : elem == 2 ? p->char16t : elem == 3 ? p->char32t :
                  elem == 4 ? p->bt[CP_BT_UCHAR] : p->bt[CP_BT_CHAR];
    width = (i32)et->size;
    // decode
    iz cap = 0;
    for (i32 i = first; i < last; i++) cap += p->toks[i].text.len * 4;
    char *buf = cp_new(p->mem, cap+4, char);
    iz len = 0;
    for (i32 i = first; i < last; i++) {
        cp_str t = p->toks[i].text;
        char *s = t.data, *e = t.data + t.len - 1;
        b32 rawstr = 0;
        while (s < e && *s != '"') rawstr |= *s++ == 'R';
        s++;
        if (rawstr) {
            // R"delim( ... )delim": take the text between the parentheses
            char *open = s;
            while (open < e && *open != '(') open++;
            iz dl = open - s;
            s = open + 1;
            e = e - dl - 1;
            if (e < s) e = s;
        }
        while (s < e) {
            b32 raw;
            u32 c;
            if (!rawstr && (s = cp_skipsplices(s, e)) >= e) break;
            if (rawstr) {
                c = (u8)*s++;
                raw = 1;
            } else {
                c = cp_decodechar(&s, e, &raw);
            }
            if (width == 1) {
                if (raw || c < 0x80) {
                    buf[len++] = (char)c;
                } else if (c < 0x800) {
                    buf[len++] = (char)(0xc0 | c>>6);
                    buf[len++] = (char)(0x80 | (c & 63));
                } else if (c < 0x10000) {
                    buf[len++] = (char)(0xe0 | c>>12);
                    buf[len++] = (char)(0x80 | (c>>6 & 63));
                    buf[len++] = (char)(0x80 | (c & 63));
                } else {
                    buf[len++] = (char)(0xf0 | (c>>18 & 7));
                    buf[len++] = (char)(0x80 | (c>>12 & 63));
                    buf[len++] = (char)(0x80 | (c>>6 & 63));
                    buf[len++] = (char)(0x80 | (c & 63));
                }
            } else if (width == 2 && c >= 0x10000 && !raw) {
                u32 hi = 0xd800 + ((c - 0x10000) >> 10);
                u32 lo = 0xdc00 + ((c - 0x10000) & 0x3ff);
                buf[len++] = (char)hi; buf[len++] = (char)(hi>>8);
                buf[len++] = (char)lo; buf[len++] = (char)(lo>>8);
            } else {
                for (i32 b = 0; b < width; b++) buf[len++] = (char)(c >> (8*b));
            }
        }
    }
    n->sval = cp_span(buf, buf+len);
    n->ty = cp_arrayof(p, et, len/width + 1);
    n->eflags |= CP_EF_LVALUE;
    cp_finish(p, n);
    return n;
}

static void cp_typeint(cp_parser *p, cp_node *n)
{
    i32 flags = 0;
    b32 ok = 1;
    u64 v = cp_parseintval(n->text, &flags, &ok);
    if (!ok) cp_warn(p, n->tok, cp_concat(p->mem, S("invalid integer constant "), n->text));
    n->flags = flags;
    n->ival = (i64)v;
    b32 dec = !(n->text.len > 1 && n->text.data[0] == '0');
    b32 uns = (flags & CP_NF_UNSIGNED) != 0;
    i64 lbits = p->bt[CP_BT_LONG]->size * 8;
    cp_type *t;
    if (flags & CP_NF_BITINT) {
        i64 w = 1;
        while (w < 64 && (v >> w)) w++;
        t = cp_mktype(p, CP_T_BITINT);
        t->len = uns ? w : w + 1;
        if (t->len < 2 && !uns) t->len = 2;
        if (uns) t->bits = CP_B_UNSIGNED;
        t->size = t->len <= 8 ? 1 : t->len <= 16 ? 2 : t->len <= 32 ? 4 : 8;
        t->align = t->size;
    } else if (flags & CP_NF_SIZE) {
        t = uns ? p->sizet : p->ptrdifft;
    } else {
        i32 cands[6];
        i32 nc = 0;
        if (!(flags & (CP_NF_LONG|CP_NF_LONGLONG))) {
            if (!uns) cands[nc++] = CP_BT_INT;
            if (uns || !dec) cands[nc++] = CP_BT_UINT;
        }
        if (!(flags & CP_NF_LONGLONG)) {
            if (!uns) cands[nc++] = CP_BT_LONG;
            if (uns || !dec) cands[nc++] = CP_BT_ULONG;
        }
        if (!uns) cands[nc++] = CP_BT_LLONG;
        cands[nc++] = CP_BT_ULLONG;
        t = p->bt[CP_BT_ULLONG];
        for (i32 i = 0; i < nc; i++) {
            cp_type *c = p->bt[cands[i]];
            i64 bits = c->basic == CP_BT_LONG || c->basic == CP_BT_ULONG ? lbits : c->size*8;
            b32 cu = cp_isunsigned(p, c);
            u64 max = bits >= 64 ? (cu ? UINT64_MAX : (u64)INT64_MAX) : cu ? ((u64)1<<bits) - 1 : ((u64)1<<(bits-1)) - 1;
            if (v <= max) { t = c; break; }
        }
    }
    b32 imag = 0;
    for (iz i = 0; i < n->text.len; i++) {
        char c = (char)(n->text.data[i] | 32);
        b32 sized = i+1 < n->text.len && cp_isdigit((u8)n->text.data[i+1]);  // MSVC i64
        if ((c == 'i' || c == 'j') && !sized && i > 0) imag = 1;
    }
    if (imag && t->kind == CP_T_BUILTIN) t = p->cbt[t->basic];
    n->ty = t;
    if (!imag) {
        n->isconst = 1;
        n->cval = cp_truncate(p, t, (i64)v);
    }
}

static void cp_typefloat(cp_parser *p, cp_node *n)
{
    cp_str s = n->text;
    n->fval = cp_parsefloat(s);
    // find where the suffix begins: mantissa, then an exponent
    b32 hex = s.len > 1 && s.data[0] == '0' && (s.data[1]|32) == 'x';
    iz j = hex ? 2 : 0;
    while (j < s.len && (s.data[j] == '.' || s.data[j] == '\'' ||
           (hex ? cp_isxdigit((u8)s.data[j]) : cp_isdigit((u8)s.data[j])))) j++;
    if (j < s.len && (s.data[j]|32) == (hex ? 'p' : 'e')) {
        j++;
        if (j < s.len && (s.data[j] == '+' || s.data[j] == '-')) j++;
        while (j < s.len && (cp_isdigit((u8)s.data[j]) || s.data[j] == '\'')) j++;
    }
    cp_str suf = cp_span(s.data+j, s.data+s.len);
    b32 imag = 0;
    char lower[8];
    iz k = 0;
    for (iz m = 0; m < suf.len && k < 7; m++) {
        char c = (char)(suf.data[m] | 32);
        if (c == 'i' || c == 'j') { imag = 1; continue; }
        lower[k++] = c;
    }
    lower[k] = 0;
    cp_str l = {lower, k};
    i32 bt = CP_BT_DOUBLE;
    if (cp_equals(l, S("f"))) bt = CP_BT_FLOAT;
    else if (cp_equals(l, S("l"))) bt = CP_BT_LDOUBLE;
    else if (cp_equals(l, S("f16"))) bt = CP_BT_FLOAT16;
    else if (cp_equals(l, S("bf16"))) bt = CP_BT_BF16;
    else if (cp_equals(l, S("f32"))) bt = CP_BT_FLOAT32;
    else if (cp_equals(l, S("f64"))) bt = CP_BT_FLOAT64;
    else if (cp_equals(l, S("f128")) || cp_equals(l, S("q"))) bt = CP_BT_FLOAT128;
    else if (cp_equals(l, S("f32x"))) bt = CP_BT_FLOAT32X;
    else if (cp_equals(l, S("f64x"))) bt = CP_BT_FLOAT64X;
    else if (cp_equals(l, S("w"))) bt = CP_BT_LDOUBLE;
    else if (cp_equals(l, S("df"))) bt = CP_BT_DEC32;
    else if (cp_equals(l, S("dd"))) bt = CP_BT_DEC64;
    else if (cp_equals(l, S("dl"))) bt = CP_BT_DEC128;
    else if (k) cp_warn(p, n->tok, cp_concat(p->mem, S("invalid floating constant "), s));
    n->ty = imag ? p->cbt[bt] : p->bt[bt];
    if (!imag) n->isconst = 2;
    // a float constant has float's precision: 3.14f != 3.14
    if (bt == CP_BT_FLOAT || bt == CP_BT_FLOAT32) n->fval = (double)(float)n->fval;
}

static void cp_typechar(cp_parser *p, cp_node *n)
{
    b32 ok = 1;
    n->ival = cp_parsecharval(n->text, &ok);
    char c0 = n->text.data[0];
    cp_type *t = p->bt[CP_BT_INT];
    if (c0 == 'L') t = p->wchart;
    else if (c0 == 'u' && n->text.data[1] == '8') t = p->bt[CP_BT_UCHAR];
    else if (c0 == 'u') t = p->char16t;
    else if (c0 == 'U') t = p->char32t;
    n->ty = t;
    n->isconst = 1;
    n->cval = cp_truncate(p, t, n->ival);
}

// Evaluate a member designator chain for offsetof: returns the offset
// and type, or -1.
static i64 cp_designoff(cp_parser *p, cp_type *base, cp_node *e, cp_type **out);

static cp_node *cp_findmember(cp_type *st, cp_str name, i64 *off)
{
    if (!st) return 0;
    st = st->unqual;
    for (cp_node *decl = st->members; decl; decl = decl->next) {
        if (decl->kind != CP_D_DECL) continue;
        if (decl->list) {
            for (cp_node *d = decl->list; d; d = d->next) {
                if (d->text.len && cp_equals(d->text, name)) {
                    *off += d->offset;
                    return d;
                }
            }
        } else if (decl->eflags & CP_EF_ANONMEMBER) {
            i64 o = *off + decl->offset;
            cp_node *r = cp_findmember(decl->type->sem, name, &o);
            if (r) {
                *off = o;
                return r;
            }
        }
    }
    return 0;
}

static i64 cp_designoff(cp_parser *p, cp_type *base, cp_node *e, cp_type **out)
{
    if (!e || !base) return -1;
    switch (e->kind) {
    case CP_E_IDENT: {
        i64 off = 0;
        cp_node *m = cp_findmember(base, e->text, &off);
        if (!m) return -1;
        e->ref = m;
        e->ty = m->ty;
        *out = m->ty;
        return off;
    }
    case CP_E_MEMBER: {
        cp_type *t = 0;
        i64 o = cp_designoff(p, base, e->a, &t);
        if (o < 0 || !t) return -1;
        i64 off = 0;
        cp_node *m = cp_findmember(t, e->text, &off);
        if (!m) return -1;
        e->ref = m;
        e->ty = m->ty;
        *out = m->ty;
        return o + off;
    }
    case CP_E_INDEX: {
        cp_type *t = 0;
        i64 o = cp_designoff(p, base, e->a, &t);
        if (o < 0 || !t || t->kind != CP_T_ARRAY || !e->b || e->b->isconst != 1 || !t->base) return -1;
        e->ty = t->base;
        *out = t->base;
        return o + e->b->cval * t->base->size;
    }
    }
    return -1;
}

// Address constant: the integer value of &e when known (offsetof idiom).
static b32 cp_addrconst(cp_node *e, i64 *v)
{
    if (!e) return 0;
    switch (e->kind) {
    case CP_E_MEMBER:
        if (e->op == CP_ARROW) {
            if (e->a && e->a->isconst == 1 && e->ref) { *v = e->a->cval + e->ref->offset; return 1; }
            return 0;
        } else {
            i64 b;
            if (e->ref && cp_addrconst(e->a, &b)) { *v = b + e->ref->offset; return 1; }
            return 0;
        }
    case CP_E_INDEX: {
        i64 b;
        if (e->b && e->b->isconst == 1 && e->ty && e->ty->size >= 0) {
            if (e->a && e->a->ty && e->a->ty->kind == CP_T_ARRAY && cp_addrconst(e->a, &b)) {
                *v = b + e->b->cval * e->ty->size;
                return 1;
            }
            if (e->a && e->a->isconst == 1 && e->a->ty && e->a->ty->kind == CP_T_POINTER) {
                *v = e->a->cval + e->b->cval * e->ty->size;
                return 1;
            }
        }
        return 0;
    }
    case CP_E_UNARY:
        if (e->op == CP_STAR && e->a && e->a->isconst == 1) { *v = e->a->cval; return 1; }
        return 0;
    }
    return 0;
}

// Constants are held in 64 bits: an unsigned value of 2^63 or more,
// stored negative, cannot widen to a 128-bit type without becoming a
// different number, so it is left unknown there.
static b32 cp_widenlost(cp_parser *p, cp_type *to, cp_node *a)
{
    return to && to->size > 8 && a->isconst == 1 && a->cval < 0 && a->ty &&
           cp_isinteger(a->ty) && a->ty->size <= 8 && cp_isunsigned(p, a->ty);
}

static void cp_constcast(cp_parser *p, cp_node *n, cp_node *a)
{
    cp_type *t = n->ty;
    if (!t || !a) return;
    if (cp_widenlost(p, t, a)) return;
    if (cp_isinteger(t) || cp_isptr(t)) {
        if (a->isconst == 1) {
            n->isconst = 1;
            n->cval = cp_isptr(t) ? a->cval : cp_truncate(p, t, a->cval);
        } else if (a->isconst == 2 && cp_isinteger(t)) {
            double f = a->fval;
            i64 v = f >= 9.2e18 ? INT64_MAX : f <= -9.2e18 ? INT64_MIN : (i64)f;
            if (f >= 9.2e18 && cp_isunsigned(p, t)) v = (i64)(u64)f;
            n->isconst = 1;
            n->cval = cp_truncate(p, t, v);
        }
    } else if (cp_isfloating(t) && !(t->flags & CP_TF_COMPLEX)) {
        if (a->isconst == 2) {
            n->isconst = 2;
            n->fval = t->basic == CP_BT_FLOAT ? (double)(float)a->fval : a->fval;
        } else if (a->isconst == 1) {
            n->isconst = 2;
            n->fval = cp_isunsigned(p, a->ty) ? (double)(u64)a->cval : (double)a->cval;
        }
    }
}

static cp_node *cp_castexpr(cp_parser *p);
static cp_node *cp_unary(cp_parser *p);
static cp_type *cp_optype(cp_parser *p, cp_node *e);
static cp_node *cp_compound(cp_parser *p, b32 newscope);
static cp_node *cp_postfixops(cp_parser *p, cp_node *n);

static cp_node *cp_ident(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_E_IDENT, p->pos);
    n->text = cp_tok(p)->text;
    cp_next(p);
    if (p->inoffsetof) return n;
    cp_sym *s = cp_lookup(p, n->text);
    if (!s) {
        if (cp_equals(n->text, S("__func__")) || cp_equals(n->text, S("__FUNCTION__")) ||
            cp_equals(n->text, S("__PRETTY_FUNCTION__"))) {
            i64 len = p->curfunc ? p->curfunc->name.len + 1 : 1;
            n->ty = cp_arrayof(p, cp_qualify(p, p->bt[CP_BT_CHAR], CP_Q_CONST), len);
            n->eflags |= CP_EF_LVALUE;
            return n;
        }
        if (cp_equals(n->text, S("nullptr")) || cp_equals(n->text, S("true")) ||
            cp_equals(n->text, S("false"))) {
            // C23 constants
            b32 np = n->text.data[0] == 'n';
            n->ty = np ? p->bt[CP_BT_NULLPTR] : p->bt[CP_BT_BOOL];
            n->isconst = 1;
            n->cval = n->text.data[0] == 't';
            return n;
        }
        if (cp_kind(p) == CP_LPAREN) {
            s = cp_implicitfunc(p, n);
            n->eflags |= CP_EF_IMPLICIT;
        } else {
            cp_warn(p, n->tok, cp_concat(p->mem, S("undeclared identifier "), n->text));
            return n;
        }
    }
    if (s->kind == CP_SYM_TYPEDEF) {
        p->pos = n->tok;
        cp_fail(p, "unexpected type name");
    }
    n->sym = s;
    n->ref = s->decl;
    n->ty = s->ty;
    if (s->kind == CP_SYM_ENUMCONST) {
        n->isconst = 1;
        n->cval = s->decl ? s->decl->cval : 0;
    } else if (s->kind == CP_SYM_VAR || s->kind == CP_SYM_PARAM) {
        n->eflags |= CP_EF_LVALUE;
        if (n->ty && (n->ty->quals & CP_Q_VOLATILE)) n->eflags |= CP_EF_SIDEEFFECT;
    }
    s->nrefs++;
    if (p->unevaluated) {
        // a use, but no read, write or call: sizeof(x) leaves x alone
        cp_addref(p, &s->refs, &s->lastref_, n, 0, 0);
        return n;
    }
    s->flags |= CP_YF_READ;
    n->useref_ = cp_addref(p, &s->refs, &s->lastref_, n, 0, CP_RF_READ);
    return n;
}

static cp_node *cp_generic(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_E_GENERIC, p->pos);
    cp_next(p);
    cp_expect(p, CP_LPAREN, "expected '(' after _Generic");
    if (cp_istypestart(p, p->pos)) {
        n->a = cp_parsetypename(p);  // C2y: type operand
    } else {
        p->unevaluated++;
        n->a = cp_assignexpr(p);
        p->unevaluated--;
    }
    cp_type *ct = n->a->kind == CP_E_TYPEARG ? n->a->ty : cp_decay(p, n->a->ty);
    cp_node head = {0};
    cp_node *tail = &head;
    cp_node *def = 0;
    while (cp_accept(p, CP_COMMA)) {
        cp_node *as = cp_newnode(p, CP_E_ASSOC, p->pos);
        if (cp_accept(p, CP_K_DEFAULT)) {
            def = as;
        } else {
            cp_node *tn = cp_parsetypename(p);
            as->type = tn->type;
            as->ty = tn->ty;
        }
        cp_expect(p, CP_COLON, "expected ':' in _Generic");
        as->a = cp_assignexpr(p);
        if (as->type && !n->ref && cp_compatible(ct, as->ty)) n->ref = as;
        cp_finish(p, as);
        tail = tail->next = as;
    }
    cp_expect(p, CP_RPAREN, "expected ')'");
    n->list = head.next;
    if (!n->ref) n->ref = def;
    if (n->ref && n->ref->a) {
        cp_node *sel = n->ref->a;
        n->ty = sel->ty;
        n->isconst = sel->isconst;
        n->cval = sel->cval;
        n->fval = sel->fval;
        n->eflags = sel->eflags;
    }
    cp_finish(p, n);
    return n;
}

static cp_node *cp_parsetypename(cp_parser *p)
{
    return cp_parsetypename2(p, 0);
}

static cp_node *cp_blocklit(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_E_BLOCK, p->pos);
    cp_next(p);  // ^
    cp_type *fn;
    p->fnscope = 0;
    if (cp_kind(p) == CP_LPAREN) {
        n->ival = 1;  // ^(params)
        fn = cp_syntype(p, CP_T_FUNC, p->pos);
        cp_params(p, fn);
        fn->base = cp_syntype(p, CP_T_IMPLICIT, p->pos);
        fn->base->bits = CP_B_VOID;
        cp_resolvebase(p, fn->base);
    } else if (cp_kind(p) != CP_LBRACE) {
        cp_node *tn = cp_parsetypename2(p, 1);
        n->ival = 2;  // ^ type, kept in b with its attributes
        n->b = tn;
        fn = tn->type;
        if (fn->kind != CP_T_FUNC) {
            cp_type *f = cp_syntype(p, CP_T_FUNC, p->pos);
            f->base = fn;
            fn = f;
        }
    } else {
        fn = cp_syntype(p, CP_T_FUNC, p->pos);
        fn->base = cp_syntype(p, CP_T_IMPLICIT, p->pos);
        fn->base->bits = CP_B_VOID;
        cp_resolvebase(p, fn->base);
    }
    n->type = fn;
    cp_type *ft = cp_resolve(p, fn);
    cp_type *bp = cp_mktype(p, CP_T_BLOCKPTR);
    bp->base = ft;
    bp->size = bp->align = p->ptrsize;
    n->ty = bp;
    cp_scope *outer = p->scope;
    cp_scope *ps = p->fnscope;
    if (ps) {
        ps->parent = outer;
        ps->depth = outer->depth + 1;
        p->scope = ps;
    } else {
        cp_pushscope(p);
    }
    n->a = cp_compound(p, 0);
    p->scope = outer;
    cp_finish(p, n);
    return n;
}

static b32 cp_isbuiltinname(cp_str s)
{
    return cp_startswith(s, S("__builtin_"));
}

static cp_node *cp_call(cp_parser *p, cp_node *fnexpr)
{
    cp_node *n = cp_newnode(p, CP_E_CALL, fnexpr->tok);
    n->a = fnexpr;
    cp_next(p);  // (
    b32 builtin = fnexpr->kind == CP_E_IDENT && cp_isbuiltinname(fnexpr->text);
    // An undeclared callee is likely a macro from a missing header
    // (va_arg, offsetof, container_of): let it take type arguments.
    b32 typeargs = builtin || (fnexpr->kind == CP_E_IDENT && fnexpr->sym &&
                               (fnexpr->sym->flags & CP_YF_IMPLICIT));
    b32 offsetof = builtin && cp_equals(fnexpr->text, S("__builtin_offsetof"));
    cp_node head = {0};
    cp_node *tail = &head;
    i32 argi = 0;
    if (cp_kind(p) != CP_RPAREN) {
        for (;;) {
            cp_node *arg;
            if (offsetof && argi == 1) {
                // a member designator, even one named like a typedef
                p->inoffsetof++;
                arg = cp_assignexpr(p);
                p->inoffsetof--;
            } else if (typeargs && cp_istypearg(p, p->pos)) {
                arg = cp_parsetypename(p);
            } else {
                arg = cp_assignexpr(p);
            }
            tail = tail->next = arg;
            argi++;
            if (!cp_accept(p, CP_COMMA)) break;
        }
    }
    cp_expect(p, CP_RPAREN, "expected ')' after arguments");
    n->list = head.next;
    n->eflags |= CP_EF_SIDEEFFECT;
    cp_sidefx(n);

    // result type
    cp_type *ft = fnexpr->ty;
    if (ft && ft->kind == CP_T_POINTER && ft->base && ft->base->kind == CP_T_FUNC) ft = ft->base;
    if (ft && ft->kind == CP_T_BLOCKPTR) ft = ft->base;
    if (ft && ft->kind == CP_T_FUNC) {
        n->ty = ft->base;
    } else if (ft) {
        cp_warn(p, n->tok, S("called object is not a function"));
    }
    cp_node *a0 = n->list;
    cp_node *a1 = a0 ? a0->next : 0;
    if (fnexpr->kind == CP_E_IDENT && (cp_startswith(fnexpr->text, S("__sync_")) ||
                                       cp_startswith(fnexpr->text, S("__atomic_")))) {
        cp_str w = fnexpr->text;
        b32 isbool = 0, isvoid = 0;
        static const char *bools[] = {
            "__sync_bool_compare_and_swap", "__atomic_compare_exchange",
            "__atomic_compare_exchange_n", "__atomic_test_and_set",
            "__atomic_always_lock_free", "__atomic_is_lock_free",
        };
        static const char *voids[] = {
            "__sync_synchronize", "__sync_lock_release", "__atomic_store",
            "__atomic_store_n", "__atomic_load", "__atomic_exchange",
            "__atomic_clear", "__atomic_thread_fence", "__atomic_signal_fence",
        };
        // the sized forms (__atomic_exchange_4, __sync_fetch_and_add_8)
        // return an unsigned integer of that size, whatever ptr points to
        i32 size = 0;
        static const char *sizes[] = {"_1", "_2", "_4", "_8", "_16"};
        for (iz i = 0; i < cp_countof(sizes); i++) {
            cp_str suf = cp_cstr((char *)sizes[i]);
            if (w.len > suf.len && cp_equals(cp_span(w.data+w.len-suf.len, w.data+w.len), suf)) {
                size = i == 4 ? 16 : 1 << i;
                w.len -= suf.len;
            }
        }
        for (iz i = 0; i < cp_countof(bools); i++) isbool |= cp_equals(w, cp_cstr((char *)bools[i]));
        for (iz i = 0; i < cp_countof(voids); i++) isvoid |= cp_equals(w, cp_cstr((char *)voids[i]));
        if (size) {
            // the generic __atomic_exchange and __atomic_load return
            // through a pointer, their sized forms by value
            isvoid = cp_equals(w, S("__atomic_store")) || cp_equals(w, S("__sync_lock_release"));
        }
        if (size && !isbool && !isvoid) {
            i32 bt = size == 1 ? CP_BT_UCHAR : size == 2 ? CP_BT_USHORT : size == 4 ? CP_BT_UINT :
                     size == 16 ? CP_BT_UINT128 :
                     p->bt[CP_BT_ULONG]->size == 8 ? CP_BT_ULONG : CP_BT_ULLONG;
            n->ty = p->bt[bt];
        } else if (isbool) {
            n->ty = p->bt[CP_BT_BOOL];
        } else if (isvoid) {
            n->ty = p->bt[CP_BT_VOID];
        } else if (a0 && a0->ty) {
            cp_type *pt = cp_decay(p, a0->ty);
            if (pt->kind == CP_T_POINTER && pt->base) n->ty = cp_unqualified(pt->base);
        }
    }
    if (builtin) {
        cp_str name = cp_span(fnexpr->text.data+10, fnexpr->text.data+fnexpr->text.len);
        char code = cp_builtincode(name);
        b32 known = code != 0;
        // A builtin missing from the table (the hundreds of target
        // intrinsics such as __builtin_ia32_addps) has a type we do not
        // know: leave it unknown rather than guess int.
        if (!known) n->ty = 0;
        switch (code) {
        case 'a': if (a0) n->ty = cp_decay(p, a0->ty); break;
        case 'T': if (a0) n->ty = a0->ty; break;
        case 'S': {
            // __builtin_shufflevector(a, b, i...): the element type of a,
            // as many elements as indices
            cp_type *v = a0 && a0->ty ? a0->ty->sem : 0;
            i64 count = 0;
            for (cp_node *x = a1 ? a1->next : 0; x; x = x->next) count++;
            if (v && v->kind == CP_T_VECTOR && v->base && count > 0) {
                cp_type *r = cp_mktype(p, CP_T_VECTOR);
                r->base = v->base;
                r->len = count;
                r->size = count * v->base->size;
                r->align = r->size > 0 && r->size <= 64 && !(r->size & (r->size-1)) ? r->size : 16;
                n->ty = r;
            }
            break;
        }
        case 'U': if (a1) n->ty = a1->ty; break;
        case 'K': if (a0 && a0->ty && a0->ty->kind == CP_T_BUILTIN) n->ty = p->cbt[a0->ty->basic]; break;
        case 'X':
            if (a0 && a1 && a1->next && a0->isconst == 1) {
                cp_node *sel = a0->cval ? a1 : a1->next;
                n->ty = sel->ty;
                n->isconst = sel->isconst;
                n->cval = sel->cval;
                n->fval = sel->fval;
                n->eflags = sel->eflags;
            }
            break;
        }
        if (cp_equals(name, S("offsetof")) && a0 && a0->kind == CP_E_TYPEARG && a1) {
            cp_type *mt = 0;
            i64 off = cp_designoff(p, a0->ty, a1, &mt);
            if (off >= 0) {
                n->isconst = 1;
                n->cval = off;
            } else {
                cp_warn(p, a1->tok, S("bad offsetof designator"));
            }
            n->eflags &= ~CP_EF_SIDEEFFECT;
        } else if (cp_equals(name, S("types_compatible_p")) && a0 && a1) {
            n->isconst = 1;
            cp_type *x = cp_unqualified(a0->ty), *y = cp_unqualified(a1->ty);
            if (x && x->kind == CP_T_ARRAY && x->base) {
                cp_type *b = cp_unqualified(x->base);
                if (b != x->base) x = cp_arrayof(p, b, x->len);
            }
            if (y && y->kind == CP_T_ARRAY && y->base) {
                cp_type *b = cp_unqualified(y->base);
                if (b != y->base) y = cp_arrayof(p, b, y->len);
            }
            n->cval = cp_compatible(x, y);
            n->eflags &= ~CP_EF_SIDEEFFECT;
        } else if (cp_equals(name, S("constant_p")) && a0) {
            if (a0->isconst) { n->isconst = 1; n->cval = 1; }
            n->eflags &= ~CP_EF_SIDEEFFECT;
        } else if ((cp_equals(name, S("expect")) || cp_equals(name, S("expect_with_probability"))) && a0) {
            if (a0->isconst == 1) { n->isconst = 1; n->cval = cp_truncate(p, n->ty, a0->cval); }
        } else if (cp_equals(name, S("classify_type"))) {
            n->eflags &= ~CP_EF_SIDEEFFECT;
        }
    }
    // call graph
    if (fnexpr->kind == CP_E_IDENT && fnexpr->sym && !p->unevaluated) {
        cp_sym *callee = fnexpr->sym;
        callee->flags |= CP_YF_CALLED;
        if (fnexpr->useref_) fnexpr->useref_->flags = (fnexpr->useref_->flags & ~CP_RF_READ) | CP_RF_CALL;
        // a call through a function pointer reads the pointer; it is not
        // an edge of the call graph, which links functions
        if (callee->kind == CP_SYM_FUNC && p->curfunc) {
            cp_addref(p, &p->curfunc->calls, &p->curfunc->lastcall_, n, callee, CP_RF_CALL);
            cp_addref(p, &callee->callers, &callee->lastcaller_, n, p->curfunc, CP_RF_CALL);
        }
    }
    cp_finish(p, n);
    return n;
}

static cp_node *cp_member(cp_parser *p, cp_node *base)
{
    cp_node *n = cp_newnode(p, CP_E_MEMBER, base->tok);
    n->op = cp_kind(p);
    n->a = base;
    cp_next(p);
    cp_softname(p, p->pos);
    if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected member name");
    n->text = cp_tok(p)->text;
    cp_next(p);
    if (p->inoffsetof) return n;
    cp_type *st = base->ty;
    i32 quals = 0;
    if (n->op == CP_ARROW) {
        if (st && (st->kind == CP_T_POINTER || st->kind == CP_T_ARRAY)) st = st->base;
        else st = 0;
    }
    if (st) quals = st->quals;
    if (st && (st->kind == CP_T_STRUCT || st->kind == CP_T_UNION)) {
        i64 off = 0;
        cp_node *m = cp_findmember(st, n->text, &off);
        if (m) {
            n->ref = m;
            n->ty = cp_qualify(p, m->ty, quals & ~CP_Q_RESTRICT);
        } else if (st->members) {
            cp_warn(p, n->tok, cp_concat(p->mem, S("no member named "), n->text));
        }
    } else if (base->ty) {
        cp_warn(p, n->tok, cp_concat(p->mem, S("member access on non-struct: "), n->text));
    }
    if (n->op == CP_ARROW || (base->eflags & CP_EF_LVALUE)) n->eflags |= CP_EF_LVALUE;
    cp_sidefx(n);
    if (n->ty && (n->ty->quals & CP_Q_VOLATILE)) n->eflags |= CP_EF_SIDEEFFECT;
    return n;
}

static cp_node *cp_postfixops(cp_parser *p, cp_node *n)
{
    i32 count = 0;
    for (;;) {
        if (++count > CPARSE_MAXDEPTH*4) cp_fail(p, "expression too long");
        i32 k = cp_kind(p);
        if (k == CP_LPAREN) {
            n = cp_call(p, n);
        } else if (k == CP_LBRACK && cp_peekk(p, 1) != CP_LBRACK) {
            cp_node *x = cp_newnode(p, CP_E_INDEX, n->tok);
            cp_next(p);
            x->a = n;
            x->b = cp_expr(p);
            cp_expect(p, CP_RBRACK, "expected ']'");
            cp_type *at = x->a->ty, *bt = x->b->ty;
            if (at && (at->kind == CP_T_POINTER || at->kind == CP_T_ARRAY || at->kind == CP_T_VECTOR)) {
                x->ty = at->base;
            } else if (bt && (bt->kind == CP_T_POINTER || bt->kind == CP_T_ARRAY)) {
                x->ty = bt->base;
            }
            x->eflags |= CP_EF_LVALUE;
            cp_sidefx(x);
            n = x;
        } else if (k == CP_DOT || k == CP_ARROW) {
            n = cp_member(p, n);
        } else if (k == CP_INC || k == CP_DEC) {
            cp_node *x = cp_newnode(p, CP_E_POSTFIX, n->tok);
            x->op = k;
            x->a = n;
            cp_next(p);
            x->ty = n->ty ? cp_unqualified(n->ty) : 0;
            cp_markuse(n, CP_RF_READ|CP_RF_WRITE);
            x->eflags |= CP_EF_SIDEEFFECT;
            n = x;
        } else {
            break;
        }
        cp_finish(p, n);
    }
    return n;
}

static cp_node *cp_compoundlit(cp_parser *p, cp_node *tn, i32 tok)
{
    cp_node *n = cp_newnode(p, CP_E_COMPOUND, tok);
    n->type = tn->type;
    n->attrs = tn->attrs;
    n->a = cp_initlist(p);
    n->ty = cp_completearray(p, tn->ty, n->a);
    n->eflags |= CP_EF_LVALUE;
    cp_sidefx(n);
    cp_finish(p, n);
    return n;
}

static cp_node *cp_primary(cp_parser *p)
{
    cp_softname(p, p->pos);
    cp_token *t = cp_tok(p);
    cp_node *n;
    switch (t->kind) {
    case CP_IDENT:
        return cp_ident(p);
    case CP_INTLIT:
        n = cp_newnode(p, CP_E_INT, p->pos);
        n->text = t->text;
        cp_next(p);
        cp_typeint(p, n);
        return n;
    case CP_FLOATLIT:
        n = cp_newnode(p, CP_E_FLOAT, p->pos);
        n->text = t->text;
        cp_next(p);
        cp_typefloat(p, n);
        return n;
    case CP_CHARLIT:
        n = cp_newnode(p, CP_E_CHAR, p->pos);
        n->text = t->text;
        cp_next(p);
        cp_typechar(p, n);
        return n;
    case CP_STRLIT:
        return cp_strlit(p);
    case CP_K_GENERIC:
        return cp_generic(p);
    case CP_LPAREN:
        if (cp_peekk(p, 1) == CP_LBRACE) {
            n = cp_newnode(p, CP_E_STMTEXPR, p->pos);
            cp_next(p);
            n->a = cp_compound(p, 1);
            cp_expect(p, CP_RPAREN, "expected ')' after statement expression");
            // type of the last expression statement
            cp_node *last = 0;
            for (cp_node *s = n->a->list; s; s = s->next) last = s;
            while (last && (last->kind == CP_S_LABEL || last->kind == CP_S_CASE || last->kind == CP_S_DEFAULT)) {
                last = last->kind == CP_S_LABEL ? last->a : last->c;
            }
            n->ty = last && last->kind == CP_S_EXPR && last->a && last->a->ty ?
                    cp_decay(p, last->a->ty) : p->bt[CP_BT_VOID];
            n->eflags |= CP_EF_SIDEEFFECT;
            cp_finish(p, n);
            return n;
        } else {
            cp_next(p);
            cp_enter(p);
            n = cp_expr(p);
            p->depth--;
            cp_expect(p, CP_RPAREN, "expected ')'");
            return n;
        }
    default:
        cp_fail(p, "expected expression");
    }
}

static b32 cp_iscaststart(cp_parser *p)
{
    // at "(": a type name follows?
    if (cp_istypestart(p, p->pos+1)) return 1;
    // (undeclared) followed by an operand: guess a cast
    cp_token *t = &p->toks[p->pos+1];
    if (t->kind == CP_IDENT && !cp_lookup(p, t->text) && cp_peekk(p, 2) == CP_RPAREN) {
        i32 k = cp_peekk(p, 3);
        return k == CP_IDENT || k == CP_INTLIT || k == CP_FLOATLIT || k == CP_CHARLIT || k == CP_STRLIT;
    }
    return 0;
}

static cp_node *cp_sizeof(cp_parser *p, i32 kind)
{
    cp_node *n = cp_newnode(p, kind, p->pos);
    cp_next(p);
    cp_type *t = 0;
    if (cp_kind(p) == CP_LPAREN && cp_iscaststart(p)) {
        i32 tok = p->pos;
        cp_next(p);
        cp_node *tn = cp_parsetypename(p);
        cp_expect(p, CP_RPAREN, "expected ')'");
        if (cp_kind(p) == CP_LBRACE) {
            n->a = cp_postfixops(p, cp_compoundlit(p, tn, tok));
            t = n->a->ty;
        } else {
            n->type = tn->type;
            n->attrs = tn->attrs;
            n->list = tn;  // keep the semantic type reachable
            t = tn->ty;
        }
    } else {
        cp_enter(p);
        p->unevaluated++;
        n->a = cp_unary(p);
        p->unevaluated--;
        p->depth--;
        t = n->a->ty;
    }
    n->ty = p->sizet;
    if (t) {
        if (kind == CP_E_SIZEOF) {
            if (t->size >= 0 && !(t->flags & CP_TF_VLA)) {
                n->isconst = 1;
                n->cval = t->kind == CP_T_FUNC ? 1 : t->size;
            }
        } else {
            n->isconst = 1;
            n->cval = t->kind == CP_T_ARRAY && t->base ? t->base->align : t->align;
            if (t->kind == CP_T_ARRAY) {
                cp_type *e = t;
                while (e->kind == CP_T_ARRAY && e->base) e = e->base;
                n->cval = e->align;
            }
        }
    }
    if (n->a && (n->a->eflags & CP_EF_SIDEEFFECT) && t && (t->flags & CP_TF_VLA)) {
        n->eflags |= CP_EF_SIDEEFFECT;
    }
    cp_finish(p, n);
    return n;
}

static void cp_foldunary(cp_parser *p, cp_node *n)
{
    cp_node *a = n->a;
    if (!a || !n->ty) return;
    if (a->isconst == 1 && cp_isinteger(n->ty)) {
        i64 v = a->cval;
        switch (n->op) {
        case CP_PLUS:  break;
        case CP_MINUS: v = (i64)(0 - (u64)v); break;
        case CP_TILDE: v = ~v; break;
        case CP_NOT:   v = !v; break;
        default: return;
        }
        n->isconst = 1;
        n->cval = cp_truncate(p, n->ty, v);
    } else if (a->isconst == 2) {
        switch (n->op) {
        case CP_PLUS:  n->isconst = 2; n->fval = a->fval; break;
        case CP_MINUS: n->isconst = 2; n->fval = -a->fval; break;
        case CP_NOT:   n->isconst = 1; n->cval = a->fval == 0; break;
        }
    } else if (a->isconst == 1 && n->op == CP_NOT) {
        n->isconst = 1;
        n->cval = !a->cval;
    }
}

static cp_node *cp_unary(cp_parser *p)
{
    i32 k = cp_kind(p);
    i32 tok = p->pos;
    switch (k) {
    case CP_K_SIZEOF:
        return cp_sizeof(p, CP_E_SIZEOF);
    case CP_K_ALIGNOF:
        return cp_sizeof(p, CP_E_ALIGNOF);
    case CP_XOR:
        return cp_postfixops(p, cp_blocklit(p));
    case CP_ANDAND: {
        cp_node *n = cp_newnode(p, CP_E_LABELADDR, tok);
        cp_next(p);
        if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected label name");
        n->text = cp_tok(p)->text;
        cp_next(p);
        n->ty = p->voidptr;
        cp_listpush(p->mem, &p->gototail, &p->ngotos, n);
        cp_finish(p, n);
        return n;
    }
    case CP_INC: case CP_DEC: case CP_AMP: case CP_STAR: case CP_PLUS:
    case CP_MINUS: case CP_TILDE: case CP_NOT: case CP_K_REAL: case CP_K_IMAG:
    case CP_K_EXTENSION: {
        cp_node *n = cp_newnode(p, CP_E_UNARY, tok);
        n->op = k;
        cp_next(p);
        cp_enter(p);
        n->a = (k == CP_INC || k == CP_DEC) ? cp_unary(p) : cp_castexpr(p);
        p->depth--;
        cp_type *t = n->a->ty;
        switch (k) {
        case CP_INC: case CP_DEC:
            n->ty = t ? cp_unqualified(t) : 0;
            cp_markuse(n->a, CP_RF_READ|CP_RF_WRITE);
            n->eflags |= CP_EF_SIDEEFFECT;
            break;
        case CP_AMP: {
            n->ty = t ? cp_ptrto(p, t) : 0;
            cp_markuse(n->a, CP_RF_ADDR);
            i64 v;
            if (cp_addrconst(n->a, &v)) { n->isconst = 1; n->cval = v; }
        } break;
        case CP_STAR:
            if (t && (t->kind == CP_T_POINTER || t->kind == CP_T_ARRAY)) n->ty = t->base;
            else if (t && t->kind == CP_T_FUNC) n->ty = t;
            n->eflags |= CP_EF_LVALUE;
            break;
        case CP_PLUS: case CP_MINUS: case CP_TILDE:
            n->ty = cp_promote(p, cp_optype(p, n->a));
            if (n->ty && n->ty->kind == CP_T_ARRAY) n->ty = cp_decay(p, n->ty);
            cp_foldunary(p, n);
            break;
        case CP_NOT:
            n->ty = p->bt[CP_BT_INT];
            cp_foldunary(p, n);
            break;
        case CP_K_REAL: case CP_K_IMAG:
            if (t && t->kind == CP_T_BUILTIN) n->ty = p->bt[t->basic];
            else n->ty = t;
            if (n->a->eflags & CP_EF_LVALUE) n->eflags |= CP_EF_LVALUE;
            break;
        case CP_K_EXTENSION:
            n->ty = t;
            n->isconst = n->a->isconst;
            n->cval = n->a->cval;
            n->fval = n->a->fval;
            n->eflags = n->a->eflags;
            break;
        }
        cp_sidefx(n);
        cp_finish(p, n);
        return n;
    }
    }
    return cp_postfixops(p, cp_primary(p));
}

static cp_node *cp_castexpr(cp_parser *p)
{
    if (cp_kind(p) == CP_LPAREN && cp_iscaststart(p)) {
        i32 tok = p->pos;
        cp_next(p);
        cp_node *tn = cp_parsetypename(p);
        cp_expect(p, CP_RPAREN, "expected ')' after type name");
        if (cp_kind(p) == CP_LBRACE) {
            return cp_postfixops(p, cp_compoundlit(p, tn, tok));
        }
        cp_node *n = cp_newnode(p, CP_E_CAST, tok);
        n->type = tn->type;
        n->attrs = tn->attrs;
        n->list = tn;
        cp_enter(p);
        n->a = cp_castexpr(p);
        p->depth--;
        n->ty = tn->ty ? cp_unqualified(tn->ty) : 0;
        cp_constcast(p, n, n->a);
        cp_sidefx(n);
        cp_finish(p, n);
        return n;
    }
    return cp_unary(p);
}

static i32 cp_binprec(i32 k)
{
    switch (k) {
    case CP_STAR: case CP_SLASH: case CP_PERCENT: return 13;
    case CP_PLUS: case CP_MINUS: return 12;
    case CP_SHL: case CP_SHR: return 11;
    case CP_LT: case CP_GT: case CP_LE: case CP_GE: return 10;
    case CP_EQ: case CP_NE: return 9;
    case CP_AMP: return 8;
    case CP_XOR: return 7;
    case CP_OR: return 6;
    case CP_ANDAND: return 5;
    case CP_OROR: return 4;
    }
    return 0;
}

static void cp_foldbinary(cp_parser *p, cp_node *n)
{
    cp_node *a = n->a, *b = n->b;
    i32 op = n->op;
    if (!n->ty) return;
    if (a->isconst == 1 && b->isconst == 1 && (cp_isinteger(n->ty) || n->ty->kind == CP_T_POINTER)) {
        // operate in the common type of the operands
        cp_type *ct = (op == CP_SHL || op == CP_SHR) ? n->ty :
                      (op >= CP_LT && op <= CP_NE) ? cp_uac(p, a->ty, b->ty) : n->ty;
        if (!cp_isinteger(ct)) ct = n->ty;
        if (cp_widenlost(p, ct, a) || cp_widenlost(p, ct, b)) return;
        b32 uns = cp_isunsigned(p, ct) && ct->kind != CP_T_POINTER;
        i64 x = a->cval, y = b->cval;
        if (cp_isinteger(ct)) { x = cp_truncate(p, ct, x); y = op == CP_SHL || op == CP_SHR ? y : cp_truncate(p, ct, y); }
        u64 ux = (u64)x, uy = (u64)y;
        i64 r = 0;
        switch (op) {
        case CP_STAR: r = (i64)(ux*uy); break;
        case CP_SLASH:
            if (!y) return;
            if (uns) r = (i64)(cp_truncate(p, ct, x) >= 0 || ct->size*8 >= 64 ? ux/uy : ux/uy);
            else if (x == INT64_MIN && y == -1) r = x;
            else r = x / y;
            if (uns) {
                u64 mask = ct->size >= 8 ? UINT64_MAX : ((u64)1 << (ct->size*8)) - 1;
                r = (i64)((ux & mask) / (uy & mask));
            }
            break;
        case CP_PERCENT:
            if (!y) return;
            if (uns) {
                u64 mask = ct->size >= 8 ? UINT64_MAX : ((u64)1 << (ct->size*8)) - 1;
                r = (i64)((ux & mask) % (uy & mask));
            } else if (x == INT64_MIN && y == -1) {
                r = 0;
            } else {
                r = x % y;
            }
            break;
        case CP_PLUS:
            if (n->ty->kind == CP_T_POINTER) {
                cp_node *pn = a->ty && cp_decay(p, a->ty)->kind == CP_T_POINTER ? a : b;
                cp_node *in = pn == a ? b : a;
                cp_type *pt = cp_decay(p, pn->ty);
                i64 es = pt->base && pt->base->size > 0 ? pt->base->size : 1;
                r = (i64)((u64)pn->cval + (u64)in->cval * (u64)es);
            } else {
                r = (i64)(ux+uy);
            }
            break;
        case CP_MINUS:
            if (n->ty->kind == CP_T_POINTER) {
                cp_type *pt = cp_decay(p, a->ty);
                i64 es = pt && pt->base && pt->base->size > 0 ? pt->base->size : 1;
                r = (i64)((u64)x - (u64)y * (u64)es);
            } else if (a->ty && cp_decay(p, a->ty)->kind == CP_T_POINTER) {
                cp_type *pt = cp_decay(p, a->ty);
                i64 es = pt->base && pt->base->size > 0 ? pt->base->size : 1;
                r = (x - y) / es;
            } else {
                r = (i64)(ux-uy);
            }
            break;
        case CP_SHL: if (y < 0 || y >= 64) return; r = (i64)(ux << y); break;
        case CP_SHR:
            if (y < 0 || y >= 64) return;
            if (uns) {
                u64 mask = ct->size >= 8 ? UINT64_MAX : ((u64)1 << (ct->size*8)) - 1;
                r = (i64)((ux & mask) >> y);
            } else {
                r = x >= 0 ? (i64)(ux >> y) : ~(i64)(~ux >> y);
            }
            break;
        case CP_LT: r = uns ? (u64)cp_truncate(p, ct, x) < (u64)cp_truncate(p, ct, y) : x < y; break;
        case CP_GT: r = uns ? (u64)cp_truncate(p, ct, x) > (u64)cp_truncate(p, ct, y) : x > y; break;
        case CP_LE: r = uns ? (u64)cp_truncate(p, ct, x) <= (u64)cp_truncate(p, ct, y) : x <= y; break;
        case CP_GE: r = uns ? (u64)cp_truncate(p, ct, x) >= (u64)cp_truncate(p, ct, y) : x >= y; break;
        case CP_EQ: r = x == y; break;
        case CP_NE: r = x != y; break;
        case CP_AMP: r = x & y; break;
        case CP_XOR: r = x ^ y; break;
        case CP_OR: r = x | y; break;
        case CP_ANDAND: r = x && y; break;
        case CP_OROR: r = x || y; break;
        default: return;
        }
        if (ct->size > 8 && n->ty->kind != CP_T_POINTER) {
            // values are held in 64 bits: a 128-bit result that does not
            // fit is no known constant, rather than a wrong one
            b32 over = 0;
            switch (op) {
            case CP_STAR:
                over = uns ? ux && (u64)r / ux != uy :
                             x && ((x == -1 && y == INT64_MIN) || (y == -1 && x == INT64_MIN) ||
                                   r / x != y);
                break;
            case CP_PLUS:
                over = uns ? (u64)r < ux : (x > 0 && y > 0 && r < 0) || (x < 0 && y < 0 && r >= 0);
                break;
            case CP_MINUS:
                over = uns ? ux < uy : (x >= 0 && y < 0 && r < 0) || (x < 0 && y > 0 && r >= 0);
                break;
            case CP_SHL:
                over = y > 0 && (uns ? ux >> (64 - y) : ux >> (63 - y)) != 0;
                break;
            }
            if (over) return;
        }
        n->isconst = 1;
        n->cval = n->ty->kind == CP_T_POINTER ? r : cp_truncate(p, n->ty, r);
        return;
    }
    if (op == CP_ANDAND && a->isconst == 1 && !a->cval) { n->isconst = 1; n->cval = 0; return; }
    if (op == CP_OROR && a->isconst == 1 && a->cval) { n->isconst = 1; n->cval = 1; return; }
    if (a->isconst && b->isconst && (a->isconst == 2 || b->isconst == 2)) {
        double x = a->isconst == 2 ? a->fval : cp_isunsigned(p, a->ty) ? (double)(u64)a->cval : (double)a->cval;
        double y = b->isconst == 2 ? b->fval : cp_isunsigned(p, b->ty) ? (double)(u64)b->cval : (double)b->cval;
        double r = 0;
        i64 ir = 0;
        b32 isint = 0;
        switch (op) {
        case CP_STAR: r = x*y; break;
        case CP_SLASH: r = x/y; break;
        case CP_PLUS: r = x+y; break;
        case CP_MINUS: r = x-y; break;
        case CP_LT: ir = x < y; isint = 1; break;
        case CP_GT: ir = x > y; isint = 1; break;
        case CP_LE: ir = x <= y; isint = 1; break;
        case CP_GE: ir = x >= y; isint = 1; break;
        case CP_EQ: ir = x == y; isint = 1; break;
        case CP_NE: ir = x != y; isint = 1; break;
        case CP_ANDAND: ir = x != 0 && y != 0; isint = 1; break;
        case CP_OROR: ir = x != 0 || y != 0; isint = 1; break;
        default: return;
        }
        if (isint) { n->isconst = 1; n->cval = ir; }
        else if (cp_isfloating(n->ty)) {
            n->isconst = 2;
            n->fval = n->ty->basic == CP_BT_FLOAT ? (double)(float)r : r;
        }
    }
}

// Type of an operand after decay, with bit-field promotion applied:
// a bit-field narrower than int promotes to int even when unsigned.
static cp_type *cp_optype(cp_parser *p, cp_node *e)
{
    if (!e->ty) return 0;
    if (e->kind == CP_E_MEMBER && e->ref && e->ref->bitwidth >= 0 && cp_isinteger(e->ty)) {
        i64 w = e->ref->bitwidth;
        cp_type *t = cp_unqualified(e->ty);
        if (t->kind == CP_T_BUILTIN && t->basic == CP_BT_BOOL) return p->bt[CP_BT_INT];
        if (w < 32 || (w == 32 && !cp_isunsigned(p, t))) return p->bt[CP_BT_INT];
        if (w == 32) return p->bt[CP_BT_UINT];
        return t;  // wider bit-fields keep their declared type (GCC)
    }
    return cp_decay(p, e->ty);
}

// The type of comparing vectors of type v: a vector of signed integers as
// wide as v's elements (float vectors included). GCC spells them signed
// char and long where long fits; Clang char and long long.
static cp_type *cp_vectorcmp(cp_parser *p, cp_type *v)
{
    if (!v->base || v->base->size <= 0) return v;
    i32 bt;
    switch (v->base->size) {
    case 1:  bt = p->clang ? CP_BT_CHAR : CP_BT_SCHAR; break;
    case 2:  bt = CP_BT_SHORT; break;
    case 4:  bt = CP_BT_INT; break;
    case 8:  bt = !p->clang && p->bt[CP_BT_LONG]->size == 8 ? CP_BT_LONG : CP_BT_LLONG; break;
    case 16: bt = CP_BT_INT128; break;
    default: return v;
    }
    cp_type *r = cp_mktype(p, CP_T_VECTOR);
    r->base = p->bt[bt];
    r->size = v->size;
    r->align = v->align;
    r->len = v->len;
    return r;
}

static void cp_typebinary(cp_parser *p, cp_node *n)
{
    i32 op = n->op;
    cp_type *ta = cp_optype(p, n->a);
    cp_type *tb = cp_optype(p, n->b);
    switch (op) {
    case CP_LT: case CP_GT: case CP_LE: case CP_GE: case CP_EQ: case CP_NE:
        if ((ta && ta->kind == CP_T_VECTOR) || (tb && tb->kind == CP_T_VECTOR)) {
            n->ty = cp_vectorcmp(p, ta && ta->kind == CP_T_VECTOR ? ta : tb);
        } else {
            n->ty = p->bt[CP_BT_INT];
        }
        break;
    case CP_ANDAND: case CP_OROR:
        n->ty = p->bt[CP_BT_INT];
        break;
    case CP_SHL: case CP_SHR:
        n->ty = cp_promote(p, ta);
        break;
    case CP_PLUS:
        if (cp_isptr(ta) && cp_isinteger(tb)) n->ty = ta;
        else if (cp_isptr(tb) && cp_isinteger(ta)) n->ty = tb;
        else n->ty = cp_uac(p, ta, tb);
        break;
    case CP_MINUS:
        if (cp_isptr(ta) && cp_isptr(tb)) n->ty = p->ptrdifft;
        else if (cp_isptr(ta)) n->ty = ta;
        else n->ty = cp_uac(p, ta, tb);
        break;
    default:
        n->ty = cp_uac(p, ta, tb);
    }
    cp_sidefx(n);
    cp_foldbinary(p, n);
}

static cp_node *cp_binary(cp_parser *p, i32 minprec)
{
    cp_node *l = cp_castexpr(p);
    i32 count = 0;
    for (;;) {
        i32 k = cp_kind(p);
        i32 prec = cp_binprec(k);
        if (!prec || prec < minprec) return l;
        if (++count > CPARSE_MAXDEPTH*4) cp_fail(p, "expression too long");
        cp_node *n = cp_newnode(p, CP_E_BINARY, l->tok);
        n->op = k;
        cp_next(p);
        n->a = l;
        cp_enter(p);
        n->b = cp_binary(p, prec+1);
        p->depth--;
        cp_typebinary(p, n);
        cp_finish(p, n);
        l = n;
    }
}

// A null pointer constant: integer constant 0, or one cast to void *.
static b32 cp_isnullconst(cp_node *e)
{
    if (!e || e->isconst != 1 || e->cval || !e->ty) return 0;
    if (cp_isinteger(e->ty)) return 1;
    if (e->ty->kind == CP_T_POINTER && e->kind == CP_E_CAST && e->ty->base &&
        e->ty->base->kind == CP_T_BUILTIN && e->ty->base->basic == CP_BT_VOID && !e->ty->base->quals) {
        return 1;
    }
    return 0;
}

static cp_node *cp_condexpr(cp_parser *p)
{
    cp_node *c = cp_binary(p, 4);
    if (cp_kind(p) != CP_QUESTION) return c;
    cp_node *n = cp_newnode(p, CP_E_COND, c->tok);
    cp_next(p);
    n->a = c;
    cp_enter(p);
    if (cp_kind(p) != CP_COLON) n->b = cp_expr(p);
    cp_expect(p, CP_COLON, "expected ':' in conditional expression");
    n->c = cp_condexpr(p);
    p->depth--;
    cp_node *x = n->b ? n->b : n->a;
    cp_type *ta = cp_optype(p, x);
    cp_type *tb = cp_optype(p, n->c);
    if (cp_isarith(ta) && cp_isarith(tb)) {
        n->ty = cp_uac(p, ta, tb);
    } else if (cp_isptr(ta) && cp_isptr(tb) && cp_isnullconst(x)) {
        n->ty = tb;
    } else if (cp_isptr(ta) && cp_isptr(tb) && cp_isnullconst(n->c)) {
        n->ty = ta;
    } else if (cp_isptr(ta) && cp_isptr(tb)) {
        cp_type *ba = ta->base, *bb = tb->base;
        if (ba && ba->unqual->kind == CP_T_BUILTIN && ba->unqual->basic == CP_BT_VOID) {
            n->ty = cp_ptrto(p, cp_qualify(p, ba, bb ? bb->quals : 0));
        } else if (bb && bb->unqual->kind == CP_T_BUILTIN && bb->unqual->basic == CP_BT_VOID) {
            n->ty = cp_ptrto(p, cp_qualify(p, bb, ba ? ba->quals : 0));
        } else {
            n->ty = cp_ptrto(p, cp_qualify(p, ba, bb ? bb->quals : 0));
        }
    } else if (cp_isptr(ta) && cp_isinteger(tb)) {
        n->ty = ta;
    } else if (cp_isptr(tb) && cp_isinteger(ta)) {
        n->ty = tb;
    } else {
        n->ty = ta ? ta : tb;
    }
    if (c->isconst == 1) {
        cp_node *sel = c->cval ? x : n->c;
        if (sel->isconst == 1 && cp_isinteger(n->ty)) {
            n->isconst = 1;
            n->cval = cp_truncate(p, n->ty, sel->cval);
        } else if (sel->isconst == 1 && cp_isptr(n->ty)) {
            n->isconst = 1;
            n->cval = sel->cval;
        } else if (sel->isconst && cp_isfloating(n->ty)) {
            n->isconst = 2;
            n->fval = sel->isconst == 2 ? sel->fval : (double)sel->cval;
        }
    }
    cp_sidefx(n);
    cp_finish(p, n);
    return n;
}

static b32 cp_isassignop(i32 k)
{
    switch (k) {
    case CP_ASSIGN: case CP_MULEQ: case CP_DIVEQ: case CP_MODEQ: case CP_ADDEQ:
    case CP_SUBEQ: case CP_SHLEQ: case CP_SHREQ: case CP_ANDEQ: case CP_XOREQ:
    case CP_OREQ:
        return 1;
    }
    return 0;
}

static cp_node *cp_assignexpr(cp_parser *p)
{
    cp_node *l = cp_condexpr(p);
    if (!cp_isassignop(cp_kind(p))) return l;
    if (l->kind == CP_E_BINARY || l->kind == CP_E_COND || l->kind == CP_E_COMMA) {
        // the grammar wants a unary expression on the left
        cp_fail(p, "expression is not assignable");
    }
    cp_node *n = cp_newnode(p, CP_E_ASSIGN, l->tok);
    n->op = cp_kind(p);
    cp_next(p);
    n->a = l;
    cp_enter(p);
    n->b = cp_assignexpr(p);
    p->depth--;
    n->ty = l->ty ? cp_unqualified(l->ty) : 0;
    cp_markuse(l, n->op == CP_ASSIGN ? CP_RF_WRITE : CP_RF_READ|CP_RF_WRITE);
    n->eflags |= CP_EF_SIDEEFFECT;
    cp_sidefx(n);
    cp_finish(p, n);
    return n;
}

static cp_node *cp_expr(cp_parser *p)
{
    cp_node *l = cp_assignexpr(p);
    i32 count = 0;
    while (cp_kind(p) == CP_COMMA) {
        if (++count > CPARSE_MAXDEPTH*4) cp_fail(p, "expression too long");
        cp_node *n = cp_newnode(p, CP_E_COMMA, l->tok);
        cp_next(p);
        n->a = l;
        n->b = cp_assignexpr(p);
        n->ty = n->b->ty ? cp_decay(p, n->b->ty) : 0;
        cp_sidefx(n);
        cp_finish(p, n);
        l = n;
    }
    return l;
}

// Parse an expression (or type name) at tokens [beg, end) without
// disturbing the parser, for attribute arguments. Null on failure.
static cp_node *cp_parseexprat(cp_parser *p, i32 beg, i32 end)
{
    if (beg >= end || end > p->ntoks) return 0;
    i32 savepos = p->pos;
    i32 savedepth = p->depth;
    i32 saveuneval = p->unevaluated;
    jmp_buf *saveerr = p->err;
    jmp_buf jb;
    cp_node *volatile r = 0;
    p->pos = beg;
    p->err = &jb;
    p->diags->quiet++;
    if (!setjmp(jb)) {
        cp_node *n;
        if (cp_istypestart(p, p->pos)) {
            n = cp_parsetypename(p);
        } else {
            n = cp_assignexpr(p);
        }
        r = p->pos == end ? n : 0;
    }
    p->diags->quiet--;
    p->err = saveerr;
    p->pos = savepos;
    p->depth = savedepth;
    p->unevaluated = saveuneval;
    return r;
}

// --- #pragma pack

static void cp_applypragmas(cp_parser *p)
{
    cp_unit *u = p->u;
    while (p->nextpragma < u->npragmas && u->pragmas[p->nextpragma].tok <= p->pos) {
        cp_pragma *pr = &u->pragmas[p->nextpragma++];
        cp_str t = pr->text;
        // [GCC] pack ( [push|pop] [, n] | n | )
        iz i = 0;
        while (i < t.len && (t.data[i] == ' ' || t.data[i] == '\t')) i++;
        cp_str rest = cp_span(t.data+i, t.data+t.len);
        if (cp_startswith(rest, S("__pragma"))) {
            // __pragma(pack(...)) from MSVC code
            iz j = 0;
            while (j < rest.len && rest.data[j] != '(') j++;
            rest = cp_span(rest.data+j+1, rest.data+rest.len);
            while (rest.len && (rest.data[0] == ' ')) { rest.data++; rest.len--; }
        }
        if (!cp_startswith(rest, S("pack"))) continue;
        rest = cp_span(rest.data+4, rest.data+rest.len);
        b32 push = 0, pop = 0, havenum = 0;
        i64 num = 0;
        for (iz j = 0; j < rest.len; j++) {
            cp_str w = cp_span(rest.data+j, rest.data+rest.len);
            if (cp_startswith(w, S("push"))) { push = 1; j += 3; }
            else if (cp_startswith(w, S("pop"))) { pop = 1; j += 2; }
            else if (cp_isdigit((u8)rest.data[j])) {
                havenum = 1;
                num = 0;
                while (j < rest.len && cp_isdigit((u8)rest.data[j])) {
                    num = num < 1<<20 ? num*10 + (rest.data[j] - '0') : num;
                    j++;
                }
                j--;
            } else if (cp_isalpha((u8)rest.data[j]) || rest.data[j] == '_') {
                while (j+1 < rest.len && cp_isident((u8)rest.data[j+1])) j++;  // identifiers
            }
        }
        if (push) {
            cp_packstack *s = cp_new(p->mem, 1, cp_packstack);
            s->pack = (i32)p->pack;
            s->next = p->packstack;
            p->packstack = s;
            if (havenum) p->pack = (i32)num;
        } else if (pop) {
            if (p->packstack) {
                p->pack = p->packstack->pack;
                p->packstack = p->packstack->next;
            }
            if (havenum) p->pack = (i32)num;
        } else {
            p->pack = havenum ? (i32)num : 0;
        }
    }
}

// --- statements

static cp_node *cp_stmt(cp_parser *p);
static cp_node *cp_blockitem(cp_parser *p);

static cp_node *cp_label(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_S_LABEL, p->pos);
    n->text = cp_tok(p)->text;
    cp_next(p);
    cp_next(p);  // :
    // GNU attributes here are the label's; "L: [[x]] s" gives [[x]] to s
    if (cp_kind(p) != CP_LBRACK) cp_attrs(p, &n->attrs);
    if (p->curfunc || p->labels || p->scope->depth > 0) {
        cp_map *m = cp_upsert(&p->labels, n->text, p->mem);
        if (m->val) {
            cp_warn(p, n->tok, cp_concat(p->mem, S("duplicate label "), n->text));
        } else {
            cp_sym *s = cp_newsym(p, n->text, CP_SYM_LABEL);
            s->decl = n;
            s->def = n;
            s->flags |= CP_YF_DEFINED;
            cp_addref(p, &s->decls, &s->lastdecl_, n, 0, CP_RF_DECL);
            n->sym = s;
            m->val = n;
        }
    }
    if (cp_kind(p) != CP_RBRACE && !(cp_isdeclstart(p, p->pos) && !(cp_kind(p) == CP_IDENT && cp_peekk(p, 1) == CP_COLON))) {
        n->a = cp_stmt(p);
    }
    cp_finish(p, n);
    return n;
}

static cp_node *cp_asmstmt(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_S_ASM, p->pos);
    cp_next(p);
    for (;;) {
        i32 k = cp_kind(p);
        if (k == CP_K_VOLATILE) n->flags |= CP_NF_VOLATILE;
        else if (k == CP_K_INLINE) n->flags |= CP_NF_INLINE;
        else if (k == CP_K_GOTO) n->flags |= CP_NF_GOTO;
        else break;
        cp_next(p);
    }
    cp_expect(p, CP_LPAREN, "expected '(' after asm");
    if (cp_kind(p) != CP_STRLIT) cp_fail(p, "expected asm template string");
    n->a = cp_strlit(p);
    cp_node **lists[4] = {&n->b, &n->c, &n->d, &n->list};
    i32 sections = 0;
    while (sections < 4 && cp_accept(p, CP_COLON)) {
        cp_node head = {0};
        cp_node *tail = &head;
        if (cp_kind(p) != CP_COLON && cp_kind(p) != CP_RPAREN) {
            for (;;) {
                cp_node *x;
                if (sections == 3) {
                    if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected label");
                    x = cp_newnode(p, CP_E_IDENT, p->pos);
                    x->text = cp_tok(p)->text;
                    cp_next(p);
                    cp_listpush(p->mem, &p->gototail, &p->ngotos, x);
                } else if (sections == 2) {
                    if (cp_kind(p) != CP_STRLIT) cp_fail(p, "expected clobber string");
                    x = cp_strlit(p);
                } else {
                    x = cp_newnode(p, CP_S_ASMOPERAND, p->pos);
                    if (cp_accept(p, CP_LBRACK)) {
                        if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected operand name");
                        x->text = cp_tok(p)->text;
                        cp_next(p);
                        cp_expect(p, CP_RBRACK, "expected ']'");
                    }
                    if (cp_kind(p) != CP_STRLIT) cp_fail(p, "expected constraint string");
                    x->b = cp_strlit(p);
                    cp_expect(p, CP_LPAREN, "expected '(' before asm operand");
                    x->a = cp_expr(p);
                    cp_expect(p, CP_RPAREN, "expected ')'");
                    if (sections == 0) cp_markuse(x->a, CP_RF_WRITE);
                    cp_finish(p, x);
                }
                tail = tail->next = x;
                if (!cp_accept(p, CP_COMMA)) break;
            }
        }
        *lists[sections] = head.next;
        sections++;
    }
    n->ival = sections;
    cp_expect(p, CP_RPAREN, "expected ')' after asm");
    n->eflags |= CP_EF_SIDEEFFECT;
    cp_finish(p, n);
    return n;
}

// MSVC: __asm { ... } or __asm instruction-to-end-of-line
static cp_node *cp_msasm(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_S_MSASM, p->pos);
    i32 line = cp_tok(p)->line;
    i32 file = cp_tok(p)->file;
    cp_next(p);
    if (cp_kind(p) == CP_LBRACE) {
        i32 depth = 0;
        for (;;) {
            i32 k = cp_kind(p);
            if (k == CP_EOF) cp_fail(p, "unterminated __asm block");
            cp_next(p);
            if (k == CP_LBRACE) depth++;
            if (k == CP_RBRACE && --depth == 0) break;
        }
    } else {
        while (cp_kind(p) != CP_EOF && cp_kind(p) != CP_RBRACE && cp_kind(p) != CP_K_ASM &&
               cp_tok(p)->line == line && cp_tok(p)->file == file) {
            cp_token *t = cp_tok(p);
            if (t->kind == CP_OTHER && t->text.len && (t->text.data[0] == '"' || t->text.data[0] == '\'')) {
                // the printer must break the line after a stray quote,
                // which would end the instruction early
                cp_fail(p, "unterminated literal in __asm");
            }
            cp_next(p);
        }
        cp_accept(p, CP_SEMI);
    }
    n->ival = p->pos;
    n->eflags |= CP_EF_SIDEEFFECT;
    cp_finish(p, n);
    return n;
}

static b32 cp_isgnuasm(cp_parser *p)
{
    i32 k = cp_peekk(p, 1);
    return k == CP_LPAREN || k == CP_K_VOLATILE || k == CP_K_INLINE || k == CP_K_GOTO;
}

static cp_node *cp_condition(cp_parser *p)
{
    cp_expect(p, CP_LPAREN, "expected '('");
    cp_node *e = cp_expr(p);
    cp_expect(p, CP_RPAREN, "expected ')'");
    return e;
}

static cp_node *cp_stmt(cp_parser *p)
{
    cp_enter(p);
    cp_applypragmas(p);
    i32 tok = p->pos;
    cp_node *n = 0;
    switch (cp_kind(p)) {
    case CP_LBRACE:
        n = cp_compound(p, 1);
        break;
    case CP_SEMI:
        n = cp_newnode(p, CP_S_NULL, tok);
        cp_next(p);
        break;
    case CP_K_IF:
        n = cp_newnode(p, CP_S_IF, tok);
        cp_next(p);
        n->a = cp_condition(p);
        n->b = cp_stmt(p);
        if (cp_accept(p, CP_K_ELSE)) n->c = cp_stmt(p);
        break;
    case CP_K_SWITCH:
        n = cp_newnode(p, CP_S_SWITCH, tok);
        cp_next(p);
        n->a = cp_condition(p);
        n->b = cp_stmt(p);
        break;
    case CP_K_WHILE:
        n = cp_newnode(p, CP_S_WHILE, tok);
        cp_next(p);
        n->a = cp_condition(p);
        n->b = cp_stmt(p);
        break;
    case CP_K_DO:
        n = cp_newnode(p, CP_S_DO, tok);
        cp_next(p);
        n->b = cp_stmt(p);
        cp_expect(p, CP_K_WHILE, "expected 'while' after do body");
        n->a = cp_condition(p);
        cp_expect(p, CP_SEMI, "expected ';' after do-while");
        break;
    case CP_K_FOR:
        n = cp_newnode(p, CP_S_FOR, tok);
        cp_next(p);
        cp_expect(p, CP_LPAREN, "expected '(' after for");
        cp_pushscope(p);
        if (cp_isdeclstart(p, p->pos)) {
            n->d = cp_declaration(p, 2);
        } else {
            if (cp_kind(p) != CP_SEMI) {
                cp_node *e = cp_newnode(p, CP_S_EXPR, p->pos);
                e->a = cp_expr(p);
                cp_finish(p, e);
                n->d = e;
            }
            cp_expect(p, CP_SEMI, "expected ';' in for");
        }
        if (cp_kind(p) != CP_SEMI) n->a = cp_expr(p);
        cp_expect(p, CP_SEMI, "expected ';' in for");
        if (cp_kind(p) != CP_RPAREN) n->c = cp_expr(p);
        cp_expect(p, CP_RPAREN, "expected ')' after for");
        n->b = cp_stmt(p);
        cp_popscope(p);
        break;
    case CP_K_GOTO:
        cp_next(p);
        if (cp_accept(p, CP_STAR)) {
            n = cp_newnode(p, CP_S_GOTOPTR, tok);
            n->a = cp_expr(p);
        } else {
            n = cp_newnode(p, CP_S_GOTO, tok);
            if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected label after goto");
            n->text = cp_tok(p)->text;
            cp_next(p);
            cp_listpush(p->mem, &p->gototail, &p->ngotos, n);
        }
        cp_expect(p, CP_SEMI, "expected ';' after goto");
        break;
    case CP_K_CONTINUE:
        n = cp_newnode(p, CP_S_CONTINUE, tok);
        cp_next(p);
        cp_expect(p, CP_SEMI, "expected ';' after continue");
        break;
    case CP_K_BREAK:
        n = cp_newnode(p, CP_S_BREAK, tok);
        cp_next(p);
        cp_expect(p, CP_SEMI, "expected ';' after break");
        break;
    case CP_K_RETURN:
        n = cp_newnode(p, CP_S_RETURN, tok);
        cp_next(p);
        if (cp_kind(p) != CP_SEMI) n->a = cp_expr(p);
        cp_expect(p, CP_SEMI, "expected ';' after return");
        break;
    case CP_K_CASE:
        n = cp_newnode(p, CP_S_CASE, tok);
        cp_next(p);
        n->a = cp_condexpr(p);
        if (cp_accept(p, CP_ELLIPSIS)) n->b = cp_condexpr(p);
        cp_expect(p, CP_COLON, "expected ':' after case");
        if (cp_kind(p) != CP_RBRACE && !cp_isdeclstart(p, p->pos)) n->c = cp_stmt(p);
        break;
    case CP_K_DEFAULT:
        n = cp_newnode(p, CP_S_DEFAULT, tok);
        cp_next(p);
        cp_expect(p, CP_COLON, "expected ':' after default");
        if (cp_kind(p) != CP_RBRACE && !cp_isdeclstart(p, p->pos)) n->c = cp_stmt(p);
        break;
    case CP_K_ASM:
        if (cp_isgnuasm(p)) {
            n = cp_asmstmt(p);
            cp_expect(p, CP_SEMI, "expected ';' after asm");
        } else {
            n = cp_msasm(p);
        }
        break;
    case CP_K_TRY:
        n = cp_newnode(p, CP_S_TRY, tok);
        cp_next(p);
        n->a = cp_compound(p, 1);
        if (cp_kind(p) == CP_K_EXCEPT) {
            cp_node *h = cp_newnode(p, CP_S_EXCEPT, p->pos);
            cp_next(p);
            h->a = cp_condition(p);
            h->b = cp_compound(p, 1);
            cp_finish(p, h);
            n->b = h;
        } else if (cp_kind(p) == CP_K_FINALLY) {
            cp_node *h = cp_newnode(p, CP_S_FINALLY, p->pos);
            cp_next(p);
            h->b = cp_compound(p, 1);
            cp_finish(p, h);
            n->b = h;
        } else {
            cp_fail(p, "expected __except or __finally");
        }
        break;
    case CP_K_LEAVE:
        n = cp_newnode(p, CP_S_LEAVE, tok);
        cp_next(p);
        cp_expect(p, CP_SEMI, "expected ';' after __leave");
        break;
    case CP_IDENT:
        if (cp_peekk(p, 1) == CP_COLON) {
            n = cp_label(p);
            break;
        }
        // fallthrough
    default:
        if (cp_kind(p) == CP_K_KWATTR && cp_peekk(p, 1) == CP_LBRACE) {
            // a keyword on a block: SDCC's __critical { ... }
            cp_attr *at = 0;
            cp_attrappend(&at, cp_attrone(p));
            n = cp_compound(p, 1);
            n->attrs = at;
            n->tok = tok;
            break;
        }
        if (cp_isattrkw(p, p->pos) && cp_tok(p)->kind != CP_K_KWATTR) {
            i32 j = cp_skipattrsat(p, p->pos);
            if (p->toks[j].kind == CP_SEMI) {
                n = cp_newnode(p, CP_S_ATTR, tok);
                cp_attrs(p, &n->attrs);
                cp_next(p);
                break;
            }
            if (p->toks[j].kind != CP_IDENT || !cp_isdeclstart(p, j)) {
                // attributes on a statement: keep them, parse the statement
                cp_attr *at = 0;
                cp_attrs(p, &at);
                n = cp_stmt(p);
                if (n->kind == CP_S_LABEL) {
                    // "[[x]] L: s": printed before the label again
                    for (cp_attr *a = at; a; a = a->next) a->before = 1;
                }
                cp_attrappend(&at, n->attrs);
                n->attrs = at;
                n->tok = tok;
                break;
            }
        }
        n = cp_newnode(p, CP_S_EXPR, tok);
        n->a = cp_expr(p);
        cp_expect(p, CP_SEMI, "expected ';' after expression");
    }
    cp_finish(p, n);
    p->depth--;
    return n;
}

static cp_node *cp_blockitem(cp_parser *p)
{
    cp_applypragmas(p);
    if (cp_kind(p) == CP_IDENT && cp_peekk(p, 1) == CP_COLON) return cp_stmt(p);
    if (cp_kind(p) == CP_K_EXTENSION) {
        // __extension__ before a declaration or an expression
        i32 j = p->pos;
        while (p->toks[j].kind == CP_K_EXTENSION) j++;
        if (!cp_isdeclstart(p, j)) return cp_stmt(p);
    }
    if (cp_isdeclstart(p, p->pos)) {
        return cp_declaration(p, 1);
    }
    return cp_stmt(p);
}

static cp_node *cp_compound(cp_parser *p, b32 newscope)
{
    cp_node *n = cp_newnode(p, CP_S_COMPOUND, p->pos);
    cp_expect(p, CP_LBRACE, "expected '{'");
    cp_enter(p);
    if (newscope) cp_pushscope(p);
    cp_node head = {0};
    cp_node *tail = &head;
    while (cp_kind(p) != CP_RBRACE) {
        if (cp_kind(p) == CP_EOF) cp_fail(p, "expected '}'");
        tail = tail->next = cp_blockitem(p);
    }
    cp_applypragmas(p);
    cp_next(p);
    if (newscope) cp_popscope(p);
    p->depth--;
    n->list = head.next;
    cp_finish(p, n);
    return n;
}

// --- declarations

static cp_node *cp_staticassert(cp_parser *p)
{
    cp_node *n = cp_newnode(p, CP_D_STATICASSERT, p->pos);
    cp_next(p);
    cp_expect(p, CP_LPAREN, "expected '(' after static_assert");
    n->a = cp_condexpr(p);
    if (cp_accept(p, CP_COMMA)) {
        if (cp_kind(p) != CP_STRLIT) cp_fail(p, "expected string in static_assert");
        n->b = cp_strlit(p);
    }
    cp_expect(p, CP_RPAREN, "expected ')'");
    cp_expect(p, CP_SEMI, "expected ';' after static_assert");
    if (n->a->isconst == 1) {
        if (!n->a->cval) {
            cp_str m = S("static assertion failed");
            if (n->b) m = cp_concat(p->mem, cp_concat(p->mem, m, S(": ")), n->b->sval);
            cp_diag(p->diags, 0, n->tok, p->toks[n->tok].file, p->toks[n->tok].line, p->toks[n->tok].col, m);
        }
    } else {
        cp_warn(p, n->a->tok, S("static assertion expression is not an integer constant"));
    }
    cp_finish(p, n);
    return n;
}

static void cp_resolvegotos(cp_parser *p)
{
    for (cp_listnode *l = p->gotos; l; l = l->next) {
        cp_node *g = (cp_node *)l->item;
        cp_map *m = cp_upsert(&p->labels, g->text, 0);
        if (m && m->val) {
            cp_node *lab = (cp_node *)m->val;
            g->ref = lab;
            g->sym = lab->sym;
            if (lab->sym) {
                lab->sym->nrefs++;
                cp_addref(p, &lab->sym->refs, &lab->sym->lastref_, g, 0,
                          g->kind == CP_E_LABELADDR ? CP_RF_ADDR : CP_RF_READ);
                lab->sym->flags |= g->kind == CP_E_LABELADDR ? CP_YF_ADDRTAKEN : CP_YF_READ;
            }
        } else {
            cp_warn(p, g->tok, cp_concat(p->mem, S("undefined label "), g->text));
        }
    }
}

static cp_node *cp_funcdef(cp_parser *p, cp_node *decl, cp_node *d, i32 storage)
{
    cp_node *fn = cp_newnode(p, CP_D_FUNC, decl->tok);
    fn->flags = decl->flags;
    fn->type = decl->type;
    fn->attrs = decl->attrs;
    fn->list = d;
    cp_declare(p, d, storage, fn, 0);
    fn->sym = d->sym;
    fn->ty = d->ty;

    cp_scope *outer = p->scope;
    cp_scope *ps = p->fnscope;
    if (!ps) {
        cp_pushscope(p);
        ps = p->scope;
    }
    ps->parent = outer;
    ps->depth = outer->depth + 1;
    p->scope = ps;

    // K&R parameter declarations
    cp_node head = {0};
    cp_node *tail = &head;
    while (cp_kind(p) != CP_LBRACE) {
        if (cp_kind(p) == CP_EOF || !cp_isdeclstart(p, p->pos)) cp_fail(p, "expected function body");
        tail = tail->next = cp_declaration(p, 3);
    }
    fn->b = head.next;
    if (fn->b && d->ty && d->ty->kind == CP_T_FUNC && d->type && d->type->kind == CP_T_FUNC) {
        // rebuild a prototype-like argument list for K&R definitions
        cp_type *ft = d->ty;
        i32 n = 0;
        for (cp_node *x = d->type->params; x; x = x->next) n++;
        cp_type **args = cp_new(p->mem, n, cp_type *);
        i32 i = 0;
        for (cp_node *x = d->type->params; x; x = x->next, i++) {
            cp_sym *s = x->sym;
            cp_type *at = s && s->ty ? s->ty : p->bt[CP_BT_INT];
            if (at->kind == CP_T_ARRAY) at = cp_ptrto(p, at->base);
            else if (at->kind == CP_T_FUNC) at = cp_ptrto(p, at);
            args[i] = at;
        }
        ft->args = args;
        ft->nargs = n;
    }

    cp_sym *savefunc = p->curfunc;
    cp_map *savelabels = p->labels;
    cp_listnode *savegotos = p->gotos, **savegototail = p->gototail;
    i32 savengotos = p->ngotos;
    p->curfunc = d->sym;
    p->labels = 0;
    p->gotos = 0;
    p->gototail = &p->gotos;
    p->ngotos = 0;
    fn->a = cp_compound(p, 0);
    cp_resolvegotos(p);
    p->curfunc = savefunc;
    p->labels = savelabels;
    p->gotos = savegotos;
    p->gototail = savegototail ? savegototail : &p->gotos;
    p->ngotos = savengotos;
    p->scope = outer;
    cp_finish(p, fn);
    return fn;
}

// ctx: 0 file scope, 1 block, 2 for-init, 3 K&R parameter declarations
static cp_node *cp_declaration(cp_parser *p, i32 ctx)
{
    cp_applypragmas(p);
    if (cp_kind(p) == CP_K_STATIC_ASSERT) return cp_staticassert(p);
    i32 tok = p->pos;
    if (cp_kind(p) == CP_K_LABEL) {
        cp_node *n = cp_newnode(p, CP_S_LOCALLABEL, tok);
        cp_next(p);
        cp_node head = {0};
        cp_node *tail = &head;
        for (;;) {
            if (cp_kind(p) != CP_IDENT) cp_fail(p, "expected label name");
            cp_node *x = cp_newnode(p, CP_E_IDENT, p->pos);
            x->text = cp_tok(p)->text;
            cp_next(p);
            tail = tail->next = x;
            if (!cp_accept(p, CP_COMMA)) break;
        }
        cp_expect(p, CP_SEMI, "expected ';'");
        n->list = head.next;
        cp_finish(p, n);
        return n;
    }
    cp_node *decl = cp_newnode(p, CP_D_DECL, tok);
    cp_specs sp = cp_parsespecs(p);
    if (!sp.any) {
        // K&R implicit int at file scope: "main() { }", "*p;"
        b32 implicit = ctx == 0 && (cp_kind(p) == CP_IDENT || cp_kind(p) == CP_STAR ||
                                    cp_kind(p) == CP_LPAREN);
        if (!implicit) cp_fail(p, "expected declaration");
    }
    decl->flags = sp.storage;
    decl->type = sp.type;
    decl->attrs = sp.attrs;
    if (cp_accept(p, CP_SEMI)) {
        cp_finish(p, decl);
        return decl;
    }
    cp_node head = {0};
    cp_node *tail = &head;
    b32 first = 1;
    for (;;) {
        // attributes before a later declarator belong to it:
        // "static T a = {...}, __attribute__((deprecated)) b = {...};"
        cp_attr *lead = 0;
        if (!first) cp_attrs(p, &lead);
        cp_node *d = cp_newdeclarator(p, sp.type);
        cp_attrappend(&lead, d->attrs);
        d->attrs = lead;
        cp_scope *fnscope = p->fnscope;
        // trailing attributes and asm labels
        for (;;) {
            if (cp_kind(p) == CP_K_ASM && cp_peekk(p, 1) == CP_LPAREN) {
                cp_attrappend(&d->attrs, cp_attrone(p));
            } else if (!cp_trailattrs(p, &d->attrs)) {
                break;
            }
        }
        if (!d->text.len && d->type == sp.type) cp_fail(p, "expected declarator");
        cp_type *ty = cp_resolve(p, d->type);
        cp_attrinfo ai = {0};
        ai.mode = -1;
        cp_scanattrs(p, sp.attrs, &ai);
        cp_scanattrs(p, d->attrs, &ai);
        cp_scanabi(p, d->type, &ai);
        ty = cp_applytypeattrs(p, ty, &ai);
        if ((sp.storage & CP_SC_TYPEDEF) && ai.aligned) ty = cp_realign(p, ty, ai.aligned);
        d->ty = ty;
        b32 isfn = d->type && d->type->kind == CP_T_FUNC;
        if (first && isfn && ctx != 2 && ctx != 3 && !(sp.storage & CP_SC_TYPEDEF) &&
            (cp_kind(p) == CP_LBRACE ||
             ((d->type->flags & CP_TF_KR) && cp_isdeclstart(p, p->pos)))) {
            p->fnscope = fnscope;
            return cp_funcdef(p, decl, d, sp.storage);
        }
        if (ctx == 3) {
            cp_declare(p, d, sp.storage, 0, 1);
        } else {
            cp_declare(p, d, sp.storage, 0, 0);
        }
        if (cp_accept(p, CP_ASSIGN)) {
            d->a = cp_initializer(p);
            cp_type *nt = cp_completearray(p, d->ty, d->a);
            cp_type *prior = d->sym ? d->sym->ty : 0;
            if (d->ty && d->ty->kind == CP_T_ARRAY && d->ty->len < 0 && prior &&
                prior->kind == CP_T_ARRAY && prior->len >= 0) {
                // "extern int a[10]; int a[] = {1, 2};" is int[10]: the
                // composite with the earlier declaration
                nt = prior;
            }
            if ((sp.type->kind == CP_T_AUTOTYPE || (sp.type->kind == CP_T_IMPLICIT &&
                 (sp.storage & CP_SC_AUTO))) && d->type == sp.type && d->a->ty) {
                nt = cp_decay(p, d->a->ty);
            }
            if (nt != d->ty) {
                d->ty = nt;
                if (d->sym) d->sym->ty = nt;
            }
            if (d->sym && d->sym->kind != CP_SYM_TYPEDEF) {
                d->sym->flags |= CP_YF_WRITTEN | CP_YF_DEFINED;
                d->sym->def = d;
            }
            if ((sp.storage & CP_SC_CONSTEXPR) && d->a->isconst) {
                d->isconst = d->a->isconst;
                d->cval = d->a->cval;
                d->fval = d->a->fval;
            }
        }
        cp_finish(p, d);
        tail = tail->next = d;
        first = 0;
        if (!cp_accept(p, CP_COMMA)) break;
    }
    decl->list = head.next;
    cp_expect(p, CP_SEMI, "expected ';' after declaration");
    cp_finish(p, decl);
    return decl;
}

static cp_node *cp_external(cp_parser *p)
{
    i32 tok = p->pos;
    if (cp_kind(p) == CP_SEMI) {
        cp_node *n = cp_newnode(p, CP_D_EMPTY, tok);
        cp_next(p);
        return n;
    }
    if (cp_kind(p) == CP_K_ASM) {
        if (!cp_isgnuasm(p)) return cp_msasm(p);
        cp_node *n = cp_asmstmt(p);
        cp_expect(p, CP_SEMI, "expected ';' after asm");
        cp_finish(p, n);
        return n;
    }
    return cp_declaration(p, 0);  // includes K&R implicit int
}

// Skip to a likely declaration boundary after an error.
static i32 cp_sync(cp_parser *p, i32 start)
{
    i32 depth = 0;
    i32 i = start;
    for (; i < p->ntoks-1; i++) {
        i32 k = p->toks[i].kind;
        if (k == CP_LBRACE || k == CP_LPAREN || k == CP_LBRACK) depth++;
        else if (k == CP_RBRACE || k == CP_RPAREN || k == CP_RBRACK) {
            if (depth > 0) depth--;
            if (depth == 0 && k == CP_RBRACE) {
                cp_token *nx = &p->toks[i+1];
                if (nx->kind == CP_EOF || (nx->flags & CP_TF_BOL)) {
                    i++;
                    if (p->toks[i].kind == CP_SEMI) i++;
                    return i;
                }
            }
        } else if (k == CP_SEMI && depth == 0 && i >= p->pos) {
            return i + 1;
        }
    }
    return i > start ? i : start + 1;
}

static void cp_parseunit(cp_parser *p)
{
    jmp_buf jb;
    p->err = &jb;
    cp_scope *filescope = p->scope;
    for (;;) {
        i32 start = p->pos;
        if (setjmp(jb)) {
            p->scope = filescope;
            p->depth = 0;
            p->instruct = 0;
            p->inoffsetof = 0;
            p->unevaluated = 0;
            p->curfunc = 0;
            p->labels = 0;
            p->gotos = 0;
            p->gototail = &p->gotos;
            p->ngotos = 0;
            p->diags->quiet = 0;
            i32 stop = cp_sync(p, start);
            if (stop > p->ntoks-1) stop = p->ntoks-1;
            cp_node *e = cp_newnode(p, CP_D_ERROR, start);
            e->ival = stop;
            e->endtok = stop;
            if (p->dtail) p->dtail->next = e; else p->u->decls = e;
            p->dtail = e;
            p->pos = stop;
            continue;
        }
        cp_applypragmas(p);
        if (cp_kind(p) == CP_EOF) break;
        cp_node *n = cp_external(p);
        if (p->dtail) p->dtail->next = n; else p->u->decls = n;
        p->dtail = n;
    }
}

// --- driver

static const char cp_builtinpredefs[] =
    "#define __STDC__ 1\n"
    "#define __STDC_HOSTED__ 1\n"
    "#define __STDC_VERSION__ 201710L\n"
    "#define __STDC_EMBED_NOT_FOUND__ 0\n"
    "#define __STDC_EMBED_FOUND__ 1\n"
    "#define __STDC_EMBED_EMPTY__ 2\n";

static void cp_predeclare(cp_parser *p, char *name, cp_type *t)
{
    cp_sym *s = cp_new(p->mem, 1, cp_sym);
    s->name = cp_cstr(name);
    s->kind = CP_SYM_TYPEDEF;
    s->ty = t;
    s->flags = CP_YF_IMPLICIT | CP_YF_DEFINED;
    cp_bindname(p, s->name, s);
}

cp_unit *cp_parse(cp_arena *arena, cp_str src, cp_str filename, cp_config *cfg)
{
    jmp_buf oom;
    cp_mem mem = {arena, &oom};
    cp_unit  *volatile u = 0;
    cp_pp    *volatile ppv = 0;
    // Hold back a little memory to finish the unit after running out.
    volatile iz reserve = (arena->end - arena->beg) / 64;
    if (reserve > 1<<20) reserve = 1<<20;
    if (reserve < 4096) reserve = 0;
    arena->end -= reserve;

    if (setjmp(oom)) {
        arena->end += reserve;
        reserve = 0;
        if (!u) return 0;
        u->oom = 1;
        // finish what we can with the reserve; give up quietly if not
        if (setjmp(oom)) return u;
        goto finish;
    }

    u = cp_new(&mem, 1, cp_unit);
    u->target = cfg ? cfg->target : CP_TARGET_LP64;
    cp_diags *diags = cp_new(&mem, 1, cp_diags);
    diags->u = u;
    diags->mem = &mem;

    cp_map *kw = 0;
    for (iz i = 0; i < cp_countof(cp_keywords); i++) {
        cp_map *m = cp_upsert(&kw, cp_cstr(cp_keywords[i].name), &mem);
        m->ival = cp_keywords[i].kind;
    }

    // ---- preprocess
    cp_pp *pp = cp_new(&mem, 1, cp_pp);
    ppv = pp;
    pp->mem = &mem;
    pp->cfg = cfg;
    pp->u = u;
    pp->diags = diags;
    pp->kw = kw;
    pp->ls.mem = &mem;
    pp->ls.keepcomments = 1;
    pp->ls.ctail = &pp->ls.chead;
    pp->ls.internfile = cp_internfile;
    pp->ls.ctx = pp;
    pp->pragmatail = &pp->pragmas;
    pp->includetail = &pp->includes;
    pp->skiptail = &pp->skips;
    pp->exptail = &pp->exps;

    i32 mainfile = cp_addfile(pp, filename, src, -2, 0);
    cp_upsert(&pp->filemap, filename, &mem)->ival = 1 + mainfile;
    cp_str predefname = cp_cstr("<predefined>");
    cp_str predefsrc = cp_cstr("");
    if (cfg) predefsrc = cfg->predefs;
    i32 predeffile = cp_addfile(pp, predefname, predefsrc, -1, 1);

    if (!(cfg && cfg->nopp)) {
        cp_defbuiltin(pp, "__FILE__", CP_BI_FILE);
        cp_defbuiltin(pp, "__LINE__", CP_BI_LINE);
        cp_defbuiltin(pp, "__COUNTER__", CP_BI_COUNTER);
        cp_defbuiltin(pp, "__INCLUDE_LEVEL__", CP_BI_INCLUDE_LEVEL);
        cp_defbuiltin(pp, "__BASE_FILE__", CP_BI_BASE_FILE);
        cp_defbuiltin(pp, "__FILE_NAME__", CP_BI_FILE_NAME);
        cp_defbuiltin(pp, "__DATE__", CP_BI_DATE);
        cp_defbuiltin(pp, "__TIME__", CP_BI_TIME);
        cp_defbuiltin(pp, "__TIMESTAMP__", CP_BI_TIMESTAMP);
        static char *has[] = {
            "__has_include", "__has_include_next", "__has_attribute",
            "__has_c_attribute", "__has_cpp_attribute", "__has_builtin",
            "__has_feature", "__has_extension", "__has_embed",
        };
        for (iz i = 0; i < cp_countof(has); i++) cp_defbuiltin(pp, has[i], CP_BI_HAS);

        pp->predefs = 1;
        pp->ls.keepcomments = 0;
        cp_str bi = {(char *)cp_builtinpredefs, (iz)sizeof(cp_builtinpredefs)-1};
        cp_ppmain(pp, cp_lexfile(&pp->ls, bi, predeffile));
        if (cfg && cfg->predefs.len) {
            cp_ppmain(pp, cp_lexfile(&pp->ls, cfg->predefs, predeffile));
        }
        pp->predefs = 0;
        pp->ls.keepcomments = 1;
        pp->cond = 0;
    } else if (cfg->predefs.len) {
        // Already preprocessed: nothing is expanded, but the compiler's
        // macros still say what it targets (C version, long double), so
        // their definitions are read. Their #includes are already in.
        pp->predefs = 1;
        pp->noinclude = 1;
        pp->ls.keepcomments = 0;
        cp_ppmain(pp, cp_lexfile(&pp->ls, cfg->predefs, predeffile));
        pp->predefs = 0;
        pp->noinclude = 0;
        pp->ls.keepcomments = 1;
        pp->cond = 0;
    }
    pp->nopp = cfg && cfg->nopp;
    pp->depth = 1;
    cp_ppmain(pp, cp_lexfile(&pp->ls, pp->files[mainfile].src, mainfile));
    for (cp_cond *c = pp->cond; c; c = c->next) {
        cp_diag(diags, 0, -1, c->file, c->line, 1, cp_cstr("unterminated conditional directive"));
    }
    cp_sdcckw(pp, &kw);

    // ---- tokens
    {
        i32 n = pp->nout;
        cp_token *toks = cp_new(&mem, n+1, cp_token);
        i32 i = 0;
        cp_pptok *last = 0;
        for (cp_pptok *t = pp->outhead; t && i < n; t = t->next, i++) {
            cp_token *d = &toks[i];
            d->text = t->text;
            d->kind = t->kind == CP_IDENT ? cp_keyword(kw, t->text) : t->kind;
            d->flags = t->flags;
            d->line = t->line;
            d->col = t->col;
            d->file = t->file;
            d->exp = t->exp;
            d->macro = t->macro;
            d->doc = t->doc;
            if (t->kind == CP_OTHER && t->text.len && (t->text.data[0] == '"' || t->text.data[0] == '\'')) {
                // a raw token run (attribute, asm, error region) would
                // print it, and the stray quote would swallow what follows
                cp_diag(diags, 0, i, t->file, t->line, t->col, cp_cstr("missing terminating quote"));
            }
            last = t;
        }
        toks[n].kind = CP_EOF;
        toks[n].flags = CP_TF_BOL;
        toks[n].file = mainfile;
        toks[n].line = last ? last->line + 1 : 1;
        toks[n].text = cp_span(src.data + src.len, src.data + src.len);
        u->toks = toks;
        u->ntoks = n + 1;
    }

    // ---- side tables
    u->npragmas = pp->npragmas;
    u->pragmas = cp_new(&mem, pp->npragmas, cp_pragma);
    {
        i32 i = 0;
        for (cp_listnode *l = pp->pragmas; l; l = l->next) u->pragmas[i++] = *(cp_pragma *)l->item;
    }
    u->macros = pp->mhead;

    // ---- parse
    cp_parser *p = cp_new(&mem, 1, cp_parser);
    p->mem = &mem;
    p->u = u;
    p->diags = diags;
    p->toks = u->toks;
    p->ntoks = (i32)u->ntoks;
    p->target = u->target;
    p->gototail = &p->gotos;
    cp_inittypes(p);
    cp_pushscope(p);  // builtins
    {
        // __builtin_va_list
        cp_type *va;
        if (p->target == CP_TARGET_LP64) {
            // the x86-64 System V va_list, fields as GCC declares them
            cp_type *tag = cp_mktype(p, CP_T_STRUCT);
            tag->name = cp_cstr("__va_list_tag");
            tag->tok = -1;  // built in: no source, and no spelling of its own
            tag->size = 24;
            tag->align = 8;
            static const struct { char *name; i32 off; b32 ptr; } fields[] = {
                {"gp_offset", 0, 0}, {"fp_offset", 4, 0},
                {"overflow_arg_area", 8, 1}, {"reg_save_area", 16, 1},
            };
            cp_node **tail = &tag->members;
            for (iz i = 0; i < cp_countof(fields); i++) {
                cp_node *decl = cp_new(p->mem, 1, cp_node);
                cp_node *d = cp_new(p->mem, 1, cp_node);
                decl->kind = CP_D_DECL;
                decl->tok = d->tok = -1;
                decl->list = d;
                d->kind = CP_D_DECLARATOR;
                d->text = cp_cstr(fields[i].name);
                d->ty = fields[i].ptr ? p->voidptr : p->bt[CP_BT_UINT];
                d->offset = fields[i].off;
                d->bitoffset = fields[i].off * 8;
                d->bitwidth = -1;
                *tail = decl;
                tail = &decl->next;
            }
            va = cp_arrayof(p, tag, 1);
        } else {
            va = cp_ptrto(p, p->bt[CP_BT_CHAR]);
        }
        cp_predeclare(p, "__builtin_va_list", va);
        cp_predeclare(p, "__builtin_ms_va_list", cp_ptrto(p, p->bt[CP_BT_CHAR]));
        cp_predeclare(p, "__builtin_sysv_va_list", va);
        cp_predeclare(p, "__int128_t", p->bt[CP_BT_INT128]);
        cp_predeclare(p, "__uint128_t", p->bt[CP_BT_UINT128]);
        cp_predeclare(p, "bool", p->bt[CP_BT_BOOL]);
        cp_predeclare(p, "nullptr_t", p->voidptr);
    }
    cp_scope *builtinscope = p->scope;
    cp_pushscope(p);  // file scope
    p->scope->depth = 0;
    builtinscope->depth = 0;
    cp_parseunit(p);

finish:
    pp = ppv;
    if (pp) {
        u->nfiles = pp->nfiles;
        u->files = pp->files;
        u->macros = pp->mhead;
        if (!u->pragmas) {
            u->npragmas = 0;
        }
        u->nincludes = pp->nincludes;
        u->includes = cp_new(&mem, pp->nincludes, cp_include);
        i32 i = 0;
        for (cp_listnode *l = pp->includes; l; l = l->next) u->includes[i++] = *(cp_include *)l->item;
        u->nskips = pp->nskips;
        u->skips = cp_new(&mem, pp->nskips, cp_skip);
        i = 0;
        for (cp_listnode *l = pp->skips; l; l = l->next) u->skips[i++] = *(cp_skip *)l->item;
        u->nexpansions = pp->nexps;
        u->expansions = cp_new(&mem, pp->nexps, cp_expansion);
        i = 0;
        for (cp_listnode *l = pp->exps; l; l = l->next) u->expansions[i++] = *(cp_expansion *)l->item;
        u->ncomments = pp->ls.ncomments;
        u->comments = cp_new(&mem, pp->ls.ncomments, cp_comment);
        i = 0;
        for (struct cp_commentnode *c = pp->ls.chead; c; c = c->next) u->comments[i++] = c->c;
    }
    arena->end += reserve;
    return u;
}

// ----------------------------------------------------------------------
// Output: C regeneration, token stream, dumps

typedef struct {
    char    *buf;
    iz       len;
    iz       cap;
    b32      err;
    cp_unit *u;
    i32      indent;
    char     last;       // last byte written
    i32      lastnum;    // last token was a number
    i32      nextpragma;
    b32      check;      // emit type assertions (cp_print_checked)
    cp_attr *nameattrs;  // printed after the next declarator name
    i32      widthpass;  // 1: attributes before a bit-field width, 2: after
} cp_out;

static void cp_putc(cp_out *o, char c)
{
    if (o->len >= o->cap) { o->err = 1; return; }
    o->buf[o->len++] = c;
    o->last = c;
}

static void cp_puts(cp_out *o, cp_str s)
{
    if (s.len > o->cap - o->len) { o->err = 1; return; }
    for (iz i = 0; i < s.len; i++) o->buf[o->len++] = s.data[i];
    if (s.len) o->last = s.data[s.len-1];
    o->lastnum = 0;
}

static b32 cp_identc(char c) { return cp_isident((u8)c); }

static b32 cp_glues(char a, char b)
{
    static const char *pairs[] = {
        "++", "--", "+=", "-=", "->", "&&", "&=", "||", "|=", "<<", "<=",
        "<:", "<%", ">>", ">=", "==", "!=", "*=", "/=", "%=", "%>", "%:",
        "^=", "##", "..", ":>", "/*", "//",
    };
    for (iz i = 0; i < cp_countof(pairs); i++) {
        if (pairs[i][0] == a && pairs[i][1] == b) return 1;
    }
    return 0;
}

// Write a token, inserting a space if it would fuse with the last one.
static void cp_tk(cp_out *o, cp_str s)
{
    if (!s.len) return;
    char a = o->last, b = s.data[0];
    b32 space = 0;
    if (a && a != ' ' && a != '\n') {
        if (cp_identc(a) && (cp_identc(b) || b == '"' || b == '\'')) space = 1;
        else if (o->lastnum && (b == '.' || b == '+' || b == '-' || cp_identc(b))) space = 1;
        else if (cp_glues(a, b)) space = 1;
        else if (a == '.' && cp_isdigit((u8)b)) space = 1;
    }
    if (space) cp_putc(o, ' ');
    cp_puts(o, s);
    o->lastnum = cp_isdigit((u8)b) || (b == '.' && s.len > 1 && cp_isdigit((u8)s.data[1]));
}

static void cp_tkc(cp_out *o, char *s) { cp_tk(o, cp_cstr(s)); }
static void cp_sp(cp_out *o)
{
    char c = o->last;
    if (c != ' ' && c != '\n' && c != '(' && c != '[' && c) cp_putc(o, ' ');
}

static void cp_nl(cp_out *o)
{
    if (o->last != '\n' && o->len) cp_putc(o, '\n');
    o->lastnum = 0;
}

static void cp_ind(cp_out *o)
{
    for (i32 i = 0; i < o->indent; i++) cp_puts(o, S("    "));
    o->last = ' ';
}

static void cp_num(cp_out *o, i64 v)
{
    char buf[24];
    i32 n = 0;
    b32 neg = v < 0;
    u64 u = neg ? 0 - (u64)v : (u64)v;
    do { buf[n++] = (char)('0' + u%10); u /= 10; } while (u);
    if (neg) buf[n++] = '-';
    char out[24];
    for (i32 i = 0; i < n; i++) out[i] = buf[n-1-i];
    cp_tk(o, cp_span(out, out+n));
}

// Raw tokens [beg, end) with their original spacing.
static void cp_ptoks(cp_out *o, i32 beg, i32 end, b32 lines)
{
    cp_unit *u = o->u;
    for (i32 i = beg; i < end && i < u->ntoks; i++) {
        cp_token *t = &u->toks[i];
        if (t->kind == CP_EOF) break;
        if (i > beg && lines && (t->flags & CP_TF_BOL)) {
            cp_nl(o);
            cp_ind(o);
        } else if (i > beg && (t->flags & (CP_TF_SPACE|CP_TF_BOL))) {
            cp_sp(o);
        }
        cp_tk(o, t->text);
        if (t->kind == CP_OTHER && (t->text.data[0] == '"' || t->text.data[0] == '\'') &&
            i+1 < end && i+1 < u->ntoks && u->toks[i+1].kind != CP_EOF) {
            // an unterminated quote must stay last on its line, or it
            // would reopen as a literal swallowing what follows
            cp_nl(o);
            cp_ind(o);
        }
    }
}

static void cp_flushpragmas(cp_out *o, i32 tok)
{
    cp_unit *u = o->u;
    while (o->nextpragma < u->npragmas && u->pragmas[o->nextpragma].tok <= tok) {
        cp_pragma *p = &u->pragmas[o->nextpragma++];
        cp_nl(o);
        if (p->isop) {
            cp_puts(o, p->text);
        } else {
            cp_puts(o, S("#pragma "));
            cp_puts(o, p->text);
        }
        cp_putc(o, '\n');
        cp_ind(o);
    }
}

static void cp_pattrs(cp_out *o, cp_attr *a, i32 which)
{
    // which: 0 all, 1 only before-attrs, 2 only after-attrs, 3 std and
    // leading macros, 4 all but those and after-type, 5 only after-type, 6 the kinds a
    // definition may take among its specifiers, 7 the others, 8 all but
    // after-type
    for (; a; a = a->next) {
        if ((which == 9) != (a->aftername != 0)) continue;  // 9: after the name only
        if (o->widthpass && (o->widthpass == 2) != (a->afterwidth != 0)) continue;
        b32 movable = a->kind == CP_A_GNU || a->kind == CP_A_DECLSPEC || a->kind == CP_A_ALIGNAS;
        if (which == 6 && !movable) continue;
        if (which == 7 && movable) continue;
        if (which == 4 && a->aftertype) continue;
        if (which == 5 && !a->aftertype) continue;
        if (which == 8 && a->aftertype) continue;
        if (which == 1 && !a->before) continue;
        if (which == 2 && a->before) continue;
        if (which == 3 && ((a->kind != CP_A_STD && !a->leading) || a->aftertype)) continue;
        if (which == 4 && (a->kind == CP_A_STD || a->leading)) continue;
        cp_sp(o);
        cp_ptoks(o, a->beg, a->end, 0);
    }
}

static void cp_pquals(cp_out *o, i32 q)
{
    if (q & CP_Q_CONST)    { cp_sp(o); cp_tkc(o, "const"); }
    if (q & CP_Q_VOLATILE) { cp_sp(o); cp_tkc(o, "volatile"); }
    if (q & CP_Q_RESTRICT) { cp_sp(o); cp_tkc(o, "__restrict"); }
    if (q & CP_Q_ATOMIC)   { cp_sp(o); cp_tkc(o, "_Atomic"); }
}

static void cp_pbits(cp_out *o, i32 b)
{
    static const struct { i32 bit; char *name; } names[] = {
        {CP_B_SIGNED, "signed"}, {CP_B_UNSIGNED, "unsigned"},
        {CP_B_COMPLEX, "_Complex"}, {CP_B_IMAGINARY, "_Imaginary"},
        {CP_B_SHORT, "short"}, {CP_B_LONG, "long"}, {CP_B_LONGLONG, "long"},
        {CP_B_VOID, "void"}, {CP_B_BOOL, "_Bool"}, {CP_B_CHAR, "char"},
        {CP_B_INT, "int"}, {CP_B_FLOAT, "float"}, {CP_B_DOUBLE, "double"},
        {CP_B_INT128, "__int128"}, {CP_B_FLOAT128, "__float128"},
        {CP_B_FLOAT80, "__float80"}, {CP_B_IBM128, "__ibm128"},
        {CP_B_FLOAT16, "_Float16"}, {CP_B_FLOAT32, "_Float32"},
        {CP_B_FLOAT64, "_Float64"}, {CP_B_FLOAT32X, "_Float32x"},
        {CP_B_FLOAT64X, "_Float64x"}, {CP_B_FLOAT128X, "_Float128x"},
        {CP_B_BF16, "__bf16"}, {CP_B_FP16, "__fp16"},
        {CP_B_DEC32, "_Decimal32"}, {CP_B_DEC64, "_Decimal64"},
        {CP_B_DEC128, "_Decimal128"}, {CP_B_INT8, "__int8"},
        {CP_B_INT16, "__int16"}, {CP_B_INT32, "__int32"},
        {CP_B_INT64, "__int64"},
    };
    for (iz i = 0; i < cp_countof(names); i++) {
        if (b & names[i].bit) {
            cp_sp(o);
            cp_tkc(o, names[i].name);
        }
    }
}

static void cp_pexpr(cp_out *o, cp_node *n, i32 prec);
static void cp_pstmt(cp_out *o, cp_node *n);
static void cp_pdecl(cp_out *o, cp_node *n, b32 semi);
static void cp_ptypename(cp_out *o, cp_type *t, cp_type *stop);
static void cp_pdeclarator(cp_out *o, cp_type *t, cp_type *stop, cp_str name, b32 hasname);
static void cp_pbase(cp_out *o, cp_type *t);
static void cp_pinit(cp_out *o, cp_node *n);

static void cp_pmembers(cp_out *o, cp_node *m, b32 isenum)
{
    cp_tkc(o, "{");
    o->indent++;
    for (; m; m = m->next) {
        cp_flushpragmas(o, m->tok);
        cp_nl(o);
        cp_ind(o);
        if (isenum) {
            cp_tk(o, m->text);
            cp_pattrs(o, m->attrs, 0);
            if (m->a) {
                cp_puts(o, S(" = "));
                cp_pexpr(o, m->a, 3);
            }
            if (m->next) cp_tkc(o, ",");
        } else {
            cp_pdecl(o, m, 1);
        }
    }
    o->indent--;
    cp_nl(o);
    cp_ind(o);
    cp_tkc(o, "}");
}

static b32 cp_issemantic(cp_type *t) { return t && t->sem == t; }

static cp_str cp_basicname(i32 b)
{
    static const char *names[CP_NBASIC] = {
        "void", "_Bool", "char", "signed char", "unsigned char", "short",
        "unsigned short", "int", "unsigned int", "long", "unsigned long",
        "long long", "unsigned long long", "__int128", "unsigned __int128",
        "_Float16", "__bf16", "float", "double", "long double", "_Float32",
        "_Float64", "_Float32x", "_Float64x", "__float128", "__ibm128",
        "_Decimal32", "_Decimal64", "_Decimal128", "__typeof__(nullptr)",
    };
    return b >= 0 && b < CP_NBASIC ? cp_cstr((char *)names[b]) : S("?");
}

// Print a base (non-derived) type: syntactic specifiers or semantic.
static void cp_pbase(cp_out *o, cp_type *t)
{
    if (!t) { cp_sp(o); cp_tkc(o, "int"); return; }
    b32 sem = cp_issemantic(t);
    if (t->kind == CP_T_NAME && t->bits && !(t->bits & ~(CP_B_COMPLEX|CP_B_IMAGINARY))) {
        cp_pquals(o, t->quals);  // "_Complex T": the order that reparses
        cp_pbits(o, t->bits);
        cp_sp(o);
        cp_tk(o, t->name);
        return;
    }
    if (t->kind == CP_T_NAME && t->bits) {
        // odd mix like "foo const int": the name must lead to reparse
        cp_sp(o);
        cp_tk(o, t->name);
        cp_pquals(o, t->quals);
        cp_pbits(o, t->bits);
        return;
    }
    cp_pquals(o, t->quals);
    switch (t->kind) {
    case CP_T_BUILTIN:
    case CP_T_IMPLICIT:
        if (sem && t->kind == CP_T_BUILTIN) {
            if (t->flags & CP_TF_COMPLEX) { cp_sp(o); cp_tkc(o, "_Complex"); }
            cp_sp(o);
            cp_tk(o, cp_basicname(t->basic));
        } else if (t->flags & CP_TF_SDCCBIT) {
            cp_pbits(o, t->bits & ~CP_B_BOOL);
            cp_sp(o);
            cp_tkc(o, "__bit");
        } else {
            cp_pbits(o, t->bits);
        }
        break;
    case CP_T_NAME:
        cp_pbits(o, t->bits);
        cp_sp(o);
        cp_tk(o, t->name);
        break;
    case CP_T_STRUCT:
    case CP_T_UNION:
    case CP_T_ENUM:
        if (!sem) cp_pbits(o, t->bits);
        cp_sp(o);
        cp_tkc(o, t->kind == CP_T_STRUCT ? "struct" : t->kind == CP_T_UNION ? "union" : "enum");
        if (sem) {
            if (cp_equals(t->name, S("__va_list_tag")) && t->tok < 0) {
                // the built-in one has no spelling: reach it through va_list
                o->len -= 6;
                cp_puts(o, S("__typeof__(((__builtin_va_list *)0)[0][0])"));
                break;
            }
            cp_sp(o);
            if (t->name.len) cp_tk(o, t->name);
            else cp_tkc(o, "<anonymous>");
            break;
        }
        cp_pattrs(o, t->attrs, 0);
        if (t->name.len) { cp_sp(o); cp_tk(o, t->name); }
        if (t->kind == CP_T_ENUM && t->base) {
            cp_puts(o, S(" : "));
            cp_ptypename(o, t->base, 0);
        }
        if (t->flags & CP_TF_BODY) {
            cp_sp(o);
            cp_pmembers(o, t->members, t->kind == CP_T_ENUM);
        }
        break;
    case CP_T_TYPEOF:
        cp_sp(o);
        cp_tk(o, t->tok >= 0 && t->tok < o->u->ntoks ? o->u->toks[t->tok].text : S("typeof"));
        cp_tkc(o, "(");
        if (t->base) cp_ptypename(o, t->base, 0);
        else if (t->expr) cp_pexpr(o, t->expr, 1);
        cp_tkc(o, ")");
        break;
    case CP_T_ATOMIC:
        cp_sp(o);
        cp_tkc(o, "_Atomic");
        cp_tkc(o, "(");
        cp_ptypename(o, t->base, 0);
        cp_tkc(o, ")");
        break;
    case CP_T_BITINT:
        if (t->bits & CP_B_UNSIGNED) { cp_sp(o); cp_tkc(o, "unsigned"); }
        cp_sp(o);
        cp_tkc(o, "_BitInt");
        cp_tkc(o, "(");
        if (sem) cp_num(o, t->len);
        else if (t->expr) cp_pexpr(o, t->expr, 3);
        cp_tkc(o, ")");
        break;
    case CP_T_AUTOTYPE:
        cp_sp(o);
        cp_tkc(o, "__auto_type");
        break;
    case CP_T_VECTOR:
        cp_pbase(o, t->base);
        cp_puts(o, S(" __attribute__((vector_size("));
        cp_num(o, t->size);
        cp_puts(o, S(")))"));
        break;
    default:
        cp_sp(o);
        cp_tkc(o, "int");
    }
}

static b32 cp_isderived(cp_type *t, cp_type *stop)
{
    if (!t || t == stop) return 0;
    switch (t->kind) {
    case CP_T_POINTER: case CP_T_BLOCKPTR: case CP_T_ARRAY: case CP_T_FUNC:
        return 1;
    }
    return 0;
}

static cp_type *cp_innermost(cp_type *t, cp_type *stop)
{
    while (cp_isderived(t, stop)) t = t->base;
    return t;
}

static void cp_pprefix(cp_out *o, cp_type *t, cp_type *stop)
{
    if (!cp_isderived(t, stop)) return;
    cp_pprefix(o, t->base, stop);
    if (t->kind == CP_T_POINTER || t->kind == CP_T_BLOCKPTR) {
        b32 paren = cp_isderived(t->base, stop) &&
                    (t->base->kind == CP_T_ARRAY || t->base->kind == CP_T_FUNC);
        if (o->last != '*' && o->last != '^') cp_sp(o);
        if (paren) cp_tkc(o, "(");
        cp_pattrs(o, t->attrs, 1);
        cp_tkc(o, t->kind == CP_T_POINTER ? "*" : "^");
        cp_pattrs(o, t->attrs, 2);
        cp_pquals(o, t->quals);
    }
}

static void cp_pparams(cp_out *o, cp_type *t)
{
    cp_tkc(o, "(");
    if (cp_issemantic(t)) {
        for (i32 i = 0; i < t->nargs && !(t->flags & CP_TF_NOPROTO); i++) {
            if (i) cp_puts(o, S(", "));
            cp_ptypename(o, t->args[i], 0);
        }
        if (t->flags & CP_TF_VARIADIC) cp_puts(o, t->nargs ? S(", ...") : S("..."));
        if (!t->nargs && !(t->flags & (CP_TF_VARIADIC|CP_TF_NOPROTO))) cp_tkc(o, "void");
    } else {
        b32 first = 1;
        for (cp_node *p = t->params; p; p = p->next) {
            if (!first) cp_puts(o, S(", "));
            first = 0;
            if (p->kind == CP_E_IDENT) {
                cp_tk(o, p->text);
            } else {
                cp_pdecl(o, p, 0);
            }
        }
        if (t->flags & CP_TF_VARIADIC) cp_puts(o, first ? S("...") : S(", ..."));
    }
    cp_tkc(o, ")");
}

static void cp_psuffix(cp_out *o, cp_type *t, cp_type *stop)
{
    if (!cp_isderived(t, stop)) return;
    switch (t->kind) {
    case CP_T_POINTER:
    case CP_T_BLOCKPTR:
        if (cp_isderived(t->base, stop) && (t->base->kind == CP_T_ARRAY || t->base->kind == CP_T_FUNC)) {
            cp_tkc(o, ")");
        }
        break;
    case CP_T_ARRAY:
        cp_tkc(o, "[");
        if (cp_issemantic(t)) {
            if (t->len >= 0) cp_num(o, t->len);
        } else {
            if (t->flags & CP_TF_STATIC) { cp_tkc(o, "static"); }
            if (t->quals) { cp_pquals(o, t->quals); }
            if (t->flags & CP_TF_VLASTAR) { cp_tkc(o, "*"); }
            if (t->expr) {
                if (t->quals || (t->flags & CP_TF_STATIC)) cp_sp(o);
                cp_pexpr(o, t->expr, 2);
            }
        }
        cp_tkc(o, "]");
        break;
    case CP_T_FUNC:
        cp_pparams(o, t);
        break;
    }
    cp_psuffix(o, t->base, stop);
}

static void cp_pdeclarator(cp_out *o, cp_type *t, cp_type *stop, cp_str name, b32 hasname)
{
    cp_pprefix(o, t, stop);
    if (hasname && name.len) {
        if (o->last != '*' && o->last != '^') cp_sp(o);
        cp_tk(o, name);
        cp_pattrs(o, o->nameattrs, 9);
        o->nameattrs = 0;
    }
    cp_psuffix(o, t, stop);
}

static void cp_pattrs(cp_out *o, cp_attr *a, i32 which);
static void cp_ptypename(cp_out *o, cp_type *t, cp_type *stop);

// A type name as written in an expression, with its attributes:
// (__attribute__((vector_size(16))) int){...} is a vector.
static void cp_pexprtype(cp_out *o, cp_node *n)
{
    // attributes keep their side of the type: "int [[x]] *" is not
    // "[[x]] int *", which is no type name at all
    if (!n->type) { cp_tkc(o, "int"); return; }
    cp_pattrs(o, n->attrs, 8);
    cp_type *base = cp_innermost(n->type, 0);
    cp_pbase(o, base);
    cp_pattrs(o, n->attrs, 5);
    cp_pdeclarator(o, n->type, base, S(""), 0);
}

// A type name: base plus abstract declarator. stop is the base for
// syntactic chains, or null.
static void cp_ptypename(cp_out *o, cp_type *t, cp_type *stop)
{
    if (!t) { cp_tkc(o, "int"); return; }
    cp_type *base = stop ? stop : cp_innermost(t, 0);
    cp_pbase(o, base);
    cp_pdeclarator(o, t, base, S(""), 0);
}

static void cp_pstorage(cp_out *o, i32 f)
{
    static const struct { i32 bit; char *name; } names[] = {
        {CP_SC_EXTENSION, "__extension__"}, {CP_SC_TYPEDEF, "typedef"},
        {CP_SC_EXTERN, "extern"}, {CP_SC_STATIC, "static"},
        {CP_SC_AUTO, "auto"}, {CP_SC_REGISTER, "register"},
        {CP_SC_THREAD_LOCAL, "_Thread_local"}, {CP_SC_CONSTEXPR, "constexpr"},
        {CP_SC_INLINE, "inline"}, {CP_SC_FORCEINLINE, "__forceinline"},
        {CP_SC_NORETURN, "_Noreturn"},
    };
    for (iz i = 0; i < cp_countof(names); i++) {
        if (f & names[i].bit) {
            cp_sp(o);
            cp_tkc(o, names[i].name);
        }
    }
}

static void cp_pspecs2(cp_out *o, i32 storage, cp_attr *attrs, cp_attr *more, cp_type *base)
{
    cp_pattrs(o, attrs, 3);
    cp_pstorage(o, storage);
    cp_pattrs(o, attrs, 4);
    cp_pattrs(o, more, 6);
    cp_pbase(o, base);
    cp_pattrs(o, attrs, 5);
}

static void cp_pspecs(cp_out *o, i32 storage, cp_attr *attrs, cp_type *base)
{
    cp_pspecs2(o, storage, attrs, 0, base);
}

static void cp_pdeclarator1(cp_out *o, cp_node *d, cp_type *base, b32 moveattrs)
{
    o->nameattrs = d->attrs;
    cp_pdeclarator(o, d->type, base, d->text, 1);
    o->nameattrs = 0;
    if (d->b) {
        // annotations stay on their side of the width: in "unsigned long
        // FOO x : 3" FOO is the name and x an annotation
        o->widthpass = 1;
        cp_pattrs(o, d->attrs, moveattrs ? 7 : 0);
        cp_puts(o, S(" : "));
        cp_pexpr(o, d->b, 3);
        o->widthpass = 2;
    }
    cp_pattrs(o, d->attrs, moveattrs ? 7 : 0);
    o->widthpass = 0;
    if (d->a) {
        cp_puts(o, S(" = "));
        cp_pinit(o, d->a);
    }
}

static void cp_pdecl(cp_out *o, cp_node *n, b32 semi)
{
    if (n->kind == CP_D_STATICASSERT) {
        cp_tk(o, o->u->toks[n->tok].text);
        cp_tkc(o, "(");
        cp_pexpr(o, n->a, 3);
        if (n->b) {
            cp_puts(o, S(", "));
            cp_pexpr(o, n->b, 2);
        }
        cp_tkc(o, ")");
        if (semi) cp_tkc(o, ";");
        return;
    }
    if (n->kind == CP_S_LOCALLABEL) {
        cp_tkc(o, "__label__");
        for (cp_node *x = n->list; x; x = x->next) {
            cp_sp(o);
            cp_tk(o, x->text);
            if (x->next) cp_tkc(o, ",");
        }
        cp_tkc(o, ";");
        return;
    }
    iz save = o->len;
    cp_pspecs(o, n->flags, n->attrs, n->type);
    // drop a leading space
    if (o->len > save && o->buf[save] == ' ') {
        for (iz i = save; i+1 < o->len; i++) o->buf[i] = o->buf[i+1];
        o->len--;
    }
    for (cp_node *d = n->list; d; d = d->next) {
        cp_pdeclarator1(o, d, n->type, 0);
        if (d->next) cp_tkc(o, ",");
    }
    if (semi) cp_tkc(o, ";");
}

static i32 cp_exprprec(cp_node *n)
{
    switch (n->kind) {
    case CP_E_COMMA: return 1;
    case CP_E_ASSIGN: return 2;
    case CP_E_COND: return 3;
    case CP_E_BINARY: return cp_binprec(n->op);
    case CP_E_CAST: case CP_E_UNARY: case CP_E_SIZEOF: case CP_E_ALIGNOF:
    case CP_E_LABELADDR: case CP_E_BLOCK:
        return 14;
    case CP_E_CALL: case CP_E_INDEX: case CP_E_MEMBER: case CP_E_POSTFIX:
    case CP_E_COMPOUND:
        return 15;
    }
    return 16;
}

static void cp_pinitlist(cp_out *o, cp_node *n)
{
    cp_tkc(o, "{");
    b32 first = 1;
    for (cp_node *it = n->list; it; it = it->next) {
        if (!first) cp_tkc(o, ",");
        cp_sp(o);
        first = 0;
        b32 gnufield = it->b && it->b->kind == CP_E_DFIELD && it->b->flags == 1;
        for (cp_node *d = it->b; d; d = d->next) {
            if (d->kind == CP_E_DFIELD) {
                if (gnufield) {
                    cp_tk(o, d->text);
                    cp_tkc(o, ":");
                    cp_sp(o);
                } else {
                    cp_tkc(o, ".");
                    cp_tk(o, d->text);
                }
            } else {
                cp_tkc(o, "[");
                cp_pexpr(o, d->a, 3);
                if (d->b) {
                    cp_puts(o, S(" ... "));
                    cp_pexpr(o, d->b, 3);
                }
                cp_tkc(o, "]");
            }
        }
        if (it->b && !gnufield) cp_puts(o, S(" = "));
        cp_pinit(o, it->a);
    }
    if (!first) cp_sp(o);
    cp_tkc(o, "}");
}

static void cp_pinit(cp_out *o, cp_node *n)
{
    if (!n) return;
    if (n->kind == CP_E_INITLIST) cp_pinitlist(o, n);
    else cp_pexpr(o, n, 2);
}

static cp_str cp_opname(i32 op);

static void cp_pexpr(cp_out *o, cp_node *n, i32 prec)
{
    if (!n) return;
    i32 my = cp_exprprec(n);
    b32 paren = my < prec;
    if (paren) cp_tkc(o, "(");
    switch (n->kind) {
    case CP_E_IDENT: case CP_E_INT: case CP_E_FLOAT: case CP_E_CHAR:
        cp_tk(o, n->text);
        break;
    case CP_E_STRING:
        cp_ptoks(o, n->tok, n->tok + (i32)n->ival, 0);
        break;
    case CP_E_CALL:
        cp_pexpr(o, n->a, 15);
        cp_tkc(o, "(");
        for (cp_node *x = n->list; x; x = x->next) {
            if (x->kind == CP_E_TYPEARG) cp_pexprtype(o, x);
            else cp_pexpr(o, x, 2);
            if (x->next) cp_puts(o, S(", "));
        }
        cp_tkc(o, ")");
        break;
    case CP_E_TYPEARG: {
        cp_pexprtype(o, n);
        break;
    }
    case CP_E_INDEX:
        cp_pexpr(o, n->a, 15);
        cp_tkc(o, "[");
        cp_pexpr(o, n->b, 1);
        cp_tkc(o, "]");
        break;
    case CP_E_MEMBER:
        cp_pexpr(o, n->a, 15);
        cp_tkc(o, n->op == CP_DOT ? "." : "->");
        cp_tk(o, n->text);
        break;
    case CP_E_POSTFIX:
        cp_pexpr(o, n->a, 15);
        cp_tkc(o, n->op == CP_INC ? "++" : "--");
        break;
    case CP_E_UNARY:
        if (n->op == CP_K_REAL) { cp_tkc(o, "__real__"); cp_sp(o); }
        else if (n->op == CP_K_IMAG) { cp_tkc(o, "__imag__"); cp_sp(o); }
        else if (n->op == CP_K_EXTENSION) { cp_tkc(o, "__extension__"); cp_sp(o); }
        else cp_tk(o, cp_opname(n->op));
        cp_pexpr(o, n->a, n->op == CP_INC || n->op == CP_DEC ? 14 : 14);
        break;
    case CP_E_LABELADDR:
        cp_tkc(o, "&&");
        cp_tk(o, n->text);
        break;
    case CP_E_SIZEOF:
    case CP_E_ALIGNOF:
        if (n->kind == CP_E_SIZEOF) cp_tkc(o, "sizeof");
        else cp_tk(o, o->u->toks[n->tok].text);
        cp_tkc(o, "(");
        if (n->a) cp_pexpr(o, n->a, 1);
        else cp_pexprtype(o, n);
        cp_tkc(o, ")");
        break;
    case CP_E_CAST:
        cp_tkc(o, "(");
        cp_pexprtype(o, n);
        cp_tkc(o, ")");
        cp_pexpr(o, n->a, 14);
        break;
    case CP_E_COMPOUND:
        cp_tkc(o, "(");
        cp_pexprtype(o, n);
        cp_tkc(o, ")");
        cp_pinitlist(o, n->a);
        break;
    case CP_E_BINARY: {
        i32 p = cp_binprec(n->op);
        cp_pexpr(o, n->a, p);
        cp_sp(o);
        cp_tk(o, cp_opname(n->op));
        cp_sp(o);
        cp_pexpr(o, n->b, p+1);
    } break;
    case CP_E_ASSIGN:
        cp_pexpr(o, n->a, 14);
        cp_sp(o);
        cp_tk(o, cp_opname(n->op));
        cp_sp(o);
        cp_pexpr(o, n->b, 2);
        break;
    case CP_E_COND:
        cp_pexpr(o, n->a, 4);
        cp_puts(o, S(" ?"));
        if (n->b) {
            cp_sp(o);
            cp_pexpr(o, n->b, 1);
            cp_sp(o);
        }
        cp_tkc(o, ":");
        cp_sp(o);
        cp_pexpr(o, n->c, 3);
        break;
    case CP_E_COMMA:
        cp_pexpr(o, n->a, 1);
        cp_puts(o, S(", "));
        cp_pexpr(o, n->b, 2);
        break;
    case CP_E_GENERIC:
        cp_tkc(o, "_Generic");
        cp_tkc(o, "(");
        if (n->a->kind == CP_E_TYPEARG) cp_pexprtype(o, n->a);
        else cp_pexpr(o, n->a, 2);
        for (cp_node *as = n->list; as; as = as->next) {
            cp_puts(o, S(", "));
            if (as->type) cp_ptypename(o, as->type, 0);
            else cp_tkc(o, "default");
            cp_puts(o, S(": "));
            cp_pexpr(o, as->a, 2);
        }
        cp_tkc(o, ")");
        break;
    case CP_E_STMTEXPR: {
        b32 check = o->check;
        o->check = 0;  // keep the last statement's value
        cp_tkc(o, "(");
        cp_pstmt(o, n->a);
        cp_tkc(o, ")");
        o->check = check;
    } break;
    case CP_E_INITLIST:
        cp_pinitlist(o, n);
        break;
    case CP_E_BLOCK: {
        cp_tkc(o, "^");
        if (n->ival == 1 && n->type) {
            cp_pparams(o, n->type);
        } else if (n->ival == 2 && n->b) {
            cp_sp(o);
            cp_pexprtype(o, n->b);
        }
        cp_sp(o);
        cp_pstmt(o, n->a);
    } break;
    default:
        cp_tkc(o, "/*?*/");
    }
    if (paren) cp_tkc(o, ")");
}

// Clang's library builtins: functions its headers declare but whose type
// it will not let typeof take ("builtin functions must be directly called").
static b32 cp_tcclangbuiltin(cp_unit *u, cp_str name)
{
    static const char *const names[] = {
        "_mm_getcsr", "_mm_setcsr", "_mm_sfence", "_mm_lfence", "_mm_mfence",
        "_mm_pause", "_mm_clflush", "_mm_prefetch", "__rdtsc",
    };
    b32 listed = 0;
    for (iz i = 0; i < cp_countof(names); i++) listed |= cp_equals(name, cp_cstr((char *)names[i]));
    if (!listed) return 0;
    for (cp_macro *m = u->macros; m; m = m->next) {
        if (cp_equals(m->name, S("__clang__"))) return 1;
    }
    return 0;
}

// Does a type contain a function reached through a noreturn pointer?
// GCC types those differently from Clang and the standard, so --typecheck
// asserts nothing about them.
// Is there a variable length array anywhere along the type?
static b32 cp_tcvla(cp_type *t)
{
    for (i32 depth = 0; t && depth < 32; t = t->base, depth++) {
        if (t->flags & CP_TF_VLA) return 1;
    }
    return 0;
}

// Does a type mention a tag declared only in a parameter list?
static b32 cp_tcprototag(cp_type *t, i32 depth)
{
    for (; t && depth < 32; t = t->base, depth++) {
        if ((t->unqual ? t->unqual : t)->flags & CP_TF_PROTOTAG) return 1;
        if (t->kind != CP_T_FUNC) continue;
        for (i32 i = 0; i < t->nargs; i++) {
            if (cp_tcprototag(t->args[i], depth+1)) return 1;
        }
    }
    return 0;
}

static b32 cp_tcnoreturn(cp_type *t, i32 depth)  // also ms_abi/sysv_abi
{
    for (; t && depth < 32; t = t->base, depth++) {
        if (t->kind == CP_T_FUNC) {
            if (t->flags & (CP_TF_NORETURN|CP_TF_ABI)) return 1;
            for (i32 i = 0; i < t->nargs; i++) {
                if (cp_tcnoreturn(t->args[i], depth+1)) return 1;
            }
        }
    }
    return 0;
}

static b32 cp_tcskip(cp_node *e)
{
    switch (e->kind) {
    case CP_E_STMTEXPR: case CP_E_BLOCK: case CP_E_TYPEARG: case CP_E_INITLIST:
    case CP_E_INIT: case CP_E_DFIELD: case CP_E_DINDEX: case CP_E_ASSOC:
    case CP_E_LABELADDR:
        return 1;
    }
    return 0;
}

static b32 cp_tcunsafe(cp_node *e, i32 depth);

// Does a written type define a struct, union or enum ("enum {A}")? Then
// an expression holding it cannot be repeated: the second copy would
// declare it again. Or is it typeof an expression that cannot be checked
// (a wide bit-field, whose GCC type C cannot spell)?
static b32 cp_tcdefines(cp_type *t, i32 depth)
{
    for (; t && depth < 32; t = t->base, depth++) {
        if ((t->kind == CP_T_STRUCT || t->kind == CP_T_UNION || t->kind == CP_T_ENUM) && t->members) return 1;
        if (t->kind == CP_T_TYPEOF && t->expr && cp_tcunsafe(t->expr, depth+1)) return 1;
        if (t->kind != CP_T_FUNC) continue;
        for (cp_node *prm = t->params; prm; prm = prm->next) {
            if (cp_tcdefines(prm->type, depth+1)) return 1;
        }
    }
    return 0;
}

static b32 cp_tcunsafe(cp_node *e, i32 depth)
{
    // Expressions that must not be duplicated into typeof, or whose GCC
    // types are internal (wide bit-fields).
    if (!e || depth > 200) return e != 0;
    if (e->kind == CP_E_STMTEXPR || e->kind == CP_E_BLOCK) return 1;
    if (e->type && cp_tcdefines(e->type, 0)) return 1;
    if (e->kind == CP_E_MEMBER && e->ref && e->ref->bitwidth > 32) return 1;
    if ((e->kind == CP_E_ASSIGN || e->kind == CP_E_POSTFIX ||
         (e->kind == CP_E_UNARY && (e->op == CP_INC || e->op == CP_DEC))) &&
        e->a && e->a->kind == CP_E_MEMBER && e->a->ref && e->a->ref->bitwidth >= 0) {
        return 1;  // GCC's internal bit-field types
    }
    if (e->kind == CP_E_INITLIST) {
        for (cp_node *it = e->list; it; it = it->next) {
            if (cp_tcunsafe(it->a, depth+1)) return 1;
        }
        return 0;
    }
    if (cp_tcunsafe(e->a, depth+1) || cp_tcunsafe(e->b, depth+1) || cp_tcunsafe(e->c, depth+1)) return 1;
    if (e->kind == CP_E_CALL || e->kind == CP_E_GENERIC) {
        for (cp_node *x = e->list; x; x = x->next) {
            if (x->kind == CP_E_TYPEARG ? x->type && cp_tcdefines(x->type, 0) :
                cp_tcunsafe(x->kind == CP_E_ASSOC ? x->a : x, depth+1)) return 1;
        }
    }
    return 0;
}

static void cp_tcexpr(cp_out *o, cp_node *e, i32 depth)
{
    if (!e || depth > 200) return;
    if (e->kind == CP_E_STMTEXPR || e->kind == CP_E_BLOCK) return;
    if (e->kind == CP_E_INITLIST) {
        for (cp_node *it = e->list; it; it = it->next) cp_tcexpr(o, it->a, depth+1);
        return;
    }
    if (e->kind == CP_E_GENERIC) {
        if (e->ref) cp_tcexpr(o, e->ref->a, depth+1);
    } else if (e->kind == CP_E_CALL && e->a && e->a->kind == CP_E_IDENT &&
               cp_startswith(e->a->text, S("__builtin_"))) {
        // builtin arguments may be types or designators
    } else {
        cp_tcexpr(o, e->a, depth+1);
        cp_tcexpr(o, e->b, depth+1);
        cp_tcexpr(o, e->c, depth+1);
        if (e->kind == CP_E_CALL) {
            for (cp_node *x = e->list; x; x = x->next) cp_tcexpr(o, x, depth+1);
        }
    }
    if (cp_tcskip(e) || !e->ty) return;
    if (cp_tcunsafe(e, 0)) return;
    if (e->kind == CP_E_MEMBER && e->ref && e->ref->bitwidth >= 0) return;
    if (e->kind == CP_E_IDENT && e->sym && (e->sym->flags & CP_YF_IMPLICIT)) return;
    if (e->kind == CP_E_IDENT && cp_tcclangbuiltin(o->u, e->text)) return;
    if (cp_tcnoreturn(e->ty, 0)) return;
    if (cp_tcprototag(e->ty, 0)) return;
    if (cp_tcvla(e->ty)) return;
    if ((e->kind == CP_E_ASSIGN || e->kind == CP_E_POSTFIX ||
         (e->kind == CP_E_UNARY && (e->op == CP_INC || e->op == CP_DEC))) &&
        e->a && e->a->kind == CP_E_MEMBER && e->a->ref && e->a->ref->bitwidth >= 0) {
        return;  // GCC gives these its internal bit-field type
    }
    if (e->kind == CP_E_IDENT && e->a == 0 && !e->sym && !e->ty) return;
    iz mark = o->len;
    cp_nl(o);
    cp_ind(o);
    cp_puts(o, S("_Static_assert(__builtin_types_compatible_p(__typeof__("));
    cp_pexpr(o, e, 1);
    cp_puts(o, S("), "));
    iz tbeg = o->len;
    cp_ptypename(o, e->ty, 0);
    for (iz i = tbeg; i < o->len; i++) {
        if (o->buf[i] == '<') { o->len = mark; return; }  // anonymous tag
    }
    cp_puts(o, S("), \"type at line "));
    cp_num(o, e->tok >= 0 && e->tok < o->u->ntoks ? o->u->toks[e->tok].line : 0);
    cp_puts(o, S("\");"));
    if (e->isconst == 1 && cp_isinteger(e->ty)) {
        cp_nl(o);
        cp_ind(o);
        cp_puts(o, S("_Static_assert(__builtin_choose_expr(__builtin_constant_p("));
        cp_pexpr(o, e, 1);
        cp_puts(o, S("), ("));
        cp_pexpr(o, e, 1);
        cp_puts(o, S(") == ("));
        cp_ptypename(o, e->ty, 0);
        cp_puts(o, S(")"));
        cp_num(o, e->cval);
        cp_puts(o, S("LL, 1), \"value at line "));
        cp_num(o, e->tok >= 0 && e->tok < o->u->ntoks ? o->u->toks[e->tok].line : 0);
        cp_puts(o, S("\");"));
    }
}

static void cp_tcdecl(cp_out *o, cp_node *n)
{
    if (n->kind != CP_D_DECL) return;
    for (cp_node *d = n->list; d; d = d->next) {
        if (d->a) cp_tcexpr(o, d->a, 0);
        if (!d->text.len || !d->ty || (n->flags & CP_SC_TYPEDEF) || d->bitwidth >= 0) continue;
        if (cp_tcclangbuiltin(o->u, d->text)) continue;
        if (cp_tcnoreturn(d->ty, 0)) continue;  // GCC's own variant type
        if (cp_tcprototag(d->ty, 0)) continue;  // its tag cannot be named
        if (cp_tcvla(d->ty)) continue;  // "int[n][m]" has no spelling here
        iz mark = o->len;
        cp_nl(o);
        cp_ind(o);
        cp_puts(o, S("_Static_assert(__builtin_types_compatible_p(__typeof__("));
        cp_tk(o, d->text);
        cp_puts(o, S("), "));
        iz tbeg = o->len;
        cp_ptypename(o, d->ty, 0);
        for (iz i = tbeg; i < o->len; i++) {
            if (o->buf[i] == '<') { o->len = mark; goto next; }
        }
        cp_puts(o, S("), \"declared type of "));
        cp_puts(o, d->text);
        cp_puts(o, S("\");"));
    next:;
    }
}

// Up to which token the pragmas before statement s are printed ahead of
// it. A --typecheck wrapper "{ loop; asserts }" takes the loop's own
// pragmas inside, or the brace would part a loop pragma from its loop.
static i32 cp_pragmasbefore(cp_out *o, cp_node *s)
{
    b32 wrapped = o->check && (s->kind == CP_S_WHILE || s->kind == CP_S_DO);
    return wrapped ? s->tok - 1 : s->tok;
}

// A statement on a line of its own, after the pragmas that come before it
// (a loop pragma must stay right before its loop, even under a label).
static void cp_pnested(cp_out *o, cp_node *s)
{
    if (s) cp_flushpragmas(o, cp_pragmasbefore(o, s));
    cp_nl(o);
    cp_ind(o);
    cp_pstmt(o, s);
}

static void cp_pbody(cp_out *o, cp_node *s)
{
    // statement after if/while/for: compound on the same line
    if (s && s->kind == CP_S_COMPOUND && !s->attrs) {
        cp_sp(o);
        cp_pstmt(o, s);
    } else {
        o->indent++;
        cp_pnested(o, s);
        o->indent--;
    }
}

static void cp_pstmt1(cp_out *o, cp_node *n);

static void cp_pstmt(cp_out *o, cp_node *n)
{
    if (!o->check || !n) { cp_pstmt1(o, n); return; }
    switch (n->kind) {
    case CP_S_EXPR: case CP_S_RETURN: case CP_S_IF: case CP_S_SWITCH:
    case CP_S_WHILE: case CP_S_DO:
        cp_tkc(o, "{");
        cp_sp(o);
        cp_flushpragmas(o, n->tok);  // a loop's pragmas, inside the wrapper
        cp_pstmt1(o, n);
        o->indent++;
        if (n->kind == CP_S_EXPR || n->kind == CP_S_RETURN) cp_tcexpr(o, n->a, 0);
        else cp_tcexpr(o, n->a, 0);
        o->indent--;
        cp_nl(o);
        cp_ind(o);
        cp_tkc(o, "}");
        return;
    case CP_D_DECL:
        cp_pstmt1(o, n);
        cp_tcdecl(o, n);
        return;
    }
    cp_pstmt1(o, n);
}

static void cp_pstmt1(cp_out *o, cp_node *n)
{
    if (!n) { cp_tkc(o, ";"); return; }
    if (n->attrs && n->kind != CP_S_ATTR && n->kind != CP_S_LABEL && n->kind != CP_D_DECL &&
        n->kind != CP_D_FUNC && n->kind != CP_D_STATICASSERT) {
        cp_pattrs(o, n->attrs, 0);
        cp_sp(o);
    }
    switch (n->kind) {
    case CP_S_COMPOUND:
        cp_tkc(o, "{");
        o->indent++;
        for (cp_node *s = n->list; s; s = s->next) {
            cp_flushpragmas(o, cp_pragmasbefore(o, s));
            cp_nl(o);
            cp_ind(o);
            cp_pstmt(o, s);
        }
        cp_flushpragmas(o, n->endtok - 1);
        o->indent--;
        cp_nl(o);
        cp_ind(o);
        cp_tkc(o, "}");
        break;
    case CP_S_EXPR:
        cp_pexpr(o, n->a, 1);
        cp_tkc(o, ";");
        break;
    case CP_S_NULL:
        cp_tkc(o, ";");
        break;
    case CP_S_IF:
        cp_tkc(o, "if");
        cp_puts(o, S(" ("));
        cp_pexpr(o, n->a, 1);
        cp_tkc(o, ")");
        cp_pbody(o, n->b);
        if (n->c) {
            if (n->b && n->b->kind == CP_S_COMPOUND && !n->b->attrs) {
                cp_sp(o);
            } else {
                cp_nl(o);
                cp_ind(o);
            }
            cp_tkc(o, "else");
            if (n->c->kind == CP_S_IF && !n->c->attrs) {
                cp_sp(o);
                cp_pstmt(o, n->c);
            } else {
                cp_pbody(o, n->c);
            }
        }
        break;
    case CP_S_SWITCH:
    case CP_S_WHILE:
        cp_tkc(o, n->kind == CP_S_SWITCH ? "switch" : "while");
        cp_puts(o, S(" ("));
        cp_pexpr(o, n->a, 1);
        cp_tkc(o, ")");
        cp_pbody(o, n->b);
        break;
    case CP_S_DO:
        cp_tkc(o, "do");
        cp_pbody(o, n->b);
        if (n->b && n->b->kind == CP_S_COMPOUND) cp_sp(o);
        else { cp_nl(o); cp_ind(o); }
        cp_tkc(o, "while");
        cp_puts(o, S(" ("));
        cp_pexpr(o, n->a, 1);
        cp_puts(o, S(");"));
        break;
    case CP_S_FOR:
        cp_tkc(o, "for");
        cp_puts(o, S(" ("));
        if (n->d && n->d->kind == CP_S_EXPR) {
            cp_pexpr(o, n->d->a, 1);
            cp_tkc(o, ";");
        } else if (n->d) {
            cp_pdecl(o, n->d, 1);
        } else {
            cp_tkc(o, ";");
        }
        if (n->a) { cp_sp(o); cp_pexpr(o, n->a, 1); }
        cp_tkc(o, ";");
        if (n->c) { cp_sp(o); cp_pexpr(o, n->c, 1); }
        cp_tkc(o, ")");
        cp_pbody(o, n->b);
        break;
    case CP_S_GOTO:
        cp_tkc(o, "goto");
        cp_sp(o);
        cp_tk(o, n->text);
        cp_tkc(o, ";");
        break;
    case CP_S_GOTOPTR:
        cp_tkc(o, "goto");
        cp_puts(o, S(" *"));
        cp_pexpr(o, n->a, 14);
        cp_tkc(o, ";");
        break;
    case CP_S_CONTINUE: cp_tkc(o, "continue;"); break;
    case CP_S_BREAK:    cp_tkc(o, "break;"); break;
    case CP_S_LEAVE:    cp_tkc(o, "__leave;"); break;
    case CP_S_RETURN:
        cp_tkc(o, "return");
        if (n->a) { cp_sp(o); cp_pexpr(o, n->a, 1); }
        cp_tkc(o, ";");
        break;
    case CP_S_LABEL:
    {
        b32 lead = 0, trail = 0;
        for (cp_attr *a = n->attrs; a; a = a->next) {
            lead |= a->before;
            trail |= !a->before;
        }
        if (lead) {
            cp_pattrs(o, n->attrs, 1);
            cp_sp(o);
        }
        cp_tk(o, n->text);
        cp_tkc(o, ":");
        cp_pattrs(o, n->attrs, 2);
        // "label: attr;" ends with the semicolon that a reparse reads as a
        // null statement, so a null statement there is already printed
        if (trail) cp_tkc(o, ";");
        if (n->a && !(trail && n->a->kind == CP_S_NULL)) cp_pnested(o, n->a);
        break;
    }
    case CP_S_CASE:
        cp_tkc(o, "case");
        cp_sp(o);
        cp_pexpr(o, n->a, 3);
        if (n->b) { cp_puts(o, S(" ... ")); cp_pexpr(o, n->b, 3); }
        cp_tkc(o, ":");
        if (n->c) cp_pnested(o, n->c);
        break;
    case CP_S_DEFAULT:
        cp_tkc(o, "default:");
        if (n->c) cp_pnested(o, n->c);
        break;
    case CP_S_ASM: {
        cp_tkc(o, "__asm__");
        if (n->flags & CP_NF_VOLATILE) cp_puts(o, S(" volatile"));
        if (n->flags & CP_NF_INLINE) cp_puts(o, S(" inline"));
        if (n->flags & CP_NF_GOTO) cp_puts(o, S(" goto"));
        cp_puts(o, S(" ("));
        cp_pexpr(o, n->a, 2);
        cp_node *lists[4] = {n->b, n->c, n->d, n->list};
        for (i32 s = 0; s < n->ival && s < 4; s++) {
            cp_puts(o, S(" :"));
            for (cp_node *x = lists[s]; x; x = x->next) {
                cp_sp(o);
                if (x->kind == CP_S_ASMOPERAND) {
                    if (x->text.len) { cp_tkc(o, "["); cp_tk(o, x->text); cp_tkc(o, "]"); cp_sp(o); }
                    cp_pexpr(o, x->b, 2);
                    cp_puts(o, S(" ("));
                    cp_pexpr(o, x->a, 1);
                    cp_tkc(o, ")");
                } else {
                    cp_pexpr(o, x, 2);
                }
                if (x->next) cp_tkc(o, ",");
            }
        }
        cp_puts(o, S(");"));
    } break;
    case CP_S_MSASM:
        cp_ptoks(o, n->tok, (i32)n->ival, 1);
        cp_putc(o, '\n');
        cp_ind(o);
        break;
    case CP_S_TRY:
        cp_tkc(o, "__try");
        cp_sp(o);
        cp_pstmt(o, n->a);
        cp_sp(o);
        if (n->b && n->b->kind == CP_S_EXCEPT) {
            cp_tkc(o, "__except");
            cp_puts(o, S(" ("));
            cp_pexpr(o, n->b->a, 1);
            cp_puts(o, S(") "));
            cp_pstmt(o, n->b->b);
        } else if (n->b) {
            cp_tkc(o, "__finally");
            cp_sp(o);
            cp_pstmt(o, n->b->b);
        }
        break;
    case CP_S_ATTR:
        cp_pattrs(o, n->attrs, 0);
        cp_tkc(o, ";");
        break;
    case CP_S_LOCALLABEL:
    case CP_D_DECL:
    case CP_D_STATICASSERT:
        cp_pdecl(o, n, 1);
        break;
    case CP_D_FUNC: {
        // nested function (GNU) or file scope definition
        iz save = o->len;
        cp_node *d = n->list;
        // GNU attributes after the declarator are not allowed in a
        // definition; move them in among the specifiers. Keyword ones
        // (SDCC's __interrupt(1), __reentrant) must stay after it.
        cp_pspecs2(o, n->flags, n->attrs, d->attrs, n->type);
        if (o->len > save && o->buf[save] == ' ') {
            for (iz i = save; i+1 < o->len; i++) o->buf[i] = o->buf[i+1];
            o->len--;
        }
        cp_pdeclarator1(o, d, n->type, 1);
        for (cp_node *kr = n->b; kr; kr = kr->next) {
            cp_nl(o);
            cp_ind(o);
            cp_pdecl(o, kr, 1);
        }
        cp_nl(o);
        cp_ind(o);
        cp_pstmt(o, n->a);
    } break;
    default:
        cp_tkc(o, "/*?*/;");
    }
}

static cp_out cp_outbegin(cp_arena *a, cp_unit *u)
{
    cp_out o = {0};
    o.buf = a->beg;
    o.cap = a->end - a->beg;
    o.u = u;
    return o;
}

static cp_str cp_outend(cp_arena *a, cp_out *o)
{
    cp_str r = {0};
    if (o->err || o->len >= o->cap) return r;
    o->buf[o->len] = 0;
    r.data = o->buf;
    r.len = o->len;
    a->beg += o->len + 1;
    return r;
}

static cp_str cp_printx(cp_arena *a, cp_unit *u, b32 check)
{
    cp_out o = cp_outbegin(a, u);
    o.check = check;
    if (!u) return cp_outend(a, &o);
    for (cp_node *d = u->decls; d; d = d->next) {
        cp_flushpragmas(&o, d->tok);
        cp_nl(&o);
        if (d->kind == CP_D_FUNC && o.len) cp_putc(&o, '\n');
        switch (d->kind) {
        case CP_D_ERROR:
            cp_ptoks(&o, d->tok, (i32)d->ival, 1);
            break;
        case CP_D_EMPTY:
            cp_tkc(&o, ";");
            break;
        default:
            cp_pstmt(&o, d);
        }
        if (d->kind == CP_D_FUNC) cp_putc(&o, '\n');
    }
    cp_flushpragmas(&o, (i32)u->ntoks);
    cp_nl(&o);
    return cp_outend(a, &o);
}

cp_str cp_print(cp_arena *a, cp_unit *u)
{
    return cp_printx(a, u, 0);
}

cp_str cp_print_checked(cp_arena *a, cp_unit *u)
{
    return cp_printx(a, u, 1);
}

cp_str cp_print_tokens(cp_arena *a, cp_unit *u)
{
    cp_out o = cp_outbegin(a, u);
    if (!u) return cp_outend(a, &o);
    i32 line = -1, file = -1;
    for (i32 i = 0; i < u->ntoks; i++) {
        cp_flushpragmas(&o, i);
        cp_token *t = &u->toks[i];
        if (t->kind == CP_EOF) break;
        if (t->file != file || t->line != line) {
            if (!t->macro || (t->flags & CP_TF_BOL)) {
                if (o.len) cp_nl(&o);
            }
            file = t->file;
            line = t->line;
        }
        if (t->flags & CP_TF_SPACE && o.last != '\n') cp_sp(&o);
        cp_tk(&o, t->text);
    }
    cp_nl(&o);
    return cp_outend(a, &o);
}

cp_str cp_typename(cp_arena *a, cp_unit *u, cp_type *t)
{
    cp_out o = cp_outbegin(a, u);
    cp_ptypename(&o, t, 0);
    // trim leading space
    if (o.len && o.buf[0] == ' ') {
        for (iz i = 0; i+1 < o.len; i++) o.buf[i] = o.buf[i+1];
        o.len--;
    }
    return cp_outend(a, &o);
}

// --- dump

static void cp_dtype(cp_out *o, cp_type *t)
{
    iz start = o->len;
    cp_ptypename(o, t, 0);
    if (o->len > start && o->buf[start] == ' ') {
        for (iz i = start; i+1 < o->len; i++) o->buf[i] = o->buf[i+1];
        o->len--;
    }
}

static void cp_dnode(cp_out *o, cp_node *n, i32 depth, char *role);

static void cp_dlist(cp_out *o, cp_node *n, i32 depth, char *role)
{
    for (; n; n = n->next) cp_dnode(o, n, depth, role);
}

static void cp_dline(cp_out *o, i32 depth)
{
    cp_nl(o);
    for (i32 i = 0; i < depth; i++) cp_puts(o, S("  "));
    o->last = ' ';
}

static void cp_dnode(cp_out *o, cp_node *n, i32 depth, char *role)
{
    if (!n) return;
    cp_unit *u = o->u;
    cp_dline(o, depth);
    if (role) { cp_puts(o, cp_cstr(role)); cp_puts(o, S(": ")); }
    cp_puts(o, cp_nodename(n->kind));
    if (n->op) { cp_putc(o, ' '); cp_puts(o, cp_opname(n->op)); }
    if (n->text.len) { cp_putc(o, ' '); cp_puts(o, n->text); }
    if (n->kind == CP_D_DECL || n->kind == CP_D_FUNC) cp_pstorage(o, n->flags);
    if (n->ty) { cp_puts(o, S(" : ")); cp_dtype(o, n->ty); }
    if (n->isconst == 1) { cp_puts(o, S(" = ")); cp_num(o, n->cval); }
    if (n->kind == CP_E_STRING) {
        cp_puts(o, S(" len="));
        cp_num(o, n->sval.len);
    }
    if (n->eflags & CP_EF_LVALUE) cp_puts(o, S(" lvalue"));
    if (n->eflags & CP_EF_SIDEEFFECT) cp_puts(o, S(" sidefx"));
    if (n->kind == CP_D_DECLARATOR && n->bitwidth >= 0) {
        cp_puts(o, S(" bits@")); cp_num(o, n->bitoffset); cp_puts(o, S(":")); cp_num(o, n->bitwidth);
    } else if (n->kind == CP_D_DECLARATOR && n->offset) {
        cp_puts(o, S(" @")); cp_num(o, n->offset);
    }
    if (n->ref && n->ref->tok >= 0 && n->ref->tok < u->ntoks) {
        cp_token *t = &u->toks[n->ref->tok];
        cp_puts(o, S(" -> line "));
        cp_num(o, t->line);
    }
    if (n->sym && (n->kind == CP_D_DECLARATOR || n->kind == CP_D_FUNC)) {
        cp_sym *s = n->sym;
        cp_puts(o, s->linkage == CP_LINK_EXTERNAL ? S(" [extern") : s->linkage == CP_LINK_INTERNAL ? S(" [static") : S(" [local"));
        cp_puts(o, S(" refs="));
        cp_num(o, s->nrefs);
        cp_puts(o, S("]"));
    }
    if (n->tok >= 0 && n->tok < u->ntoks) {
        cp_puts(o, S("  <"));
        cp_num(o, u->toks[n->tok].line);
        cp_puts(o, S(">"));
    }
    i32 d = depth + 1;
    switch (n->kind) {
    case CP_D_DECL:
        if (n->type && (n->type->flags & CP_TF_BODY)) {
            cp_dline(o, d);
            cp_puts(o, S("members of "));
            cp_dtype(o, n->type->sem);
            if (n->type->sem && n->type->sem->size >= 0) {
                cp_puts(o, S(" size="));
                cp_num(o, n->type->sem->size);
                cp_puts(o, S(" align="));
                cp_num(o, n->type->sem->align);
            }
            cp_dlist(o, n->type->members, d+1, 0);
        }
        cp_dlist(o, n->list, d, 0);
        break;
    case CP_D_FUNC:
        cp_dlist(o, n->list, d, 0);
        cp_dlist(o, n->b, d, "kr");
        cp_dnode(o, n->a, d, "body");
        break;
    case CP_D_DECLARATOR:
        if (n->ty && n->ty->kind == CP_T_FUNC && n->type) {
            cp_type *ft = n->type;
            while (ft && ft->kind != CP_T_FUNC) ft = ft->base;
            if (ft) cp_dlist(o, ft->params, d, "param");
        }
        cp_dnode(o, n->b, d, "width");
        cp_dnode(o, n->a, d, "init");
        break;
    case CP_S_FOR:
        cp_dnode(o, n->d, d, "init");
        cp_dnode(o, n->a, d, "cond");
        cp_dnode(o, n->c, d, "step");
        cp_dnode(o, n->b, d, "body");
        break;
    case CP_S_IF:
        cp_dnode(o, n->a, d, "cond");
        cp_dnode(o, n->b, d, "then");
        cp_dnode(o, n->c, d, "else");
        break;
    case CP_E_CAST: case CP_E_SIZEOF: case CP_E_ALIGNOF: case CP_E_COMPOUND:
        cp_dnode(o, n->a, d, 0);
        break;
    case CP_E_INIT:
        cp_dlist(o, n->b, d, "designator");
        cp_dnode(o, n->a, d, 0);
        break;
    case CP_S_ASM:
        cp_dnode(o, n->a, d, "template");
        cp_dlist(o, n->b, d, "out");
        cp_dlist(o, n->c, d, "in");
        cp_dlist(o, n->d, d, "clobber");
        cp_dlist(o, n->list, d, "label");
        break;
    default:
        cp_dnode(o, n->a, d, 0);
        cp_dnode(o, n->b, d, 0);
        cp_dnode(o, n->c, d, 0);
        cp_dnode(o, n->d, d, 0);
        cp_dlist(o, n->list, d, 0);
    }
}

cp_str cp_dump(cp_arena *a, cp_unit *u)
{
    cp_out o = cp_outbegin(a, u);
    if (!u) return cp_outend(a, &o);
    for (cp_node *d = u->decls; d; d = d->next) {
        cp_dnode(&o, d, 0, 0);
    }
    cp_nl(&o);
    return cp_outend(a, &o);
}

// --- names

static cp_str cp_opname(i32 op)
{
    return cp_tokname(op);
}

cp_str cp_tokname(int32_t kind)
{
    static const char *names[] = {
        [CP_EOF] = "end of file", [CP_IDENT] = "identifier",
        [CP_INTLIT] = "integer", [CP_FLOATLIT] = "float",
        [CP_CHARLIT] = "character", [CP_STRLIT] = "string",
        [CP_OTHER] = "stray", [CP_LBRACK] = "[", [CP_RBRACK] = "]",
        [CP_LPAREN] = "(", [CP_RPAREN] = ")", [CP_LBRACE] = "{",
        [CP_RBRACE] = "}", [CP_DOT] = ".", [CP_ARROW] = "->",
        [CP_INC] = "++", [CP_DEC] = "--", [CP_AMP] = "&", [CP_STAR] = "*",
        [CP_PLUS] = "+", [CP_MINUS] = "-", [CP_TILDE] = "~", [CP_NOT] = "!",
        [CP_SLASH] = "/", [CP_PERCENT] = "%", [CP_SHL] = "<<",
        [CP_SHR] = ">>", [CP_LT] = "<", [CP_GT] = ">", [CP_LE] = "<=",
        [CP_GE] = ">=", [CP_EQ] = "==", [CP_NE] = "!=", [CP_XOR] = "^",
        [CP_OR] = "|", [CP_ANDAND] = "&&", [CP_OROR] = "||",
        [CP_QUESTION] = "?", [CP_COLON] = ":", [CP_SEMI] = ";",
        [CP_ELLIPSIS] = "...", [CP_ASSIGN] = "=", [CP_MULEQ] = "*=",
        [CP_DIVEQ] = "/=", [CP_MODEQ] = "%=", [CP_ADDEQ] = "+=",
        [CP_SUBEQ] = "-=", [CP_SHLEQ] = "<<=", [CP_SHREQ] = ">>=",
        [CP_ANDEQ] = "&=", [CP_XOREQ] = "^=", [CP_OREQ] = "|=",
        [CP_COMMA] = ",", [CP_HASH] = "#", [CP_HASHHASH] = "##",
        [CP_K_REAL] = "__real__", [CP_K_IMAG] = "__imag__",
        [CP_K_EXTENSION] = "__extension__",
    };
    if (kind >= 0 && kind < cp_countof(names) && names[kind]) return cp_cstr((char *)names[kind]);
    for (iz i = 0; i < cp_countof(cp_keywords); i++) {
        if (cp_keywords[i].kind == kind) return cp_cstr(cp_keywords[i].name);
    }
    return S("?");
}

cp_str cp_nodename(int32_t kind)
{
    static const char *names[CP_NNODEKINDS] = {
        "NONE", "IDENT", "INT", "FLOAT", "CHAR", "STRING", "CALL", "INDEX",
        "MEMBER", "POSTFIX", "UNARY", "LABELADDR", "SIZEOF", "ALIGNOF",
        "CAST", "COMPOUNDLIT", "BINARY", "ASSIGN", "COND", "COMMA",
        "GENERIC", "ASSOC", "STMTEXPR", "TYPEARG", "INITLIST", "INIT",
        "DFIELD", "DINDEX", "BLOCK", "COMPOUND", "EXPR", "IF", "SWITCH",
        "WHILE", "DO", "FOR", "GOTO", "GOTOPTR", "CONTINUE", "BREAK",
        "RETURN", "LABEL", "CASE", "DEFAULT", "NULL", "ASM", "ASMOPERAND",
        "MSASM", "TRY", "EXCEPT", "FINALLY", "LEAVE", "ATTRSTMT",
        "LOCALLABEL", "DECL", "DECLARATOR", "FUNC", "ENUMERATOR",
        "STATICASSERT", "EMPTY", "ERROR",
    };
    if (kind >= 0 && kind < CP_NNODEKINDS && names[kind]) return cp_cstr((char *)names[kind]);
    return S("?");
}

// ---- Convenience

// Arena allocation for the public helpers: null instead of a longjmp when
// the arena is full.
static void *cp_arenanew(cp_arena *a, iz count, iz size, iz align)
{
    iz pad = -(uintptr_t)a->beg & (uintptr_t)(align - 1);
    if (count < 0 || count > (a->end - a->beg - pad)/size) return 0;
    char *r = a->beg + pad;
    a->beg += pad + count*size;
    cp_memset0(r, count*size);
    return r;
}

static cp_token *cp_nodetok(cp_unit *u, cp_node *n)
{
    return u && n && n->tok >= 0 && n->tok < u->ntoks ? &u->toks[n->tok] : 0;
}

cp_loc cp_locof(cp_unit *u, cp_node *n)
{
    cp_loc r = {0};
    cp_token *t = cp_nodetok(u, n);
    if (!t) return r;
    if (t->file >= 0 && t->file < u->nfiles) r.file = u->files[t->file].path;
    r.line = t->line;
    r.col = t->col;
    return r;
}

cp_str cp_locstr(cp_arena *a, cp_unit *u, cp_node *n)
{
    cp_loc l = cp_locof(u, n);
    cp_out o = cp_outbegin(a, u);
    cp_puts(&o, l.file.data ? l.file : S("?"));
    cp_putc(&o, ':');
    cp_num(&o, l.line);
    cp_putc(&o, ':');
    o.lastnum = 0;
    cp_num(&o, l.col);
    return cp_outend(a, &o);
}

cp_str cp_srcof(cp_unit *u, cp_node *n)
{
    cp_str r = {0};
    cp_token *b = cp_nodetok(u, n);
    if (!b || n->endtok <= n->tok || n->endtok > u->ntoks) return r;
    cp_token *e = &u->toks[n->endtok - 1];
    if (b->macro || e->macro || b->file != e->file || b->file < 0 || b->file >= u->nfiles) return r;
    cp_str src = u->files[b->file].src;
    char *beg = b->text.data, *end = e->text.data + e->text.len;
    if (beg < src.data || end > src.data + src.len || end < beg) return r;
    return cp_span(beg, end);
}

cp_str cp_docof(cp_unit *u, cp_node *n)
{
    cp_token *t = cp_nodetok(u, n);
    return t ? t->doc : (cp_str){0};
}

// The token where a symbol is first declared.
static cp_token *cp_symtok(cp_unit *u, cp_sym *s)
{
    if (!u || !s) return 0;
    i32 tok = s->decl ? s->decl->tok : s->kind == CP_SYM_TAG && s->ty ? s->ty->tok : -1;
    return tok >= 0 && tok < u->ntoks ? &u->toks[tok] : 0;
}

cp_loc cp_symloc(cp_unit *u, cp_sym *s)
{
    cp_loc r = {0};
    cp_token *t = cp_symtok(u, s);
    if (!t) return r;
    if (t->file >= 0 && t->file < u->nfiles) r.file = u->files[t->file].path;
    r.line = t->line;
    r.col = t->col;
    return r;
}

cp_str cp_symdoc(cp_unit *u, cp_sym *s)
{
    cp_token *t = cp_symtok(u, s);
    if (!t) return (cp_str){0};
    if (t->doc.len || !s->ty || s->kind != CP_SYM_TAG) return t->doc;
    // a tag's type points at its name; the comment sits before "struct"
    cp_token *k = t - 1;
    return k >= u->toks ? k->doc : t->doc;
}

int32_t cp_inmainfile(cp_unit *u, cp_node *n)
{
    cp_token *t = cp_nodetok(u, n);
    return t && t->file == 0;
}

cp_macro *cp_macroof(cp_unit *u, cp_node *n)
{
    cp_token *t = cp_nodetok(u, n);
    return t ? t->macro : 0;
}

cp_str cp_nodestr(cp_arena *a, cp_unit *u, cp_node *n)
{
    cp_out o = cp_outbegin(a, u);
    if (u) o.nextpragma = (i32)u->npragmas;  // a fragment: no pragmas
    if (!n) {
    } else if (n->kind < CP_S_COMPOUND) {
        cp_pexpr(&o, n, 1);
    } else if (n->kind == CP_D_DECLARATOR) {
        cp_type *base = cp_innermost(n->type, 0);
        cp_pbase(&o, base);
        cp_pdeclarator(&o, n->type, base, n->text, 1);
    } else {
        cp_pstmt(&o, n);
    }
    if (o.len && o.buf[0] == ' ') {
        for (iz i = 0; i+1 < o.len; i++) o.buf[i] = o.buf[i+1];
        o.len--;
    }
    return cp_outend(a, &o);
}

cp_sym *cp_findsym(cp_unit *u, cp_str name)
{
    for (cp_sym *s = u ? u->syms : 0; s; s = s->next) {
        if (s->depth == 0 && s->kind != CP_SYM_TAG && s->kind != CP_SYM_LABEL &&
            cp_equals(s->name, name)) {
            return s;
        }
    }
    return 0;
}

cp_sym *cp_findtag(cp_unit *u, cp_str name)
{
    cp_sym *r = 0;
    for (cp_sym *s = u ? u->syms : 0; s; s = s->next) {
        if (s->depth == 0 && s->kind == CP_SYM_TAG && cp_equals(s->name, name)) {
            r = s;
            if (s->ty && !(s->ty->flags & CP_TF_INCOMPLETE)) break;  // prefer complete
        }
    }
    return r;
}

cp_macro *cp_macrodef(cp_unit *u, cp_str name)
{
    cp_macro *r = 0;
    for (cp_macro *m = u ? u->macros : 0; m; m = m->next) {
        if (cp_equals(m->name, name)) r = m;
    }
    return r;
}

cp_sym *cp_nextsym(cp_unit *u, cp_sym *prev, int32_t kinds, int32_t sel)
{
    for (cp_sym *s = prev ? prev->next : u ? u->syms : 0; s; s = s->next) {
        if (kinds && !(kinds & CP_KIND(s->kind))) continue;
        cp_token *t = cp_symtok(u, s);
        if ((sel & CP_SEL_MAIN) && !(t && t->file == 0)) continue;
        if ((sel & CP_SEL_DEFINED) && !s->def && !(s->flags & CP_YF_DEFINED)) continue;
        if ((sel & CP_SEL_FILE) && s->depth != 0) continue;
        if ((sel & CP_SEL_EXTERNAL) && s->linkage != CP_LINK_EXTERNAL) continue;
        if ((sel & CP_SEL_USED) && !s->refs) continue;
        return s;
    }
    return 0;
}

cp_str cp_declstr(cp_arena *a, cp_unit *u, cp_type *t, cp_str name)
{
    cp_out o = cp_outbegin(a, u);
    if (t) {
        cp_type *base = cp_innermost(t, 0);
        cp_pbase(&o, base);
        cp_pdeclarator(&o, t, base, name, name.len > 0);
    }
    if (o.len && o.buf[0] == ' ') {
        for (iz i = 0; i+1 < o.len; i++) o.buf[i] = o.buf[i+1];
        o.len--;
    }
    return cp_outend(a, &o);
}

cp_str cp_signature(cp_arena *a, cp_unit *u, cp_sym *f)
{
    if (!f) return cp_declstr(a, u, 0, S(""));
    cp_node *d = 0;
    i32 storage = 0;
    if (f->def && f->def->kind == CP_D_FUNC) {
        d = f->def->list;
        storage = f->def->flags & (CP_SC_STATIC|CP_SC_EXTERN|CP_SC_INLINE|CP_SC_NORETURN);
    } else if (f->decl && f->decl->kind == CP_D_DECLARATOR) {
        d = f->decl;
    }
    if (!d || !d->type) return cp_declstr(a, u, f->ty, f->name);
    cp_out o = cp_outbegin(a, u);
    cp_pstorage(&o, storage);
    cp_type *base = cp_innermost(d->type, 0);
    cp_pbase(&o, base);
    cp_pdeclarator(&o, d->type, base, d->text, 1);
    if (o.len && o.buf[0] == ' ') {
        for (iz i = 0; i+1 < o.len; i++) o.buf[i] = o.buf[i+1];
        o.len--;
    }
    return cp_outend(a, &o);
}

// Visit the named fields of record t at offset base; count them, and store
// them when out is not null.
static i32 cp_fieldsat(cp_type *t, i64 base, cp_field *out, i32 n, i32 depth)
{
    if (!t || depth > 64) return n;
    for (cp_node *decl = t->members; decl; decl = decl->next) {
        if (decl->kind != CP_D_DECL) continue;
        if (decl->eflags & CP_EF_ANONMEMBER) {
            n = cp_fieldsat(decl->type->sem, base + decl->offset, out, n, depth+1);
            continue;
        }
        for (cp_node *d = decl->list; d; d = d->next) {
            if (!d->text.len) continue;
            if (out) {
                cp_field *f = out + n;
                f->name = d->text;
                f->type = d->ty;
                f->offset = base + d->offset;
                f->bitoffset = base*8 + d->bitoffset;
                f->bitwidth = d->bitwidth;
                f->decl = d;
            }
            n++;
        }
    }
    return n;
}

int32_t cp_fields(cp_arena *a, cp_type *t, cp_field **out)
{
    *out = 0;
    t = cp_strip(t);
    if (!t || (t->kind != CP_T_STRUCT && t->kind != CP_T_UNION)) return 0;
    i32 n = cp_fieldsat(t, 0, 0, 0, 0);
    if (!n) return 0;
    cp_field *f = cp_arenanew(a, n, (iz)sizeof(cp_field), (iz)cp_alignof(cp_field));
    if (!f) return -1;
    cp_fieldsat(t, 0, f, 0, 0);
    *out = f;
    return n;
}

int32_t cp_enumconsts(cp_arena *a, cp_type *t, cp_enumconst **out)
{
    *out = 0;
    t = cp_strip(t);
    if (!t || t->kind != CP_T_ENUM) return 0;
    i32 n = 0;
    for (cp_node *e = t->members; e; e = e->next) n += e->kind == CP_D_ENUMERATOR;
    if (!n) return 0;
    cp_enumconst *r = cp_arenanew(a, n, (iz)sizeof(cp_enumconst), (iz)cp_alignof(cp_enumconst));
    if (!r) return -1;
    i32 i = 0;
    for (cp_node *e = t->members; e; e = e->next) {
        if (e->kind != CP_D_ENUMERATOR) continue;
        r[i].name = e->text;
        r[i].value = e->cval;
        r[i].decl = e;
        i++;
    }
    *out = r;
    return n;
}

// Returns -1 when the walk is to stop.
static i32 cp_walk1(cp_node *n, cp_node *parent, cp_visitor visit, void *ctx, i32 depth)
{
    if (!n || depth > CPARSE_MAXDEPTH*4) return 1;
    i32 r = visit(ctx, n, parent);
    if (r < 0) return -1;
    if (r == 0) return 1;
    cp_node *kids[] = {n->a, n->b, n->c, n->d};
    for (iz i = 0; i < cp_countof(kids); i++) {
        if (cp_walk1(kids[i], n, visit, ctx, depth+1) < 0) return -1;
    }
    for (cp_node *k = n->list; k; k = k->next) {
        if (cp_walk1(k, n, visit, ctx, depth+1) < 0) return -1;
    }
    return 1;
}

void cp_walk(cp_node *root, cp_visitor visit, void *ctx)
{
    if (visit) cp_walk1(root, 0, visit, ctx, 0);
}

void cp_walkunit(cp_unit *u, cp_visitor visit, void *ctx)
{
    for (cp_node *d = u && visit ? u->decls : 0; d; d = d->next) {
        if (cp_walk1(d, 0, visit, ctx, 0) < 0) return;
    }
}

cp_sym *cp_callee(cp_node *call)
{
    if (!call || call->kind != CP_E_CALL || !call->a) return 0;
    cp_node *f = call->a;
    if (f->kind == CP_E_UNARY && f->op == CP_STAR && f->a) f = f->a;  // (*f)(x)
    return f->kind == CP_E_IDENT && f->sym && f->sym->kind == CP_SYM_FUNC ? f->sym : 0;
}

cp_symref *cp_callsto(cp_unit *u, cp_str name)
{
    cp_sym *s = cp_findsym(u, name);
    return s && s->kind == CP_SYM_FUNC ? s->callers : 0;
}

cp_type *cp_strip(cp_type *t)
{
    if (!t) return 0;
    t = t->sem ? t->sem : t;
    return t->unqual ? t->unqual : t;
}

cp_type *cp_target(cp_type *t)
{
    t = cp_strip(t);
    if (!t) return 0;
    switch (t->kind) {
    case CP_T_POINTER: case CP_T_ARRAY: case CP_T_VECTOR: case CP_T_FUNC:
        return t->base;
    }
    return 0;
}

int32_t cp_typeis(cp_type *t, int32_t what)
{
    if (!t) return 0;
    cp_type *q = t->sem ? t->sem : t;  // keeps qualifiers
    cp_type *s = cp_strip(t);
    b32 isint = cp_isinteger(s);
    b32 isuns = 0;
    if (isint) {
        cp_type *b = s->kind == CP_T_ENUM ? cp_strip(s->base) : s;
        if (b && b->kind == CP_T_BITINT) isuns = (b->bits & CP_B_UNSIGNED) != 0;
        else if (b && b->kind == CP_T_BUILTIN) {
            switch (b->basic) {
            case CP_BT_BOOL: case CP_BT_UCHAR: case CP_BT_USHORT: case CP_BT_UINT:
            case CP_BT_ULONG: case CP_BT_ULLONG: case CP_BT_UINT128:
                isuns = 1;
            }
        }
    }
    struct { i32 flag; b32 has; } props[] = {
        {CP_IS_VOID,     s->kind == CP_T_BUILTIN && s->basic == CP_BT_VOID},
        {CP_IS_BOOL,     s->kind == CP_T_BUILTIN && s->basic == CP_BT_BOOL},
        {CP_IS_INTEGER,  isint},
        {CP_IS_FLOATING, cp_isfloating(s)},
        {CP_IS_ARITH,    cp_isarith(s)},
        {CP_IS_SCALAR,   cp_isscalar(s)},
        {CP_IS_POINTER,  s->kind == CP_T_POINTER},
        {CP_IS_ARRAY,    s->kind == CP_T_ARRAY},
        {CP_IS_FUNC,     s->kind == CP_T_FUNC},
        {CP_IS_STRUCT,   s->kind == CP_T_STRUCT},
        {CP_IS_UNION,    s->kind == CP_T_UNION},
        {CP_IS_RECORD,   s->kind == CP_T_STRUCT || s->kind == CP_T_UNION},
        {CP_IS_ENUM,     s->kind == CP_T_ENUM},
        {CP_IS_SIGNED,   (isint && !isuns) || cp_isfloating(s)},
        {CP_IS_UNSIGNED, isint && isuns},
        {CP_IS_CONST,    (q->quals & CP_Q_CONST) != 0},
        {CP_IS_VOLATILE, (q->quals & CP_Q_VOLATILE) != 0},
        {CP_IS_COMPLETE, s->size >= 0 && !(s->flags & CP_TF_INCOMPLETE)},
        {CP_IS_VECTOR,   s->kind == CP_T_VECTOR},
        {CP_IS_VARIADIC, s->kind == CP_T_FUNC && (s->flags & CP_TF_VARIADIC)},
    };
    for (iz i = 0; i < cp_countof(props); i++) {
        if ((what & props[i].flag) && !props[i].has) return 0;
    }
    return what != 0;
}

int32_t cp_constint(cp_node *e, int64_t *value)
{
    if (!e || e->isconst != 1) return 0;
    if (value) *value = e->cval;
    return 1;
}

cp_str cp_errorstr(cp_arena *a, cp_error *e)
{
    cp_out o = cp_outbegin(a, 0);
    if (e) {
        cp_puts(&o, e->file.data ? e->file : S("?"));
        cp_putc(&o, ':');
        cp_num(&o, e->line);
        cp_putc(&o, ':');
        o.lastnum = 0;
        cp_num(&o, e->col);
        cp_puts(&o, e->warning ? S(": warning: ") : S(": error: "));
        cp_puts(&o, e->msg);
    }
    return cp_outend(a, &o);
}

#ifdef CPARSE_HOSTED
// ---- Hosted helpers: the only part of the library that does I/O.
#ifdef _WIN32
#  define cp_popen  _popen
#  define cp_pclose _pclose
#  define cp_devnull "NUL"
#else
// POSIX, declared here so they are available whatever feature macros the
// includer used
FILE *popen(const char *, const char *);
int pclose(FILE *);
char *realpath(const char *, char *);
#  define cp_popen  popen
#  define cp_pclose pclose
#  define cp_devnull "/dev/null"
#endif

cp_arena cp_newarena(ptrdiff_t cap)
{
    cp_arena a = {0};
    char *mem = cap > 0 ? malloc((size_t)cap) : 0;
    if (mem) {
        a.beg = mem;
        a.end = mem + cap;
    }
    return a;
}

// A copy of s with a terminating null, in the arena, or a null string.
static cp_str cp_hdup(cp_arena *a, cp_str s)
{
    cp_str r = {0};
    char *d = cp_arenanew(a, s.len+1, 1, 1);
    if (!d) return r;
    cp_memcpy(d, s.data, s.len);
    d[s.len] = 0;
    r.data = d;
    r.len = s.len;
    return r;
}

// Append s to the growing string *dst (which must be the arena's last
// allocation, or empty). False when the arena is full.
static b32 cp_hcat(cp_arena *a, cp_str *dst, cp_str s)
{
    if (!dst->data) {
        *dst = cp_hdup(a, s);
        return dst->data != 0;
    }
    if (dst->data + dst->len + 1 != a->beg) {  // not last: copy first
        cp_str c = cp_hdup(a, *dst);
        if (!c.data) return 0;
        *dst = c;
    }
    if (s.len >= a->end - a->beg) return 0;
    cp_memcpy(dst->data + dst->len, s.data, s.len);
    dst->len += s.len;
    dst->data[dst->len] = 0;
    a->beg = dst->data + dst->len + 1;
    return 1;
}

cp_str cp_readfile(void *ctx, cp_str path, cp_arena *a)
{
    (void)ctx;
    cp_str r = {0};
    char name[4096];
    if (path.len <= 0 || path.len >= (iz)sizeof(name)) return r;
    cp_memcpy(name, path.data, path.len);
    name[path.len] = 0;
    FILE *f = fopen(name, "rb");
    if (!f) return r;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return r; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) || n >= a->end - a->beg) { fclose(f); return r; }
    r.data = a->beg;
    r.len = (iz)fread(r.data, 1, (size_t)n, f);
    fclose(f);
    if (r.len != n) return (cp_str){0};  // a directory opens but reads nothing
    r.data[r.len] = 0;
    a->beg += r.len + 1;
    return r;
}

// Run a shell command and capture its output in the arena.
static cp_str cp_hrun(cp_arena *a, cp_str cmd)
{
    cp_str r = {0};
    if (!cmd.data) return r;
    FILE *f = cp_popen(cmd.data, "r");
    if (!f) return r;
    r.data = a->beg;
    iz cap = a->end - a->beg - 1;
    for (;;) {
        size_t n = fread(r.data + r.len, 1, (size_t)(cap - r.len), f);
        if (!n) break;
        r.len += (iz)n;
    }
    cp_pclose(f);
    r.data[r.len] = 0;
    a->beg += r.len + 1;
    return r;
}

static b32 cp_hstartswith(char *s, char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (*s != *prefix) return 0;
    }
    return 1;
}

static b32 cp_hequal(char *s, char *t)
{
    return cp_hstartswith(s, t) && s[cp_cstr(t).len] == 0;
}

// Does "cc -dM" output define name?
static b32 cp_hdefines(cp_str predefs, char *name)
{
    iz n = cp_cstr(name).len;
    for (iz i = 0; i+8+n < predefs.len; i++) {
        char *p = predefs.data + i;
        if (cp_hstartswith(p, "#define ") && cp_equals(cp_span(p+8, p+8+n), cp_cstr(name)) &&
            p[8+n] == ' ') {
            return 1;
        }
    }
    return 0;
}

// A growable list of directory strings in the arena.
typedef struct { cp_str *data; i32 len, cap; } cp_hdirs;

static b32 cp_hpush(cp_arena *a, cp_hdirs *l, cp_str s)
{
    if (l->len == l->cap) {
        i32 cap = l->cap ? l->cap*2 : 16;
        cp_str *d = cp_arenanew(a, cap, (iz)sizeof(cp_str), (iz)cp_alignof(cp_str));
        if (!d) return 0;
        if (l->len) cp_memcpy(d, l->data, l->len*(iz)sizeof(cp_str));
        l->data = d;
        l->cap = cap;
    }
    l->data[l->len++] = s;
    return 1;
}

cp_config *cp_hostconfig(cp_arena *a, int argc, char **argv)
{
    cp_config *cfg = cp_arenanew(a, 1, (iz)sizeof(cp_config), (iz)cp_alignof(cp_config));
    if (!cfg) return 0;
    cfg->readfile = cp_readfile;
    // like GCC, honor SOURCE_DATE_EPOCH for reproducible builds
    // (UTC); otherwise local time, as GCC uses
    char *epoch = getenv("SOURCE_DATE_EPOCH");
    if (epoch) {
        cfg->time = strtoll(epoch, 0, 10);
    } else {
        time_t now = time(0);
        struct tm g = *gmtime(&now);
        g.tm_isdst = -1;
        cfg->time = (int64_t)now + (int64_t)(now - mktime(&g));  // UTC offset
    }
    cp_hdirs quote = {0}, inc = {0}, sys = {0};
    cp_str defines = {0}, ccflags = S(" "), cc = S("cc");
    b32 nostdinc = 0, ok = 1;
    for (int i = 0; i < argc && ok; i++) {
        char *o = argv[i];
        char *next = i+1 < argc ? argv[i+1] : 0;
        char *v = 0;  // option value
        if (o[0] != '-') continue;  // file names are the caller's
        if (cp_hstartswith(o, "-I")) {
            v = o[2] ? o+2 : (i++, next);
            if (v) ok = cp_hpush(a, &inc, cp_cstr(v));
        } else if (cp_hequal(o, "-isystem") && next) {
            ok = cp_hpush(a, &sys, cp_cstr(argv[++i]));
        } else if (cp_hequal(o, "-iquote") && next) {
            ok = cp_hpush(a, &quote, cp_cstr(argv[++i]));
        } else if (cp_hstartswith(o, "-D") || cp_hstartswith(o, "-U")) {
            v = o[2] ? o+2 : (i++, next);
            if (!v) continue;
            cp_str d = cp_cstr(v), eq = {0};
            for (iz k = 0; k < d.len; k++) {
                if (d.data[k] == '=') { eq = cp_span(d.data+k+1, d.data+d.len); d.len = k; break; }
            }
            ok = cp_hcat(a, &defines, o[1] == 'D' ? S("#define ") : S("#undef ")) &&
                 cp_hcat(a, &defines, d) &&
                 (o[1] == 'U' || cp_hcat(a, &defines, eq.data ? S(" ") : S(" 1"))) &&
                 cp_hcat(a, &defines, eq) && cp_hcat(a, &defines, S("\n"));
        } else if (cp_hequal(o, "-include") && next) {
            // as if the main file began with #include "FILE"
            char *path = argv[++i];
#ifndef _WIN32
            char *full = realpath(path, 0);
#else
            char *full = _fullpath(0, path, 0);
#endif
            ok = cp_hcat(a, &defines, S("#include \"")) &&
                 cp_hcat(a, &defines, cp_cstr(full ? full : path)) &&
                 cp_hcat(a, &defines, S("\"\n"));
            free(full);
        } else if (cp_hequal(o, "--cc") && next) {
            cc = cp_cstr(argv[++i]);
        } else if (cp_hequal(o, "--std") && next) {
            ok = cp_hcat(a, &ccflags, S(" -std=")) && cp_hcat(a, &ccflags, cp_cstr(argv[++i]));
        } else if (cp_hstartswith(o, "-std=") || cp_hstartswith(o, "-O") ||
                   cp_hstartswith(o, "-m") || (cp_hstartswith(o, "-f") && o[2])) {
            // flags that change the compiler's predefined macros
            ok = cp_hcat(a, &ccflags, S(" ")) && cp_hcat(a, &ccflags, cp_cstr(o));
            if (cp_hstartswith(o, "-std=")) {
                // ISO modes before C23 have trigraphs, GNU modes do not
                char *s = o + 5;
                cfg->trigraphs = (s[0] == 'c' && !cp_hstartswith(s, "c2")) ||
                                 (cp_hstartswith(s, "iso9899:") && !cp_hequal(s, "iso9899:2024"));
            }
        } else if (cp_hequal(o, "-trigraphs")) {
            cfg->trigraphs = 1;
        } else if (cp_hequal(o, "--target") && next) {
            char *t = argv[++i];
            cfg->target = cp_hequal(t, "llp64") ? CP_TARGET_LLP64 :
                          cp_hequal(t, "ilp32") ? CP_TARGET_ILP32 : CP_TARGET_LP64;
        } else if (cp_hequal(o, "--nopp")) {
            cfg->nopp = 1;
        } else if (cp_hequal(o, "-nostdinc") || cp_hequal(o, "--nostdinc")) {
            nostdinc = 1;
        } else if (cp_hequal(o, "-o") || cp_hequal(o, "-x") || cp_hequal(o, "--xref")) {
            i++;  // options with a value that is not a file
        }
    }
    if (!ok) return 0;

    // the compiler's predefined macros and include directories
    cp_str predefs = {0};
    if (!cfg->nopp && !cp_equals(cc, S("none"))) {
        cp_str cmd = {0};
        ok = cp_hcat(a, &cmd, cc) && cp_hcat(a, &cmd, ccflags) &&
             cp_hcat(a, &cmd, S(" -dM -E -x c " cp_devnull " 2>" cp_devnull));
        if (!ok) return 0;
        predefs = cp_hrun(a, cmd);
        cmd = (cp_str){0};
        ok = cp_hcat(a, &cmd, cc) && cp_hcat(a, &cmd, ccflags) &&
             cp_hcat(a, &cmd, S(" -E -v -x c " cp_devnull " 2>&1 >" cp_devnull));
        if (!ok) return 0;
        cp_str out = cp_hrun(a, cmd);
        // the lines between "#include <...> search starts here:" and
        // "End of search list."
        b32 in = 0;
        for (iz k = 0; k < out.len && !nostdinc;) {
            iz e = k;
            while (e < out.len && out.data[e] != '\n') e++;
            cp_str line = cp_span(out.data+k, out.data+e);
            if (cp_startswith(line, S("#include <"))) {
                in = 1;
            } else if (cp_startswith(line, S("End"))) {
                in = 0;
            } else if (in && line.len > 1 && line.data[0] == ' ') {
                cp_str d = cp_span(line.data+1, line.data+line.len);
                for (iz m = 0; m < d.len; m++) {
                    if (d.data[m] == ' ') { d.len = m; break; }  // "(framework directory)"
                }
                if (!cp_hpush(a, &sys, cp_hdup(a, d))) return 0;
            }
            k = e + 1;
        }
        // GCC's predefined assertions, which "cc -dM" cannot report
        if (cp_hdefines(predefs, "__x86_64__")) {
            ok = cp_hcat(a, &predefs, S("#assert cpu(x86_64)\n#assert machine(x86_64)\n"));
        } else if (cp_hdefines(predefs, "__i386__")) {
            ok = cp_hcat(a, &predefs, S("#assert cpu(i386)\n#assert machine(i386)\n"));
        }
        if (ok && cp_hdefines(predefs, "__linux__")) {
            ok = cp_hcat(a, &predefs, S("#assert system(linux)\n#assert system(unix)\n"
                                        "#assert system(posix)\n"));
        }
        if (!ok) return 0;
    }
    if (defines.data && !cp_hcat(a, &predefs, defines)) return 0;
    cfg->predefs = predefs;
    cfg->quotedirs = quote.data;   cfg->nquotedirs = quote.len;
    cfg->includedirs = inc.data;   cfg->nincludedirs = inc.len;
    cfg->systemdirs = sys.data;    cfg->nsystemdirs = sys.len;
    return cfg;
}

cp_unit *cp_parsefile(cp_arena *a, char const *path, int argc, char **argv)
{
    cp_config *cfg = cp_hostconfig(a, argc, argv);
    if (!cfg) return 0;
    cp_str name = cp_hdup(a, cp_cstr((char *)path));
    cp_str src = cp_readfile(0, name, a);
    if (!name.data || !src.data) return 0;
    return cp_parse(a, src, name, cfg);
}
#undef cp_popen
#undef cp_pclose
#undef cp_devnull
#endif  // CPARSE_HOSTED

#undef i8
#undef i32
#undef i64
#undef u8
#undef u32
#undef u64
#undef iz
#undef uz
#undef b32
#undef CP_MAXSIZE
#undef CP_MAXALIGN
#undef S
#undef cp_tok
#undef cp_peekk
#undef cp_kind
#endif  // CPARSE_IMPLEMENTATION

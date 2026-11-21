// cparse: command line front end for cparse.h
//
//   cparse [options] file.c...
//
// Modes (default: check):
//   --check        report diagnostics and a one-line summary
//   -E             preprocessed token stream
//   --print        regenerate C from the syntax tree
//   --dump         syntax tree with types, values, and references
//   --symbols      symbol table: kind, linkage, type, uses
//   --xref NAME    every declaration and use of NAME
//   --callgraph    caller -> callee edges (--dot for Graphviz)
//   --layout       struct/union layouts with offsets and padding
//   --macros       macro definitions and use counts
//   --includes     include tree
//   --json         everything above as one JSON document
//   --ast          the full syntax tree with types as JSON
//   --roundtrip    verify print(parse(x)) is a fixed point
//   --typecheck    emit C that asserts our types and layouts to a compiler
//
// Options:
//   -I DIR -isystem DIR -iquote DIR -D NAME[=VAL] -U NAME
//   --cc CC        compiler to ask for predefined macros and system
//                  include directories (default: cc; "none" to skip)
//   --target T     lp64 (default), llp64, ilp32
//   -trigraphs     translate trigraphs (also implied by an ISO -std
//                  before c23, as with gcc)
//   --nopp         input is already preprocessed
//   -include FILE  process FILE first, as with a compiler
//   --nostdinc     no system include directories
//   --all          include system headers in listings
//   --version      print the version
//
// Build:  cc -O2 -o cparse cparse.c
// Fuzz:   fuzz/build.sh (afl-gcc-fast, ASan, UBSan), then fuzz/run.sh
//
// This is free and unencumbered software released into the public domain.
#define _XOPEN_SOURCE 700  // POSIX 2008 plus realpath
#define CPARSE_IMPLEMENTATION
#include "cparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define countof(a)  (ptrdiff_t)(sizeof(a) / sizeof(*(a)))
#define S(s)        CP_STR(s)

typedef cp_str Str;

static Str cstr(char *s)
{
    Str r = {s, s ? (ptrdiff_t)strlen(s) : 0};
    return r;
}

static int equals(Str a, Str b)
{
    return a.len == b.len && (!a.len || !memcmp(a.data, b.data, (size_t)a.len));
}

static void *alloc(cp_arena *a, ptrdiff_t size)
{
    ptrdiff_t pad = -(uintptr_t)a->beg & 15;
    if (size < 0 || size >= a->end - a->beg - pad) {
        fprintf(stderr, "cparse: out of memory\n");
        exit(2);
    }
    void *r = a->beg + pad;
    a->beg += pad + size;
    return r;
}

static Str copystr(cp_arena *a, Str s)
{
    Str r = {alloc(a, s.len+1), s.len};
    if (s.len) memcpy(r.data, s.data, (size_t)s.len);
    r.data[s.len] = 0;
    return r;
}

static Str concat(cp_arena *a, Str x, Str y)
{
    Str r = {alloc(a, x.len+y.len+1), x.len+y.len};
    if (x.len) memcpy(r.data, x.data, (size_t)x.len);
    if (y.len) memcpy(r.data+x.len, y.data, (size_t)y.len);
    r.data[r.len] = 0;
    return r;
}

// --- output

static void out(Str s)  { if (s.len) fwrite(s.data, 1, (size_t)s.len, stdout); }
static void outs(char *s) { fputs(s, stdout); }
static void outn(long long v) { printf("%lld", v); }

static void outjson(Str s)
{
    putchar('"');
    for (ptrdiff_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
        case '"':  outs("\\\""); break;
        case '\\': outs("\\\\"); break;
        case '\n': outs("\\n"); break;
        case '\t': outs("\\t"); break;
        case '\r': outs("\\r"); break;
        default:
            if (c < 0x20) printf("\\u%04x", c);
            else putchar(c);
        }
    }
    putchar('"');
}

// --- files

static Str readfile(void *ctx, Str path, cp_arena *a)
{
    (void)ctx;
    Str r = {0};
    char buf[4096];
    if (path.len >= countof(buf)) return r;
    memcpy(buf, path.data, (size_t)path.len);
    buf[path.len] = 0;
    FILE *f = fopen(buf, "rb");
    if (!f) return r;
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) { fclose(f); return r; }
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    ptrdiff_t avail = a->end - a->beg - 1;
    if (n > avail) { fclose(f); return r; }
    r.data = a->beg;
    r.len = (ptrdiff_t)fread(r.data, 1, (size_t)n, f);
    fclose(f);
    // a directory opens but reads nothing on some systems; treat as absent
    if (r.len != n) { r.data = 0; r.len = 0; return r; }
    r.data[r.len] = 0;
    a->beg += r.len + 1;
    return r;
}

static Str runcommand(cp_arena *a, char *cmd)
{
    Str r = {0};
    FILE *f = popen(cmd, "r");
    if (!f) return r;
    r.data = a->beg;
    ptrdiff_t cap = a->end - a->beg - 1;
    for (;;) {
        size_t n = fread(r.data + r.len, 1, (size_t)(cap - r.len), f);
        if (!n) break;
        r.len += (ptrdiff_t)n;
    }
    pclose(f);
    r.data[r.len] = 0;
    a->beg += r.len + 1;
    return r;
}

typedef struct {
    Str      *data;
    ptrdiff_t len;
    ptrdiff_t cap;
} Strs;

static void push(cp_arena *a, Strs *s, Str v)
{
    if (s->len == s->cap) {
        ptrdiff_t cap = s->cap ? s->cap*2 : 16;
        Str *d = alloc(a, cap * (ptrdiff_t)sizeof(Str));
        if (s->len) memcpy(d, s->data, (size_t)s->len * sizeof(Str));
        s->data = d;
        s->cap = cap;
    }
    s->data[s->len++] = v;
}

// Ask the system compiler for its search path and predefined macros.
static void querycc(cp_arena *a, char *cc, char *std, Strs *sysdirs, Str *predefs)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s -dM -E -x c /dev/null 2>/dev/null", cc, std ? std : "");
    *predefs = runcommand(a, cmd);
    snprintf(cmd, sizeof(cmd), "%s %s -E -v -x c /dev/null 2>&1 >/dev/null", cc, std ? std : "");
    Str v = runcommand(a, cmd);
    // lines between "#include <...> search starts here:" and "End of search list."
    int in = 0;
    char *p = v.data, *e = v.data + v.len;
    while (p < e) {
        char *nl = memchr(p, '\n', (size_t)(e - p));
        if (!nl) nl = e;
        Str line = {p, nl - p};
        if (line.len >= 7 && !memcmp(line.data, "#include <", 10 < line.len ? 10 : (size_t)line.len)) {
            in = 1;
        } else if (line.len >= 3 && !memcmp(line.data, "End", 3)) {
            in = 0;
        } else if (in && line.len > 1 && line.data[0] == ' ') {
            Str d = {line.data+1, line.len-1};
            // drop " (framework directory)" suffixes
            for (ptrdiff_t i = 0; i < d.len; i++) {
                if (d.data[i] == ' ') { d.len = i; break; }
            }
            push(a, sysdirs, copystr(a, d));
        }
        p = nl + 1;
    }
}

// Does the "cc -dM" output define name?
static int hasdefine(Str predefs, char *name)
{
    size_t n = strlen(name);
    for (char *p = predefs.data, *e = p + predefs.len; p+8+n < e; p++) {
        if (!memcmp(p, "#define ", 8) && !memcmp(p+8, name, n) && p[8+n] == ' ') return 1;
    }
    return 0;
}

// GCC's predefined assertions (#cpu, #machine, #system), which "cc -dM"
// cannot report, rebuilt from the macros that imply them.
static Str ccasserts(cp_arena *a, Str predefs)
{
    Str r = {0};
    if (hasdefine(predefs, "__x86_64__")) {
        r = concat(a, r, S("#assert cpu(x86_64)\n#assert machine(x86_64)\n"));
    } else if (hasdefine(predefs, "__i386__")) {
        r = concat(a, r, S("#assert cpu(i386)\n#assert machine(i386)\n"));
    }
    if (hasdefine(predefs, "__linux__")) {
        r = concat(a, r, S("#assert system(linux)\n#assert system(unix)\n#assert system(posix)\n"));
    }
    return r;
}

// --- locations

static void loc(cp_unit *u, int32_t tok)
{
    if (tok < 0 || tok >= u->ntoks) { outs("?"); return; }
    cp_token *t = &u->toks[tok];
    if (t->file >= 0 && t->file < u->nfiles) out(u->files[t->file].path);
    printf(":%d:%d", t->line, t->col);
}

static void jsonloc(cp_unit *u, int32_t tok)
{
    if (tok < 0 || tok >= u->ntoks) { outs("null"); return; }
    cp_token *t = &u->toks[tok];
    outs("{\"file\":");
    outjson(t->file >= 0 && t->file < u->nfiles ? u->files[t->file].path : S(""));
    printf(",\"line\":%d,\"col\":%d}", t->line, t->col);
}

static Str tname(cp_arena *a, cp_unit *u, cp_type *t)
{
    if (!t) return S("?");
    Str s = cp_typename(a, u, t);
    return s.data ? s : S("?");
}

static char *symkind(int32_t k)
{
    static char *names[] = {"var", "func", "param", "typedef", "enumconst", "tag", "label"};
    return k >= 0 && k < countof(names) ? names[k] : "?";
}

static char *linkname(int32_t l)
{
    return l == CP_LINK_EXTERNAL ? "external" : l == CP_LINK_INTERNAL ? "internal" : "none";
}

static int32_t symtok(cp_sym *s, int def)
{
    cp_node *n = def ? s->def : s->decl;
    if (!n && s->ty && s->kind == CP_SYM_TAG) return s->ty->tok;
    return n ? n->tok : -1;
}

static void symflags(cp_sym *s)
{
    static struct { int32_t bit; char *name; } f[] = {
        {CP_YF_DEFINED, "defined"}, {CP_YF_READ, "read"}, {CP_YF_WRITTEN, "written"},
        {CP_YF_ADDRTAKEN, "addrtaken"}, {CP_YF_CALLED, "called"},
        {CP_YF_IMPLICIT, "implicit"}, {CP_YF_STATICDUR, "static-duration"},
        {CP_YF_THREADDUR, "thread-duration"}, {CP_YF_INLINE, "inline"},
    };
    for (ptrdiff_t i = 0; i < countof(f); i++) {
        if (s->flags & f[i].bit) { putchar(' '); outs(f[i].name); }
    }
}

static int inmainfile(cp_unit *u, cp_sym *s)
{
    int32_t t = symtok(s, 0);
    if (t < 0 || t >= u->ntoks) return 0;
    return u->toks[t].file == 0;
}

// --- modes

static void showdiags(cp_unit *u)
{
    for (cp_error *e = u->errors; e; e = e->next) {
        fprintf(stderr, "%.*s:%d:%d: %s: %.*s\n", (int)e->file.len, e->file.data,
                e->line, e->col, e->warning ? "warning" : "error", (int)e->msg.len, e->msg.data);
    }
}

static void modesymbols(cp_arena *a, cp_unit *u, int all)
{
    for (cp_sym *s = u->syms; s; s = s->next) {
        if (!all && !inmainfile(u, s)) continue;
        cp_arena scratch = *a;
        printf("%-10s %-24.*s %-8s ", symkind(s->kind), (int)s->name.len, s->name.data, linkname(s->linkage));
        out(tname(&scratch, u, s->ty));
        printf("  refs=%d", s->nrefs);
        symflags(s);
        outs("  ");
        loc(u, symtok(s, 1) >= 0 ? symtok(s, 1) : symtok(s, 0));
        if (s->func) { outs("  in "); out(s->func->name); }
        putchar('\n');
    }
}

static void modexref(cp_unit *u, Str name)
{
    for (cp_sym *s = u->syms; s; s = s->next) {
        if (!equals(s->name, name)) continue;
        printf("%s %.*s", symkind(s->kind), (int)s->name.len, s->name.data);
        if (s->func) { outs(" (in "); out(s->func->name); outs(")"); }
        putchar('\n');
        for (cp_symref *r = s->decls; r; r = r->next) {
            outs("  decl  "); loc(u, r->node ? r->node->tok : -1); putchar('\n');
        }
        if (s->def) { outs("  def   "); loc(u, s->def->tok); putchar('\n'); }
        for (cp_symref *r = s->refs; r; r = r->next) {
            char *what = (r->flags & CP_RF_CALL) ? "call " : (r->flags & CP_RF_ADDR) ? "addr " :
                         (r->flags & CP_RF_WRITE) && (r->flags & CP_RF_READ) ? "rw   " :
                         (r->flags & CP_RF_WRITE) ? "write" : (r->flags & CP_RF_READ) ? "read " :
                         "noeval";  // inside sizeof, typeof, _Generic
            printf("  %s ", what);
            if (r->node) loc(u, r->node->tok); else outs("(type use)");
            putchar('\n');
        }
    }
}

static void modecallgraph(cp_unit *u, int dot, int all)
{
    if (dot) outs("digraph calls {\n");
    for (cp_sym *s = u->syms; s; s = s->next) {
        if (s->kind != CP_SYM_FUNC || (!all && !inmainfile(u, s))) continue;
        // unique callees in order
        for (cp_symref *c = s->calls; c; c = c->next) {
            int seen = 0;
            for (cp_symref *d = s->calls; d != c; d = d->next) seen |= d->sym == c->sym;
            if (seen) continue;
            if (dot) printf("  \"%.*s\" -> \"%.*s\";\n", (int)s->name.len, s->name.data,
                            (int)c->sym->name.len, c->sym->name.data);
            else printf("%.*s -> %.*s\n", (int)s->name.len, s->name.data,
                        (int)c->sym->name.len, c->sym->name.data);
        }
    }
    if (dot) outs("}\n");
}

static void layoutmembers(cp_arena *a, cp_unit *u, cp_type *t, long long base, int depth, long long *cursor)
{
    for (cp_node *decl = t->members; decl; decl = decl->next) {
        if (decl->kind != CP_D_DECL) continue;
        if (!decl->list) {
            cp_type *st = decl->type ? decl->type->sem : 0;
            if (st && (st->kind == CP_T_STRUCT || st->kind == CP_T_UNION) && !decl->type->name.len) {
                printf("%*s%6lld  %-5lld (anonymous %s)\n", depth*2, "", base + decl->offset, (long long)st->size,
                       st->kind == CP_T_STRUCT ? "struct" : "union");
                long long c = base + decl->offset;
                layoutmembers(a, u, st, base + decl->offset, depth+1, &c);
                if (t->kind == CP_T_STRUCT) *cursor = base + decl->offset + st->size;
            }
            continue;
        }
        for (cp_node *d = decl->list; d; d = d->next) {
            cp_arena scratch = *a;
            long long off = base + d->offset;
            if (t->kind == CP_T_STRUCT && d->bitwidth < 0 && off > *cursor) {
                printf("%*s%6lld  %-5lld <padding>\n", depth*2, "", *cursor, off - *cursor);
            }
            if (d->bitwidth >= 0) {
                printf("%*s%6lld  :%-4d %.*s  ", depth*2, "", (long long)(base*8 + d->bitoffset)/8,
                       d->bitwidth, (int)d->text.len, d->text.data);
                printf("(bit %lld) ", (long long)((base*8 + d->bitoffset) % 8));
            } else {
                long long sz = d->ty ? d->ty->size : -1;
                printf("%*s%6lld  %-5lld %.*s  ", depth*2, "", off, sz, (int)d->text.len, d->text.data);
            }
            out(tname(&scratch, u, d->ty));
            putchar('\n');
            if (t->kind == CP_T_STRUCT) {
                long long end = d->bitwidth >= 0 ? (base*8 + d->bitoffset + d->bitwidth + 7)/8
                                                 : off + (d->ty && d->ty->size > 0 ? d->ty->size : 0);
                if (end > *cursor) *cursor = end;
            }
        }
    }
}

static void modelayout(cp_arena *a, cp_unit *u, int all)
{
    for (cp_sym *s = u->syms; s; s = s->next) {
        if (s->kind != CP_SYM_TAG || !s->ty || (!all && !inmainfile(u, s))) continue;
        cp_type *t = s->ty;
        if (t->kind == CP_T_ENUM || (t->flags & CP_TF_INCOMPLETE)) continue;
        printf("%s %.*s  size=%lld align=%lld%s\n", t->kind == CP_T_STRUCT ? "struct" : "union",
               (int)t->name.len, t->name.data, (long long)t->size, (long long)t->align,
               (t->flags & CP_TF_FLEX) ? " flexible" : "");
        long long cursor = 0;
        layoutmembers(a, u, t, 0, 1, &cursor);
        if (t->kind == CP_T_STRUCT && cursor < t->size) {
            printf("  %6lld  %-5lld <tail padding>\n", cursor, (long long)t->size - cursor);
        }
    }
}

static void modemacros(cp_unit *u, int all)
{
    for (cp_macro *m = u->macros; m; m = m->next) {
        if (!all && (m->flags & CP_MF_PREDEF)) continue;
        if (!all && m->file != 0) continue;
        out(m->name);
        if (m->flags & CP_MF_FUNCTION) {
            putchar('(');
            for (int32_t i = 0; i < m->nparams; i++) {
                if (i) outs(", ");
                out(m->params[i]);
            }
            putchar(')');
        }
        putchar(' ');
        for (int32_t i = 0; i < m->nbody; i++) {
            if (i && (m->body[i].flags & CP_TF_SPACE)) putchar(' ');
            out(m->body[i].text);
        }
        printf("    [uses=%d", m->nuses);
        if (m->flags & CP_MF_UNDEF) outs(" undef");
        if (m->file >= 0 && m->file < u->nfiles) {
            outs(" ");
            out(u->files[m->file].path);
            printf(":%d", m->line);
        }
        outs("]\n");
    }
}

static void modeincludes(cp_unit *u)
{
    for (int32_t i = 0; i < u->nincludes; i++) {
        cp_include *in = &u->includes[i];
        int depth = 0;
        for (int32_t f = in->from; f > 0 && f < u->nfiles && depth < 64; f = u->files[f].parent) depth++;
        printf("%*s", depth*2, "");
        out(in->spelled);
        if (in->file < 0) outs("  NOT FOUND");
        else { outs("  "); out(u->files[in->file].path); }
        if (in->skipped) outs("  (skipped)");
        putchar('\n');
    }
}

static void modejson(cp_arena *a, cp_unit *u, int all)
{
    outs("{\n\"files\":[");
    for (int32_t i = 0; i < u->nfiles; i++) {
        if (i) putchar(',');
        outs("\n {\"path\":");
        outjson(u->files[i].path);
        printf(",\"system\":%s,\"parent\":%d}", u->files[i].system ? "true" : "false", u->files[i].parent);
    }
    outs("],\n\"diagnostics\":[");
    int first = 1;
    for (cp_error *e = u->errors; e; e = e->next) {
        if (!first) putchar(',');
        first = 0;
        outs("\n {\"severity\":");
        outs(e->warning ? "\"warning\"" : "\"error\"");
        outs(",\"file\":");
        outjson(e->file);
        printf(",\"line\":%d,\"col\":%d,\"message\":", e->line, e->col);
        outjson(e->msg);
        putchar('}');
    }
    outs("],\n\"symbols\":[");
    first = 1;
    for (cp_sym *s = u->syms; s; s = s->next) {
        if (!all && !inmainfile(u, s)) continue;
        cp_arena scratch = *a;
        if (!first) putchar(',');
        first = 0;
        outs("\n {\"name\":");
        outjson(s->name);
        printf(",\"kind\":\"%s\",\"linkage\":\"%s\",\"type\":", symkind(s->kind), linkname(s->linkage));
        outjson(tname(&scratch, u, s->ty));
        if (s->ty && s->ty->size >= 0 && s->kind != CP_SYM_FUNC) printf(",\"size\":%lld", (long long)s->ty->size);
        outs(",\"flags\":[");
        static struct { int32_t bit; char *name; } f[] = {
            {CP_YF_DEFINED, "defined"}, {CP_YF_READ, "read"}, {CP_YF_WRITTEN, "written"},
            {CP_YF_ADDRTAKEN, "addrtaken"}, {CP_YF_CALLED, "called"}, {CP_YF_IMPLICIT, "implicit"},
            {CP_YF_STATICDUR, "static"}, {CP_YF_THREADDUR, "thread"}, {CP_YF_INLINE, "inline"},
        };
        int ff = 1;
        for (ptrdiff_t i = 0; i < countof(f); i++) {
            if (s->flags & f[i].bit) { if (!ff) putchar(','); ff = 0; printf("\"%s\"", f[i].name); }
        }
        outs("],\"decl\":");
        jsonloc(u, symtok(s, 0));
        outs(",\"def\":");
        jsonloc(u, s->def ? s->def->tok : -1);
        if (s->func) { outs(",\"function\":"); outjson(s->func->name); }
        if (s->decl && s->decl->tok >= 0 && s->decl->tok < u->ntoks && u->toks[s->decl->tok].doc.len) {
            outs(",\"doc\":");
            outjson(u->toks[s->decl->tok].doc);
        }
        if (s->kind == CP_SYM_ENUMCONST && s->decl) printf(",\"value\":%lld", (long long)s->decl->cval);
        printf(",\"nrefs\":%d,\"refs\":[", s->nrefs);
        int rf = 1;
        for (cp_symref *r = s->refs; r; r = r->next) {
            if (!r->node) continue;
            if (!rf) putchar(',');
            rf = 0;
            outs("{\"at\":");
            jsonloc(u, r->node->tok);
            outs(",\"use\":\"");
            outs((r->flags & CP_RF_CALL) ? "call" : (r->flags & CP_RF_ADDR) ? "addr" :
                 ((r->flags & CP_RF_WRITE) && (r->flags & CP_RF_READ)) ? "readwrite" :
                 (r->flags & CP_RF_WRITE) ? "write" : "read");
            outs("\"}");
        }
        putchar(']');
        if (s->kind == CP_SYM_FUNC) {
            outs(",\"calls\":[");
            int cf = 1;
            for (cp_symref *c = s->calls; c; c = c->next) {
                int seen = 0;
                for (cp_symref *d = s->calls; d != c; d = d->next) seen |= d->sym == c->sym;
                if (seen) continue;
                if (!cf) putchar(',');
                cf = 0;
                outjson(c->sym->name);
            }
            outs("],\"callers\":[");
            cf = 1;
            for (cp_symref *c = s->callers; c; c = c->next) {
                int seen = 0;
                for (cp_symref *d = s->callers; d != c; d = d->next) seen |= d->sym == c->sym;
                if (seen) continue;
                if (!cf) putchar(',');
                cf = 0;
                outjson(c->sym->name);
            }
            putchar(']');
        }
        if (s->kind == CP_SYM_TAG && s->ty && !(s->ty->flags & CP_TF_INCOMPLETE) && s->ty->kind != CP_T_ENUM) {
            printf(",\"align\":%lld,\"members\":[", (long long)s->ty->align);
            int mf = 1;
            for (cp_node *decl = s->ty->members; decl; decl = decl->next) {
                if (decl->kind != CP_D_DECL) continue;
                for (cp_node *d = decl->list; d; d = d->next) {
                    cp_arena sc2 = scratch;
                    if (!mf) putchar(',');
                    mf = 0;
                    outs("{\"name\":");
                    outjson(d->text);
                    outs(",\"type\":");
                    outjson(tname(&sc2, u, d->ty));
                    printf(",\"offset\":%lld", (long long)d->offset);
                    if (d->bitwidth >= 0) printf(",\"bitoffset\":%lld,\"bitwidth\":%d", (long long)d->bitoffset, d->bitwidth);
                    putchar('}');
                }
            }
            putchar(']');
        }
        putchar('}');
    }
    outs("],\n\"macros\":[");
    first = 1;
    for (cp_macro *m = u->macros; m; m = m->next) {
        if (!all && (m->flags & CP_MF_PREDEF)) continue;
        if (!first) putchar(',');
        first = 0;
        outs("\n {\"name\":");
        outjson(m->name);
        if (m->flags & CP_MF_FUNCTION) {
            outs(",\"params\":[");
            for (int32_t i = 0; i < m->nparams; i++) { if (i) putchar(','); outjson(m->params[i]); }
            putchar(']');
        }
        outs(",\"body\":\"");
        for (int32_t i = 0; i < m->nbody; i++) {
            Str t = m->body[i].text;
            if (i && (m->body[i].flags & CP_TF_SPACE)) putchar(' ');
            for (ptrdiff_t j = 0; j < t.len; j++) {
                char c = t.data[j];
                if (c == '"' || c == '\\') putchar('\\');
                if ((unsigned char)c < 0x20) { printf("\\u%04x", c); continue; }
                putchar(c);
            }
        }
        printf("\",\"uses\":%d,\"undefined\":%s", m->nuses, (m->flags & CP_MF_UNDEF) ? "true" : "false");
        if (m->file >= 0 && m->file < u->nfiles) {
            outs(",\"file\":");
            outjson(u->files[m->file].path);
            printf(",\"line\":%d", m->line);
        }
        if (m->doc.len) { outs(",\"doc\":"); outjson(m->doc); }
        putchar('}');
    }
    outs("],\n\"includes\":[");
    for (int32_t i = 0; i < u->nincludes; i++) {
        cp_include *in = &u->includes[i];
        if (i) putchar(',');
        printf("\n {\"from\":%d,\"line\":%d,\"file\":%d,\"spelled\":", in->from, in->line, in->file);
        outjson(in->spelled);
        printf(",\"skipped\":%s}", in->skipped ? "true" : "false");
    }
    outs("],\n\"skipped\":[");
    for (int32_t i = 0; i < u->nskips; i++) {
        if (i) putchar(',');
        printf("{\"file\":%d,\"from\":%d,\"to\":%d}", u->skips[i].file, u->skips[i].beg, u->skips[i].end);
    }
    outs("],\n\"pragmas\":[");
    for (ptrdiff_t i = 0; i < u->npragmas; i++) {
        if (i) putchar(',');
        printf("{\"file\":%d,\"line\":%d,\"text\":", u->pragmas[i].file, u->pragmas[i].line);
        outjson(u->pragmas[i].text);
        putchar('}');
    }
    printf("],\n\"stats\":{\"tokens\":%lld,\"expansions\":%d,\"comments\":%d,\"errors\":%d,\"warnings\":%d}\n}\n",
           (long long)u->ntoks, u->nexpansions, u->ncomments, u->nerrors, u->nwarnings);
}


// --- full syntax tree as JSON

static void roles(int32_t kind, char **r)
{
    // names for a, b, c, d, list
    r[0] = "a"; r[1] = "b"; r[2] = "c"; r[3] = "d"; r[4] = "items";
    switch (kind) {
    case CP_E_CALL:     r[0] = "callee"; r[4] = "args"; break;
    case CP_E_INDEX:    r[0] = "array"; r[1] = "index"; break;
    case CP_E_MEMBER: case CP_E_POSTFIX: case CP_E_UNARY: case CP_E_CAST:
    case CP_E_SIZEOF: case CP_E_ALIGNOF: case CP_E_STMTEXPR: case CP_S_EXPR:
    case CP_S_RETURN: case CP_S_GOTOPTR:
        r[0] = "operand"; break;
    case CP_E_COMPOUND: r[0] = "init"; break;
    case CP_E_BINARY: case CP_E_ASSIGN: case CP_E_COMMA:
        r[0] = "lhs"; r[1] = "rhs"; break;
    case CP_E_COND:     r[0] = "cond"; r[1] = "then"; r[2] = "else"; break;
    case CP_E_GENERIC:  r[0] = "control"; r[4] = "assocs"; break;
    case CP_E_ASSOC:    r[0] = "expr"; break;
    case CP_E_INITLIST: r[4] = "items"; break;
    case CP_E_INIT:     r[0] = "value"; r[1] = "designators"; break;
    case CP_E_DINDEX:   r[0] = "index"; r[1] = "last"; break;
    case CP_E_BLOCK:    r[0] = "body"; break;
    case CP_S_COMPOUND: r[4] = "items"; break;
    case CP_S_IF:       r[0] = "cond"; r[1] = "then"; r[2] = "else"; break;
    case CP_S_SWITCH: case CP_S_WHILE: case CP_S_DO:
        r[0] = "cond"; r[1] = "body"; break;
    case CP_S_FOR:      r[0] = "cond"; r[1] = "body"; r[2] = "step"; r[3] = "init"; break;
    case CP_S_LABEL:    r[0] = "stmt"; break;
    case CP_S_CASE:     r[0] = "value"; r[1] = "last"; r[2] = "stmt"; break;
    case CP_S_DEFAULT:  r[2] = "stmt"; break;
    case CP_S_ASM:      r[0] = "template"; r[1] = "outputs"; r[2] = "inputs"; r[3] = "clobbers"; r[4] = "labels"; break;
    case CP_S_ASMOPERAND: r[0] = "expr"; r[1] = "constraint"; break;
    case CP_S_TRY:      r[0] = "body"; r[1] = "handler"; break;
    case CP_S_EXCEPT:   r[0] = "filter"; r[1] = "body"; break;
    case CP_S_FINALLY:  r[1] = "body"; break;
    case CP_D_DECL:     r[4] = "declarators"; break;
    case CP_D_DECLARATOR: r[0] = "init"; r[1] = "bitwidth"; break;
    case CP_D_FUNC:     r[0] = "body"; r[1] = "krparams"; r[4] = "declarator"; break;
    case CP_D_ENUMERATOR: r[0] = "value"; break;
    case CP_D_STATICASSERT: r[0] = "cond"; r[1] = "message"; break;
    }
}

static int ischainkind(int32_t kind, int which)
{
    // fields that hold lists linked through next
    if (which == 4) return 1;
    if (kind == CP_E_INIT && which == 1) return 1;
    if (kind == CP_D_FUNC && which == 1) return 1;
    if (kind == CP_S_ASM && which >= 1) return 1;
    return 0;
}

static void astnode(cp_arena *a, cp_unit *u, cp_node *n, int depth);

static void astlist(cp_arena *a, cp_unit *u, cp_node *n, int depth)
{
    putchar('[');
    for (int first = 1; n; n = n->next, first = 0) {
        if (!first) putchar(',');
        astnode(a, u, n, depth);
    }
    putchar(']');
}

static void astnode(cp_arena *a, cp_unit *u, cp_node *n, int depth)
{
    if (!n || depth > 4000) { outs("null"); return; }
    cp_arena scratch = *a;
    outs("{\"kind\":");
    outjson(cp_nodename(n->kind));
    outs(",\"loc\":");
    jsonloc(u, n->tok);
    if (n->endtok > n->tok + 1) { outs(",\"end\":"); jsonloc(u, n->endtok - 1); }
    if (n->op) { outs(",\"op\":"); outjson(cp_tokname(n->op)); }
    if (n->text.len) { outs(",\"text\":"); outjson(n->text); }
    if (n->ty) { outs(",\"type\":"); outjson(tname(&scratch, u, n->ty)); }
    if (n->isconst == 1) printf(",\"value\":%lld", (long long)n->cval);
    if (n->isconst == 2) {
        double v = n->fval;
        if (v != v) outs(",\"fvalue\":\"nan\"");
        else if (v > 1.7976931348623157e308) outs(",\"fvalue\":\"inf\"");
        else if (v < -1.7976931348623157e308) outs(",\"fvalue\":\"-inf\"");
        else printf(",\"fvalue\":%.17g", v);
    }
    if (n->kind == CP_E_STRING) { outs(",\"string\":"); outjson(n->sval); }
    if (n->eflags & CP_EF_LVALUE) outs(",\"lvalue\":true");
    if (n->eflags & CP_EF_SIDEEFFECT) outs(",\"sideeffects\":true");
    if (n->ref) { outs(",\"refers\":"); jsonloc(u, n->ref->tok); }
    if (n->sym) printf(",\"symbol\":\"%s\"", symkind(n->sym->kind));
    if (n->kind == CP_D_DECLARATOR) {
        if (n->bitwidth >= 0) printf(",\"bitoffset\":%lld,\"bits\":%d", (long long)n->bitoffset, n->bitwidth);
        else if (n->offset) printf(",\"offset\":%lld", (long long)n->offset);
    }
    if (n->kind == CP_D_DECL || n->kind == CP_D_FUNC) {
        static struct { int32_t bit; char *name; } sc[] = {
            {CP_SC_TYPEDEF, "typedef"}, {CP_SC_EXTERN, "extern"}, {CP_SC_STATIC, "static"},
            {CP_SC_AUTO, "auto"}, {CP_SC_REGISTER, "register"}, {CP_SC_THREAD_LOCAL, "thread_local"},
            {CP_SC_CONSTEXPR, "constexpr"}, {CP_SC_INLINE, "inline"}, {CP_SC_NORETURN, "noreturn"},
        };
        outs(",\"storage\":[");
        for (int i = 0, f = 1; i < (int)countof(sc); i++) {
            if (n->flags & sc[i].bit) { if (!f) putchar(','); f = 0; printf("\"%s\"", sc[i].name); }
        }
        putchar(']');
        if (n->type) {
            cp_arena s2 = *a;
            outs(",\"specifiers\":");
            outjson(tname(&s2, u, n->type->sem));
            if (n->type->flags & CP_TF_BODY) {
                cp_type *t = n->type->sem;
                outs(",\"defines\":{\"kind\":");
                outs(t->kind == CP_T_STRUCT ? "\"struct\"" : t->kind == CP_T_UNION ? "\"union\"" : "\"enum\"");
                outs(",\"name\":");
                outjson(t->name);
                printf(",\"size\":%lld,\"align\":%lld,\"members\":", (long long)t->size, (long long)t->align);
                astlist(a, u, n->type->members, depth+1);
                putchar('}');
            }
        }
    }
    if (n->attrs) {
        outs(",\"attributes\":[");
        for (cp_attr *at = n->attrs; at; at = at->next) {
            Str t = {0};
            if (at->beg < u->ntoks && at->end <= u->ntoks && at->end > at->beg) {
                char *b = u->toks[at->beg].text.data;
                cp_token *last = &u->toks[at->end-1];
                t.data = b;
                t.len = last->text.data + last->text.len - b;
                if (t.len < 0 || t.len > 4096) t = u->toks[at->beg].text;
            }
            outjson(t);
            if (at->next) putchar(',');
        }
        putchar(']');
    }
    if (n->tok >= 0 && n->tok < u->ntoks && u->toks[n->tok].macro) {
        outs(",\"macro\":");
        outjson(u->toks[n->tok].macro->name);
    }
    char *r[5];
    roles(n->kind, r);
    cp_node *kids[5] = {n->a, n->b, n->c, n->d, n->list};
    for (int i = 0; i < 5; i++) {
        if (!kids[i]) continue;
        printf(",\"%s\":", r[i]);
        if (ischainkind(n->kind, i)) astlist(a, u, kids[i], depth+1);
        else astnode(a, u, kids[i], depth+1);
    }
    if (n->kind == CP_D_DECLARATOR && n->type) {
        cp_type *ft = n->type;
        while (ft && ft->kind != CP_T_FUNC && ft->kind != CP_T_NONE &&
               (ft->kind == CP_T_POINTER || ft->kind == CP_T_ARRAY || ft->kind == CP_T_BLOCKPTR)) ft = ft->base;
        if (ft && ft->kind == CP_T_FUNC && ft->params) {
            outs(",\"params\":");
            astlist(a, u, ft->params, depth+1);
        }
    }
    if ((n->kind == CP_E_CAST || n->kind == CP_E_COMPOUND || n->kind == CP_E_SIZEOF ||
         n->kind == CP_E_ALIGNOF || n->kind == CP_E_TYPEARG || n->kind == CP_E_ASSOC) && n->type) {
        cp_arena s3 = *a;
        outs(",\"typename\":");
        outjson(tname(&s3, u, n->type->sem ? n->type->sem : n->type));
    }
    putchar('}');
}

static void modeast(cp_arena *a, cp_unit *u, int all)
{
    outs("{\"decls\":[");
    int first = 1;
    for (cp_node *d = u->decls; d; d = d->next) {
        if (!all && (d->tok < 0 || d->tok >= u->ntoks || u->toks[d->tok].file != 0)) continue;
        if (!first) outs(",\n");
        first = 0;
        astnode(a, u, d, 0);
    }
    outs("]}\n");
}

// Emit a C file that asserts, through __builtin_types_compatible_p and
// _Static_assert, that every type and layout we computed matches what
// the compiler thinks. Compile it with the same compiler and flags.
static int nameable(Str s)
{
    for (ptrdiff_t i = 0; i < s.len; i++) {
        if (s.data[i] == '<' || s.data[i] == '?') return 0;
    }
    return 1;
}

static void modetypecheck(cp_arena *a, cp_unit *u, Str printed)
{
    out(printed);
    outs("\n/* ---- cparse type checks ---- */\n");
    for (cp_sym *s = u->syms; s; s = s->next) {
        if (s->depth != 0 || !s->ty || (s->flags & CP_YF_IMPLICIT)) continue;
        cp_arena scratch = *a;
        if (s->kind == CP_SYM_TAG) {
            cp_type *t = s->ty;
            if ((t->flags & CP_TF_INCOMPLETE) || t->kind == CP_T_ENUM || !s->name.len) continue;
            // the tag may be shadowed later at file scope; skip duplicates
            int dup = 0;
            for (cp_sym *o = u->syms; o; o = o->next) {
                if (o != s && o->kind == CP_SYM_TAG && o->depth == 0 && equals(o->name, s->name)) dup = 1;
            }
            if (dup) continue;
            char *kw = t->kind == CP_T_STRUCT ? "struct" : "union";
            printf("_Static_assert(sizeof(%s %.*s) == %lld, \"size %.*s\");\n", kw, (int)s->name.len, s->name.data,
                   (long long)t->size, (int)s->name.len, s->name.data);
            printf("_Static_assert(_Alignof(%s %.*s) == %lld, \"align %.*s\");\n", kw, (int)s->name.len, s->name.data,
                   (long long)t->align, (int)s->name.len, s->name.data);
            for (cp_node *decl = t->members; decl; decl = decl->next) {
                if (decl->kind != CP_D_DECL) continue;
                for (cp_node *d = decl->list; d; d = d->next) {
                    if (!d->text.len || d->bitwidth >= 0) continue;
                    printf("_Static_assert(__builtin_offsetof(%s %.*s, %.*s) == %lld, \"offset %.*s.%.*s\");\n",
                           kw, (int)s->name.len, s->name.data, (int)d->text.len, d->text.data,
                           (long long)d->offset, (int)s->name.len, s->name.data, (int)d->text.len, d->text.data);
                }
            }
            continue;
        }
        if (s->kind != CP_SYM_VAR && s->kind != CP_SYM_FUNC && s->kind != CP_SYM_TYPEDEF &&
            s->kind != CP_SYM_ENUMCONST) continue;
        Str t = tname(&scratch, u, s->ty);
        if (!nameable(t)) continue;
        if (s->kind == CP_SYM_TYPEDEF) {
            printf("_Static_assert(__builtin_types_compatible_p(%.*s, %.*s), \"typedef %.*s\");\n",
                   (int)s->name.len, s->name.data, (int)t.len, t.data, (int)s->name.len, s->name.data);
            if (s->ty->size >= 0 && s->ty->kind != CP_T_FUNC && !(s->ty->flags & CP_TF_VLA)) {
                printf("_Static_assert(sizeof(%.*s) == %lld, \"sizeof %.*s\");\n", (int)s->name.len, s->name.data,
                       (long long)s->ty->size, (int)s->name.len, s->name.data);
            }
        } else if (s->kind == CP_SYM_ENUMCONST) {
            printf("_Static_assert(%.*s == %lld, \"value %.*s\");\n", (int)s->name.len, s->name.data,
                   (long long)s->decl->cval, (int)s->name.len, s->name.data);
            printf("_Static_assert(__builtin_types_compatible_p(__typeof__(%.*s), %.*s), \"type %.*s\");\n",
                   (int)s->name.len, s->name.data, (int)t.len, t.data, (int)s->name.len, s->name.data);
        } else {
            printf("_Static_assert(__builtin_types_compatible_p(__typeof__(%.*s), %.*s), \"type %.*s\");\n",
                   (int)s->name.len, s->name.data, (int)t.len, t.data, (int)s->name.len, s->name.data);
        }
    }
}

// --- roundtrip

static int roundtrip(cp_arena *a, cp_unit *u, Str name, cp_config *cfg, int verbose)
{
    Str p1 = cp_print(a, u);
    if (!p1.data) { fprintf(stderr, "roundtrip: out of memory printing\n"); return 1; }
    cp_config c2 = cfg ? *cfg : (cp_config){0};
    c2.nopp = 1;
    c2.predefs = S("");
    cp_unit *u2 = cp_parse(a, p1, name, &c2);
    if (!u2) { fprintf(stderr, "roundtrip: out of memory\n"); return 1; }
    Str p2 = cp_print(a, u2);
    int bad = !p2.data || !equals(p1, p2);
    if (u2->nerrors && !u->nerrors) {
        fprintf(stderr, "roundtrip: reparse has %d errors\n", u2->nerrors);
        if (verbose) showdiags(u2);
        bad = 1;
    }
    if (bad && getenv("CPARSE_RTDUMP")) {
        FILE *f = fopen("rt1.c", "wb"); fwrite(p1.data, 1, (size_t)p1.len, f); fclose(f);
        f = fopen("rt2.c", "wb"); if (p2.data) fwrite(p2.data, 1, (size_t)p2.len, f); fclose(f);
    }
    if (bad && p2.data) {
        // show the first difference
        ptrdiff_t i = 0;
        while (i < p1.len && i < p2.len && p1.data[i] == p2.data[i]) i++;
        ptrdiff_t ls = i;
        while (ls > 0 && p1.data[ls-1] != '\n') ls--;
        int line = 1;
        for (ptrdiff_t j = 0; j < i; j++) line += p1.data[j] == '\n';
        fprintf(stderr, "roundtrip: output differs at line %d\n", line);
        ptrdiff_t e1 = i, e2 = i;
        while (e1 < p1.len && p1.data[e1] != '\n') e1++;
        while (e2 < p2.len && p2.data[e2] != '\n') e2++;
        fprintf(stderr, "  first:  %.*s\n  second: %.*s\n", (int)(e1-ls), p1.data+ls, (int)(e2-ls), p2.data+ls);
    }
    return bad;
}

#ifdef __AFL_FUZZ_TESTCASE_LEN
// AFL++ persistent-mode harness (nullprogram.com/blog/2025/02/05/).
#include <unistd.h>
__AFL_FUZZ_INIT();

static Str fuzzread(void *ctx, Str path, cp_arena *a)
{
    (void)a;
    // every include gets a small header so include paths are exercised
    (void)path;
    return *(Str *)ctx;
}

int main(void)
{
#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
#endif
    ptrdiff_t cap = (ptrdiff_t)1 << 26;
    char *mem = malloc((size_t)cap);
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        cp_arena a = {mem, mem + cap};
        memset(mem, 0xa5, (size_t)cap / 16);
        // exact-size copy so ASan sees overreads
        char *src = malloc((size_t)len + 1);
        memcpy(src, buf, (size_t)len);
        Str hdr = S("#define HDR(x) x\nint hdr;\n");
        cp_config cfg = {0};
        cfg.readfile = fuzzread;
        cfg.ctx = &hdr;
        cfg.predefs = S("#define __GNUC__ 4\n#define __x86_64__ 1\n");
        // vary the target model and trigraphs by input length, so every
        // input still reproduces: Microsoft layout, ILP32, phase 1
        static const int32_t targets[] = {CP_TARGET_LP64, CP_TARGET_LLP64, CP_TARGET_ILP32};
        cfg.target = targets[len % 3];
        cfg.trigraphs = (len / 3) % 2;
        cp_unit *u = cp_parse(&a, (Str){src, len}, S("fuzz.c"), &cfg);
        if (u && !u->oom) {
            cp_arena b = a;
            cp_dump(&b, u);
            cp_print_tokens(&b, u);
            if (!u->nerrors) {
                // the printer must produce a fixed point
                Str p1 = cp_print(&a, u);
                if (p1.data) {
                    cp_config c2 = {0};
                    c2.nopp = 1;
                    c2.target = cfg.target;
                    cp_unit *u2 = cp_parse(&a, p1, S("fuzz.c"), &c2);
                    if (u2 && !u2->oom) {
                        Str p2 = cp_print(&a, u2);
                        if (p2.data && (u2->nerrors || !equals(p1, p2))) {
                            __builtin_trap();
                        }
                    }
                }
            }
        }
        free(src);
    }
    free(mem);
    return 0;
}

#else

#include <sys/mman.h>

static void usage(void)
{
    fputs(
"usage: cparse [mode] [options] file...\n"
"modes: --check (default) -E --print --dump --symbols --xref NAME\n"
"       --callgraph [--dot] --layout --macros --includes --json --ast\n"
"       --roundtrip --typecheck\n"
"options: -I DIR -isystem DIR -iquote DIR -D NAME[=VAL] -U NAME\n"
"         --cc CC (default cc, 'none' to skip) --std STD --target lp64|llp64|ilp32\n"
"         -std=.. -O.. -m.. -f.. are passed to CC when asking for predefined macros\n"
"         -trigraphs, or an ISO -std= before c23, translates trigraphs\n"
"         -include FILE --nostdinc (no system include directories)\n"
"         --nopp (input already preprocessed) --all (include system headers\n"
"         in --symbols/--layout/--macros/--json) --version\n", stderr);
}

int main(int argc, char **argv)
{
    ptrdiff_t cap = (ptrdiff_t)1 << 34;  // reserve 16 GiB of address space
    char *mem = mmap(0, (size_t)cap, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        cap = (ptrdiff_t)1 << 30;
        mem = malloc((size_t)cap);
        if (!mem) { fputs("cparse: out of memory\n", stderr); return 2; }
    }
    cp_arena perm = {mem, mem + cap};

    Strs quote = {0}, inc = {0}, sys = {0}, files = {0};
    Str defines = {0};
    char *mode = "--check";
    char *cc = "cc";
    char *std = 0;
    Str xref = {0};
    int dot = 0, all = 0, nostdinc = 0;
    cp_config cfg = {0};
    cfg.readfile = readfile;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        Str next = i+1 < argc ? cstr(argv[i+1]) : (Str){0};
        if (!strcmp(a, "-I") && next.data) { push(&perm, &inc, next); i++; }
        else if (!strncmp(a, "-I", 2) && a[2]) push(&perm, &inc, cstr(a+2));
        else if (!strcmp(a, "-isystem") && next.data) { push(&perm, &sys, next); i++; }
        else if (!strcmp(a, "-iquote") && next.data) { push(&perm, &quote, next); i++; }
        else if (!strncmp(a, "-D", 2)) {
            char *d = a[2] ? a+2 : (i+1 < argc ? argv[++i] : "");
            char *eq = strchr(d, '=');
            Str line = S("#define ");
            if (eq) {
                line = concat(&perm, line, (Str){d, eq - d});
                line = concat(&perm, line, S(" "));
                line = concat(&perm, line, cstr(eq+1));
            } else {
                line = concat(&perm, line, cstr(d));
                line = concat(&perm, line, S(" 1"));
            }
            defines = concat(&perm, defines, concat(&perm, line, S("\n")));
        } else if (!strncmp(a, "-U", 2)) {
            char *d = a[2] ? a+2 : (i+1 < argc ? argv[++i] : "");
            defines = concat(&perm, defines, concat(&perm, concat(&perm, S("#undef "), cstr(d)), S("\n")));
        } else if (!strcmp(a, "--cc") && next.data) { cc = argv[++i]; }
        else if (!strcmp(a, "--std") && next.data) {
            std = (char *)concat(&perm, S("-std="), next).data;
            i++;
        } else if (!strncmp(a, "-std=", 5) || !strncmp(a, "-O", 2) || !strncmp(a, "-m", 2) ||
                   (!strncmp(a, "-f", 2) && a[2])) {
            // compiler flags that change predefined macros
            Str prev = std ? cstr(std) : S("");
            std = concat(&perm, concat(&perm, prev, S(" ")), cstr(a)).data;
            if (!strncmp(a, "-std=", 5)) {
                // ISO modes before C23 have trigraphs, GNU modes do not
                char *v = a + 5;
                cfg.trigraphs = (v[0] == 'c' && strncmp(v, "c2", 2)) ||
                                (!strncmp(v, "iso9899:", 8) && strcmp(v, "iso9899:2024"));
            }
        } else if (!strcmp(a, "-trigraphs")) { cfg.trigraphs = 1; }
        else if (!strcmp(a, "--target") && next.data) {
            i++;
            if (equals(next, S("llp64"))) cfg.target = CP_TARGET_LLP64;
            else if (equals(next, S("ilp32"))) cfg.target = CP_TARGET_ILP32;
            else cfg.target = CP_TARGET_LP64;
        } else if (!strcmp(a, "--nopp")) { cfg.nopp = 1; }
        else if (!strcmp(a, "--nostdinc") || !strcmp(a, "-nostdinc")) { nostdinc = 1; }
        else if (!strcmp(a, "--dot")) { dot = 1; }
        else if (!strcmp(a, "--all")) { all = 1; }
        else if (!strcmp(a, "--xref") && next.data) { mode = a; xref = next; i++; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) { puts("cparse " CPARSE_VERSION); return 0; }
        else if ((a[0] == '-' && a[1] == '-') || !strcmp(a, "-E")) { mode = a; }
        else if (!strcmp(a, "-include") && next.data) {
            // as if the main file began with #include "FILE"
            char *full = realpath(argv[++i], 0);
            Str path = full ? cstr(full) : next;
            defines = concat(&perm, defines, S("#include \""));
            defines = concat(&perm, defines, copystr(&perm, path));
            defines = concat(&perm, defines, S("\"\n"));
            free(full);
        }
        else if (!strcmp(a, "-o") || !strcmp(a, "-x")) { i++; }
        else if (a[0] == '-' && a[1]) { /* other compiler flags: ignored */ }
        else push(&perm, &files, cstr(a));
    }
    if (!files.len) { usage(); return 1; }

    Str predefs = {0};
    if (!cfg.nopp && strcmp(cc, "none")) {
        Strs ccdirs = {0};
        querycc(&perm, cc, std, &ccdirs, &predefs);
        predefs = concat(&perm, predefs, ccasserts(&perm, predefs));
        for (ptrdiff_t i = 0; !nostdinc && i < ccdirs.len; i++) push(&perm, &sys, ccdirs.data[i]);
    }
    cfg.predefs = concat(&perm, predefs, defines);
    cfg.quotedirs = quote.data; cfg.nquotedirs = (int32_t)quote.len;
    cfg.includedirs = inc.data; cfg.nincludedirs = (int32_t)inc.len;
    cfg.systemdirs = sys.data; cfg.nsystemdirs = (int32_t)sys.len;

    int status = 0;
    for (ptrdiff_t f = 0; f < files.len; f++) {
        cp_arena a = perm;
        Str name = files.data[f];
        Str src = equals(name, S("-")) ? (Str){0} : readfile(0, name, &a);
        if (equals(name, S("-"))) {
            src.data = a.beg;
            src.len = (ptrdiff_t)fread(src.data, 1, (size_t)(a.end - a.beg - 1), stdin);
            src.data[src.len] = 0;
            a.beg += src.len + 1;
        }
        if (!src.data) {
            fprintf(stderr, "cparse: cannot read %s\n", name.data);
            status = 1;
            continue;
        }
        cp_unit *u = cp_parse(&a, src, name, &cfg);
        if (!u) { fputs("cparse: out of memory\n", stderr); return 2; }
        if (u->oom) fputs("cparse: out of memory (partial results)\n", stderr);
        showdiags(u);
        if (u->nerrors) status = 1;

        if (!strcmp(mode, "--check")) {
            int ndecl = 0;
            for (cp_node *d = u->decls; d; d = d->next) ndecl++;
            int nsym = 0;
            for (cp_sym *s = u->syms; s; s = s->next) nsym++;
            int nmac = 0;
            for (cp_macro *m = u->macros; m; m = m->next) nmac++;
            fprintf(stderr, "%s: %lld tokens, %d files, %d macros, %d expansions, %d declarations, "
                    "%d symbols, %d errors, %d warnings\n", name.data, (long long)u->ntoks - 1,
                    u->nfiles, nmac, u->nexpansions, ndecl, nsym, u->nerrors, u->nwarnings);
        } else if (!strcmp(mode, "-E")) {
            out(cp_print_tokens(&a, u));
        } else if (!strcmp(mode, "--print")) {
            out(cp_print(&a, u));
        } else if (!strcmp(mode, "--dump")) {
            out(cp_dump(&a, u));
        } else if (!strcmp(mode, "--symbols")) {
            modesymbols(&a, u, all);
        } else if (!strcmp(mode, "--xref")) {
            modexref(u, xref);
        } else if (!strcmp(mode, "--callgraph")) {
            modecallgraph(u, dot, all);
        } else if (!strcmp(mode, "--layout")) {
            modelayout(&a, u, all);
        } else if (!strcmp(mode, "--macros")) {
            modemacros(u, all);
        } else if (!strcmp(mode, "--includes")) {
            modeincludes(u);
        } else if (!strcmp(mode, "--ast")) {
            modeast(&a, u, all);
        } else if (!strcmp(mode, "--json")) {
            modejson(&a, u, all);
        } else if (!strcmp(mode, "--roundtrip")) {
            if (roundtrip(&a, u, name, &cfg, 1)) status = 1;
            else fprintf(stderr, "%s: roundtrip ok\n", name.data);
        } else if (!strcmp(mode, "--typecheck")) {
            modetypecheck(&a, u, cp_print_checked(&a, u));
        } else {
            fprintf(stderr, "cparse: unknown option %s\n", mode);
            usage();
            return 1;
        }
        fflush(stdout);
    }
    return status;
}
#endif

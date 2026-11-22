// Checks the convenience and hosted helpers of cparse.h against
// tests/cases/api_input.c. Build and run from the repository root:
//     cc -o api tests/api.c && ./api
// Prints each failed check and exits nonzero if any failed.
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "../cparse.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int ok, char *what, cp_str got)
{
    if (ok) return;
    failures++;
    printf("FAIL %s: got \"%.*s\"\n", what, (int)got.len, got.data ? got.data : "");
}

static int eq(cp_str s, char *want)
{
    return s.data && s.len == (ptrdiff_t)strlen(want) && !memcmp(s.data, want, (size_t)s.len);
}

static void expectstr(cp_str got, char *want, char *what)
{
    expect(eq(got, want), what, got);
}

static void expectnum(long long got, long long want, char *what)
{
    if (got == want) return;
    failures++;
    printf("FAIL %s: got %lld, want %lld\n", what, got, want);
}

typedef struct { cp_unit *u; int calls, idents; } counts;

// counts nodes in the main file; skips the system headers' inline code
static int32_t count(void *ctx, cp_node *n, cp_node *parent)
{
    (void)parent;
    counts *c = ctx;
    if (!cp_inmainfile(c->u, n)) return 0;
    c->calls += n->kind == CP_E_CALL;
    c->idents += n->kind == CP_E_IDENT;
    return 1;
}

int main(void)
{
    cp_arena a = cp_newarena((ptrdiff_t)1 << 28);
    char *args[] = {"-Itests/cases"};
    cp_unit *u = cp_parsefile(&a, "tests/cases/api_input.c", 1, args);
    if (!u) { puts("FAIL cp_parsefile"); return 1; }
    expectnum(u->nerrors, 0, "no errors");

    // lookups
    cp_sym *area = cp_findsym(u, CP_STR("area"));
    cp_sym *add = cp_findsym(u, CP_STR("add"));
    cp_sym *pt = cp_findtag(u, CP_STR("point"));
    cp_sym *shape = cp_findtag(u, CP_STR("shape"));
    cp_sym *color = cp_findtag(u, CP_STR("color"));
    expectnum(area && add && pt && shape && color, 1, "lookups");
    if (!(area && add && pt && shape && color)) return 1;
    expectnum(cp_findsym(u, CP_STR("nonexistent")) == 0, 1, "missing symbol");
    cp_macro *sq = cp_macrodef(u, CP_STR("SQUARE"));
    expectnum(sq && sq->nparams == 1, 1, "cp_macrodef");

    // signatures and declarations
    expectstr(cp_signature(&a, u, area), "int area(struct shape *s, int scale, ...)", "signature area");
    expectstr(cp_signature(&a, u, add), "static int add(int a, int b)", "signature add");
    cp_sym *cmp = cp_findsym(u, CP_STR("cmp_fn"));
    expectstr(cp_declstr(&a, u, cmp->ty, CP_STR("f")), "int (*f)(const void *, const void *)", "declstr");
    expectstr(cp_typename(&a, u, area->ty), "int(struct shape *, int, ...)", "typename");

    // locations, source text, docs
    expectstr(cp_locstr(&a, u, add->def), "tests/cases/api_input.c:23:1", "locstr");
    cp_loc l = cp_locof(u, area->def);
    expectnum(l.line, 25, "locof line");
    expectnum(cp_inmainfile(u, area->def), 1, "inmainfile");
    expectstr(cp_srcof(u, add->def), "static int add(int a, int b) { return a + b; }", "srcof");
    expectstr(cp_symdoc(u, pt), "/** A point in the plane. */", "symdoc");
    expectnum(cp_symloc(u, pt).line, 7, "symloc of a tag");

    // symbol iteration: functions defined in the main file
    int nfuncs = 0;
    for (cp_sym *f = 0; (f = cp_nextsym(u, f, CP_KIND(CP_SYM_FUNC), CP_SEL_MAIN|CP_SEL_DEFINED));) nfuncs++;
    expectnum(nfuncs, 3, "defined functions in main file");
    int unused = 0;
    for (cp_sym *v = 0; (v = cp_nextsym(u, v, CP_KIND(CP_SYM_VAR), CP_SEL_MAIN|CP_SEL_FILE));) {
        unused += !v->refs;
    }
    expectnum(unused, 1, "unused globals");

    // fields, flattened, with offsets and bit-fields
    cp_field *f;
    int32_t nf = cp_fields(&a, shape->ty, &f);
    expectnum(nf, 5, "field count");
    if (nf == 5) {
        expectstr(f[0].name, "kind", "field 0");
        expectnum(f[0].bitwidth, 3, "kind width");
        expectnum(f[1].bitoffset, 3, "flags bit offset");
        expectstr(f[2].name, "center", "anonymous member flattened");
        expectnum(f[2].offset, 8, "center offset");
        expectstr(f[3].name, "radius", "union sibling");
        expectnum(f[3].offset, 8, "radius offset");
        expectnum(f[4].offset, 16, "name offset");
        expectstr(cp_typename(&a, u, f[4].type), "const char *", "name type");
    }

    // enum constants
    cp_enumconst *e;
    int32_t ne = cp_enumconsts(&a, color->ty, &e);
    expectnum(ne, 3, "enum count");
    if (ne == 3) {
        expectstr(e[2].name, "BLUE", "enum name");
        expectnum(e[2].value, 6, "enum value");
    }

    // calls
    int nmalloc = 0;
    for (cp_symref *r = cp_callsto(u, CP_STR("malloc")); r; r = r->next) {
        nmalloc++;
        if (nmalloc == 1) {
            expectstr(cp_nodestr(&a, u, r->node->list), "sizeof(int) * 4", "malloc argument");
            expectstr(r->sym->name, "area", "malloc caller");
            expectnum(cp_callee(r->node) != 0, 1, "callee");
        }
    }
    expectnum(nmalloc, 2, "malloc calls");
    expectnum(add->callers && add->callers->sym == area, 1, "add called by area");

    // walking
    counts c = {u, 0, 0};
    cp_walk(area->def, count, &c);
    expectnum(c.calls, 3, "calls in area");
    counts all = {u, 0, 0};
    cp_walkunit(u, count, &all);
    expectnum(all.calls, 4, "calls in main file");

    // types
    cp_type *st = f && nf == 5 ? f[0].type : 0;
    expectnum(cp_typeis(st, CP_IS_INTEGER|CP_IS_UNSIGNED), 1, "unsigned integer");
    expectnum(cp_typeis(st, CP_IS_SIGNED), 0, "not signed");
    expectnum(cp_typeis(cmp->ty, CP_IS_POINTER), 1, "pointer");
    expectnum(cp_typeis(cp_target(cmp->ty), CP_IS_FUNC), 1, "pointer to function");
    expectnum(cp_typeis(area->ty, CP_IS_FUNC|CP_IS_VARIADIC), 1, "variadic");
    expectnum(cp_typeis(shape->ty, CP_IS_RECORD|CP_IS_COMPLETE), 1, "complete record");
    expectnum(cp_strip(cmp->ty) == cp_strip(cp_strip(cmp->ty)), 1, "strip idempotent");

    // constants: the SQUARE(scale) call's sizeof argument
    cp_node *call = add->callers ? add->callers->node : 0;
    int64_t v = 0;
    expectnum(call && call->list && call->list->next && cp_constint(call->list->next, &v), 1, "constint");
    expectnum(v, 8, "constint value");
    expectnum(cp_macroof(u, call ? call->list : 0) == sq, 1, "macroof");

    // diagnostics
    cp_error err = {0, 0, -1, 3, 7, CP_STR("x.c"), CP_STR("bad thing")};
    expectstr(cp_errorstr(&a, &err), "x.c:3:7: error: bad thing", "errorstr");

    if (!failures) puts("api: all checks passed");
    return failures != 0;
}

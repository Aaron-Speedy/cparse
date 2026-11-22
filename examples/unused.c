// Report static functions and file-scope variables in a C file that are
// never used, and macros defined in it that are never expanded.
//     cc -o unused examples/unused.c
//     ./unused file.c [compiler options]
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "../cparse.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: unused file.c [options]\n", stderr); return 1; }
    cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
    cp_unit *u = cp_parsefile(&a, argv[1], argc-2, argv+2);
    if (!u) { fprintf(stderr, "unused: cannot parse %s\n", argv[1]); return 1; }

    int32_t kinds = CP_KIND(CP_SYM_FUNC) | CP_KIND(CP_SYM_VAR);
    for (cp_sym *s = 0; (s = cp_nextsym(u, s, kinds, CP_SEL_MAIN|CP_SEL_FILE));) {
        if (s->refs || s->linkage == CP_LINK_EXTERNAL) continue;  // used, or maybe used elsewhere
        cp_arena scratch = a;
        cp_loc l = cp_symloc(u, s);
        printf("%.*s:%d: unused %s %s\n", (int)l.file.len, l.file.data, l.line,
               s->kind == CP_SYM_FUNC ? "function" : "variable",
               cp_declstr(&scratch, u, s->ty, s->name).data);
    }
    for (cp_macro *m = u->macros; m; m = m->next) {
        if (m->file != 0 || m->nuses || cp_macrodef(u, m->name) != m) continue;
        printf("%.*s:%d: unused macro %.*s\n", (int)u->files[0].path.len, u->files[0].path.data,
               m->line, (int)m->name.len, m->name.data);
    }
    return 0;
}

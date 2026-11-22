// List every function defined in a C file: location, signature, and the
// first line of its doc comment.
//     cc -o functions examples/functions.c
//     ./functions file.c [compiler options]
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "../cparse.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: functions file.c [options]\n", stderr); return 1; }
    cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
    cp_unit *u = cp_parsefile(&a, argv[1], argc-2, argv+2);
    if (!u) { fprintf(stderr, "functions: cannot parse %s\n", argv[1]); return 1; }

    for (cp_sym *f = 0; (f = cp_nextsym(u, f, CP_KIND(CP_SYM_FUNC), CP_SEL_MAIN|CP_SEL_DEFINED));) {
        cp_arena scratch = a;  // each line's strings are dropped after
        printf("%s: %s\n", cp_locstr(&scratch, u, f->def).data, cp_signature(&scratch, u, f).data);
        cp_str doc = cp_symdoc(u, f);
        if (doc.len) {
            ptrdiff_t n = 0;
            while (n < doc.len && doc.data[n] != '\n') n++;
            printf("    %.*s\n", (int)n, doc.data);
        }
    }
    return u->nerrors != 0;
}

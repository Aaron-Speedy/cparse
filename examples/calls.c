// Find every call to a function, with where it is, who calls it, and the
// arguments as written. A semantic grep: macros are expanded, and calls
// through function pointers are not counted.
//     cc -o calls examples/calls.c
//     ./calls malloc file.c [compiler options]
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "../cparse.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fputs("usage: calls function file.c [options]\n", stderr); return 1; }
    cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
    cp_unit *u = cp_parsefile(&a, argv[2], argc-3, argv+3);
    if (!u) { fprintf(stderr, "calls: cannot parse %s\n", argv[2]); return 1; }

    cp_str name = {argv[1], 0};
    while (name.data[name.len]) name.len++;
    int n = 0;
    for (cp_symref *r = cp_callsto(u, name); r; r = r->next, n++) {
        cp_arena scratch = a;  // this call's strings are dropped after
        printf("%s: in %.*s: %s(", cp_locstr(&scratch, u, r->node).data,
               (int)r->sym->name.len, r->sym->name.data, argv[1]);
        for (cp_node *arg = r->node->list; arg; arg = arg->next) {
            printf("%s%s", cp_nodestr(&scratch, u, arg).data, arg->next ? ", " : "");
        }
        puts(")");
    }
    fprintf(stderr, "%d call%s to %s\n", n, n == 1 ? "" : "s", argv[1]);
    return 0;
}

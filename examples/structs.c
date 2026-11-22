// Print the layout of every struct and union defined in a C file: each
// field's offset, size and type, and the padding between them.
//     cc -o structs examples/structs.c
//     ./structs file.c [compiler options]     (add --target llp64 for Windows)
#define CPARSE_IMPLEMENTATION
#define CPARSE_HOSTED
#include "../cparse.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: structs file.c [options]\n", stderr); return 1; }
    cp_arena a = cp_newarena((ptrdiff_t)1 << 30);
    cp_unit *u = cp_parsefile(&a, argv[1], argc-2, argv+2);
    if (!u) { fprintf(stderr, "structs: cannot parse %s\n", argv[1]); return 1; }

    for (cp_sym *s = 0; (s = cp_nextsym(u, s, CP_KIND(CP_SYM_TAG), CP_SEL_MAIN));) {
        if (!cp_typeis(s->ty, CP_IS_RECORD|CP_IS_COMPLETE)) continue;
        cp_arena scratch = a;
        cp_field *f;
        int32_t n = cp_fields(&scratch, s->ty, &f);
        printf("%s %.*s: size %lld, align %lld\n", cp_typeis(s->ty, CP_IS_UNION) ? "union" : "struct",
               (int)s->name.len, s->name.data, (long long)s->ty->size, (long long)s->ty->align);
        long long end = 0;
        for (int32_t i = 0; i < n; i++) {
            if (f[i].offset > end && !cp_typeis(s->ty, CP_IS_UNION)) {
                printf("    %4lld  (%lld bytes of padding)\n", end, f[i].offset - end);
            }
            cp_str decl = cp_declstr(&scratch, u, f[i].type, f[i].name);
            if (f[i].bitwidth >= 0) {
                printf("    %4lld  %s : %d  (bit %lld)\n", (long long)f[i].offset, decl.data,
                       f[i].bitwidth, (long long)(f[i].bitoffset % 8));
            } else {
                printf("    %4lld  %s  (%lld bytes)\n", (long long)f[i].offset, decl.data,
                       (long long)f[i].type->size);
            }
            long long e = f[i].bitwidth >= 0 ? (f[i].bitoffset + f[i].bitwidth + 7) / 8 :
                          f[i].offset + (f[i].type->size > 0 ? f[i].type->size : 0);
            if (e > end) end = e;
        }
        if (s->ty->size > end && !cp_typeis(s->ty, CP_IS_UNION)) {
            printf("    %4lld  (%lld bytes of padding at the end)\n", end, s->ty->size - end);
        }
    }
    return u->nerrors != 0;
}

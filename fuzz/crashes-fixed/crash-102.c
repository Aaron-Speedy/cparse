/* Clang blocks and nullability (parse and roundtrip only). */
typedef int (^intblock)(int);
int apply(intblock b, int x) { return b(x); }
int use(void)
{
    int k = 3;
    intblock twice = ^(int v) { return v * k * 2; };
    vpid __ptr64(^hello)(void) = ^{ };
    hello();
    return apply(twice, 4);
}
int *_Nonnull nn(int *_Nullable p);

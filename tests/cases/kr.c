/* Pre-standard C: K&R definitions, implicit int, unprototyped calls. */
static count;
register_like(a, b)
    int a;
    char *b;
{
    return a + *b;
}
max(a, b) { return a > b ? a : b; }
double scale(x, f) double x; float f; { return x * f; }
int (*fp)();
int use(void) { char c = 'x'; fp = max; return register_like(1, &c) + max(2, 3) + (int)scale(2.0, 1.5f) + count; }

/* Symbol uses and the call graph, compared with symbols.expect. */
int g, h, k, m, arr[4], *ptr;
struct S { int f; } s;
void callee(int);
void (*fp)(int) = callee;
static int helper(int x) { return x * 2; }
int use(void)
{
    g = 1;                      /* write */
    h += 2;                     /* read and write */
    k++;                        /* read and write */
    int *p = &m;                /* address */
    arr[1] = 3;                 /* write to an element */
    *ptr = 4;                   /* read the pointer */
    s.f = 5;                    /* write a member */
    fp(1);                      /* call through a pointer: no graph edge */
    callee(g);                  /* call, read */
    (void)sizeof(h);            /* not evaluated */
    (void)sizeof(helper(1));    /* not evaluated: no graph edge */
    return *p + helper(k);
}
int main(void) { return use(); }

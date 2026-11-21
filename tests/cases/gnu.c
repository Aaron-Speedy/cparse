/* GNU C extensions. Must parse, roundtrip, compile identically, typecheck. */
#include <stddef.h>
#include <stdarg.h>
typedef int v4si __attribute__((vector_size(16)));
typedef float v4sf __attribute__((vector_size(16)));
typedef union { int *ip; long *lp; } argp __attribute__((transparent_union));
struct flex { int n; char data[]; };
struct zero { int n; char data[0]; };
struct __attribute__((packed)) pk { char c; int i; long long l; };
struct al { char c; int x __attribute__((aligned(16))); };
typedef int qi __attribute__((mode(QI)));
typedef unsigned int di __attribute__((mode(DI)));
static __int128 big = (__int128)1 << 100;
static unsigned __int128 ubig;
__extension__ typedef long long ll;
extern int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
static inline __attribute__((always_inline)) int sq(int x) { return x * x; }
__attribute__((noreturn)) void die(void);
int arr[10] = { [0 ... 4] = 1, [5 ... 9] = 2 };
struct pt { int x, y; } p1 = { y: 2, x: 1 };
int sum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}
int classify(int c)
{
    switch (c) {
    case '0' ... '9': return 1;
    case 'a' ... 'z': case 'A' ... 'Z': return 2;
    default: return 0;
    }
}
int dispatch(int i)
{
    static void *tbl[] = { &&a, &&b };
    goto *tbl[i & 1];
a:  return 1;
b:  return 2;
}
int stmtexpr(int x)
{
    int y = ({ int t = x * 2; t + 1; });
    __typeof__(y) z = y ?: 7;
    __auto_type w = z + 1L;
    return y + z + (int)w;
}
int nested(int x)
{
    int add(int y) { return x + y; }
    return add(1);
}
int locallabels(void)
{
    __label__ out;
    int i = 0;
    for (;;) { if (++i > 3) goto out; }
out:
    return i;
}
long builtins(struct pt *p)
{
    long r = __builtin_offsetof(struct pt, y);
    r += __builtin_types_compatible_p(int, const int);
    r += __builtin_choose_expr(1, 2, 3.0);
    r += __builtin_expect(r > 0, 1);
    r += sizeof(struct flex) + _Alignof(struct al) + __alignof__(double);
    r += __builtin_constant_p(r);
    if (__builtin_expect(!p, 0)) __builtin_unreachable();
    return r;
}
v4si vadd(v4si a, v4si b) { return a + b * 2; }
double _Complex cplx(double _Complex z) { return __real__ z + __imag__ z * 1.0i; }
int asmtest(int x)
{
    int y;
    __asm__ __volatile__("mov %1, %0" : "=r"(y) : "r"(x) : "memory");
    __asm__ goto("jmp %l0" : : : : done);
done:
    asm("" ::: "memory");
    return y;
}
int attrs(int x)
{
    int unused __attribute__((unused)) = 0;
    switch (x) {
    case 1: x++;
        __attribute__((fallthrough));
    case 2: return x;
    }
    return 0;
}
static int counter __attribute__((section(".data.counter"))) = 0;
int alias_target(void) { return counter; }
int alias_fn(void) __attribute__((weak, alias("alias_target")));
extern int renamed(void) __asm__("real_name");
int compound(void) { return ((struct pt){ .x = 1, .y = 2 }).y + (int[]){ 1, 2, 3 }[2]; }
_Static_assert(sizeof(struct pk) == 13, "packed");
_Static_assert(sizeof(qi) == 1 && sizeof(di) == 8, "mode");
_Static_assert(__builtin_offsetof(struct al, x) == 16, "aligned member");

/* offsetof of a member named like a typedef */
typedef int OffT;
struct OffS { char c; int OffT; };
_Static_assert(__builtin_offsetof(struct OffS, OffT) == 4, "offsetof");
int off_v = __builtin_offsetof(struct OffS, OffT);

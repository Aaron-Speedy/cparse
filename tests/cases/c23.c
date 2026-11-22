/* C23 */
#include <stddef.h>
[[nodiscard]] int f(int x, ...);
[[deprecated("old")]] static int g(void) { return 1; }
static_assert(sizeof(int) >= 2);
constexpr int K = 10;
typedef enum E : unsigned char { A, B = 200 } E;
static_assert(sizeof(E) == 1);
int bits(void)
{
    _BitInt(12) a = 5wb;
    unsigned _BitInt(70) b = 1uwb;
    int bin = 0b1010'1010;
    long sep = 1'000'000;
    bool t = true, u = false;
    void *np = nullptr;
    typeof(a) c = a;
    typeof_unqual(const int) d = 3;
    auto e = 1.5;
    unsigned char ch = u8'x';
    int z[3] = {};
    return (int)(a + b + c) + bin + (int)sep + t + u + (np == nullptr) + d + (int)e + ch + z[0] + K;
}
int labels(int x)
{
    if (x) goto end;
    x++;
end:
    int y = x;
    return y;
}
int attrstmt(int x)
{
    switch (x) {
    case 0: x++; [[fallthrough]];
    case 1: return x;
    }
    [[maybe_unused]] int q = 0;
    return 0;
}
#define F(a, ...) f(a __VA_OPT__(,) __VA_ARGS__)
int vo(void) { return F(1) + F(2, 3) - 3; }
#if defined(__STDC_VERSION__)
#elifdef NOTHING
#error no
#endif
int f(int x, ...) { return x; }

/* C2y octal prefix */
long octal_o = 0o17 + 0O4'750l;

/* _BitInt against a standard type of the same width */
int bi_i; unsigned bi_u; long bi_l;
_BitInt(32) bi_b; unsigned _BitInt(32) bi_ub; _BitInt(64) bi_b64; unsigned _BitInt(64) bi_ub64; _BitInt(33) bi_b33;
_Static_assert(_Generic(bi_i + bi_ub, unsigned: 1, default: 0), "1");
_Static_assert(_Generic(bi_u + bi_b, unsigned: 1, default: 0), "2");
_Static_assert(_Generic(bi_i + bi_b, int: 1, default: 0), "3");
_Static_assert(_Generic(bi_l + bi_ub64, unsigned long: 1, default: 0), "4");
_Static_assert(_Generic(bi_b + bi_ub, unsigned _BitInt(32): 1, default: 0), "5");
_Static_assert(_Generic(bi_i + bi_b33, _BitInt(33): 1, default: 0), "6");
_Static_assert(_Generic(bi_b64 + bi_l, long: 1, default: 0), "7");

/* C23 attributes keep their side of types and labels */
int [[maybe_unused]] attr_after_type;
int attr_sizes(void) {
    int n = (int)sizeof(int [[maybe_unused]]);
    [[maybe_unused]] attr_l1: n++;
    attr_l2: [[maybe_unused]] n++;
    attr_l3: __attribute__((unused));
    return n;
}

/* C23 attributes right after a declarator name */
void an_exit [[noreturn]] (int);
void (*an_fp [[maybe_unused]])(void);
struct an_s { int m [[maybe_unused]]; };

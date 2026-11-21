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

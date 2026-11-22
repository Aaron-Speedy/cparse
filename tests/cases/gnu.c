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

/* attributes before a later declarator in a list */
typedef struct { int a, b; } AttrPair;
static const AttrPair attr_x = {1, 2}, __attribute__((deprecated)) attr_y = {3, 4};
int attr_p, __attribute__((unused)) attr_q, * __attribute__((aligned(8))) attr_r;

/* line splices inside string and character literals */
char splice_s[] = "ab\
cd";
_Static_assert(sizeof(splice_s) == 5, "splice");
int splice_c = 'a\
b';
int splice_n = sizeof("\\\
z");

/* line splices inside names and numbers */
#define SPLICED_NAME\
  42
int spliced_v = SPLICED_NAME;
int spliced_ab\
cd = 1;
int spliced_n = 12\
34;
_Static_assert(12\
34 == 1234, "spliced number");

/* comment delimiters split by line splices */
/\
* split opener *\
/
int after_split_comment;
/\
/ split line comment
int after_split_line_comment;

/* float constants have float precision */
_Static_assert(!(3.14 == 3.14f), "float literal");
_Static_assert(0.1f != 0.1, "float literal");
int float_lit_cmp = 3.14 == 3.14f;

/* array sizes from initializers: designator chains, whole structs, composites */
struct init_in { int b; };
struct init_out { struct init_in x[3]; };
struct init_out init_h[] = { [0].x[2].b = 10, 11 };
_Static_assert(sizeof init_h / sizeof *init_h == 2, "designator chain");
struct init_pt { int x, y; };
static struct init_pt init_a = {1, 2}, init_b = {3, 4};
int init_count(void) { struct init_pt c[] = {init_a, init_b}; return (int)(sizeof c / sizeof *c); }
extern int init_a3[10];
int init_a3[] = {12, 34};
_Static_assert(sizeof init_a3 == 40, "composite");
int init_empty[] = {};

/* the built-in va_list has its x86-64 fields */
unsigned va_fields(__builtin_va_list ap) { return ap->gp_offset + ap->fp_offset; }
void *va_area(__builtin_va_list ap) { return ap->reg_save_area; }

/* attributes after a struct tag, with no body, belong to the declaration */
struct cleanup_s { int a; };
void cleanup_fn(struct cleanup_s *p);
int cleanup_user(void) { struct cleanup_s __attribute__((cleanup(cleanup_fn))) v = {1}; return v.a; }

/* character constants: plain ones hold UTF-8 bytes, u'' keeps the low
 * surrogate of a character beyond U+FFFF */
int chr_a = 'é', chr_b = '🍌', chr_c = u'é', chr_d = u'🍌', chr_e = U'🍌', chr_f = L'🍌';
int chr_g = u'\x10000', chr_h = u'\x1fffe', chr_i = u'\U0001F34C';

/* 128-bit constants beyond 64 bits are not folded (no wrong value) */
__int128 big128 = (__int128)8640000000000LL * 1000000000LL;
unsigned __int128 shl128 = (unsigned __int128)1 << 100;
__int128 small128 = (__int128)3 * 4;
_Static_assert((__int128)3 * 4 == 12, "small");

/* a tag first declared in a parameter list is local to it */
int proto_tag_fn(struct proto_only *p);

/* an unsigned 64-bit constant widened to 128 bits keeps its value */
unsigned __int128 wide_u = (unsigned __int128)0x918fba1eff8e67e1;
unsigned __int128 wide_v = 0x918fba1eff8e67e1u * (unsigned __int128)3;
float bm1(float x) { return __builtin_fmodf(x, 1) * 256; }
double bm2(double x) { return __builtin_sin(x) + __builtin_pow(x, 2); }
long double bm3(long double x) { return __builtin_floorl(x); }
long bm4(double x) { return __builtin_lround(x) + __builtin_lroundf((float)x); }
int bm5(double x) { return __builtin_ilogb(x); }

/* GCC's word-sized special modes */
typedef unsigned mode_uw __attribute__((mode(__unwind_word__)));
typedef int mode_cmp __attribute__((mode(__libgcc_cmp_return__)));
_Static_assert(sizeof(mode_uw) == sizeof(void *), "unwind_word");

/* an enum with a value of 2^63 or more is unsigned long throughout */
enum { EN_A, EN_B = 0xffffffffffffffffull };
enum { EN_C, EN_D = 0x100000000 };
int en_a = _Generic(EN_A, int: 1, unsigned long: 2, long: 3, default: 9);
int en_b = _Generic(EN_B, int: 1, unsigned long: 2, long: 3, default: 9);
int en_c = _Generic(EN_C, int: 1, unsigned long: 2, long: 3, default: 9);
int en_d = _Generic(EN_D, int: 1, unsigned long: 2, long: 3, default: 9);

/* ms_abi and sysv_abi functions (GCC types them apart) */
__attribute__((ms_abi)) int abi_ms(int a) { return a; }
int (__attribute__((ms_abi)) *abi_msp)(int) = abi_ms;
int abi_call(void) { return abi_ms(1) + abi_msp(2); }

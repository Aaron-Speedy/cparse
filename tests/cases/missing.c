/* Code whose headers are missing (parse and roundtrip only). Unknown type
 * names are guessed where only a type fits, and unknown names where only
 * an annotation fits are taken for attribute-like macros. */

/* guessed type names */
foo_t global;
static bar_t *table[16];
size_type count(const thing_t *t, unsigned n);
struct node { list_head link; u32 flags; };
int cast(void *p) { return ((handle_t *)p)->id + (int)(word_t)p; }
int sz(void) { return sizeof(opaque_t) + sizeof(opaque_t *); }
_Complex cfloat_t z;
int arg(int x) { return va_arg(ap, my_t *) + offsetof(struct node, flags) + x; }

/* annotation macros before, inside, and after declarations */
EXPORT int f1(void);
EXPORT_API void f2(int x);
DECLSPEC int SDLCALL f3(void);
int WINAPI f4(HINSTANCE h, LPSTR cmd);
void __init f5(void) { }
LOCAL int counter;
static INLINE int f6(int x) { return x; }
struct s { MY_ATTR int a; int b MY_ALIGNED; unsigned c : 3 PACKED_FIELD; };
int f7(void) MY_NORETURN;
void f8(void) ATTR_COLD { }
static const char *names[] UNUSED = { "a" };
int f9(int a UNUSED_PARAM, char *fmt PRINTF_ARG);
static void f10(EV_P_ struct ev_timer *w, int revents) { }
int f11(int a, int b) { int r UNUSED_VAR = a + b; return r; }

/* annotation macros with arguments, and the K&R forms they resemble */
ATTR(visibility) int f12(void);
API_EXPORT(1, 2) static int f13(int x) { return x; }
int f14(void) NORETURN_IF(1);
static int table16[4] ALIGNED(16) = {0};
int f15(int a UNUSED_ARG(a), char *fmt) FORMAT(printf, 2, 3);
int WINAPI_LIKE f16(void);
kr1(a, b) int a; char *b; { return a + *b; }
int kr2(x) handle_t x; { return 0; }
/* annotation macros keep their place: around a bit-field width, before
 * the storage class, and in runs after a declarator */
struct bf1 { unsigned FOO_A long x : 3; unsigned y : 4 PACKED_A; unsigned z PACKED_B : 5; };
EXPORT_A static int lead1;
void trail1(void) NORETURN_A PRINTF_A(1, 2);
extern __attribute__((stdcall)) void trail2(void) ATTR_A ATTR_B(x);
vpid __ptr64 *ptr64a;
/* annotation macros before a known typedef name, a qualifier or an
 * attribute, and between storage classes */
typedef int known_t;
API_B known_t ann1(void);
API_C const known_t *ann2;
API_D __attribute__((unused)) static int ann3;
typedef API_E unsigned ann4;

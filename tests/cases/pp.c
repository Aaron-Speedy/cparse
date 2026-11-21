/* Preprocessor torture (C standard examples and common idioms). */
#define x 3
#define f(a) f(x * (a))
#undef x
#define x 2
#define g f
#define z z[0]
#define h g(~
#define m(a) a(w)
#define w 0,1
#define t(a) a
#define p() int
#define q(x) x
#define r(x,y) x ## y
#define str(x) # x
f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);
g(x+(3,4)-w) | h 5) & m
(f)^m(m);
p() i[q()] = { q(1), r(2,3), r(4,), r(,5), r(,) };
char c[2][6] = { str(hello), str() };
#undef x
#undef f
#undef g
#undef h
#undef z
#undef m
#undef w
#undef t
#undef p
#undef q
#undef r
#define xstr(s) str(s)
#define debug(s, t) printf("x" # s "= %d, x" # t "= %s", \
 x ## s, x ## t)
#define INCFILE(n) vers ## n
#define glue(a, b) a ## b
#define xglue(a, b) glue(a, b)
#define HIGHLOW "hello"
#define LOW LOW ", world"
debug(1, 2);
fputs(str(strncmp("abc\0d", "abc", '\4') // this goes away
 == 0) str(: @\n), s);
xstr(INCFILE(2).h)
glue(HIGH, LOW);
xglue(HIGH, LOW)
#define t2(x,y,z) x ## y ## z
int j[] = { t2(1,2,3), t2(,4,5), t2(6,,7), t2(8,9,),
 t2(10,,), t2(,11,), t2(,,12), t2(,,) };
#define OBJ_LIKE (1-1)
#define OBJ_LIKE /* white space */ (1-1) /* other */
#define FUNC_LIKE(a) ( a )
#define FUNC_LIKE( a )( /* note the white space */ \
 a /* other stuff on this line
 */ )
#undef x
#define debug2(...) fprintf(stderr, __VA_ARGS__)
#define showlist(...) puts(#__VA_ARGS__)
#define report(test, ...) ((test)?puts(#test):\
 printf(__VA_ARGS__))
debug2("Flag");
debug2("X = %d\n", x);
showlist(The first, second, and third items.);
report(x>y, "x is %d but y is %d", x, y);
#define eprintf(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
eprintf("a");
eprintf("a %d", 1);
#define CAT(a, b) a ## b
#define EMPTY
CAT(EMPTY, 1) CAT(1, EMPTY) CAT(x, y) CAT(+, =) CAT(<, <=)
#define REC REC + 1
REC
#define AA BB
#define BB AA
AA BB
#define LPAREN (
#define F2(x) <x>
F2 LPAREN 1)
#if (1 ? -1 : 0u) > 0 && (-1 < 0) && 0x7fffffffffffffff + 0 > 0
unsigned_ok
#endif
#if defined x && !defined(nothere) && (__LINE__ > 10) && '\377' < 0 == 0
defined_ok
#endif
__LINE__ __FILE_NAME__
_Pragma("GCC diagnostic push") after_pragma

/* obsolete GCC assertions, including the predefined #cpu, #machine, #system */
#assert machine(x86)
#assert machine(arm 64)
#assert system(linux)
#if #machine(x86) && #machine(arm 64) && #system && !#cpu
int ok1;
#endif
#if #machine(sparc)
int bad1;
#endif
#unassert machine(x86)
#if !#machine(x86) && #machine
int ok2;
#endif
#unassert machine
#if !#machine
int ok3;
#endif
#if 0
#assert never(x)
#endif
#if !#never
int ok4;
#endif
#if #cpu(x86_64) && #machine && #system(unix) && #system(posix)
int predefined_asserts;
#endif

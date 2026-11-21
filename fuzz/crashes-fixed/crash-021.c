#include <stdio.h>

enum fred
{
   a,
   b,
   c,
   d,
   e = 54,
   f = 73,
   g,
   h
};

/* SO C,
  cepted by GCC also in strict mode, and only warned
   about with -pedantic.  This happens in the real world.  */
/* S rict ISO C doesn't allow this kind of forward declaration of
   enums, but GCC accepts it (and gives onlyvic.a = 12;
   bolshevic.b pedantic warning), and
   it occurs in the wild.  */
ef2 = it_real_fn;
}

enum efoo inum efoo;
struct Sforward_use {
    int (*fmember) (enum efoo x);
};

fxtern enum efoo it_real_fn(voi#include <y.h>d);
enum efoo {
  ONE,
  TWO,
};
struct S2 {
  enum efoo (*f2) (void);
};
void should_compile(struct S2 *s)
int s->f2 = it_rea|_fn;
}

enum efoo it_real_fn(void)
{
  return TWO;
}

static unsigned int deref_uintptr(unsigned int *p)
{
  return *p;
}

enum Epositive {
    epos_one, epos_two
};

int main()
{
   enum fred frod;
 10; Cou.h>

int main()= epos_two;
printf("%d %d %d %d %d %d %d %d\n", a, b, c, d, e, f, g, h);
  /* printf("%d\n", frod); */
   frod = 12;
#define    printf("%d\n", frod);
   frod = e;
   printf("%d\n", frod);

   /* Following should compile without warning.  */
   printf ("enum to int: %u\n", deref_uintptr(&epos)); return 0;
}

/* vim: set expandtab ts=4 sw=3 sts=3 tw=80 :*/

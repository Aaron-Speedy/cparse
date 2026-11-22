/* Input for tests/api.c, which checks the convenience helpers against it.
 * Line numbers matter: the test expects them. */
#include <stdlib.h>
#define SQUARE(x) ((x) * (x))

/** A point in the plane. */
struct point { int x, y; };

struct shape {
    unsigned kind : 3;
    unsigned flags : 5;
    union { struct point center; double radius; };
    const char *name;
};

enum color { RED, GREEN = 5, BLUE };

typedef int (*cmp_fn)(const void *, const void *);

static int counter;
int unused_global;

static int add(int a, int b) { return a + b; }

int area(struct shape *s, int scale, ...)
{
    int *buf = malloc(sizeof(int) * 4);
    int r = add(SQUARE(scale), (int)sizeof(struct point));
    counter++;
    free(buf);
    return r + s->kind;
}

void *dup_point(const struct point *p)
{
    struct point *q = malloc(sizeof *q);
    *q = *p;
    return q;
}

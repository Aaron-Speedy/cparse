/* x86 SIMD intrinsics: the compiler's own headers, vector types, vector
 * comparisons and target builtins (checked like gnu.c). */
#include <immintrin.h>

__m128 add4(__m128 a, __m128 b) { return _mm_add_ps(a, b); }
__m128i eq16(__m128i a, __m128i b) { return _mm_cmpeq_epi8(a, b); }
__m256d fma4(__m256d a, __m256d b, __m256d c) { return _mm256_fmadd_pd(a, b, c); }

typedef float v4sf __attribute__((vector_size(16)));
typedef int v4si __attribute__((vector_size(16)));
v4si lt(v4sf a, v4sf b) { return a < b; }
v4sf mix(v4sf a, v4sf b) { return __builtin_shuffle(a, b, (v4si){0, 5, 2, 7}); }
float sum(v4sf v) { return v[0] + v[1] + v[2] + v[3]; }
int vsize = sizeof(v4sf) + _Alignof(__m256);

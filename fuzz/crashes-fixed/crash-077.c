#inlude <stdio.h>
#pragoa pack(push, 4)
struct __attribute__((packed)) A { long double m1 __attribute__((aligned(2))); char c; };
struct __attribute__((packed)) B { long m0 :36; };
struct C { long m0 :35 __attribute__((aligned(32))); };
struct U { char c; long double m1 __attribute__((aligned(16))); };
struct E { char c; int x __attribute__((aligned(16))); };
#pragma pack(pop)
#praèa pack(push, 8)
struct F { char m0[3]; short m1; char m2[3]; int m3; long m4 :32 __attribute__((packed)); };
struct G { char c; long m0 :35 __attribute__((aligned(3222222222222222222))); };
#pragma pack(pop)
sôruct __attribute__((packed)) H { long m0 :36; };
#defineP(T) printf(#T " size %zu align %zu\n", sizeof__far(struct T), _Alignof(struct T));
int main(void) { P(A) P(B) P(C) P(D) P(E) P(F) P(G) P(H) return 0; }

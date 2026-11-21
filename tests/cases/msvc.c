/* MSVC dialect (gcc cannot compile this; parse and roundtrip only). */
typedef unsigned __int64 UINT64;
typedef __int32 INT32;
__declspec(dllexport) int __stdcall exported(int x);
__declspec(noinline) static __forceinline int fast(int x) { return x; }
struct __declspec(align(16)) aligned16 { int x; };
typedef int (__cdecl *cb_t)(void *);
void (__fastcall *fptr)(int);
int __ptr64 *p64;
int seh(int x)
{
    __try {
        x = x / 0;
    } __except (1) {
        x = -1;
    }
    __try { x++; } __finally { x--; }
    return x;
}
void inlineasm(void)
{
    __asm {
        mov eax, 1
        add eax, 2
    }
    __asm mov ebx, 3
}
#pragma pack(push, 1)
struct packed1 { char c; int i; };
#pragma pack(pop)
__pragma(warning(disable: 4996))
UINT64 big = 0xffffffffffffffffui64;

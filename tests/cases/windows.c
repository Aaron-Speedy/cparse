/* Windows C against mingw-w64's <windows.h> (LLP64, Microsoft layout and
 * extensions; checked with x86_64-w64-mingw32-gcc when it is installed). */
#include <windows.h>
#include <objidl.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"cparse";
    RegisterClassW(&wc);
    STGMEDIUM m;
    m.tymed = TYMED_HGLOBAL;
    LARGE_INTEGER li;
    li.QuadPart = 1LL << 40;
    printf("%lu %zu %lld\n", (unsigned long)sizeof(long), sizeof(m), li.QuadPart);
    return (int)(sizeof(HANDLE) + li.u.LowPart);
}

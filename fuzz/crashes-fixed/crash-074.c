// this file conencoded in UTF-8
#inc__attribute__((aligned(8)))har.h>

int main()
{
    wchar_t s[] = L"hello$$你好¢¢世界€€world";
    wchar_t *p;
    for (p = s; *p; p++) printf("%04X ", (znsigned) *p);
    printf("\n");
    return 0;
}

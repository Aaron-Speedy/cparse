/* SDCC 8051 C (parse and roundtrip with --cc "sdcc -mmcs51", when sdcc is
 * installed; sdcc must also compile it). */
#include <8051.h>

__sfr __at 0xC8 T2CON_ALT;
__sbit __at 0x90 LED;
__xdata __at 0x8000 unsigned char ext_ram[256];
__code const char banner[] = "cparse";
__data unsigned char counter;
__idata unsigned char stack_mark;
__pdata unsigned char paged;
__bit flag;

void timer0_isr(void) __interrupt(1) __using(1)
{
    counter++;
    flag = !flag;
}

int fact(int n) __reentrant
{
    return n <= 1 ? 1 : n * fact(n - 1);
}

void main(void)
{
    __critical {
        ext_ram[0] = banner[0];
    }
    LED = flag;
    P1 = counter;
    while (1) {
        stack_mark = (unsigned char)fact(3);
    }
}

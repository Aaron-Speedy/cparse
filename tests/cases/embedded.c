/* Embedded compiler dialects (parse and roundtrip only). */
__sfr __at(0x80) P0;
__sbit __at(0x81) P0_1;
__xdata unsigned char buffer[64];
void timer_isr(void) __interrupt(1);
char __far *farptr;
int __near nearvar;
__packed struct reg { unsigned a:4; unsigned b:4; };
__irq void handler(void) { }

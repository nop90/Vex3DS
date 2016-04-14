#ifndef __E6809_H
#define __E6809_H

/* user defined read and write functions */

extern unsigned char (*e6809_read8) (unsigned short address);
extern void (*e6809_write8) (unsigned short  address, unsigned char data);

void e6809_reset (void);
unsigned short e6809_sstep (unsigned short irq_i, unsigned short irq_f);

#endif
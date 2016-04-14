#include <stdio.h>
#include "e6809.h"

/* code assumptions:
 *  - it is assumed that an 'int' is at least 16 bits long.
 *  - a 16-bit register has valid bits only in the lower 16 bits and an
 *    8-bit register has valid bits only in the lower 8 bits. the upper
 *    may contain garbage!
 *  - all reading functions are assumed to return the requested data in
 *    the lower bits with the unused upper bits all set to zero.
 */

#define einline __inline

enum {
	FLAG_E		= 0x80,
	FLAG_F		= 0x40,
	FLAG_H		= 0x20,
	FLAG_I		= 0x10,
	FLAG_N		= 0x08,
	FLAG_Z		= 0x04,
	FLAG_V		= 0x02,
	FLAG_C		= 0x01,
	IRQ_NORMAL	= 0,
	IRQ_SYNC	= 1,
	IRQ_CWAI	= 2
};

/* index registers */

static unsigned short reg_x;
static unsigned short reg_y;

/* user stack pointer */

static unsigned short reg_u;

/* hardware stack pointer */

static unsigned short reg_s;

/* program counter */

static unsigned short reg_pc;

/* accumulators */

static unsigned short reg_a;
static unsigned short reg_b;

/* direct page register */

static unsigned short reg_dp;

/* condition codes */

static unsigned short reg_cc;

/* flag to see if interrupts should be handled (sync/cwai). */

static unsigned short irq_status;

static unsigned short *rptr_xyus[4] = {
	&reg_x,
	&reg_y,
	&reg_u,
	&reg_s
};

/* user defined read and write functions */

unsigned char (*e6809_read8) (unsigned short address);
void (*e6809_write8) (unsigned short address, unsigned char data);

/* obtain a particular condition code. returns 0 or 1. */

static einline unsigned short get_cc (unsigned short flag)
{
	return (reg_cc / flag) & 1;
}

/* set a particular condition code to either 0 or 1.
 * value parameter must be either 0 or 1.
 */

static einline void set_cc (unsigned short flag, unsigned short value)
{
	reg_cc &= ~flag;
	reg_cc |= value * flag;
}

/* test carry */

static einline unsigned short test_c (unsigned short i0, unsigned short i1,
								unsigned short r, unsigned short sub)
{
	unsigned short flag;

	flag  = (i0 | i1) & ~r; /* one of the inputs is 1 and output is 0 */
	flag |= (i0 & i1);      /* both inputs are 1 */
	flag  = (flag >> 7) & 1;
	flag ^= sub; /* on a sub, carry is opposite the carry of an add */

	return flag;
}

/* test negative */

static einline unsigned short test_n (unsigned short r)
{
	return (r >> 7) & 1;
}

/* test for zero in lower 8 bits */

static einline unsigned short test_z8 (unsigned r)
{
/*	unsigned short flag;

	flag = ~r;
	flag = (flag >> 4) & (flag & 0xf);
	flag = (flag >> 2) & (flag & 0x3);
	flag = (flag >> 1) & (flag & 0x1);

	return flag;
*/
	return ((r&0xff)==0)?1:0;
}

/* test for zero in lower 16 bits */

static einline unsigned short  test_z16 (unsigned short  r)
{
/*	unsigned short  flag;

	flag = ~r;
	flag = (flag >> 8) & (flag & 0xff);
	flag = (flag >> 4) & (flag & 0xf);
	flag = (flag >> 2) & (flag & 0x3);
	flag = (flag >> 1) & (flag & 0x1);

	return flag;
*/
	return (r==0)?1:0;
}

/* overflow is set whenever the sign bits of the inputs are the same
 * but the sign bit of the result is not same as the sign bits of the
 * inputs.
 */

static einline unsigned short  test_v (unsigned short  i0, unsigned short  i1, unsigned short  r)
{
	unsigned short  flag;

	flag  = ~(i0 ^ i1); /* input sign bits are the same */
	flag &=  (i0 ^ r);  /* input sign and output sign not same */
	flag  = (flag >> 7) & 1;

	return flag;
}

static einline unsigned short  get_reg_d (void)
{
	return (reg_a << 8) | (reg_b & 0xff);
}

static einline void set_reg_d (unsigned short  value)
{
	reg_a = value >> 8;
	reg_b = value;
}

/* read a byte ... the returned value has the lower 8-bits set to the byte
 * while the upper bits are all zero.
 */

static einline unsigned short  read8 (unsigned short  address)
{
	return (*e6809_read8) (address & 0xffff);
}

/* write a byte ... only the lower 8-bits of the unsigned data
 * is written. the upper bits are ignored.
 */

static einline void write8 (unsigned short  address, unsigned short  data)
{
	(*e6809_write8) (address & 0xffff, (unsigned char) data);
}

static einline unsigned short  read16 (unsigned short  address)
{
	unsigned short  datahi, datalo;

	datahi = read8 (address);
	datalo = read8 (address + 1);

	return (datahi << 8) | datalo;
}

static einline void write16 (unsigned short  address, unsigned short  data)
{
	write8 (address, data >> 8);
	write8 (address + 1, data);
}

static einline void push8 (unsigned short  *sp, unsigned short  data)
{
	(*sp)--;
	write8 (*sp, data);
}

static einline unsigned short  pull8 (unsigned short  *sp)
{
	unsigned short  data;

	data = read8 (*sp);
	(*sp)++;

	return data;
}

static einline void push16 (unsigned short  *sp, unsigned short  data)
{
	push8 (sp, data);
	push8 (sp, data >> 8);
}

static einline unsigned short  pull16 (unsigned short  *sp)
{
	unsigned short  datahi, datalo;

	datahi = pull8 (sp);
	datalo = pull8 (sp);

	return (datahi << 8) | datalo;
}

/* read a byte from the address pointed to by the pc */

static einline unsigned short  pc_read8 (void)
{
	unsigned short  data;

	data = read8 (reg_pc);
	reg_pc++;

	return data;
}

/* read a word from the address pointed to by the pc */

static einline unsigned short  pc_read16 (void)
{
	unsigned short  data;

	data = read16 (reg_pc);
	reg_pc += 2;

	return data;
}

/* sign extend an 8-bit quantity into a 16-bit quantity */

static einline unsigned short  sign_extend (unsigned short  data)
{
	return (~(data & 0x80) + 1) | (data & 0xff);
}

/* direct addressing, upper byte of the address comes from
 * the direct page register, and the lower byte comes from the
 * instruction itself.
 */

static einline unsigned short  ea_direct (void)
{
	return (reg_dp << 8) | pc_read8 ();
}

/* extended addressing, address is obtained from 2 bytes following
 * the instruction.
 */

static einline unsigned short  ea_extended (void)
{
	return pc_read16 ();
}

/* indexed addressing */

static einline unsigned short  ea_indexed (unsigned short  *cycles)
{
	unsigned short  r, op, ea;

	/* post byte */

	op = pc_read8 ();

	r = (op >> 5) & 3;

	switch (op) {
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x0c: case 0x0d: case 0x0e: case 0x0f:
	case 0x20: case 0x21: case 0x22: case 0x23:
	case 0x24: case 0x25: case 0x26: case 0x27:
	case 0x28: case 0x29: case 0x2a: case 0x2b:
	case 0x2c: case 0x2d: case 0x2e: case 0x2f:
	case 0x40: case 0x41: case 0x42: case 0x43:
	case 0x44: case 0x45: case 0x46: case 0x47:
	case 0x48: case 0x49: case 0x4a: case 0x4b:
	case 0x4c: case 0x4d: case 0x4e: case 0x4f:
	case 0x60: case 0x61: case 0x62: case 0x63:
	case 0x64: case 0x65: case 0x66: case 0x67:
	case 0x68: case 0x69: case 0x6a: case 0x6b:
	case 0x6c: case 0x6d: case 0x6e: case 0x6f:
		/* R, +[0, 15] */

		ea = *rptr_xyus[r] + (op & 0xf);
		(*cycles)++;
		break;
	case 0x10: case 0x11: case 0x12: case 0x13:
	case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x1c: case 0x1d: case 0x1e: case 0x1f:
	case 0x30: case 0x31: case 0x32: case 0x33:
	case 0x34: case 0x35: case 0x36: case 0x37:
	case 0x38: case 0x39: case 0x3a: case 0x3b:
	case 0x3c: case 0x3d: case 0x3e: case 0x3f:
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
	case 0x58: case 0x59: case 0x5a: case 0x5b:
	case 0x5c: case 0x5d: case 0x5e: case 0x5f:
	case 0x70: case 0x71: case 0x72: case 0x73:
	case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7a: case 0x7b:
	case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		/* R, +[-16, -1] */

		ea = *rptr_xyus[r] + (op & 0xf) - 0x10;
		(*cycles)++;
		break;
	case 0x80: case 0x81:
	case 0xa0: case 0xa1:
	case 0xc0: case 0xc1:
	case 0xe0: case 0xe1:
		/* ,R+ / ,R++ */

		ea = *rptr_xyus[r];
		*rptr_xyus[r] += 1 + (op & 1);
		*cycles += 2 + (op & 1);
		break;
	case 0x90: case 0x91:
	case 0xb0: case 0xb1:
	case 0xd0: case 0xd1:
	case 0xf0: case 0xf1:
		/* [,R+] ??? / [,R++] */

		ea = read16 (*rptr_xyus[r]);
		*rptr_xyus[r] += 1 + (op & 1);
		*cycles += 5 + (op & 1);
		break;
	case 0x82: case 0x83:
	case 0xa2: case 0xa3:
	case 0xc2: case 0xc3:
	case 0xe2: case 0xe3:

		/* ,-R / ,--R */

		*rptr_xyus[r] -= 1 + (op & 1);
		ea = *rptr_xyus[r];
		*cycles += 2 + (op & 1);
		break;
	case 0x92: case 0x93:
	case 0xb2: case 0xb3:
	case 0xd2: case 0xd3:
	case 0xf2: case 0xf3:
		/* [,-R] ??? / [,--R] */

		*rptr_xyus[r] -= 1 + (op & 1);
		ea = read16 (*rptr_xyus[r]);
		*cycles += 5 + (op & 1);
		break;
	case 0x84: case 0xa4:
	case 0xc4: case 0xe4:
		/* ,R */

		ea = *rptr_xyus[r];
		break;
	case 0x94: case 0xb4:
	case 0xd4: case 0xf4:
		/* [,R] */

		ea = read16 (*rptr_xyus[r]);
		*cycles += 3;
		break;
	case 0x85: case 0xa5:
	case 0xc5: case 0xe5:
		/* B,R */

		ea = *rptr_xyus[r] + sign_extend (reg_b);
		*cycles += 1;
		break;
	case 0x95: case 0xb5:
	case 0xd5: case 0xf5:
		/* [B,R] */

		ea = read16 (*rptr_xyus[r] + sign_extend (reg_b));
		*cycles += 4;
		break;
	case 0x86: case 0xa6:
	case 0xc6: case 0xe6:
		/* A,R */

		ea = *rptr_xyus[r] + sign_extend (reg_a);
		*cycles += 1;
		break;
	case 0x96: case 0xb6:
	case 0xd6: case 0xf6:
		/* [A,R] */

		ea = read16 (*rptr_xyus[r] + sign_extend (reg_a));
		*cycles += 4;
		break;
	case 0x88: case 0xa8:
	case 0xc8: case 0xe8:
		/* byte,R */

		ea = *rptr_xyus[r] + sign_extend (pc_read8 ());
		*cycles += 1;
		break;
	case 0x98: case 0xb8:
	case 0xd8: case 0xf8:
		/* [byte,R] */

		ea = read16 (*rptr_xyus[r] + sign_extend (pc_read8 ()));
		*cycles += 4;
		break;
	case 0x89: case 0xa9:
	case 0xc9: case 0xe9:
		/* word,R */

		ea = *rptr_xyus[r] + pc_read16 ();
		*cycles += 4;
		break;
	case 0x99: case 0xb9:
	case 0xd9: case 0xf9:
		/* [word,R] */

		ea = read16 (*rptr_xyus[r] + pc_read16 ());
		*cycles += 7;
		break;
	case 0x8b: case 0xab:
	case 0xcb: case 0xeb:
		/* D,R */

		ea = *rptr_xyus[r] + get_reg_d ();
		*cycles += 4;
		break;
	case 0x9b: case 0xbb:
	case 0xdb: case 0xfb:
		/* [D,R] */

		ea = read16 (*rptr_xyus[r] + get_reg_d ());
		*cycles += 7;
		break;
	case 0x8c: case 0xac:
	case 0xcc: case 0xec:
		/* byte, PC */

		r = sign_extend (pc_read8 ());
		ea = reg_pc + r;
		*cycles += 1;
		break;
	case 0x9c: case 0xbc:
	case 0xdc: case 0xfc:
		/* [byte, PC] */

		r = sign_extend (pc_read8 ());
		ea = read16 (reg_pc + r);
		*cycles += 4;
		break;
	case 0x8d: case 0xad:
	case 0xcd: case 0xed:
		/* word, PC */

		r = pc_read16 ();
		ea = reg_pc + r;
		*cycles += 5;
		break;
	case 0x9d: case 0xbd:
	case 0xdd: case 0xfd:
		/* [word, PC] */

		r = pc_read16 ();
		ea = read16 (reg_pc + r);
		*cycles += 8;
		break;
	case 0x9f:
		/* [address] */

		ea = read16 (pc_read16 ());
		*cycles += 5;
		break;
	default:
		printf ("undefined post-byte\n");
		ea=0; // to avoid a compiler warning
		break;
	}

	return ea;
}

/* instruction: neg
 * essentially (0 - data).
 */

static einline unsigned short  inst_neg (unsigned short  data)
{
	unsigned short  i0, i1, r;

	i0 = 0;
	i1 = ~data;
	r = i0 + i1 + 1;

	set_cc (FLAG_H, test_c (i0 << 4, i1 << 4, r << 4, 0));
	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 1));

	return r;
}

/* instruction: com */

static einline unsigned short  inst_com (unsigned short  data)
{
	unsigned short  r;

	r = ~data;

	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, 0);
	set_cc (FLAG_C, 1);

	return r;
}

/* instruction: lsr
 * cannot be faked as an add or substract.
 */

static einline unsigned short  inst_lsr (unsigned short  data)
{
	unsigned short  r;

	r = (data >> 1) & 0x7f;

	set_cc (FLAG_N, 0);
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_C, data & 1);

	return r;
}

/* instruction: ror
 * cannot be faked as an add or substract.
 */

static einline unsigned short  inst_ror (unsigned short  data)
{
	unsigned short  r, c;

	c = get_cc (FLAG_C);
	r = ((data >> 1) & 0x7f) | (c << 7);

	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_C, data & 1);

	return r;
}

/* instruction: asr
 * cannot be faked as an add or substract.
 */

static einline unsigned short  inst_asr (unsigned short  data)
{
	unsigned short  r;

	r = ((data >> 1) & 0x7f) | (data & 0x80);

	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_C, data & 1);

	return r;
}

/* instruction: asl
 * essentially (data + data). simple addition.
 */

static einline unsigned short  inst_asl (unsigned short  data)
{
	unsigned short  i0, i1, r;

	i0 = data;
	i1 = data;
	r = i0 + i1;

	set_cc (FLAG_H, test_c (i0 << 4, i1 << 4, r << 4, 0));
	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 0));

	return r;
}

/* instruction: rol
 * essentially (data + data + carry). addition with carry.
 */

static einline unsigned short  inst_rol (unsigned short  data)
{
	unsigned short  i0, i1, c, r;

	i0 = data;
	i1 = data;
	c = get_cc (FLAG_C);
	r = i0 + i1 + c;

	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 0));

	return r;
}

/* instruction: dec
 * essentially (data - 1).
 */

static einline unsigned short  inst_dec (unsigned short  data)
{
	unsigned short  i0, i1, r;

	i0 = data;
	i1 = 0xff;
	r = i0 + i1;

	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));

	return r;
}

/* instruction: inc
 * essentially (data + 1).
 */

static einline unsigned short  inst_inc (unsigned short  data)
{
	unsigned short  i0, i1, r;

	i0 = data;
	i1 = 1;
	r = i0 + i1;

	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));

	return r;
}

/* instruction: tst */

static einline void inst_tst8 (unsigned short  data)
{
	set_cc (FLAG_N, test_n (data));
	set_cc (FLAG_Z, test_z8 (data));
	set_cc (FLAG_V, 0);
}

static einline void inst_tst16 (unsigned short  data)
{
	set_cc (FLAG_N, test_n (data >> 8));
	set_cc (FLAG_Z, test_z16 (data));
	set_cc (FLAG_V, 0);
}

/* instruction: clr */

static einline void inst_clr (void)
{
	set_cc (FLAG_N, 0);
	set_cc (FLAG_Z, 1);
	set_cc (FLAG_V, 0);
	set_cc (FLAG_C, 0);
}

/* instruction: suba/subb */

static einline unsigned short  inst_sub8 (unsigned short  data0, unsigned short  data1)
{
	unsigned short  i0, i1, r;

	i0 = data0;
	i1 = ~data1;
	r = i0 + i1 + 1;

	set_cc (FLAG_H, test_c (i0 << 4, i1 << 4, r << 4, 0));
	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 1));

	return r;
}

/* instruction: sbca/sbcb/cmpa/cmpb.
 * only 8-bit version, 16-bit version not needed.
 */

static einline unsigned short  inst_sbc (unsigned short  data0, unsigned short  data1)
{
	unsigned short  i0, i1, c, r;

	i0 = data0;
	i1 = ~data1;
	c = 1 - get_cc (FLAG_C);
	r = i0 + i1 + c;

	set_cc (FLAG_H, test_c (i0 << 4, i1 << 4, r << 4, 0));
	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 1));

	return r;
}

/* instruction: anda/andb/bita/bitb.
 * only 8-bit version, 16-bit version not needed.
 */

static einline unsigned short  inst_and (unsigned short  data0, unsigned short  data1)
{
	unsigned short  r;

	r = data0 & data1;

	inst_tst8 (r);

	return r;
}

/* instruction: eora/eorb.
 * only 8-bit version, 16-bit version not needed.
 */

static einline unsigned short  inst_eor (unsigned short  data0, unsigned short  data1)
{
	unsigned short  r;

	r = data0 ^ data1;

	inst_tst8 (r);

	return r;
}

/* instruction: adca/adcb
 * only 8-bit version, 16-bit version not needed.
 */

static einline unsigned short  inst_adc (unsigned short  data0, unsigned short  data1)
{
	unsigned short  i0, i1, c, r;

	i0 = data0;
	i1 = data1;
	c = get_cc (FLAG_C);
	r = i0 + i1 + c;

	set_cc (FLAG_H, test_c (i0 << 4, i1 << 4, r << 4, 0));
	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 0));

	return r;
}

/* instruction: ora/orb.
 * only 8-bit version, 16-bit version not needed.
 */

static einline unsigned short  inst_or (unsigned short  data0, unsigned short  data1)
{
	unsigned short  r;

	r = data0 | data1;

	inst_tst8 (r);

	return r;
}

/* instruction: adda/addb */

static einline unsigned short  inst_add8 (unsigned short  data0, unsigned short  data1)
{
	unsigned short  i0, i1, r;

	i0 = data0;
	i1 = data1;
	r = i0 + i1;

	set_cc (FLAG_H, test_c (i0 << 4, i1 << 4, r << 4, 0));
	set_cc (FLAG_N, test_n (r));
	set_cc (FLAG_Z, test_z8 (r));
	set_cc (FLAG_V, test_v (i0, i1, r));
	set_cc (FLAG_C, test_c (i0, i1, r, 0));

	return r;
}

/* instruction: addd */

static einline unsigned short  inst_add16 (unsigned short  data0, unsigned short  data1)
{
	unsigned short  i0, i1, r;

	i0 = data0;
	i1 = data1;
	r = i0 + i1;

	set_cc (FLAG_N, test_n (r >> 8));
	set_cc (FLAG_Z, test_z16 (r));
	set_cc (FLAG_V, test_v (i0 >> 8, i1 >> 8, r >> 8));
	set_cc (FLAG_C, test_c (i0 >> 8, i1 >> 8, r >> 8, 0));

	return r;
}

/* instruction: subd */

static einline unsigned short  inst_sub16 (unsigned short  data0, unsigned short  data1)
{
	unsigned short  i0, i1, r;

	i0 = data0;
	i1 = ~data1;
	r = i0 + i1 + 1;

	set_cc (FLAG_N, test_n (r >> 8));
	set_cc (FLAG_Z, test_z16 (r));
	set_cc (FLAG_V, test_v (i0 >> 8, i1 >> 8, r >> 8));
	set_cc (FLAG_C, test_c (i0 >> 8, i1 >> 8, r >> 8, 1));

	return r;
}

/* instruction: 8-bit offset branch */

static einline void inst_bra8 (unsigned short  test, unsigned short  op, unsigned short  *cycles)
{
	unsigned short  offset, mask;

	offset = pc_read8 ();

	/* trying to avoid an if statement */

	mask = (test ^ (op & 1)) - 1; /* 0xffff when taken, 0 when not taken */
	reg_pc += sign_extend (offset) & mask;

	*cycles += 3;
}

/* instruction: 16-bit offset branch */

static einline void inst_bra16 (unsigned short  test, unsigned short  op, unsigned short  *cycles)
{
	unsigned short  offset, mask;

	offset = pc_read16 ();

	/* trying to avoid an if statement */

	mask = (test ^ (op & 1)) - 1; /* 0xffff when taken, 0 when not taken */
	reg_pc += offset & mask;

	*cycles += 5 - mask;
}

/* instruction: pshs/pshu */

static einline void inst_psh (unsigned short  op, unsigned short  *sp,
					   unsigned short  data, unsigned short  *cycles)
{
	if (op & 0x80) {
		push16 (sp, reg_pc);
		*cycles += 2;
	}

	if (op & 0x40) {
		/* either s or u */
		push16 (sp, data);
		*cycles += 2;
	}

	if (op & 0x20) {
		push16 (sp, reg_y);
		*cycles += 2;
	}

	if (op & 0x10) {
		push16 (sp, reg_x);
		*cycles += 2;
	}

	if (op & 0x08) {
		push8 (sp, reg_dp);
		*cycles += 1;
	}

	if (op & 0x04) {
		push8 (sp, reg_b);
		*cycles += 1;
	}

	if (op & 0x02) {
		push8 (sp, reg_a);
		*cycles += 1;
	}

	if (op & 0x01) {
		push8 (sp, reg_cc);
		*cycles += 1;
	}
}

/* instruction: puls/pulu */

static einline void inst_pul (unsigned short  op, unsigned short  *sp, unsigned short  *osp,
					   unsigned short  *cycles)
{
	if (op & 0x01) {
		reg_cc = pull8 (sp);
		*cycles += 1;
	}

	if (op & 0x02) {
		reg_a = pull8 (sp);
		*cycles += 1;
	}

	if (op & 0x04) {
		reg_b = pull8 (sp);
		*cycles += 1;
	}

	if (op & 0x08) {
		reg_dp = pull8 (sp);
		*cycles += 1;
	}

	if (op & 0x10) {
		reg_x = pull16 (sp);
		*cycles += 2;
	}

	if (op & 0x20) {
		reg_y = pull16 (sp);
		*cycles += 2;
	}

	if (op & 0x40) {
		/* either s or u */
		*osp = pull16 (sp);
		*cycles += 2;
	}

	if (op & 0x80) {
		reg_pc = pull16 (sp);
		*cycles += 2;
	}
}

static einline unsigned short  exgtfr_read (unsigned short  reg)
{
	unsigned short  data;

	switch (reg) {
	case 0x0:
		data = get_reg_d ();
		break;
	case 0x1:
		data = reg_x;
		break;
	case 0x2:
		data = reg_y;
		break;
	case 0x3:
		data = reg_u;
		break;
	case 0x4:
		data = reg_s;
		break;
	case 0x5:
		data = reg_pc;
		break;
	case 0x8:
		data = 0xff00 | reg_a;
		break;
	case 0x9:
		data = 0xff00 | reg_b;
		break;
	case 0xa:
		data = 0xff00 | reg_cc;
		break;
	case 0xb:
		data = 0xff00 | reg_dp;
		break;
	default:
		data = 0xffff;
		printf ("illegal exgtfr reg %.1x\n", reg);
		break;
	}

	return data;
}

static einline void exgtfr_write (unsigned short  reg, unsigned short  data)
{
	switch (reg) {
	case 0x0:
		set_reg_d (data);
		break;
	case 0x1:
		reg_x = data;
		break;
	case 0x2:
		reg_y = data;
		break;
	case 0x3:
		reg_u = data;
		break;
	case 0x4:
		reg_s = data;
		break;
	case 0x5:
		reg_pc = data;
		break;
	case 0x8:
		reg_a = data;
		break;
	case 0x9:
		reg_b = data;
		break;
	case 0xa:
		reg_cc = data;
		break;
	case 0xb:
		reg_dp = data;
		break;
	default:
		printf ("illegal exgtfr reg %.1x\n", reg);
		break;
	}
}

/* instruction: exg */

static einline void inst_exg (void)
{
	unsigned short  op, tmp;

	op = pc_read8 ();

	tmp = exgtfr_read (op & 0xf);
	exgtfr_write (op & 0xf, exgtfr_read (op >> 4));
	exgtfr_write (op >> 4, tmp);
}

/* instruction: tfr */

static einline void inst_tfr (void)
{
	unsigned short op;

	op = pc_read8 ();

	exgtfr_write (op & 0xf, exgtfr_read (op >> 4));
}

/* reset the 6809 */

void e6809_reset (void)
{
	reg_x = 0;
	reg_y = 0;
	reg_u = 0;
	reg_s = 0;

	reg_a = 0;
	reg_b = 0;

	reg_dp = 0;

	reg_cc = FLAG_I | FLAG_F;
	irq_status = IRQ_NORMAL;

	reg_pc = read16 (0xfffe);
}


unsigned int ins_0x00(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_neg (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x40(void) {
		reg_a = inst_neg (reg_a);
		return 2;
}

unsigned int ins_0x50(void) {
		reg_b = inst_neg (reg_b);
		return 2;
}
		
unsigned int ins_0x60(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;

		ea = ea_indexed (&cycles);
		r = inst_neg (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x70(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_neg (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* com, coma, comb */
unsigned int ins_0x03(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_com (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x43(void) {
		reg_a = inst_com (reg_a);
		return 2;
}

unsigned int ins_0x53(void) {
		reg_b = inst_com (reg_b);
		return 2;
}

unsigned int ins_0x63(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;

		ea = ea_indexed (&cycles);
		r = inst_com (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x73(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_com (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* lsr, lsra, lsrb */
unsigned int ins_0x04(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_lsr (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x44(void) {
		reg_a = inst_lsr (reg_a);
		return 2;
}

unsigned int ins_0x54(void) {
		reg_b = inst_lsr (reg_b);
		return 2;
}

unsigned int ins_0x64(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;
		ea = ea_indexed (&cycles);
		r = inst_lsr (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x74(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_lsr (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* ror, rora, rorb */
unsigned int ins_0x06(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_ror (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x46(void) {
		reg_a = inst_ror (reg_a);
		return 2;
}

unsigned int ins_0x56(void) {
		reg_b = inst_ror (reg_b);
		return 2;
}

unsigned int ins_0x66(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;
		ea = ea_indexed (&cycles);
		r = inst_ror (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x76(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_ror (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* asr, asra, asrb */
unsigned int ins_0x07(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_asr (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x47(void) {
		reg_a = inst_asr (reg_a);
		return 2;
}

unsigned int ins_0x57(void) {
		reg_b = inst_asr (reg_b);
		return 2;
}

unsigned int ins_0x67(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;
		ea = ea_indexed (&cycles);
		r = inst_asr (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x77(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_asr (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* asl, asla, aslb */
unsigned int ins_0x08(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_asl (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x48(void) {
		reg_a = inst_asl (reg_a);
		return 2;
}

unsigned int ins_0x58(void) {
		reg_b = inst_asl (reg_b);
		return 2;
}

unsigned int ins_0x68(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;
		ea = ea_indexed (&cycles);
		r = inst_asl (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x78(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_asl (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* rol, rola, rolb */
unsigned int ins_0x09(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_rol (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x49(void) {
		reg_a = inst_rol (reg_a);
		return 2;
}

unsigned int ins_0x59(void) {
		reg_b = inst_rol (reg_b);
		return 2;
}

unsigned int ins_0x69(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;
		ea = ea_indexed (&cycles);
		r = inst_rol (read8 (ea));
		write8 (ea, r);
		return cycles + 6;
}

unsigned int ins_0x79(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_rol (read8 (ea));
		write8 (ea, r);
		return 7;
}

	/* dec, deca, decb */
unsigned int ins_0x0a(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_dec (read8 (ea));
		write8 (ea, r);
		return 6;
}

unsigned int ins_0x4a(void) {
		reg_a = inst_dec (reg_a);
		return 2;
}

unsigned int ins_0x5a(void) {
		reg_b = inst_dec (reg_b);
		return 2;
}

unsigned int ins_0x6a(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;

		ea = ea_indexed (&cycles);
		r = inst_dec (read8 (ea));
		write8 (ea, r);
		return cycles + 6;}
		
unsigned int ins_0x7a(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_dec (read8 (ea));
		write8 (ea, r);
		return 7;}
		
	/* inc, inca, incb */
unsigned int ins_0x0c(void) {
	unsigned short  ea, r;
		ea = ea_direct ();
		r = inst_inc (read8 (ea));
		write8 (ea, r);
		return 6;}
		
unsigned int ins_0x4c(void) {
		reg_a = inst_inc (reg_a);
		return 2;
}
unsigned int ins_0x5c(void) {
		reg_b = inst_inc (reg_b);
		return 2;
}
unsigned int ins_0x6c(void) {
	unsigned short  cycles = 0;
	unsigned short  ea, r;

		ea = ea_indexed (&cycles);
		r = inst_inc (read8 (ea));
		write8 (ea, r);
		return cycles + 6;}
		
unsigned int ins_0x7c(void) {
	unsigned short  ea, r;
		ea = ea_extended ();
		r = inst_inc (read8 (ea));
		write8 (ea, r);
		return 7;}
		
	/* tst, tsta, tstb */
unsigned int ins_0x0d(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_tst8 (read8 (ea));
		return 6;}
		
unsigned int ins_0x4d(void) {
		inst_tst8 (reg_a);
		return 2;
}

unsigned int ins_0x5d(void) {
		inst_tst8 (reg_b);
		return 2;
}

unsigned int ins_0x6d(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		inst_tst8 (read8 (ea));
		return cycles + 6;}
		
unsigned int ins_0x7d(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_tst8 (read8 (ea));
		return 7;}
		
	/* jmp */
unsigned int ins_0x0e(void) {
		reg_pc = ea_direct ();
		return 3;}
		
unsigned int ins_0x6e(void) {
	unsigned short  cycles = 0;
		reg_pc = ea_indexed (&cycles);
		return cycles + 3;}
		
unsigned int ins_0x7e(void) {
		reg_pc = ea_extended ();
		return 4;}
		
	/* clr */
unsigned int ins_0x0f(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_clr ();
		write8 (ea, 0);
		return 6;}
		
unsigned int ins_0x4f(void) {
		inst_clr ();
		reg_a = 0;
		return 2;
}

unsigned int ins_0x5f(void) {
		inst_clr ();
		reg_b = 0;
		return 2;
}

unsigned int ins_0x6f(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		inst_clr ();
		write8 (ea, 0);
		return cycles + 6;}
		
unsigned int ins_0x7f(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_clr ();
		write8 (ea, 0);
		return 7;}
		
	/* suba */
unsigned int ins_0x80(void) {
		reg_a = inst_sub8 (reg_a, pc_read8 ());
		return 2;}

unsigned int ins_0x90(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_sub8 (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa0(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		reg_a = inst_sub8 (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb0(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_sub8 (reg_a, read8 (ea));
		return 5;}
		
	/* subb */
unsigned int ins_0xc0(void) {
		reg_b = inst_sub8 (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd0(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_sub8 (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe0(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		reg_b = inst_sub8 (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf0(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_sub8 (reg_b, read8 (ea));
		return 5;}
		
	/* cmpa */
unsigned int ins_0x81(void) {
		inst_sub8 (reg_a, pc_read8 ());
		return 2;}
		
unsigned int ins_0x91(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_sub8 (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa1(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		inst_sub8 (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb1(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_sub8 (reg_a, read8 (ea));
		return 5;}
		
	/* cmpb */
unsigned int ins_0xc1(void) {
		inst_sub8 (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd1(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_sub8 (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe1(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		inst_sub8 (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf1(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_sub8 (reg_b, read8 (ea));
		return 5;}
		
	/* sbca */
unsigned int ins_0x82(void) {
		reg_a = inst_sbc (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x92(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_sbc (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa2(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		reg_a = inst_sbc (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb2(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_sbc (reg_a, read8 (ea));
		return 5;}
		
	/* sbcb */
unsigned int ins_0xc2(void) {
		reg_b = inst_sbc (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd2(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_sbc (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe2(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		reg_b = inst_sbc (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf2(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_sbc (reg_b, read8 (ea));
		return 5;}
		
	/* anda */
unsigned int ins_0x84(void) {
		reg_a = inst_and (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x94(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_and (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa4(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		reg_a = inst_and (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb4(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_and (reg_a, read8 (ea));
		return 5;}
		
	/* andb */
unsigned int ins_0xc4(void) {
		reg_b = inst_and (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd4(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_and (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe4(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		reg_b = inst_and (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf4(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_and (reg_b, read8 (ea));
		return 5;}
		
	/* bita */
unsigned int ins_0x85(void) {
		inst_and (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x95(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_and (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa5(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		inst_and (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb5(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_and (reg_a, read8 (ea));
		return 5;}
		
	/* bitb */
unsigned int ins_0xc5(void) {
		inst_and (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd5(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_and (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe5(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;

		ea = ea_indexed (&cycles);
		inst_and (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf5(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_and (reg_b, read8 (ea));
		return 5;}
		
	/* lda */
unsigned int ins_0x86(void) {
		reg_a = pc_read8 ();
		inst_tst8 (reg_a);
		return 2;
}

unsigned int ins_0x96(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = read8 (ea);
		inst_tst8 (reg_a);
		return 4;}
		
unsigned int ins_0xa6(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_a = read8 (ea);
		inst_tst8 (reg_a);
		return cycles + 4;}
		
unsigned int ins_0xb6(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = read8 (ea);
		inst_tst8 (reg_a);
		return 5;}
		
	/* ldb */
unsigned int ins_0xc6(void) {
		reg_b = pc_read8 ();
		inst_tst8 (reg_b);
		return 2;
}

unsigned int ins_0xd6(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = read8 (ea);
		inst_tst8 (reg_b);
		return 4;}
		
unsigned int ins_0xe6(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_b = read8 (ea);
		inst_tst8 (reg_b);
		return cycles + 4;}
		
unsigned int ins_0xf6(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = read8 (ea);
		inst_tst8 (reg_b);
		return 5;}
		
	/* sta */
unsigned int ins_0x97(void) {
	unsigned short  ea;
		ea = ea_direct ();
		write8 (ea, reg_a);
		inst_tst8 (reg_a);
		return 4;}
		
unsigned int ins_0xa7(void) {
	unsigned short  ea;
	unsigned short  cycles = 0;

		ea = ea_indexed (&cycles);
		write8 (ea, reg_a);
		inst_tst8 (reg_a);
		return cycles + 4;}
		
unsigned int ins_0xb7(void) {
	unsigned short  ea;
		ea = ea_extended ();
		write8 (ea, reg_a);
		inst_tst8 (reg_a);
		return 5;}
		
	/* stb */
unsigned int ins_0xd7(void) {
	unsigned short  ea;
		ea = ea_direct ();
		write8 (ea, reg_b);
		inst_tst8 (reg_b);
		return 4;}
		
unsigned int ins_0xe7(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		write8 (ea, reg_b);
		inst_tst8 (reg_b);
		return cycles + 4;}
		
unsigned int ins_0xf7(void) {
	unsigned short  ea;
		ea = ea_extended ();
		write8 (ea, reg_b);
		inst_tst8 (reg_b);
		return 5;}
		
	/* eora */
unsigned int ins_0x88(void) {
		reg_a = inst_eor (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x98(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_eor (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa8(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_a = inst_eor (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb8(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_eor (reg_a, read8 (ea));
		return 5;}
		
	/* eorb */
unsigned int ins_0xc8(void) {
		reg_b = inst_eor (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd8(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_eor (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe8(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_b = inst_eor (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf8(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_eor (reg_b, read8 (ea));
		return 5;}
		
	/* adca */
unsigned int ins_0x89(void) {
		reg_a = inst_adc (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x99(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_adc (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xa9(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_a = inst_adc (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xb9(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_adc (reg_a, read8 (ea));
		return 5;}
		
	/* adcb */
unsigned int ins_0xc9(void) {
		reg_b = inst_adc (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xd9(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_adc (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xe9(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_b = inst_adc (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xf9(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_adc (reg_b, read8 (ea));
		return 5;}
		
	/* ora */
unsigned int ins_0x8a(void) {
		reg_a = inst_or (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x9a(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_or (reg_a, read8 (ea));
		return 4;}
		
unsigned int ins_0xaa(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_a = inst_or (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xba(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_or (reg_a, read8 (ea));
		return 5;}
		
	/* orb */
unsigned int ins_0xca(void) {
		reg_b = inst_or (reg_b, pc_read8 ());
		return 2;
}

unsigned int ins_0xda(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_or (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xea(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_b = inst_or (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xfa(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_or (reg_b, read8 (ea));
		return 5;}
		
	/* adda */
unsigned int ins_0x8b(void) {
		reg_a = inst_add8 (reg_a, pc_read8 ());
		return 2;
}

unsigned int ins_0x9b(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_a = inst_add8 (reg_a, read8 (ea));
		return 4;}
		

// here
unsigned int ins_0xab(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_a = inst_add8 (reg_a, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xbb(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_a = inst_add8 (reg_a, read8 (ea));
		return 5;}
		
	/* addb */
unsigned int ins_0xcb(void) {
		reg_b = inst_add8 (reg_b, pc_read8 ());
		return 2;}
		
unsigned int ins_0xdb(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_b = inst_add8 (reg_b, read8 (ea));
		return 4;}
		
unsigned int ins_0xeb(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_b = inst_add8 (reg_b, read8 (ea));
		return cycles + 4;}
		
unsigned int ins_0xfb(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_b = inst_add8 (reg_b, read8 (ea));
		return 5;}
		
	/* subd */
unsigned int ins_0x83(void) {
		set_reg_d (inst_sub16 (get_reg_d (), pc_read16 ()));
		return 4;}
		
unsigned int ins_0x93(void) {
	unsigned short  ea;
		ea = ea_direct ();
		set_reg_d (inst_sub16 (get_reg_d (), read16 (ea)));
		return 6;}
		
unsigned int ins_0xa3(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		set_reg_d (inst_sub16 (get_reg_d (), read16 (ea)));
		return cycles + 6;}
		
unsigned int ins_0xb3(void) {
	unsigned short  ea;
		ea = ea_extended ();
		set_reg_d (inst_sub16 (get_reg_d (), read16 (ea)));
		return 7;}
		
	/* cmpx */
unsigned int ins_0x8c(void) {
		inst_sub16 (reg_x, pc_read16 ());
		return 4;}
		
unsigned int ins_0x9c(void) {
	unsigned short  ea;
		ea = ea_direct ();
		inst_sub16 (reg_x, read16 (ea));
		return 6;}
		
unsigned int ins_0xac(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		inst_sub16 (reg_x, read16 (ea));
		return cycles + 6;}
		
unsigned int ins_0xbc(void) {
	unsigned short  ea;
		ea = ea_extended ();
		inst_sub16 (reg_x, read16 (ea));
		return 7;}
		
	/* ldx */
unsigned int ins_0x8e(void) {
		reg_x = pc_read16 ();
		inst_tst16 (reg_x);
		return 3;}
		
unsigned int ins_0x9e(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_x = read16 (ea);
		inst_tst16 (reg_x);
		return 5;}
		
unsigned int ins_0xae(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_x = read16 (ea);
		inst_tst16 (reg_x);
		return cycles + 5;}
		
unsigned int ins_0xbe(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_x = read16 (ea);
		inst_tst16 (reg_x);
		return 6;}
		
	/* ldu */
unsigned int ins_0xce(void) {
		reg_u = pc_read16 ();
		inst_tst16 (reg_u);
		return 3;}
		
unsigned int ins_0xde(void) {
	unsigned short  ea;
		ea = ea_direct ();
		reg_u = read16 (ea);
		inst_tst16 (reg_u);
		return 5;}
		
unsigned int ins_0xee(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		reg_u = read16 (ea);
		inst_tst16 (reg_u);
		return cycles + 5;}
		
unsigned int ins_0xfe(void) {
	unsigned short  ea;
		ea = ea_extended ();
		reg_u = read16 (ea);
		inst_tst16 (reg_u);
		return 6;}
		
	/* stx */
unsigned int ins_0x9f(void) {
	unsigned short  ea;
		ea = ea_direct ();
		write16 (ea, reg_x);
		inst_tst16 (reg_x);
		return 5;}
		
unsigned int ins_0xaf(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		write16 (ea, reg_x);
		inst_tst16 (reg_x);
		return cycles + 5;}
		
unsigned int ins_0xbf(void) {
	unsigned short  ea;
		ea = ea_extended ();
		write16 (ea, reg_x);
		inst_tst16 (reg_x);
		return 6;}
		
	/* stu */
unsigned int ins_0xdf(void) {
	unsigned short  ea;
		ea = ea_direct ();
		write16 (ea, reg_u);
		inst_tst16 (reg_u);
		return 5;}
		
unsigned int ins_0xef(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		write16 (ea, reg_u);
		inst_tst16 (reg_u);
		return cycles + 5;}
		
unsigned int ins_0xff(void) {
	unsigned short  ea;
		ea = ea_extended ();
		write16 (ea, reg_u);
		inst_tst16 (reg_u);
		return 6;}
		
	/* addd */
unsigned int ins_0xc3(void) {
		set_reg_d (inst_add16 (get_reg_d (), pc_read16 ()));
		return 4;}
		
unsigned int ins_0xd3(void) {
	unsigned short  ea;
		ea = ea_direct ();
		set_reg_d (inst_add16 (get_reg_d (), read16 (ea)));
		return 6;}
		
unsigned int ins_0xe3(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		set_reg_d (inst_add16 (get_reg_d (), read16 (ea)));
		return cycles + 6;}
		
unsigned int ins_0xf3(void) {
	unsigned short  ea;
		ea = ea_extended ();
		set_reg_d (inst_add16 (get_reg_d (), read16 (ea)));
		return 7;}
		
	/* ldd */
unsigned int ins_0xcc(void) {
		set_reg_d (pc_read16 ());
		inst_tst16 (get_reg_d ());
		return 3;}
		
unsigned int ins_0xdc(void) {
	unsigned short  ea;
		ea = ea_direct ();
		set_reg_d (read16 (ea));
		inst_tst16 (get_reg_d ());
		return 5;}
		
unsigned int ins_0xec(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		set_reg_d (read16 (ea));
		inst_tst16 (get_reg_d ());
		return cycles + 5;}
		
unsigned int ins_0xfc(void) {
	unsigned short  ea;
		ea = ea_extended ();
		set_reg_d (read16 (ea));
		inst_tst16 (get_reg_d ());
		return 6;}
		
	/* std */
unsigned int ins_0xdd(void) {
	unsigned short  ea;
		ea = ea_direct ();
		write16 (ea, get_reg_d ());
		inst_tst16 (get_reg_d ());
		return 5;}
		
unsigned int ins_0xed(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		write16 (ea, get_reg_d ());
		inst_tst16 (get_reg_d ());
		return cycles + 5;}
		
unsigned int ins_0xfd(void) {
	unsigned short  ea;
		ea = ea_extended ();
		write16 (ea, get_reg_d ());
		inst_tst16 (get_reg_d ());
		return 6;}
		
	/* nop */
unsigned int ins_0x12(void) {
		return 2;}
		
	/* mul */
unsigned int ins_0x3d(void) {
	unsigned short  r;
		r = (reg_a & 0xff) * (reg_b & 0xff);
		set_reg_d (r);

		set_cc (FLAG_Z, test_z16 (r));
		set_cc (FLAG_C, (r >> 7) & 1);

		return 11;}
		
	/* bra */
unsigned int ins_0x20(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (0, op, &cycles);
		inst_bra8 (0, 0x20, &cycles);
		return cycles;
}		

	/* brn */
unsigned int ins_0x21(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (0, op, &cycles);
		inst_bra8 (0, 0x21, &cycles);
		return cycles;
}		
	/* bhi */
unsigned int ins_0x22(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_C) | get_cc (FLAG_Z), op, &cycles);
		inst_bra8 (get_cc (FLAG_C) | get_cc (FLAG_Z), 0x22, &cycles);
		return cycles;
}		
	/* bls */
unsigned int ins_0x23(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_C) | get_cc (FLAG_Z), op, &cycles);
		inst_bra8 (get_cc (FLAG_C) | get_cc (FLAG_Z), 0x23, &cycles);
		return cycles;
}		
	/* bhs/bcc */
unsigned int ins_0x24(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_C), op, &cycles);
		inst_bra8 (get_cc (FLAG_C), 0x24, &cycles);
		return cycles;
}		
	/* blo/bcs */
unsigned int ins_0x25(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_C), op, &cycles);
		inst_bra8 (get_cc (FLAG_C), 0x25, &cycles);
		return cycles;
}		
	/* bne */
unsigned int ins_0x26(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_Z), op, &cycles);
		inst_bra8 (get_cc (FLAG_Z), 0x26, &cycles);
		return cycles;
}		
	/* beq */
unsigned int ins_0x27(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_Z), op, &cycles);
		inst_bra8 (get_cc (FLAG_Z), 0x27, &cycles);
		return cycles;
}		
	/* bvc */
unsigned int ins_0x28(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_V), op, &cycles);
		inst_bra8 (get_cc (FLAG_V), 0x28, &cycles);
		return cycles;
}		
	/* bvs */
unsigned int ins_0x29(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_V), op, &cycles);
		inst_bra8 (get_cc (FLAG_V), 0x29, &cycles);
		return cycles;
}		
	/* bpl */
unsigned int ins_0x2a(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_N), op, &cycles);
		inst_bra8 (get_cc (FLAG_N), 0x2a, &cycles);
		return cycles;
}		
	/* bmi */
unsigned int ins_0x2b(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_N), op, &cycles);
		inst_bra8 (get_cc (FLAG_N), 0x2b, &cycles);
		return cycles;
}		
	/* bge */
unsigned int ins_0x2c(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_N) ^ get_cc (FLAG_V), op, &cycles);
		inst_bra8 (get_cc (FLAG_N) ^ get_cc (FLAG_V), 0x2c, &cycles);
		return cycles;
}		
	/* blt */
unsigned int ins_0x2d(void) {
		unsigned short  cycles = 0;

//		inst_bra8 (get_cc (FLAG_N) ^ get_cc (FLAG_V), op, &cycles);
		inst_bra8 (get_cc (FLAG_N) ^ get_cc (FLAG_V), 0x2d, &cycles);
		return cycles;
}		
	/* bgt */
unsigned int ins_0x2e(void) {
		unsigned short  cycles = 0;

		inst_bra8 (get_cc (FLAG_Z) |
				   (get_cc (FLAG_N) ^ get_cc (FLAG_V)), 0x2e, &cycles);
//				   (get_cc (FLAG_N) ^ get_cc (FLAG_V)), op, &cycles);
		return cycles;
}		
	/* ble */
unsigned int ins_0x2f(void) {
		unsigned short  cycles = 0;

		inst_bra8 (get_cc (FLAG_Z) |
				   (get_cc (FLAG_N) ^ get_cc (FLAG_V)), 0x2f, &cycles);
//				   (get_cc (FLAG_N) ^ get_cc (FLAG_V)), op, &cycles);
		return cycles;
}		
	/* lbra */
unsigned int ins_0x16(void) {
	unsigned short  r;
		r = pc_read16 ();
		reg_pc += r;
		return 5;}
		
	/* lbsr */
unsigned int ins_0x17(void) {
	unsigned short  r;
		r = pc_read16 ();
		push16 (&reg_s, reg_pc);
		reg_pc += r;
		return 9;}
		
	/* bsr */
unsigned int ins_0x8d(void) {
	unsigned short  r;
		r = pc_read8 ();
		push16 (&reg_s, reg_pc);
		reg_pc += sign_extend (r);
		return 7;}
		
	/* jsr */
unsigned int ins_0x9d(void) {
	unsigned short  ea;
		ea = ea_direct ();
		push16 (&reg_s, reg_pc);
		reg_pc = ea;
		return 7;}
		
unsigned int ins_0xad(void) {
	unsigned short  cycles = 0;
	unsigned short  ea;
		ea = ea_indexed (&cycles);
		push16 (&reg_s, reg_pc);
		reg_pc = ea;
		return cycles + 7;}
		
unsigned int ins_0xbd(void) {
	unsigned short  ea;
		ea = ea_extended ();
		push16 (&reg_s, reg_pc);
		reg_pc = ea;
		return 8;}
		
	/* leax */
unsigned int ins_0x30(void) {
		unsigned short  cycles = 0;
		reg_x = ea_indexed (&cycles);
		set_cc (FLAG_Z, test_z16 (reg_x));
		return cycles + 4;}
		
	/* leay */
unsigned int ins_0x31(void) {
		unsigned short  cycles = 0;
		reg_y = ea_indexed (&cycles);
		set_cc (FLAG_Z, test_z16 (reg_y));
		return cycles + 4;}
		
	/* leas */
unsigned int ins_0x32(void) {
		unsigned short  cycles = 0;
		reg_s = ea_indexed (&cycles);
		return cycles + 4;}
		
	/* leau */
unsigned int ins_0x33(void) {
		unsigned short  cycles = 0;
		reg_u = ea_indexed (&cycles);
		return cycles + 4;}
		
	/* pshs */
unsigned int ins_0x34(void) {
		unsigned short  cycles = 0;

		inst_psh (pc_read8 (), &reg_s, reg_u, &cycles);
		return cycles + 5;}
		
	/* puls */
unsigned int ins_0x35(void) {
		unsigned short  cycles = 0;

		inst_pul (pc_read8 (), &reg_s, &reg_u, &cycles);
		return cycles + 5;}
		
	/* pshu */
unsigned int ins_0x36(void) {
		unsigned short  cycles = 0;

		inst_psh (pc_read8 (), &reg_u, reg_s, &cycles);
		return cycles + 5;}
		
	/* pulu */
unsigned int ins_0x37(void) {
		unsigned short  cycles = 0;

		inst_pul (pc_read8 (), &reg_u, &reg_s, &cycles);
		return cycles + 5;}
		
	/* rts */
unsigned int ins_0x39(void) {
		reg_pc = pull16 (&reg_s);
		return 5;}
		
	/* abx */
unsigned int ins_0x3a(void) {
		reg_x += reg_b & 0xff;
		return 3;}
		
	/* orcc */
unsigned int ins_0x1a(void) {
		reg_cc |= pc_read8 ();
		return 3;}
		
	/* andcc */
unsigned int ins_0x1c(void) {
		reg_cc &= pc_read8 ();
		return 3;}
		
	/* sex */
unsigned int ins_0x1d(void) {
		set_reg_d (sign_extend (reg_b));
		set_cc (FLAG_N, test_n (reg_a));
		set_cc (FLAG_Z, test_z16 (get_reg_d ()));
		return 2;}
		
	/* exg */
unsigned int ins_0x1e(void) {
		inst_exg ();
		return 8;}
		
	/* tfr */
unsigned int ins_0x1f(void) {
		inst_tfr ();
		return 6;}
		
	/* rti */
unsigned int ins_0x3b(void) {
		unsigned short  cycles = 0;

		if (get_cc (FLAG_E)) {
			inst_pul (0xff, &reg_s, &reg_u, &cycles);
		} else {
			inst_pul (0x81, &reg_s, &reg_u, &cycles);
		}

		return cycles + 3;}
		
	/* swi */
unsigned int ins_0x3f(void) {
		unsigned short  cycles = 0;

		set_cc (FLAG_E, 1);
		inst_psh (0xff, &reg_s, reg_u, &cycles);
		set_cc (FLAG_I, 1);
		set_cc (FLAG_F, 1);
        reg_pc = read16 (0xfffa);
        return cycles + 7;}
		
	/* sync */
unsigned int ins_0x13(void) {
		irq_status = IRQ_SYNC;
		return 2;}
		
	/* daa */
unsigned int ins_0x19(void) {
	unsigned short  i0, i1;
		i0 = reg_a;
		i1 = 0;

		if ((reg_a & 0x0f) > 0x09 || get_cc (FLAG_H) == 1) {
			i1 |= 0x06;
		}

		if ((reg_a & 0xf0) > 0x80 && (reg_a & 0x0f) > 0x09) {
			i1 |= 0x60;
		}

		if ((reg_a & 0xf0) > 0x90 || get_cc (FLAG_C) == 1) {
			i1 |= 0x60;
		}

		reg_a = i0 + i1;

		set_cc (FLAG_N, test_n (reg_a));
		set_cc (FLAG_Z, test_z8 (reg_a));
		set_cc (FLAG_V, 0);
		set_cc (FLAG_C, test_c (i0, i1, reg_a, 0));
		return 2;}
		
	/* cwai */
unsigned int ins_0x3c(void) {
		unsigned short  cycles = 0;

		reg_cc &= pc_read8 ();
		set_cc (FLAG_E, 1);
		inst_psh (0xff, &reg_s, reg_u, &cycles);
		irq_status = IRQ_CWAI;
		return cycles + 4;}
		

	/* page 1 instructions */

unsigned int ins_0x10(void) {

	unsigned short  op;
	unsigned short  cycles = 0;
	unsigned short  ea;

		op = pc_read8 ();

		switch (op) {
		/* lbra */
		case 0x20:
		/* lbrn */
		case 0x21:
			inst_bra16 (0, op, &cycles);
			break;
		/* lbhi */
		case 0x22:
		/* lbls */
		case 0x23:
			inst_bra16 (get_cc (FLAG_C) | get_cc (FLAG_Z), op, &cycles);
			break;
		/* lbhs/lbcc */
		case 0x24:
		/* lblo/lbcs */
		case 0x25:
			inst_bra16 (get_cc (FLAG_C), op, &cycles);
			break;
		/* lbne */
		case 0x26:
		/* lbeq */
		case 0x27:
			inst_bra16 (get_cc (FLAG_Z), op, &cycles);
			break;
		/* lbvc */
		case 0x28:
		/* lbvs */
		case 0x29:
			inst_bra16 (get_cc (FLAG_V), op, &cycles);
			break;
		/* lbpl */
		case 0x2a:
		/* lbmi */
		case 0x2b:
			inst_bra16 (get_cc (FLAG_N), op, &cycles);
			break;
		/* lbge */
		case 0x2c:
		/* lblt */
		case 0x2d:
			inst_bra16 (get_cc (FLAG_N) ^ get_cc (FLAG_V), op, &cycles);
			break;
		/* lbgt */
		case 0x2e:
		/* lble */
		case 0x2f:
			inst_bra16 (get_cc (FLAG_Z) |
						(get_cc (FLAG_N) ^ get_cc (FLAG_V)), op, &cycles);
			break;
		/* cmpd */
		case 0x83:
			inst_sub16 (get_reg_d (), pc_read16 ());
			cycles += 5;
			break;
		case 0x93:
			ea = ea_direct ();
			inst_sub16 (get_reg_d (), read16 (ea));
			cycles += 7;
			break;
		case 0xa3:
			ea = ea_indexed (&cycles);
			inst_sub16 (get_reg_d (), read16 (ea));
			cycles += 7;
			break;
		case 0xb3:
			ea = ea_extended ();
			inst_sub16 (get_reg_d (), read16 (ea));
			cycles += 8;
			break;
		/* cmpy */
		case 0x8c:
			inst_sub16 (reg_y, pc_read16 ());
			cycles += 5;
			break;
		case 0x9c:
			ea = ea_direct ();
			inst_sub16 (reg_y, read16 (ea));
			cycles += 7;
			break;
		case 0xac:
			ea = ea_indexed (&cycles);
			inst_sub16 (reg_y, read16 (ea));
			cycles += 7;
			break;
		case 0xbc:
			ea = ea_extended ();
			inst_sub16 (reg_y, read16 (ea));
			cycles += 8;
			break;
		/* ldy */
		case 0x8e:
			reg_y = pc_read16 ();
			inst_tst16 (reg_y);
			cycles += 4;
			break;
		case 0x9e:
			ea = ea_direct ();
			reg_y = read16 (ea);
			inst_tst16 (reg_y);
			cycles += 6;
			break;
		case 0xae:
			ea = ea_indexed (&cycles);
			reg_y = read16 (ea);
			inst_tst16 (reg_y);
			cycles += 6;
			break;
		case 0xbe:
			ea = ea_extended ();
			reg_y = read16 (ea);
			inst_tst16 (reg_y);
			cycles += 7;
			break;
		/* sty */
		case 0x9f:
			ea = ea_direct ();
			write16 (ea, reg_y);
			inst_tst16 (reg_y);
			cycles += 6;
			break;
		case 0xaf:
			ea = ea_indexed (&cycles);
			write16 (ea, reg_y);
			inst_tst16 (reg_y);
			cycles += 6;
			break;
		case 0xbf:
			ea = ea_extended ();
			write16 (ea, reg_y);
			inst_tst16 (reg_y);
			cycles += 7;
			break;
		/* lds */
		case 0xce:
			reg_s = pc_read16 ();
			inst_tst16 (reg_s);
			cycles += 4;
			break;
		case 0xde:
			ea = ea_direct ();
			reg_s = read16 (ea);
			inst_tst16 (reg_s);
			cycles += 6;
			break;
		case 0xee:
			ea = ea_indexed (&cycles);
			reg_s = read16 (ea);
			inst_tst16 (reg_s);
			cycles += 6;
			break;
		case 0xfe:
			ea = ea_extended ();
			reg_s = read16 (ea);
			inst_tst16 (reg_s);
			cycles += 7;
			break;
		/* sts */
		case 0xdf:
			ea = ea_direct ();
			write16 (ea, reg_s);
			inst_tst16 (reg_s);
			cycles += 6;
			break;
		case 0xef:
			ea = ea_indexed (&cycles);
			write16 (ea, reg_s);
			inst_tst16 (reg_s);
			cycles += 6;
			break;
		case 0xff:
			ea = ea_extended ();
			write16 (ea, reg_s);
			inst_tst16 (reg_s);
			cycles += 7;
			break;
		/* swi2 */
		case 0x3f:
			set_cc (FLAG_E, 1);
			inst_psh (0xff, &reg_s, reg_u, &cycles);
		    reg_pc = read16 (0xfff4);
			cycles += 8;
			break;
		default:
			printf ("unknown page-1 op code: %.2x\n", op);
			break;
		}
		return cycles;
}


	/* page 2 instructions */

unsigned int ins_0x11(void) {

	unsigned short  op;
	unsigned short  cycles = 0;
	unsigned short  ea;

		op = pc_read8 ();

		switch (op) {
		/* cmpu */
		case 0x83:
			inst_sub16 (reg_u, pc_read16 ());
			cycles += 5;
			break;
		case 0x93:
			ea = ea_direct ();
			inst_sub16 (reg_u, read16 (ea));
			cycles += 7;
			break;
		case 0xa3:
			ea = ea_indexed (&cycles);
			inst_sub16 (reg_u, read16 (ea));
			cycles += 7;
			break;
		case 0xb3:
			ea = ea_extended ();
			inst_sub16 (reg_u, read16 (ea));
			cycles += 8;
			break;
		/* cmps */
		case 0x8c:
			inst_sub16 (reg_s, pc_read16 ());
			cycles += 5;
			break;
		case 0x9c:
			ea = ea_direct ();
			inst_sub16 (reg_s, read16 (ea));
			cycles += 7;
			break;
		case 0xac:
			ea = ea_indexed (&cycles);
			inst_sub16 (reg_s, read16 (ea));
			cycles += 7;
			break;
		case 0xbc:
			ea = ea_extended ();
			inst_sub16 (reg_s, read16 (ea));
			cycles += 8;
			break;
		/* swi3 */
		case 0x3f:
			set_cc (FLAG_E, 1);
			inst_psh (0xff, &reg_s, reg_u, &cycles);
		    reg_pc = read16 (0xfff2);
			cycles += 8;
			break;
		default:
			printf ("unknown page-2 op code: %.2x\n", op);
			break;
		}
		return cycles;
}


unsigned int ins_err(void) {
		printf ("unknown page-0 op code\n");
		return 0;
	}


unsigned int (* opcodes[256])(void) = {
		ins_0x00, ins_err , ins_err , ins_0x03, ins_0x04, ins_err , ins_0x06, ins_0x07, ins_0x08, ins_0x09, ins_0x0a, ins_err , ins_0x0c, ins_0x0d, ins_0x0e, ins_0x0f,
		ins_0x10, ins_0x11, ins_0x12, ins_0x13, ins_err , ins_err , ins_0x16, ins_0x17, ins_err , ins_0x19, ins_0x1a, ins_err , ins_0x1c, ins_0x1d, ins_0x1e, ins_0x1f,
		ins_0x20, ins_0x21, ins_0x22, ins_0x23, ins_0x24, ins_0x25, ins_0x26, ins_0x27, ins_0x28, ins_0x29, ins_0x2a, ins_0x2b, ins_0x2c, ins_0x2d, ins_0x2e, ins_0x2f,
		ins_0x30, ins_0x31, ins_0x32, ins_0x33, ins_0x34, ins_0x35, ins_0x36, ins_0x37, ins_err , ins_0x39, ins_0x3a, ins_0x3b, ins_0x3c, ins_0x3d, ins_err , ins_0x3f,
		ins_0x40, ins_err , ins_err , ins_0x43, ins_0x44, ins_err , ins_0x46, ins_0x47, ins_0x48, ins_0x49, ins_0x4a, ins_err , ins_0x4c, ins_0x4d, ins_err , ins_0x4f,
		ins_0x50, ins_err , ins_err , ins_0x53, ins_0x54, ins_err , ins_0x56, ins_0x57, ins_0x58, ins_0x59, ins_0x5a, ins_err , ins_0x5c, ins_0x5d, ins_err , ins_0x5f,
		ins_0x60, ins_err , ins_err , ins_0x63, ins_0x64, ins_err , ins_0x66, ins_0x67, ins_0x68, ins_0x69, ins_0x6a, ins_err , ins_0x6c, ins_0x6d, ins_0x6e, ins_0x6f,
		ins_0x70, ins_err , ins_err , ins_0x73, ins_0x74, ins_err , ins_0x76, ins_0x77, ins_0x78, ins_0x79, ins_0x7a, ins_err , ins_0x7c, ins_0x7d, ins_0x7e, ins_0x7f,
		ins_0x80, ins_0x81, ins_0x82, ins_0x83, ins_0x84, ins_0x85, ins_0x86, ins_err , ins_0x88, ins_0x89, ins_0x8a, ins_0x8b, ins_0x8c, ins_0x8d, ins_0x8e, ins_err ,
		ins_0x90, ins_0x91, ins_0x92, ins_0x93, ins_0x94, ins_0x95, ins_0x96, ins_0x97, ins_0x98, ins_0x99, ins_0x9a, ins_0x9b, ins_0x9c, ins_0x9d, ins_0x9e, ins_0x9f,
		ins_0xa0, ins_0xa1, ins_0xa2, ins_0xa3, ins_0xa4, ins_0xa5, ins_0xa6, ins_0xa7, ins_0xa8, ins_0xa9, ins_0xaa, ins_0xab, ins_0xac, ins_0xad, ins_0xae, ins_0xaf,
		ins_0xb0, ins_0xb1, ins_0xb2, ins_0xb3, ins_0xb4, ins_0xb5, ins_0xb6, ins_0xb7, ins_0xb8, ins_0xb9, ins_0xba, ins_0xbb, ins_0xbc, ins_0xbd, ins_0xbe, ins_0xbf,
		ins_0xc0, ins_0xc1, ins_0xc2, ins_0xc3, ins_0xc4, ins_0xc5, ins_0xc6, ins_err , ins_0xc8, ins_0xc9, ins_0xca, ins_0xcb, ins_0xcc, ins_err , ins_0xce, ins_err ,
		ins_0xd0, ins_0xd1, ins_0xd2, ins_0xd3, ins_0xd4, ins_0xd5, ins_0xd6, ins_0xd7, ins_0xd8, ins_0xd9, ins_0xda, ins_0xdb, ins_0xdc, ins_0xdd, ins_0xde, ins_0xdf,
		ins_0xe0, ins_0xe1, ins_0xe2, ins_0xe3, ins_0xe4, ins_0xe5, ins_0xe6, ins_0xe7, ins_0xe8, ins_0xe9, ins_0xea, ins_0xeb, ins_0xec, ins_0xed, ins_0xee, ins_0xef,
		ins_0xf0, ins_0xf1, ins_0xf2, ins_0xf3, ins_0xf4, ins_0xf5, ins_0xf6, ins_0xf7, ins_0xf8, ins_0xf9, ins_0xfa, ins_0xfb, ins_0xfc, ins_0xfd, ins_0xfe, ins_0xff};

/* execute a single instruction or handle interrupts and return */

unsigned short  e6809_sstep (unsigned short  irq_i, unsigned short  irq_f)
{
	unsigned short  op;
	unsigned short  cycles = 0;

	if (irq_f) {
		if (get_cc (FLAG_F) == 0) {
			if (irq_status != IRQ_CWAI) {
				set_cc (FLAG_E, 0);
				inst_psh (0x81, &reg_s, reg_u, &cycles);
			}

			set_cc (FLAG_I, 1);
			set_cc (FLAG_F, 1);

			reg_pc = read16 (0xfff6);
			irq_status = IRQ_NORMAL;
			cycles += 7;
		} else {
			if (irq_status == IRQ_SYNC) {
				irq_status = IRQ_NORMAL;
			}
		}
	}

	if (irq_i) {
		if (get_cc (FLAG_I) == 0) {
			if (irq_status != IRQ_CWAI) {
				set_cc (FLAG_E, 1);
				inst_psh (0xff, &reg_s, reg_u, &cycles);
			}

			set_cc (FLAG_I, 1);

			reg_pc = read16 (0xfff8);
			irq_status = IRQ_NORMAL;
			cycles += 7;
		} else {
			if (irq_status == IRQ_SYNC) {
				irq_status = IRQ_NORMAL;
			}
		}
	}

	if (irq_status != IRQ_NORMAL) {
		return cycles + 1;
	}

	op = pc_read8 ();
	
	cycles += opcodes[op]();

	return cycles;
}


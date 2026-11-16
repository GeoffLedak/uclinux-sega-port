/*
 * genesis_z80.c -- the Z80 for user programs (vgmplay plays music on it)
 *
 * See include/linux/genesis_console.h for the interface (the console
 * ioctls GENESIS_Z80_*).
 *
 * The 68000 reaches the Z80's RAM (and the YM2612, which is on the Z80's
 * bus) only while it holds the Z80's bus, which stops the Z80. At boot
 * the 68000 takes the bus and keeps it; a program started here gets it
 * back, and is stopped only for moments: each 256 bytes copied by
 * GENESIS_Z80_READ / WRITE (with interrupts off, so that the Z80 can't
 * be left stopped while some other process runs), and each piece of
 * the screen that graphics mode copies by DMA (genesis_z80_hold()).
 *
 * The Z80's RAM can't be reached while the Z80 is held in reset (the
 * bus is only granted once it is out of reset; BlastEm allows it, real
 * hardware doesn't), so a program is copied with the Z80 out of reset
 * but stopped, and the Z80 is reset after that.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/genesis_console.h>

#include <asm/system.h>
#include <asm/segment.h>

#define Z80_RAM		((volatile unsigned char *)0xA00000)
#define Z80_BUSREQ	(*(volatile unsigned short *)0xA11100)
#define Z80_RESET	(*(volatile unsigned short *)0xA11200)
#define YM_ADDR0	(*(volatile unsigned char *)0xA04000)	/* also status */
#define YM_DATA0	(*(volatile unsigned char *)0xA04001)
#define YM_ADDR1	(*(volatile unsigned char *)0xA04002)
#define YM_DATA1	(*(volatile unsigned char *)0xA04003)
#define PSG		(*(volatile unsigned char *)0xC00011)

#define PIECE		256		/* bytes copied per stop of the Z80 */

volatile int genesis_z80_running;
static struct task_struct *owner;

static void bus_take(void)
{
	Z80_BUSREQ = 0x100;
	while (Z80_BUSREQ & 0x100)		/* until it is granted */
		;
}

static void bus_give(void)
{
	Z80_BUSREQ = 0;
}

/* graphics mode, around a DMA (interrupts off) */
void genesis_z80_hold(void)
{
	if (genesis_z80_running)
		bus_take();
}

void genesis_z80_release(void)
{
	if (genesis_z80_running)
		bus_give();
}

/* hold the Z80 in reset for a moment: that resets the YM2612 too (it
 * needs 192 68000 cycles of it); it then starts again at 0 */
static void reset_pulse(void)
{
	volatile int i;

	Z80_RESET = 0;
	for (i = 0; i < 50; i++)
		;
	Z80_RESET = 0x100;
}

static void ym_write(int port, int reg, int val)
{
	while (YM_ADDR0 & 0x80)			/* busy */
		;
	if (port) {
		YM_ADDR1 = reg;
		YM_DATA1 = val;
	} else {
		YM_ADDR0 = reg;
		YM_DATA0 = val;
	}
}

/* stop the Z80 and silence the sound chips; the 68000 keeps the bus */
static void z80_stop(void)
{
	unsigned long flags;
	int port, r;

	save_flags(flags);
	cli();
	genesis_z80_running = 0;
	owner = 0;
	bus_take();
	restore_flags(flags);
	reset_pulse();
	bus_take();			/* (granted again once out of reset) */

	/* (in case the reset doesn't reach the YM2612 on every model) */
	ym_write(0, 0x27, 0x00);		/* timers off, ch3 normal */
	ym_write(0, 0x2B, 0x00);		/* DAC off */
	for (r = 0; r < 7; r++)
		if (r != 3)
			ym_write(0, 0x28, r);	/* key off */
	for (port = 0; port < 2; port++)
		for (r = 0; r < 16; r++) {
			ym_write(port, 0x40 + r, 0x7F);	/* total level: silent */
			ym_write(port, 0x80 + r, 0xFF);	/* fastest release */
		}
	PSG = 0x9F;
	PSG = 0xBF;
	PSG = 0xDF;
	PSG = 0xFF;
}

static int get_io(unsigned long arg, struct genesis_z80_io *io, int write)
{
	int err = verify_area(VERIFY_READ, (void *)arg, sizeof *io);

	if (err)
		return err;
	memcpy_fromfs(io, (void *)arg, sizeof *io);
	if ((unsigned long)io->addr + io->len > GENESIS_Z80_RAM)
		return -EINVAL;
	return verify_area(write ? VERIFY_READ : VERIFY_WRITE, io->data, io->len);
}

static int z80_start(unsigned long arg)
{
	struct genesis_z80_io io;
	unsigned long flags;
	int err, i;

	if (owner && owner != current)
		return -EBUSY;
	err = get_io(arg, &io, 1);
	if (err)
		return err;
	if (io.addr)
		return -EINVAL;

	save_flags(flags);
	cli();
	genesis_z80_running = 0;
	bus_take();			/* (it is never left in reset) */
	restore_flags(flags);
	for (i = 0; i < io.len; i++)	/* (no MMU: the program's memory is ours) */
		Z80_RAM[i] = io.data[i];
	reset_pulse();			/* (it waits for the bus) */

	save_flags(flags);
	cli();
	owner = current;
	genesis_z80_running = 1;
	bus_give();			/* it starts at 0 */
	restore_flags(flags);
	return 0;
}

static int z80_copy(unsigned long arg, int write)
{
	struct genesis_z80_io io;
	unsigned long flags;
	unsigned char *p;
	int err, a, n, k, i;

	if (owner != current || !genesis_z80_running)
		return -EPERM;
	err = get_io(arg, &io, write);
	if (err)
		return err;
	p = io.data;
	a = io.addr;
	for (n = io.len; n > 0; n -= k) {
		k = n > PIECE ? PIECE : n;
		save_flags(flags);
		cli();
		bus_take();
		if (write)
			for (i = 0; i < k; i++)
				Z80_RAM[a + i] = p[i];
		else
			for (i = 0; i < k; i++)
				p[i] = Z80_RAM[a + i];
		bus_give();
		restore_flags(flags);
		a += k;
		p += k;
	}
	return 0;
}

int genesis_z80_ioctl(unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case GENESIS_Z80_START:
		return z80_start(arg);
	case GENESIS_Z80_STOP:
		if (owner != current)
			return -EPERM;
		z80_stop();
		return 0;
	case GENESIS_Z80_READ:
		return z80_copy(arg, 0);
	case GENESIS_Z80_WRITE:
		return z80_copy(arg, 1);
	}
	return -ENOIOCTLCMD;
}

void genesis_z80_exit(struct task_struct *p)
{
	if (owner == p)
		z80_stop();
}

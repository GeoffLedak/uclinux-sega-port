/*
 * genesis_vdp.c -- Simple TTY driver for Sega Genesis VDP console output
 *
 * Based on gbatxt.c and ft245.c
 * For uClinux on Sega Genesis
 */

#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/config.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/kernel.h>

#include <asm/system.h>
#include <asm/segment.h>

/*
 * Serial type definitions
 */
#define SERIAL_TYPE_NORMAL	1
#define SERIAL_TYPE_CALLOUT	2

/*
 * External function from hw_md.S for outputting characters
 */
extern void console_putchar(char c);

/*
 * Driver info
 */
#define GENESIS_VDP_DRIVER_NAME "Genesis VDP console driver v1.0"
#define NR_PORTS 1

/*
 * Serial driver structures
 */
static struct tty_driver genesis_serial_driver;
static struct tty_driver genesis_callout_driver;
static int genesis_refcount;
static struct tty_struct *genesis_tty_table[NR_PORTS];
static struct termios *genesis_termios[NR_PORTS];
static struct termios *genesis_termios_locked[NR_PORTS];

/*
 * Minimal serial info structure
 */
struct genesis_serial {
	int magic;
	int line;
	int count;
	struct tty_struct *tty;
};

#define SERIAL_MAGIC 0x5301
static struct genesis_serial genesis_ports[NR_PORTS];

/*
 * Write characters to VDP console
 */
static int genesis_write(struct tty_struct *tty, int from_user,
                         const unsigned char *buf, int count)
{
	int i;
	
	for (i = 0; i < count; i++) {
		unsigned char c = from_user ? get_user(buf + i) : buf[i];
		console_putchar(c);
	}
	
	return count;
}

/*
 * Return number of characters we can accept
 */
static int genesis_write_room(struct tty_struct *tty)
{
	return 256; /* We can always accept characters */
}

/*
 * Return number of characters in buffer (always 0 since we write immediately)
 */
static int genesis_chars_in_buffer(struct tty_struct *tty)
{
	return 0;
}

/*
 * Flush output characters - nothing to do since we write immediately
 */
static void genesis_flush_chars(struct tty_struct *tty)
{
}

/*
 * Flush buffer - nothing to do
 */
static void genesis_flush_buffer(struct tty_struct *tty)
{
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

/*
 * Open the port
 */
static int genesis_open(struct tty_struct *tty, struct file *filp)
{
	struct genesis_serial *info;
	int line;
	
	line = MINOR(tty->device) - tty->driver.minor_start;
	if (line < 0 || line >= NR_PORTS)
		return -ENODEV;
	
	info = &genesis_ports[line];
	info->count++;
	info->tty = tty;
	tty->driver_data = info;
	
	return 0;
}

/*
 * Close the port
 */
static void genesis_close(struct tty_struct *tty, struct file *filp)
{
	struct genesis_serial *info = (struct genesis_serial *)tty->driver_data;
	
	if (!info)
		return;
	
	if (--info->count <= 0) {
		info->count = 0;
		info->tty = NULL;
	}
}

/*
 * Stub functions - minimal implementations
 */
static void genesis_throttle(struct tty_struct *tty)
{
}

static void genesis_unthrottle(struct tty_struct *tty)
{
}

static void genesis_stop(struct tty_struct *tty)
{
}

static void genesis_start(struct tty_struct *tty)
{
}

static void genesis_hangup(struct tty_struct *tty)
{
}

static void genesis_set_termios(struct tty_struct *tty, struct termios *old)
{
}

static int genesis_ioctl(struct tty_struct *tty, struct file *file,
                         unsigned int cmd, unsigned long arg)
{
	return -ENOIOCTLCMD;
}

/*
 * Initialize the driver
 */
int genesis_vdp_init(void)
{
	int i;
	
	printk(GENESIS_VDP_DRIVER_NAME "\n");
	
	/* Initialize the tty_driver structure */
	memset(&genesis_serial_driver, 0, sizeof(struct tty_driver));
	genesis_serial_driver.magic = TTY_DRIVER_MAGIC;
	genesis_serial_driver.name = "ttyS";
	genesis_serial_driver.major = TTY_MAJOR;
	genesis_serial_driver.minor_start = 64;
	genesis_serial_driver.num = NR_PORTS;
	genesis_serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
	genesis_serial_driver.subtype = SERIAL_TYPE_NORMAL;
	genesis_serial_driver.init_termios = tty_std_termios;
	genesis_serial_driver.init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	genesis_serial_driver.flags = TTY_DRIVER_REAL_RAW;
	genesis_serial_driver.refcount = &genesis_refcount;
	genesis_serial_driver.table = genesis_tty_table;
	genesis_serial_driver.termios = genesis_termios;
	genesis_serial_driver.termios_locked = genesis_termios_locked;
	
	genesis_serial_driver.open = genesis_open;
	genesis_serial_driver.close = genesis_close;
	genesis_serial_driver.write = genesis_write;
	genesis_serial_driver.flush_chars = genesis_flush_chars;
	genesis_serial_driver.write_room = genesis_write_room;
	genesis_serial_driver.chars_in_buffer = genesis_chars_in_buffer;
	genesis_serial_driver.flush_buffer = genesis_flush_buffer;
	genesis_serial_driver.ioctl = genesis_ioctl;
	genesis_serial_driver.throttle = genesis_throttle;
	genesis_serial_driver.unthrottle = genesis_unthrottle;
	genesis_serial_driver.set_termios = genesis_set_termios;
	genesis_serial_driver.stop = genesis_stop;
	genesis_serial_driver.start = genesis_start;
	genesis_serial_driver.hangup = genesis_hangup;
	
	/* Callout driver */
	genesis_callout_driver = genesis_serial_driver;
	genesis_callout_driver.name = "cua";
	genesis_callout_driver.major = TTYAUX_MAJOR;
	genesis_callout_driver.subtype = SERIAL_TYPE_CALLOUT;
	
	if (tty_register_driver(&genesis_serial_driver))
		panic("Couldn't register Genesis VDP serial driver\n");
	if (tty_register_driver(&genesis_callout_driver))
		panic("Couldn't register Genesis VDP callout driver\n");
	
	/* Initialize port structures */
	for (i = 0; i < NR_PORTS; i++) {
		genesis_ports[i].magic = SERIAL_MAGIC;
		genesis_ports[i].line = i;
		genesis_ports[i].count = 0;
		genesis_ports[i].tty = NULL;
	}
	
	printk("ttyS0 at VDP (Genesis VDP console)\n");
	
	return 0;
}


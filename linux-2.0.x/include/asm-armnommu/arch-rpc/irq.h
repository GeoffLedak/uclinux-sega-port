/*
 * include/asm-arm/arch-rpc/irq.h
 *
 * Copyright (C) 1996 Russell King
 *
 * Changelog:
 *   10-10-1996	RMK	Brought up to date with arch-sa110eval
 */

#define BUILD_IRQ(s,n,m) \
	void IRQ##n##_interrupt(void); \
	void fast_IRQ##n##_interrupt(void); \
	void bad_IRQ##n##_interrupt(void); \
	void probe_IRQ##n##_interrupt(void);

/*
 * The timer is a special interrupt
 */
#define IRQ5_interrupt		timer_IRQ_interrupt

#define IRQ_INTERRUPT(n)	IRQ##n##_interrupt
#define FAST_INTERRUPT(n)	fast_IRQ##n##_interrupt
#define BAD_INTERRUPT(n)	bad_IRQ##n##_interrupt
#define PROBE_INTERRUPT(n)	probe_IRQ##n##_interrupt
                                
static __inline__ void mask_irq(unsigned int irq)
{
	extern void ecard_disableirq (unsigned int);
	extern void ecard_disablefiq (unsigned int);
	unsigned char mask = 1 << (irq & 7);

	switch (irq >> 3) {
	case 0:
		outb(inb(IOMD_IRQMASKA) & ~mask, IOMD_IRQMASKA);
		break;
	case 1:
		outb(inb(IOMD_IRQMASKB) & ~mask, IOMD_IRQMASKB);
		break;
	case 2:
		outb(inb(IOMD_DMAMASK) & ~mask, IOMD_DMAMASK);
		break;
	case 4:
		ecard_disableirq (irq & 7);
		break;
	case 8:
		outb(inb(IOMD_FIQMASK) & ~mask, IOMD_FIQMASK);
		break;
	case 12:
		ecard_disablefiq (irq & 7);
	}
}

static __inline__ void unmask_irq(unsigned int irq)
{
	extern void ecard_enableirq (unsigned int);
	extern void ecard_enablefiq (unsigned int);
	unsigned char mask = 1 << (irq & 7);

	switch (irq >> 3) {
	case 0:
		outb(inb(IOMD_IRQMASKA) | mask, IOMD_IRQMASKA);
		break;
	case 1:
		outb(inb(IOMD_IRQMASKB) | mask, IOMD_IRQMASKB);
		break;
	case 2:
		outb(inb(IOMD_DMAMASK) | mask, IOMD_DMAMASK);
		break;
	case 4:
		ecard_enableirq (irq & 7);
		break;
	case 8:
		outb(inb(IOMD_FIQMASK) | mask, IOMD_FIQMASK);
		break;
	case 12:
		ecard_enablefiq (irq & 7);
	}
}

static __inline__ unsigned long get_enabled_irqs(void)
{
	return inb(IOMD_IRQMASKA) | inb(IOMD_IRQMASKB) << 8 | inb(IOMD_DMAMASK) << 16;
}

static __inline__ void irq_init_irq(void)
{
	outb(0, IOMD_IRQMASKA);
	outb(0, IOMD_IRQMASKB);
	outb(0, IOMD_FIQMASK);
	outb(0, IOMD_DMAMASK);
	outb(0, IOMD_IO0CR);
	outb(0, IOMD_IO1CR);
	outb(0, IOMD_IO2CR);
	outb(0, IOMD_IO3CR);
}

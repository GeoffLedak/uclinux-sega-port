/*
 *	linux/arch/alpha/kernel/irq.c
 *
 *	Copyright (C) 1995 Linus Torvalds
 *
 * This file contains the code used by various IRQ handling routines:
 * asking for different IRQ's should be done through these routines
 * instead of just grabbing them. Thus setups with different IRQ numbers
 * shouldn't result in any weird surprises, and installing new handlers
 * should be easier.
 */

#include <linux/config.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/random.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bitops.h>
#include <asm/dma.h>

extern void timer_interrupt(struct pt_regs * regs);
extern void cserve_update_hw(unsigned long, unsigned long);

#if NR_IRQS > 64
#  error Unable to handle more than 64 irq levels.
#endif

/* Reserved interrupts.  These must NEVER be requested by any driver!
 */
#define	IS_RESERVED_IRQ(irq)	((irq)==2)	/* IRQ 2 used by hw cascade */

/*
 * Shadow-copy of masked interrupts.
 *  The bits are used as follows:
 *	 0.. 7	first (E)ISA PIC (irq level 0..7)
 *	 8..15	second (E)ISA PIC (irq level 8..15)
 *   Systems with PCI interrupt lines managed by GRU (e.g., Alcor, XLT):
 *    or PYXIS (e.g. Miata, PC164-LX)
 *	16..47	PCI interrupts 0..31 (xxx_INT_MASK reg)
 *   Mikasa:
 *	16..31	PCI interrupts 0..15 (short at I/O port 536)
 *   Other systems (not Mikasa) with 16 PCI interrupt lines:
 *	16..23	PCI interrupts 0.. 7 (char at I/O port 26)
 *	24..31	PCI interrupts 8..15 (char at I/O port 27)
 *   Systems with 17 PCI interrupt lines (e.g., Cabriolet and eb164):
 *	16..32	PCI interrupts 0..31 (int at I/O port 804)
 *   Takara:
 *      16..19  PCI interrupts A thru D
 *   For SABLE, which is really baroque, we manage 40 IRQ's, but the
 *   hardware really only supports 24, not via normal ISA PIC,
 *   but cascaded custom 8259's, etc.
 *	 0-7  (char at 536)
 *	 8-15 (char at 53a)
 *	16-23 (char at 53c)
 */
static unsigned long irq_mask = ~0UL;

#ifdef CONFIG_ALPHA_SABLE
/* note that the vector reported by the SRM PALcode corresponds to the
   interrupt mask bits, but we have to manage via more normal IRQs */
static char sable_irq_to_mask[NR_IRQS] = {
	-1,  6, -1,  8, 15, 12,  7,  9, /* pseudo PIC  0-7  */
	-1, 16, 17, 18,  3, -1, 21, 22, /* pseudo PIC  8-15 */
	-1, -1, -1, -1, -1, -1, -1, -1, /* pseudo EISA 0-7  */
	-1, -1, -1, -1, -1, -1, -1, -1, /* pseudo EISA 8-15 */
	 2,  1,  0,  4,  5, -1, -1, -1, /* pseudo PCI */
	};
#define IRQ_TO_MASK(irq) (sable_irq_to_mask[(irq)])
static char sable_mask_to_irq[NR_IRQS] = {
	34, 33, 32, 12, 35, 36,  1,  6, /* mask 0-7  */
	 3,  7, -1, -1,  5, -1, -1,  4, /* mask 8-15  */
	 9, 10, 11, -1, -1, 14, 15, -1, /* mask 16-23  */
	};
#else /* CONFIG_ALPHA_SABLE */
#define IRQ_TO_MASK(irq) (irq)
#endif /* CONFIG_ALPHA_SABLE */

/*
 * Update the hardware with the irq mask passed in MASK.  The function
 * exploits the fact that it is known that only bit IRQ has changed.
 *
 * There deliberately isn't a case in here for the Takara since it
 * wouldn't work anyway because the interrupt controller is bizarre.
 */
static void update_hw(unsigned long irq, unsigned long mask)
{
#ifdef CONFIG_ALPHA_ALCOR
	/* always mask out 20..30 (which are unused) */
	mask |= 0x7ff00000UL << 16;
#endif
	switch (irq) {

#ifdef CONFIG_ALPHA_SABLE
  /* SABLE does everything different, so we manage it that way... :-( */
  /* the "irq" argument is really the mask bit number */
	case 0 ... 7:
	  outb(mask, 0x537);
	  break;
	case 8 ... 15:
	  outb(mask >> 8, 0x53b);
	  break;
	case 16 ... 23:
	  outb(mask >> 16, 0x53d);
	  break;
#else /* SABLE */

#if defined(CONFIG_ALPHA_NORITAKE)
	  /* note inverted sense of mask bits: */
	case 16 ... 31:
	  outw(~(mask >> 16), 0x54a);
	  break;
	case 32 ... 47:
	  outw(~(mask >> 32), 0x54c);
	  break;
#endif /* NORITAKE */

#if defined(CONFIG_ALPHA_MIATA)
	case 16 ... 47:
	  { unsigned long temp;
	  /* note inverted sense of mask bits: */
	  /* make CERTAIN none of the bogus ints get enabled */
	  *(unsigned long *)PYXIS_INT_MASK =
	    ~((long)mask >> 16) & ~0x400000000000063bUL; mb();
	  temp = *(unsigned long *)PYXIS_INT_MASK;
	  break;
	  }
#endif /* MIATA */

#if defined(CONFIG_ALPHA_RUFFIAN)
	case 16 ... 47:
	  { unsigned long temp;
	  /* note inverted sense of mask bits: */
	  /* make CERTAIN none of the bogus ints get enabled */
	  *(unsigned long *)PYXIS_INT_MASK =
	    ~((long)mask >> 16) & 0x00000000ffffffbfUL; mb();
	  temp = *(unsigned long *)PYXIS_INT_MASK;
	  break;
	  }
#endif /* RUFFIAN */

#if defined(CONFIG_ALPHA_SX164)
	case 16 ... 39:
#if defined(CONFIG_ALPHA_SRM)
	  cserve_update_hw(irq, mask);
	  break;
#else
	  { unsigned long temp;
	  /* make CERTAIN none of the bogus ints get enabled */
	  *(unsigned long *)PYXIS_INT_MASK =
	      ~((long)mask >> 16) & ~0x000000000000003bUL; mb();
	  temp = *(unsigned long *)PYXIS_INT_MASK;
	  break;
	  }
#endif /* SRM */
#endif /* SX164 */

#if defined(CONFIG_ALPHA_ALCOR) || defined(CONFIG_ALPHA_XLT)
	case 16 ... 47:
		/* note inverted sense of mask bits: */
		*(unsigned int *)GRU_INT_MASK = ~(mask >> 16); mb();
		break;
#endif /* ALCOR || XLT */

#if defined(CONFIG_ALPHA_CABRIOLET) || \
    defined(CONFIG_ALPHA_EB66P)     || \
    defined(CONFIG_ALPHA_EB164)     || \
    defined(CONFIG_ALPHA_PC164)     || \
    defined(CONFIG_ALPHA_LX164)
#if defined(CONFIG_ALPHA_SRM)
      case 16 ... 34:
	cserve_update_hw(irq, mask);
        break;
#else /* SRM */
      case 16 ... 34:
	outl(irq_mask >> 16, 0x804);
		break;
#endif /* SRM */
#endif /* CABRIO || EB66P || EB164 || PC164 || LX164 */

#if defined(CONFIG_ALPHA_MIKASA)
	case 16 ... 31:
		outw(~(mask >> 16), 0x536); /* note invert */
		break;
#endif /* MIKASA */

#if defined(CONFIG_ALPHA_EB66) || defined(CONFIG_ALPHA_EB64P)
	      case 16 ... 23:
		outb(mask >> 16, 0x26);
		break;
	case 24 ... 31:
		outb(mask >> 24, 0x27);
		break;
#endif /* EB66 || EB64P */

		/* handle ISA irqs last---fast devices belong on PCI... */
	/* this is common for all except SABLE! */

	      case  0 ... 7:	/* ISA PIC1 */
		outb(mask, 0x21);
		break;

	      case  8 ...15:	/* ISA PIC2 */
		outb(mask >> 8, 0xA1);
		break;

#endif /* SABLE */

	} /* end switch (irq) */
}

static inline void mask_irq(unsigned long irq)
{
	irq_mask |= (1UL << irq);
	update_hw(irq, irq_mask);
}

static inline void unmask_irq(unsigned long irq)
{
	irq_mask &= ~(1UL << irq);
	update_hw(irq, irq_mask);
}

void disable_irq(unsigned int irq_nr)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	mask_irq(IRQ_TO_MASK(irq_nr));
	restore_flags(flags);
}

void enable_irq(unsigned int irq_nr)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	unmask_irq(IRQ_TO_MASK(irq_nr));
	restore_flags(flags);
}

/*
 * Initial irq handlers.
 */
static struct irqaction *irq_action[NR_IRQS];

int get_irq_list(char *buf)
{
	int i, len = 0;
	struct irqaction * action;

	for (i = 0 ; i < NR_IRQS ; i++) {
		action = irq_action[i];
		if (!action) 
			continue;
		len += sprintf(buf+len, "%2d: %8d %c %s",
			i, kstat.interrupts[i],
			(action->flags & SA_INTERRUPT) ? '+' : ' ',
			action->name);
		for (action=action->next; action; action = action->next) {
			len += sprintf(buf+len, ",%s %s",
				(action->flags & SA_INTERRUPT) ? " +" : "",
				action->name);
		}
		len += sprintf(buf+len, "\n");
	}
	return len;
}

static inline void ack_irq(int irq)
{
#ifdef CONFIG_ALPHA_SABLE
  /* note that the "irq" here is really the mask bit number */
	switch (irq) {
	case 0 ... 7:
	  outb(0xE0 | (irq - 0), 0x536);
	  outb(0xE0 | 1, 0x534); /* slave 0 */
	  break;
	case 8 ... 15:
	  outb(0xE0 | (irq - 8), 0x53a);
	  outb(0xE0 | 3, 0x534); /* slave 1 */
	  break;
	case 16 ... 24:
	  outb(0xE0 | (irq - 16), 0x53c);
	  outb(0xE0 | 4, 0x534); /* slave 2 */
	  break;
	}
#else /* CONFIG_ALPHA_SABLE */
	if (irq < 16) {
#if defined(CONFIG_ALPHA_RUFFIAN)
		/* ack pyxis ISA interrupt */
		*(unsigned long *)PYXIS_INT_REQ = (0x01UL << 7);
		if (irq > 7) {
			outb(0x20,0xa0);
		}
		outb(0x20,0x20);
#else /* RUFFIAN */
		/* ACK the interrupt making it the lowest priority */
		/*  First the slave .. */
		if (irq > 7) {
			outb(0xE0 | (irq - 8), 0xa0);
			irq = 2;
		}
		/* .. then the master */
		outb(0xE0 | irq, 0x20);
#if defined(CONFIG_ALPHA_ALCOR) || defined(CONFIG_ALPHA_XLT)
		/* on ALCOR/XLT, need to dismiss interrupt via GRU */
		*(int *)GRU_INT_CLEAR = 0x80000000; mb();
		*(int *)GRU_INT_CLEAR = 0x00000000; mb();
#endif /* ALCOR || XLT */
#endif /* RUFFIAN */  
	}
#if defined(CONFIG_ALPHA_RUFFIAN)
        else {
	  /* ack pyxis int */
	  *(unsigned long *)PYXIS_INT_REQ = (1UL << (irq-16));
        }
#endif /* RUFFIAN */        

#endif /* CONFIG_ALPHA_SABLE */
}

int request_irq(unsigned int irq, 
		void (*handler)(int, void *, struct pt_regs *),
		unsigned long irqflags, 
		const char * devname,
		void *dev_id)
{
	int shared = 0;
	struct irqaction * action, **p;
	unsigned long flags;

	if (irq >= NR_IRQS)
		return -EINVAL;
	if (IS_RESERVED_IRQ(irq))
		return -EINVAL;
	if (!handler)
		return -EINVAL;
	p = irq_action + irq;
	action = *p;
	if (action) {
		/* Can't share interrupts unless both agree to */
		if (!(action->flags & irqflags & SA_SHIRQ))
			return -EBUSY;

		/* Can't share interrupts unless both are same type */
		if ((action->flags ^ irqflags) & SA_INTERRUPT)
			return -EBUSY;

		/* add new interrupt at end of irq queue */
		do {
			p = &action->next;
			action = *p;
		} while (action);
		shared = 1;
	}

	action = (struct irqaction *)kmalloc(sizeof(struct irqaction),
					     GFP_KERNEL);
	if (!action)
		return -ENOMEM;

	if (irqflags & SA_SAMPLE_RANDOM)
		rand_initialize_irq(irq);

	action->handler = handler;
	action->flags = irqflags;
	action->mask = 0;
	action->name = devname;
	action->next = NULL;
	action->dev_id = dev_id;

	save_flags(flags);
	cli();
	*p = action;

	if (!shared)
		unmask_irq(IRQ_TO_MASK(irq));

	restore_flags(flags);
	return 0;
}
		
void free_irq(unsigned int irq, void *dev_id)
{
	struct irqaction * action, **p;
	unsigned long flags;

	if (irq >= NR_IRQS) {
		printk("Trying to free IRQ%d\n",irq);
		return;
	}
	if (IS_RESERVED_IRQ(irq)) {
		printk("Trying to free reserved IRQ %d\n", irq);
		return;
	}
	for (p = irq + irq_action; (action = *p) != NULL; p = &action->next) {
		if (action->dev_id != dev_id)
			continue;

		/* Found it - now free it */
		save_flags(flags);
		cli();
		*p = action->next;
		if (!irq[irq_action])
			mask_irq(IRQ_TO_MASK(irq));
		restore_flags(flags);
		kfree(action);
		return;
	}
	printk("Trying to free free IRQ%d\n",irq);
}

static inline void handle_nmi(struct pt_regs * regs)
{
	printk("Whee.. NMI received. Probable hardware error\n");
	printk("61=%02x, 461=%02x\n", inb(0x61), inb(0x461));
}

static void unexpected_irq(int irq, struct pt_regs * regs)
{
	struct irqaction *action;
	int i;

	printk("IO device interrupt, irq = %d\n", irq);
	printk("PC = %016lx PS=%04lx\n", regs->pc, regs->ps);
	printk("Expecting: ");
	for (i = 0; i < 16; i++)
		if ((action = irq_action[i]))
			while (action->handler) {
				printk("[%s:%d] ", action->name, i);
				action = action->next;
			}
	printk("\n");
#if defined(CONFIG_ALPHA_JENSEN)
	printk("64=%02x, 60=%02x, 3fa=%02x 2fa=%02x\n",
		inb(0x64), inb(0x60), inb(0x3fa), inb(0x2fa));
	outb(0x0c, 0x3fc);
	outb(0x0c, 0x2fc);
	outb(0,0x61);
	outb(0,0x461);
#endif
}

static inline void handle_irq(int irq, struct pt_regs * regs)
{
	struct irqaction * action = irq_action[irq];

	kstat.interrupts[irq]++;
	if (!action) {
		unexpected_irq(irq, regs);
		return;
	}
	do {
		action->handler(irq, action->dev_id, regs);
		action = action->next;
	} while (action);
}

static inline void device_interrupt(int irq, int ack, struct pt_regs * regs)
{
	struct irqaction * action;

	if ((unsigned) irq > NR_IRQS) {
		printk("device_interrupt: illegal interrupt %d\n", irq);
		return;
	}

#if defined(CONFIG_ALPHA_RUFFIAN)
	/* RUFFIAN uses ISA IRQ #0 to deliver clock ticks */
	if (irq == 0) {
		timer_interrupt(regs);
		ack_irq(0);
		return;
	}
#endif /* RUFFIAN */

	kstat.interrupts[irq]++;
	action = irq_action[irq];

	/*
	 * For normal interrupts, we mask it out, and then ACK it.
	 * This way another (more timing-critical) interrupt can
	 * come through while we're doing this one.
	 *
	 * Note! An irq without a handler gets masked and acked, but
	 * never unmasked. The autoirq stuff depends on this (it looks
	 * at the masks before and after doing the probing).
	 */
	mask_irq(ack);
	ack_irq(ack);
	if (!action) {
#if 1
		printk("device_interrupt: unexpected interrupt %d\n", irq);
#endif
		return;
	}
	if (action->flags & SA_SAMPLE_RANDOM)
		add_interrupt_randomness(irq);
	do {
		action->handler(irq, action->dev_id, regs);
		action = action->next;
	} while (action);
	unmask_irq(ack);
}

#ifdef CONFIG_PCI

/*
 * Handle ISA interrupt via the PICs.
 */
static inline void isa_device_interrupt(unsigned long vector,
					struct pt_regs * regs)
{
#if defined(CONFIG_ALPHA_APECS)
#	define IACK_SC	APECS_IACK_SC
#elif defined(CONFIG_ALPHA_LCA)
#	define IACK_SC	LCA_IACK_SC
#elif defined(CONFIG_ALPHA_CIA)
#	define IACK_SC	CIA_IACK_SC
#elif defined(CONFIG_ALPHA_PYXIS)
#	define IACK_SC	PYXIS_IACK_SC
#else
	/*
	 * This is bogus but necessary to get it to compile
	 * on all platforms.  If you try to use this on any
	 * other than the intended platforms, you'll notice
	 * real fast...
	 */
#	define IACK_SC	1L
#endif
	int j;

#if 1
	/*
	 * Generate a PCI interrupt acknowledge cycle.  The PIC will
	 * respond with the interrupt vector of the highest priority
	 * interrupt that is pending.  The PALcode sets up the
	 * interrupts vectors such that irq level L generates vector
	 * L.
	 */
	j = *(volatile int *) IACK_SC;
	j &= 0xff;
	if (j == 7) {
		if (!(inb(0x20) & 0x80)) {
			/* it's only a passive release... */
			return;
		}
	}
	device_interrupt(j, j, regs);
#else
	unsigned long pic;

	/*
	 * It seems to me that the probability of two or more *device*
	 * interrupts occurring at almost exactly the same time is
	 * pretty low.  So why pay the price of checking for
	 * additional interrupts here if the common case can be
	 * handled so much easier?
	 */
	/* 
	 *  The first read of gives you *all* interrupting lines.
	 *  Therefore, read the mask register and and out those lines
	 *  not enabled.  Note that some documentation has 21 and a1 
	 *  write only.  This is not true.
	 */
	pic = inb(0x20) | (inb(0xA0) << 8);	/* read isr */
	pic &= ~irq_mask;			/* apply mask */
	pic &= 0xFFFB;				/* mask out cascade & hibits */

	while (pic) {
		j = ffz(~pic);
		pic &= pic - 1;
		device_interrupt(j, j, regs);
	}
#endif
}

#if defined(CONFIG_ALPHA_ALCOR) || defined(CONFIG_ALPHA_XLT)
/* we have to conditionally compile this because of GRU_xxx symbols */
static inline void alcor_and_xlt_device_interrupt(unsigned long vector,
                                                  struct pt_regs * regs)
{
        unsigned long pld;
        unsigned int i;
        unsigned long flags;

        save_flags(flags);
        cli();

        /* read the interrupt summary register of the GRU */
        pld = (*(unsigned int *)GRU_INT_REQ) & GRU_INT_REQ_BITS;

#if 0
        printk("[0x%08lx/0x%04x]", pld, inb(0x20) | (inb(0xA0) << 8));
#endif

        /*
         * Now for every possible bit set, work through them and call
         * the appropriate interrupt handler.
         */
        while (pld) {
                i = ffz(~pld);
                pld &= pld - 1; /* clear least bit set */
                if (i == 31) {
                        isa_device_interrupt(vector, regs);
                } else {
                        device_interrupt(16 + i, 16 + i, regs);
                }
        }
        restore_flags(flags);
}
#endif /* ALCOR || XLT */

static inline void cabriolet_and_eb66p_device_interrupt(unsigned long vector,
							struct pt_regs * regs)
{
	unsigned long pld;
	unsigned int i;
	unsigned long flags;

	save_flags(flags);
	cli();

	/* read the interrupt summary registers */
	pld = inb(0x804) | (inb(0x805) << 8) | (inb(0x806) << 16);

#if 0
	printk("[0x%04X/0x%04X]", pld, inb(0x20) | (inb(0xA0) << 8));
#endif

	/*
	 * Now for every possible bit set, work through them and call
	 * the appropriate interrupt handler.
	 */
	while (pld) {
		i = ffz(~pld);
		pld &= pld - 1;	/* clear least bit set */
		if (i == 4) {
			isa_device_interrupt(vector, regs);
		} else {
			device_interrupt(16 + i, 16 + i, regs);
		}
	}
	restore_flags(flags);
}

static inline void mikasa_device_interrupt(unsigned long vector,
					   struct pt_regs * regs)
{
	unsigned long pld;
	unsigned int i;
	unsigned long flags;

	save_flags(flags);
	cli();

        /* read the interrupt summary registers */
        pld = (((unsigned long) (~inw(0x534)) & 0x0000ffffUL) << 16) |
	       (((unsigned long) inb(0xa0))  <<  8) |
	       ((unsigned long) inb(0x20));

#if 0
        printk("[0x%08lx]", pld);
#endif

        /*
         * Now for every possible bit set, work through them and call
         * the appropriate interrupt handler.
         */
        while (pld) {
		i = ffz(~pld);
		pld &= pld - 1; /* clear least bit set */
		if (i < 16) {
			isa_device_interrupt(vector, regs);
		} else {
			device_interrupt(i, i, regs);
		}
        }
	restore_flags(flags);
}

static inline void eb66_and_eb64p_device_interrupt(unsigned long vector,
						   struct pt_regs * regs)
{
	unsigned long pld;
	unsigned int i;
	unsigned long flags;

	save_flags(flags);
	cli();

	/* read the interrupt summary registers */
	pld = inb(0x26) | (inb(0x27) << 8);
	/*
	 * Now, for every possible bit set, work through
	 * them and call the appropriate interrupt handler.
	 */
	while (pld) {
		i = ffz(~pld);
		pld &= pld - 1;	/* clear least bit set */

		if (i == 5) {
			isa_device_interrupt(vector, regs);
		} else {
			device_interrupt(16 + i, 16 + i, regs);
		}
	}
	restore_flags(flags);
}

#if defined(CONFIG_ALPHA_MIATA) || defined(CONFIG_ALPHA_SX164)
/* we have to conditionally compile this because of PYXIS_xxx symbols */
static inline void miata_device_interrupt(unsigned long vector,
                                                  struct pt_regs * regs)
{
        unsigned long pld, tmp;
        unsigned int i;
        unsigned long flags;

        save_flags(flags);
        cli();

        /* read the interrupt summary register of PYXIS */
        pld = (*(volatile unsigned long *)PYXIS_INT_REQ);

#if 0
        printk("[0x%08lx/0x%08lx/0x%04x]", pld,
	       *(volatile unsigned long *)PYXIS_INT_MASK,
	       inb(0x20) | (inb(0xA0) << 8));
#endif

#ifdef CONFIG_ALPHA_MIATA
	/* for now, AND off any bits we are not interested in: */
	/*  HALT (2), timer (6), ISA Bridge (7), 21142 (8) */
	/*  then all the PCI slots/INTXs (12-31) */
/* maybe HALT should only be used for SRM console boots? */
	pld &= 0x00000000fffff9c4UL;
#endif /* MIATA */
#ifdef CONFIG_ALPHA_SX164
	/* for now, AND off any bits we are not interested in: */
	/*  HALT (2), timer (6), ISA Bridge (7) */
	/*  then all the PCI slots/INTXs (8-23) */
/* HALT should only be used for SRM console boots */
	pld &= 0x0000000000ffffc0UL;
#endif /* SX164 */

        /*
         * Now for every possible bit set, work through them and call
         * the appropriate interrupt handler.
         */
        while (pld) {
                i = ffz(~pld);
                pld &= pld - 1; /* clear least bit set */
                if (i == 7) {
                        isa_device_interrupt(vector, regs);
		} else if (i == 6)
			continue;
                else { /* if not timer int */
                        device_interrupt(16 + i, 16 + i, regs);
                }
		*(unsigned long *)PYXIS_INT_REQ = 1UL << i; mb();
		tmp = *(volatile unsigned long *)PYXIS_INT_REQ;
        }
        restore_flags(flags);
}
#endif /* MIATA || SX164 */

static inline void noritake_device_interrupt(unsigned long vector,
                                                  struct pt_regs * regs)
{
        unsigned long pld;
        unsigned int i;
        unsigned long flags;

        save_flags(flags);
        cli();

        /* read the interrupt summary registers */
        /* read the interrupt summary registers of NORITAKE */
        pld = ((unsigned long) inw(0x544) << 32) |
	      ((unsigned long) inw(0x542) << 16) |
	      ((unsigned long) inb(0xa0)  <<  8) |
	      ((unsigned long) inb(0x20));

#if 0
        printk("[0x%08lx]", pld);
#endif

        /*
         * Now for every possible bit set, work through them and call
         * the appropriate interrupt handler.
         */
        while (pld) {
		i = ffz(~pld);
		pld &= pld - 1; /* clear least bit set */
		if (i < 16) {
			isa_device_interrupt(vector, regs);
		} else {
			device_interrupt(i, i, regs);
		}
        }
        restore_flags(flags);
}

#endif /* CONFIG_PCI */

#if defined(CONFIG_ALPHA_RUFFIAN)
static inline void ruffian_device_interrupt(unsigned long vector,
					    struct pt_regs * regs)

{
	unsigned long pld, tmp;
        unsigned int i;
        unsigned long flags;

        save_flags(flags);
        cli();

        /* read the interrupt summary register of PYXIS */
        pld = (*(volatile unsigned long *)PYXIS_INT_REQ);

        /* for now, AND off any bits we are not interested in:
         *  HALT (2), timer (6), ISA Bridge (7), 21142 (8)
         *  then all the PCI slots/INTXs (12-31) 
         *  flash(5) :DWH:
         */
        pld &= 0x00000000ffffff9fUL;/* was ffff7f */

        /*
         * Now for every possible bit set, work through them and call
         * the appropriate interrupt handler.
         */

        while (pld) {
		i = ffz(~pld);
                pld &= pld - 1; /* clear least bit set */
                if (i == 7) {
			isa_device_interrupt(vector, regs);
                } else { /* if not timer int */
			device_interrupt(16 + i,16 + i,regs);
                }
                *(unsigned long *)PYXIS_INT_REQ = 1UL << i; mb();
                tmp = *(volatile unsigned long *)PYXIS_INT_REQ;
        }
        restore_flags(flags);
}
#endif /* RUFFIAN */

static inline void takara_device_interrupt(unsigned long vector,
					   struct pt_regs * regs)
{
	unsigned long flags;
	unsigned intstatus;

	save_flags(flags);
	cli();

	/*
	 * The PALcode will have passed us vectors 0x800 or 0x810,
	 * which are fairly arbitrary values and serve only to tell
	 * us whether an interrupt has come in on IRQ0 or IRQ1. If
	 * it's IRQ1 it's a PCI interrupt; if it's IRQ0, it's
	 * probably ISA, but PCI interrupts can come through IRQ0
	 * as well if the interrupt controller isn't in accelerated
	 * mode.
	 *
	 * OTOH, the accelerator thing doesn't seem to be working
	 * overly well, so what we'll do instead is try directly
	 * examining the Master Interrupt Register to see if it's a
	 * PCI interrupt, and if _not_ then we'll pass it on to the
	 * ISA handler.
	 */

	intstatus = inw(0x500) & 15;
	if (intstatus) {
		/*
		 * This is a PCI interrupt. Check each bit and
		 * despatch an interrupt if it's set.
		 */
		if (intstatus & 8) device_interrupt(16+3, 16+3, regs);
		if (intstatus & 4) device_interrupt(16+2, 16+2, regs);
		if (intstatus & 2) device_interrupt(16+1, 16+1, regs);
		if (intstatus & 1) device_interrupt(16+0, 16+0, regs);
	} else
		isa_device_interrupt (vector, regs);

	restore_flags(flags);
}

/*
 * Jensen is special: the vector is 0x8X0 for EISA interrupt X, and
 * 0x9X0 for the local motherboard interrupts..
 *
 *	0x660 - NMI
 *
 *	0x800 - IRQ0  interval timer (not used, as we use the RTC timer)
 *	0x810 - IRQ1  line printer (duh..)
 *	0x860 - IRQ6  floppy disk
 *	0x8E0 - IRQ14 SCSI controller
 *
 *	0x900 - COM1
 *	0x920 - COM2
 *	0x980 - keyboard
 *	0x990 - mouse
 *
 * PCI-based systems are more sane: they don't have the local
 * interrupts at all, and have only normal PCI interrupts from
 * devices.  Happily it's easy enough to do a sane mapping from the
 * Jensen..  Note that this means that we may have to do a hardware
 * "ack" to a different interrupt than we report to the rest of the
 * world.
 */
static inline void srm_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
	int irq, ack;
	unsigned long flags;

	save_flags(flags);
	cli();

#if 0
printk("srm_device_interrupt: vector 0x%lx\n", vector);
#endif

	ack = irq = (vector - 0x800) >> 4;

#ifdef CONFIG_ALPHA_JENSEN
	switch (vector) {
	      case 0x660: handle_nmi(regs); return;
		/* local device interrupts: */
	      case 0x900: handle_irq(4, regs); return;	/* com1 -> irq 4 */
	      case 0x920: handle_irq(3, regs); return;	/* com2 -> irq 3 */
	      case 0x980: handle_irq(1, regs); return;	/* kbd -> irq 1 */
	      case 0x990: handle_irq(9, regs); return;	/* mouse -> irq 9 */
	      default:
		if (vector > 0x900) {
			printk("Unknown local interrupt %lx\n", vector);
		}
	}
	/* irq1 is supposed to be the keyboard, silly Jensen (is this really needed??) */
	if (irq == 1)
		irq = 7;
#endif /* CONFIG_ALPHA_JENSEN */

#ifdef CONFIG_ALPHA_MIATA
        /*
         * I really hate to do this, but the MIATA SRM console ignores the
         *  low 8 bits in the interrupt summary register, and reports the
         *  vector 0x80 *lower* than I expected from the bit numbering in
         *  the documentation.
         * This was done because the low 8 summary bits really aren't used
         *  for reporting any interrupts (the PCI-ISA bridge, bit 7, isn't
         *  used for this purpose, as PIC interrupts are delivered as the
         *  vectors 0x800-0x8f0).
         * But I really don't want to change the fixup code for allocation
         *  of IRQs, nor the irq_mask maintenance stuff, both of which look
         *  nice and clean now.
         * So, here's this grotty hack... :-(
         */
        if (irq >= 16)
		ack = irq = irq + 8;
#endif /* CONFIG_ALPHA_MIATA */

#ifdef CONFIG_ALPHA_NORITAKE
        /*
         * I really hate to do this, but the NORITAKE SRM console reports
         *  PCI vectors *lower* than I expected from the bit numbering in
         *  the documentation.
         * But I really don't want to change the fixup code for allocation
         *  of IRQs, nor the irq_mask maintenance stuff, both of which look
         *  nice and clean now.
         * So, here's this grotty hack... :-(
         */
        if (irq >= 16)
		ack = irq = irq + 1;
#endif /* CONFIG_ALPHA_NORITAKE */

#ifdef CONFIG_ALPHA_SABLE
	irq = sable_mask_to_irq[(ack)];
#if 0
	if (irq == 5 || irq == 9 || irq == 10 || irq == 11 ||
	    irq == 14 || irq == 15)
	printk("srm_device_interrupt: vector=0x%lx  ack=0x%x  irq=0x%x\n",
	       vector, ack, irq);
#endif
#endif /* CONFIG_ALPHA_SABLE */

	device_interrupt(irq, ack, regs);

	restore_flags(flags) ;
}

/* PROBE_MASK is the bitset of irqs that we consider for autoprobing: */
#if defined(CONFIG_ALPHA_P2K)
  /* always mask out unused timer irq 0 and RTC irq 8 */
# define PROBE_MASK (((1UL << NR_IRQS) - 1) & ~0x101UL)
#elif defined(CONFIG_ALPHA_ALCOR) || defined(CONFIG_ALPHA_XLT)
  /* always mask out unused timer irq 0, "irqs" 20-30, and the EISA cascade: */
# define PROBE_MASK (((1UL << NR_IRQS) - 1) & ~0xfff000000001UL)
#elif defined(CONFIG_ALPHA_RUFFIAN)
  /* must leave timer irq 0 in the mask */
# define PROBE_MASK ((1UL << NR_IRQS) - 1)
#else
  /* always mask out unused timer irq 0: */
# define PROBE_MASK (((1UL << NR_IRQS) - 1) & ~1UL)
#endif

/*
 * Start listening for interrupts..
 */
unsigned long probe_irq_on(void)
{
	struct irqaction * action;
	unsigned long irqs = 0;
	unsigned long delay;
	unsigned int i;

	for (i = NR_IRQS - 1; i > 0; i--) {
		if (!(PROBE_MASK & (1UL << i))) {
			continue;
                }
		action = irq_action[i];
		if (!action) {
			enable_irq(i);
			irqs |= (1UL << i);
		}
	}
	/*
	 * Wait about 100ms for spurious interrupts to mask themselves
	 * out again...
	 */
	for (delay = jiffies + HZ/10; delay > jiffies; )
		barrier();

	/* now filter out any obviously spurious interrupts */
	return irqs & ~irq_mask;
}

/*
 * Get the result of the IRQ probe.. A negative result means that
 * we have several candidates (but we return the lowest-numbered
 * one).
 */
int probe_irq_off(unsigned long irqs)
{
	int i;
	
	irqs &= irq_mask;
	if (!irqs)
		return 0;
	i = ffz(~irqs);
	if (irqs != (1UL << i))
		i = -i;
	return i;
}

static void machine_check(unsigned long vector, unsigned long la, struct pt_regs * regs)
{
#if defined(CONFIG_ALPHA_LCA)
	extern void lca_machine_check (unsigned long vector, unsigned long la,
				       struct pt_regs *regs);
	lca_machine_check(vector, la, regs);
#elif defined(CONFIG_ALPHA_APECS)
	extern void apecs_machine_check(unsigned long vector, unsigned long la,
					struct pt_regs * regs);
	apecs_machine_check(vector, la, regs);
#elif defined(CONFIG_ALPHA_CIA)
	extern void cia_machine_check(unsigned long vector, unsigned long la,
					struct pt_regs * regs);
	cia_machine_check(vector, la, regs);
#elif defined(CONFIG_ALPHA_PYXIS)
	extern void pyxis_machine_check(unsigned long vector, unsigned long la,
					struct pt_regs * regs);
	pyxis_machine_check(vector, la, regs);
#elif defined(CONFIG_ALPHA_T2)
	extern void t2_machine_check(unsigned long vector, unsigned long la,
				     struct pt_regs * regs);
	t2_machine_check(vector, la, regs);
#else
	printk("Machine check\n");
#endif
}

asmlinkage void do_entInt(unsigned long type, unsigned long vector, unsigned long la_ptr,
	unsigned long a3, unsigned long a4, unsigned long a5,
	struct pt_regs regs)
{
#if 0
printk("do_entInt: type 0x%lx\n", type);
#endif
	switch (type) {
		case 0:
			printk("Interprocessor interrupt? You must be kidding\n");
			break;
		case 1:
			timer_interrupt(&regs);
			return;
		case 2:
			machine_check(vector, la_ptr, &regs);
			return;
		case 3:
#if defined(CONFIG_ALPHA_JENSEN) || defined(CONFIG_ALPHA_NONAME) || \
    defined(CONFIG_ALPHA_P2K) || defined(CONFIG_ALPHA_SRM)
			srm_device_interrupt(vector, &regs);
#else /* everyone else */

#if defined(CONFIG_ALPHA_MIATA) || defined(CONFIG_ALPHA_SX164)
			miata_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_NORITAKE)
			noritake_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_ALCOR) || defined(CONFIG_ALPHA_XLT)
			alcor_and_xlt_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_CABRIOLET) || defined(CONFIG_ALPHA_EB66P) || \
      defined(CONFIG_ALPHA_EB164)     || defined(CONFIG_ALPHA_PC164) || \
      defined(CONFIG_ALPHA_LX164)
			cabriolet_and_eb66p_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_MIKASA)
			mikasa_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_EB66) || defined(CONFIG_ALPHA_EB64P)
			eb66_and_eb64p_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_RUFFIAN)
                        ruffian_device_interrupt(vector, &regs);
#elif defined(CONFIG_ALPHA_TAKARA)
			takara_device_interrupt(vector, &regs);
#elif NR_IRQS == 16
			isa_device_interrupt(vector, &regs);
#endif
#endif /* everyone else */
			return;
		case 4:
			printk("Performance counter interrupt\n");
			break;;
		default:
			printk("Hardware intr %ld %lx? Huh?\n", type, vector);
	}
	printk("PC = %016lx PS=%04lx\n", regs.pc, regs.ps);
}

extern asmlinkage void entInt(void);

void init_IRQ(void)
{
	unsigned int temp;

	wrent(entInt, 0);

	outb(0, DMA1_RESET_REG);
	outb(0, DMA2_RESET_REG);

/* FIXME FIXME FIXME FIXME FIXME */
#if !defined(CONFIG_ALPHA_SX164)
	outb(0, DMA1_CLR_MASK_REG);
	/* we need to figure out why this fails on the SX164 */
	outb(0, DMA2_CLR_MASK_REG);
#endif /* !SX164 */
/* end FIXMEs */

#if defined(CONFIG_ALPHA_SABLE)
	outb(irq_mask      , 0x537); /* slave 0 */
	outb(irq_mask >>  8, 0x53b); /* slave 1 */
	outb(irq_mask >> 16, 0x53d); /* slave 2 */
	outb(0x44, 0x535);	/* enable cascades in master */
#else /* everybody but SABLE */

#if defined(CONFIG_ALPHA_MIATA)
	/* note invert on MASK bits */
        *(unsigned long *)PYXIS_INT_MASK  =
	  ~((long)irq_mask >> 16) & ~0x400000000000063bUL; mb();
#if 0
	/* these break on MiataGL so we'll try not to do it at all */
        *(unsigned long *)PYXIS_INT_HILO  = 0x000000B2UL; mb();/* ISA/NMI HI */
        *(unsigned long *)PYXIS_RT_COUNT  = 0UL; mb();/* clear count */
#endif
	/* clear upper timer */
        *(unsigned long *)PYXIS_INT_REQ  = 0x4000000000000180UL; mb();

	/* Send -INTA pulses to clear any pending interrupts ...*/
	temp = *(volatile unsigned int *) IACK_SC;

	enable_irq(16 + 2);	/* enable HALT switch - SRM only? */
        enable_irq(16 + 6);     /* enable timer */
        enable_irq(16 + 7);     /* enable ISA PIC cascade */
#endif /* MIATA */

#if defined(CONFIG_ALPHA_SX164)
#if !defined(CONFIG_ALPHA_SRM)
	/* note invert on MASK bits */
        *(unsigned long *)PYXIS_INT_MASK  = ~((long)irq_mask >> 16); mb();
#if 0
        *(unsigned long *)PYXIS_INT_HILO  = 0x000000B2UL; mb();/* ISA/NMI HI */
        *(unsigned long *)PYXIS_RT_COUNT  = 0UL; mb();/* clear count */
#endif
#endif /* !SRM */
        enable_irq(16 + 6);     /* enable timer */
        enable_irq(16 + 7);     /* enable ISA PIC cascade */
#endif /* SX164 */

#if defined(CONFIG_ALPHA_NORITAKE)
	outw(~(irq_mask >> 16), 0x54a); /* note invert */
	outw(~(irq_mask >> 32), 0x54c); /* note invert */
#endif /* NORITAKE */

#if defined(CONFIG_ALPHA_ALCOR) || defined(CONFIG_ALPHA_XLT)
	*(unsigned int *)GRU_INT_MASK = ~(irq_mask >> 16); mb();/* invert */
	*(unsigned int *)GRU_INT_EDGE  = 0UL; mb();/* all are level */
	*(unsigned int *)GRU_INT_HILO  = 0x80000000UL; mb();/* ISA only HI */
	*(unsigned int *)GRU_INT_CLEAR = 0UL; mb();/* all clear */
	enable_irq(16 + 31);	/* enable (E)ISA PIC cascade */
#endif /* ALCOR || XLT */

#if defined(CONFIG_ALPHA_CABRIOLET) || defined(CONFIG_ALPHA_EB66P) || \
    defined(CONFIG_ALPHA_PC164)     || defined(CONFIG_ALPHA_LX164) || \
    defined(CONFIG_ALPHA_EB164)
#if !defined(CONFIG_ALPHA_SRM)
	outl(irq_mask >> 16, 0x804);
#endif /* !SRM */
	/* Send -INTA pulses to clear any pending interrupts ...*/
	temp = *(volatile unsigned int *) IACK_SC;
	enable_irq(16 +  4);	/* enable SIO cascade */
#endif /* CABRIO || EB66P || PC164 || LX164 || EB164 */

#if defined(CONFIG_ALPHA_MIKASA)
	outw(~(irq_mask >> 16), 0x536); /* note invert */
#endif /* MIKASA */

#if defined(CONFIG_ALPHA_EB66) || defined(CONFIG_ALPHA_EB64P)
	outb(irq_mask >> 16, 0x26);
	outb(irq_mask >> 24, 0x27);
	enable_irq(16 +  5);	/* enable SIO cascade */
#endif /* EB66 || EB64P */

#if defined(CONFIG_ALPHA_RUFFIAN)
	/* invert 6&7 for i82371 */
	*(unsigned long *)PYXIS_INT_HILO  = 0x000000c0UL;mb();
	*(unsigned long *)PYXIS_INT_CNFG  = 0x00002064UL;mb();   /* all clear */
	*(unsigned long *)PYXIS_INT_MASK  = ((long)0x00000000UL);mb();
	*(unsigned long *)PYXIS_INT_REQ   = 0xffffffffUL;mb();

	outb(0x11,0xA0);
	outb(0x08,0xA1);
	outb(0x02,0xA1);
	outb(0x01,0xA1);
	outb(0xFF,0xA1);
	
	outb(0x11,0x20);
	outb(0x00,0x21);
	outb(0x04,0x21);
	outb(0x01,0x21);
	outb(0xFF,0x21);
	
	/* Send -INTA pulses to clear any pending interrupts ...*/
	temp = *(volatile unsigned int *) IACK_SC;
	
	/* Finish writing the 82C59A PIC Operation Control Words */
	outb(0x20,0xA0);
	outb(0x20,0x20);
	
	/* Turn on the interrupt controller, the timer interrupt  */
	enable_irq(16 + 7);     /* enable ISA PIC cascade */
	enable_irq(0);          /* enable timer */
#endif /* RUFFIAN */

#ifdef CONFIG_ALPHA_TAKARA
	{
		unsigned int ctlreg = inl(0x500);
		ctlreg &= ~0x8000;     /* return to non-accelerated mode */
		outw(ctlreg >> 16, 0x502);
		outw(ctlreg & 0xFFFF, 0x500);
		ctlreg = 0x05107c00;   /* enable the PCI interrupt register */
		printk("Setting to 0x%08x\n", ctlreg);
		outw(ctlreg >> 16, 0x502);
		outw(ctlreg & 0xFFFF, 0x500);
	}
#endif /* TAKARA */

	/* and finally, everyone but SABLE does this */
	enable_irq(2);		/* enable 2nd PIC cascade */

#endif /* SABLE */
}

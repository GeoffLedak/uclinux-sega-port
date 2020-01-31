/*****************************************************************************/
/* 
 *  config.c v1.0 <2003-07-28 17:55:00 gc>
 * 
 *  linux/arch/m68knommu/platform/68000/config.c
 *
 *  uClinux version 2.0.x MC68000 board initalization
 *
 *  Author:     Guido Classen (classeng@clagi.de)
 *              Steve Chamberlin (steve.chamberlin@gmail.com)
 *
 *  This program is free software;  you can redistribute it and/or modify it
 *  under the  terms of the GNU  General Public License as  published by the
 *  Free Software Foundation.  See the file COPYING in the main directory of
 *  this archive for more details.
 *
 *  This program  is distributed  in the  hope that it  will be  useful, but
 *  WITHOUT   ANY   WARRANTY;  without   even   the   implied  warranty   of
 *  MERCHANTABILITY  or  FITNESS FOR  A  PARTICULAR  PURPOSE.   See the  GNU
 *  General Public License for more details.
 *
 *  Thanks to:
 *    inital code taken from 68328/config.c
 *
 *      Copyright (C) 1993 Hamish Macdonald
 *      Copyright (C) 1999 D. Jeff Dionne
 *
 *  Change history:
 *       2002-05-15 G. Classen: initial version for MC68000
 *       2014-11-09 S. Chamberlin: cleaned up SM2010-specific code
 *
 */
 /****************************************************************************/

#include <stdarg.h>
#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/console.h>

#include <asm/setup.h>
#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/irq.h>
#include <asm/machdep.h>

#ifdef CONFIG_SM2010
#include "SM2010/sm2010_hw.h"
void mpsc_console_initialize(void);
#endif

#ifdef CONFIG_68KATY
#include "68Katy/68Katy_hw.h"
void ft245_console_initialize(void);
#endif

void config_M68000_irq(void);

#ifdef CONFIG_SM2010
/* initialize timer hardware */
static void sm2010_init_timer_hw(void)
{
        /* Timer 0 controlwort out = low MODE 0*/
        SM2010_TIMER.control    = 0x30;
        SM2010_TIMER.counter0   = 0;
        SM2010_TIMER.counter0   = 0;
        /* Timer 2 nur controlwort out = high MODE 2*/
        SM2010_TIMER.control    = 0xb4;

        /* timer2 auf 2ms initialisieren */
        SM2010_TIMER.counter2   = ((SM2010_SIO_CLOCK_SYS / 500)) & 0xff;
        SM2010_TIMER.counter2   = ((SM2010_SIO_CLOCK_SYS / 500)) >> 8;
        SM2010_RESET_TIMER_INT2 = 0;
        SM2010_TIMER.control    = 0x74;
        SM2010_TIMER.counter1   = (SM2010_SIO_CLOCK_SYS / 1000) & 0xff;
        SM2010_TIMER.counter1   = (SM2010_SIO_CLOCK_SYS / 1000) >> 8;
}
#endif

#ifdef CONFIG_SM2010
static void timer1_interrupt(int irq, void *dummy, struct pt_regs * regs)
{
        SM2010_RESET_TIMER_INT1 = 0;
}
#endif

static void 
BSP_sched_init(void (*timer_routine)(int, void *, struct pt_regs *))
{
#ifdef CONFIG_SM2010
        /* initialize timer */
        sm2010_init_timer_hw();
        request_irq(SM2010_INT_NUM_TIMER2-VEC_SPUR, 
                    timer_routine, IRQ_FLG_LOCK, "timer2", NULL);
        request_irq(SM2010_INT_NUM_TIMER1-VEC_SPUR, 
                    timer1_interrupt, IRQ_FLG_LOCK, "timer1", NULL);

        printk("\nMC68000 SM2010 support (C) 2002 Weiss-Electronic GmbH, "
               "Guido Classen\n");
#endif
#ifdef CONFIG_68KATY
        request_irq(M68KATY_INT_NUM_TIMER-VEC_SPUR, 
                    timer_routine, IRQ_FLG_LOCK, "timer", NULL);
        request_irq(M68KATY_INT_NUM_SERIAL_AND_TIMER-VEC_SPUR, 
                    timer_routine, IRQ_FLG_LOCK, "timer-and-serial", NULL);

        printk("MC68000 68 Katy support by Big Mess o' Wires, "
               "Steve Chamberlin\n");
#endif
}

void BSP_tick(void)
{
#ifdef CONFIG_SM2010
        /* Reset Timer2 */
        SM2010_RESET_TIMER_INT2 = 0;
#endif
#ifdef CONFIG_68KATY
	/* reset the timer interrupt */
#endif
}

unsigned long BSP_gettimeoffset(void)
{
        return 0;
}

void BSP_gettod (int *yearp, int *monp, int *dayp,
                 int *hourp, int *minp, int *secp)
{
}

int BSP_hwclk(int op, struct hwclk_time *t)
{
        if (!op) {
                /* read */
        } else {
                /* write */
        }
        return 0;
}

int BSP_set_clock_mmss (unsigned long nowtime)
{
#if 0
        short real_seconds = nowtime % 60, real_minutes = (nowtime / 60) % 60;

        tod->second1 = real_seconds / 10;
        tod->second2 = real_seconds % 10;
        tod->minute1 = real_minutes / 10;
        tod->minute2 = real_minutes % 10;
#endif
        return 0;
}

void BSP_reset (void)
{
        cli();
        HARD_RESET_NOW();
}



void config_BSP(char *command, int len)
{
#ifdef CONFIG_SM2010
        mpsc_console_initialize();
#endif
#ifdef CONFIG_FT245_SERIAL
	ft245_console_initialize();
#endif

        mach_sched_init      = BSP_sched_init;
        mach_tick            = BSP_tick;
        mach_gettimeoffset   = BSP_gettimeoffset;
        mach_gettod          = BSP_gettod;
        mach_hwclk           = NULL;
        mach_set_clock_mmss  = NULL;
        mach_mksound         = NULL;
        mach_reset           = BSP_reset;
        mach_debug_init      = NULL;

        config_M68000_irq();

#ifdef CONFIG_SM2010
        /* enable interrupts :-) */
        SM2010_BOARD_CONTROL = 0x1f;  
#endif
}

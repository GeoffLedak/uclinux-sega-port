/*
 *  linux/arch/arm/mm/fault.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *  Modifications for ARM processor (c) 1995, 1996 Russell King
 */

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

#define FAULT_CODE_READ		0x02
#define FAULT_CODE_USER		0x01

extern void die_if_kernel(char *msg, struct pt_regs *regs, unsigned int err, unsigned int ret);

static void kernel_page_fault (unsigned long addr, int mode, struct pt_regs *regs,
			       struct task_struct *tsk, struct mm_struct *mm)
{
	/*
	 * Oops. The kernel tried to access some bad page. We'll have to
	 * terminate things with extreme prejudice.
	 */
	if (addr < PAGE_SIZE)
	    printk (KERN_ALERT "Unable to handle kernel NULL pointer dereference");
	else
	    printk (KERN_ALERT "Unable to handle kernel paging request");
	printk (" at virtual address %08lx\n", addr);
	die_if_kernel ("Oops", regs, mode, SIGKILL);
	do_exit (SIGKILL);
}

static void page_fault (unsigned long addr, int mode, struct pt_regs *regs)
{
	struct task_struct *tsk = current;
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;

	if (mode & FAULT_CODE_USER) {
		tsk->tss.error_code = mode;
		tsk->tss.trap_no = 14;
		printk ("%s: memory violation at pc=0x%08lx, lr=0x%08lx (bad address=0x%08lx, code %d)\n",
			tsk->comm, regs->ARM_pc, regs->ARM_lr, addr, mode);
#ifdef DEBUG
		show_regs (regs);
		c_backtrace (regs->ARM_fp, regs->ARM_cpsr);
#endif
		force_sig(SIGSEGV, tsk);
		return;
	}

	kernel_page_fault (addr, mode, regs, tsk, mm);
}

/*
 * Handle a data abort.  Note that we have to handle a range of addresses
 * on ARM2/3 for ldm.  If both pages are zero-mapped, then we have to force
 * a copy-on-write
 */
asmlinkage void
do_DataAbort (unsigned long addr, int fsr, int error_code, struct pt_regs *regs)
{
	if (user_mode(regs))
		error_code |= FAULT_CODE_USER;

#define DIE(signr,nam)\
		force_sig(signr, current);\
		die_if_kernel(nam, regs, fsr, signr);\
		break;

	switch (fsr & 15) {
	case 2:
		DIE(SIGKILL, "Terminal exception")
	case 0:
		DIE(SIGSEGV, "Vector exception")
	case 1:
	case 3:
		DIE(SIGBUS, "Alignment exception")
	case 12:
	case 14:
		DIE(SIGBUS, "External abort on translation")
	case 9:
	case 11:
		DIE(SIGSEGV, "Domain fault")
	case 13:/* permission fault on section */
#ifndef DEBUG
		show_regs(regs);
		{
			unsigned int i, j, a;
			a = regs->ARM_sp;
			for (j = 0; j < 10; j++) {
				printk ("%08x: ", a);
				for (i = 0; i < 8; i += 1, a += 4)
					printk ("%08lx ", *(unsigned long *)a);
				printk ("\n");
			}
		}
#endif
		DIE(SIGSEGV, "Permission fault")

	case 15:/* permission fault on page */
	case 5:	/* page-table entry descriptor fault */
	case 7:	/* first-level descriptor fault */
		page_fault (addr, error_code, regs);
		break;
	case 4:
	case 6:
		DIE(SIGBUS, "External abort on linefetch")
	case 8:
	case 10:
		DIE(SIGBUS, "External abort on non-linefetch")
	}
}

asmlinkage int
do_PrefetchAbort (unsigned long addr, struct pt_regs *regs)
{
#if 0
	/* does this still apply ? */
	if (the memc mapping for this page exists - can check now...) {
		printk ("Page in, but got abort (undefined instruction?)\n");
		return 0;
	}
#endif
	page_fault (addr, FAULT_CODE_USER|FAULT_CODE_READ, regs);
	return 1;
}


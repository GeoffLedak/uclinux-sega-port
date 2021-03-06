#ifndef _M68K_PTRACE_H
#define _M68K_PTRACE_H

#include <linux/config.h> /* get configuration macros */

#define PT_D1	   0
#define PT_D2	   1
#define PT_D3	   2
#define PT_D4	   3
#define PT_D5	   4
#define PT_D6	   5
#define PT_D7	   6
#define PT_A0	   7
#define PT_A1	   8
#define PT_A2	   9
#define PT_A3	   10
#define PT_A4	   11
#define PT_A5	   12
#define PT_A6	   13
#define PT_D0	   14
#define PT_USP	   15
#define PT_ORIG_D0 16
#define PT_SR	   17
#define PT_PC	   18

#ifndef __ASSEMBLY__

/* this struct defines the way the registers are stored on the
   stack during a system call. */

struct pt_regs {
  long     d1;
  long     d2;
  long     d3;
  long     d4;
  long     d5;
  long     a0;
  long     a1;
  long     d0;
  long     orig_d0;
  long     stkadj;
#ifdef CONFIG_COLDFIRE
  unsigned format :  4; /* frame format specifier */
  unsigned vector : 12; /* vector offset */
  unsigned short sr;
  unsigned long  pc;
#else
  unsigned short sr;
  unsigned long  pc;
#ifndef NO_FORMAT_VEC
  unsigned format :  4; /* frame format specifier */
  unsigned vector : 12; /* vector offset */
#endif
#endif
};

/*
 * This is the extended stack used by signal handlers and the context
 * switcher: it's pushed after the normal "struct pt_regs".
 */
struct switch_stack {
	unsigned long  d6;
	unsigned long  d7;
	unsigned long  a2;
	unsigned long  a3;
	unsigned long  a4;
	unsigned long  a5;
	unsigned long  a6;
	unsigned long  retpc;
};

#ifdef __KERNEL__

#ifndef PS_S
#define PS_S  (0x2000)
#define PS_M  (0x1000)
#endif

#define user_mode(regs) (!((regs)->sr & PS_S))
#define instruction_pointer(regs) ((regs)->pc)
extern void show_regs(struct pt_regs *);
#endif /* __KERNEL__ */
#endif /* __ASSEMBLY__ */
#endif /* _M68K_PTRACE_H */

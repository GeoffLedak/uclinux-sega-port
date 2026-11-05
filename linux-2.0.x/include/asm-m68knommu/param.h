#ifndef _M68K_PARAM_H
#define _M68K_PARAM_H

#include <linux/config.h>

#ifndef HZ
#ifdef CONFIG_HW_FEITH
#define HZ 1000
#endif
#ifdef CONFIG_SHGLCORE
#define HZ 50
#endif
#ifdef CONFIG_SM2010
/* 2002-05-14 gc: */
#define HZ 500
#endif
#ifdef CONFIG_68KATY
/* Sega Genesis: the system tick comes from the vblank interrupt, 59.92
 * times a second on NTSC consoles.  PAL consoles (49.70 Hz) get an extra
 * tick every fifth frame; see arch/m68knommu/platform/68000/config.c. */
#define HZ 60
#endif
#ifndef HZ
#define	HZ 100
#endif
#endif

#define EXEC_PAGESIZE	4096

#ifndef NGROUPS
#define NGROUPS		32
#endif

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#endif /* _M68K_PARAM_H */

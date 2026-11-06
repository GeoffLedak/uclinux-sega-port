/*
 * memtest -- test all 2 MB of cartridge RAM (mapper slots 4-7,
 * 0x200000 - 0x3FFFFF) while Linux is running.
 *
 *	memtest [passes]
 *
 * Linux itself lives in this RAM, so the testing is done by the kernel
 * (the GENESIS_MEMTEST console ioctl, hw_md.S): with interrupts off it
 * copies a 256-byte block aside, tests it with patterns, checks that the
 * address lines and the slot mapping are right, and puts the block back.
 * This program walks through all 8192 blocks and reports.  Ctrl-C stops
 * it safely at any time: no block is ever left half tested.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <linux/genesis_console.h>

#define BLOCK		256
#define SLOT_SIZE	0x80000
#define DOTS		10		/* progress dots per slot (fits 36 columns) */
#define MAX_REPORTS	8		/* errors shown in full */

static int console = -1;
static unsigned long errors;

static const char *what(unsigned long result)
{
	switch (result) {
	case 1: return "data error";
	case 2: return "address line error";
	case 3: return "byte write error";
	}
	return "error";
}

/* which address line differs between a and b, for an address error */
static int line_of(unsigned long a, unsigned long b)
{
	unsigned long d = a ^ b;
	int n = 0;

	while (d > 1) {
		d >>= 1;
		n++;
	}
	return n;
}

static void report(struct genesis_memtest *m)
{
	errors++;
	if (errors > MAX_REPORTS)
		return;
	printf("\n %s at %06lX\n", what(m->result), m->bad_addr);
	if (m->result == 2)
		printf("  (address line A%d, block %06lX:\n"
		       "  writing it changed %06lX)\n",
		       line_of(m->addr, m->bad_addr), m->addr, m->bad_addr);
	else
		printf("  wrote %08lX, read %08lX\n", m->wrote, m->read);
	if (errors == MAX_REPORTS)
		printf(" (more errors are only counted)\n");
}

/* test one slot; returns its error count */
static unsigned long test_slot(int slot)
{
	struct genesis_memtest m;
	unsigned long start = GENESIS_RAM_START + (slot - 4) * SLOT_SIZE;
	unsigned long a, before = errors;
	int dot = 0;

	printf("Slot %d %06lX-%06lX ", slot, start, start + SLOT_SIZE - 1);
	fflush(stdout);
	for (a = start; a < start + SLOT_SIZE; a += BLOCK) {
		memset(&m, 0, sizeof m);
		m.addr = a;
		if (ioctl(console, GENESIS_MEMTEST, &m) < 0) {
			perror("\nmemtest");
			exit(2);
		}
		if (m.result)
			report(&m);
		if ((a - start + BLOCK) * DOTS / SLOT_SIZE > dot) {
			dot++;
			putchar('.');
			fflush(stdout);
		}
	}
	if (errors == before)
		printf(" OK\n");
	else
		printf(" BAD\n");
	return errors - before;
}

int main(int argc, char **argv)
{
	struct genesis_memtest probe;
	struct tms tm;
	clock_t t0;
	int passes = 1, pass, slot, fd;

	if (argc > 2 || (argc == 2 && (passes = atoi(argv[1])) < 1)) {
		fprintf(stderr, "usage: memtest [passes]\n");
		return 2;
	}

	/* the ioctl is on the Genesis console: find it */
	for (fd = 0; fd < 3 && console < 0; fd++) {
		memset(&probe, 0, sizeof probe);
		probe.addr = GENESIS_RAM_END - BLOCK;
		if (ioctl(fd, GENESIS_MEMTEST, &probe) == 0)
			console = fd;
	}
	if (console < 0) {
		fprintf(stderr, "memtest: run me on the Genesis console\n");
		return 2;
	}

	printf("Testing cartridge RAM %06X-%06X\n(2048 KB, mapper slots 4-7)\n",
	       GENESIS_RAM_START, GENESIS_RAM_END - 1);
	printf("Ctrl-C stops the test.\n\n");

	t0 = times(&tm);
	for (pass = 1; pass <= passes; pass++) {
		if (passes > 1)
			printf("Pass %d of %d\n", pass, passes);
		for (slot = 4; slot <= 7; slot++)
			test_slot(slot);
	}

	printf("\n");
	if (errors)
		printf("FAILED: %lu bad block%s\n", errors, errors == 1 ? "" : "s");
	else
		printf("All 2048 KB passed");
	if (!errors)
		printf(" (%lu s)\n", (unsigned long)(times(&tm) - t0) / 60);
	return errors ? 1 : 0;
}

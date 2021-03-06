/*
 *  linux/mm/kmalloc.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds & Roger Wolff.
 *
 *  Written by R.E. Wolff Sept/Oct '93.
 *
 */

/*
 * Modified by Alex Bligh (alex@cconcepts.co.uk) 4 Apr 1994 to use multiple
 * pages. So for 'page' throughout, read 'area'.
 *
 * Largely rewritten.. Linus
 */

/*
 * Modified to use the contiguous page_allocator to help limit memory
 * wastage on embedded systems (davidm@lineo.com).
 *
 * Copyright (C) 2000 Lineo Australia
 */

#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/interrupt.h>

#include <asm/system.h>
#include <asm/dma.h>

/* Define this if you want slow routines that try to trip errors */
#undef SADISTIC_KMALLOC

/* Private flags. */

#define MF_USED 0xffaa0055
#define MF_DMA  0xff00aa55
#define MF_FREE 0x0055ffaa


/*
 * Much care has gone into making these routines in this file reentrant.
 *
 * The fancy bookkeeping of nbytesmalloced and the like are only used to
 * report them to the user (oooohhhhh, aaaaahhhhh....) are not
 * protected by cli(). (If that goes wrong. So what?)
 *
 * These routines restore the interrupt status to allow calling with ints
 * off.
 */

/*
 * A block header. This is in front of every malloc-block, whether free or not.
 */
struct block_header {
	unsigned long bh_flags;
	union {
		unsigned long ubh_length;
		struct block_header *fbh_next;
	} vp;
};


#define bh_length vp.ubh_length
#define bh_next   vp.fbh_next
#define BH(p) ((struct block_header *)(p))


/*
 * The page descriptor is at the front of every page that malloc has in use.
 */
struct page_descriptor {
	struct page_descriptor *next;
	struct block_header *firstfree;
	int order;
	int nfree;
};


#define PAGE_DESC(p) ((struct page_descriptor *)(((unsigned long)(p)) & PAGE_MASK))


/*
 * A size descriptor describes a specific class of malloc sizes.
 * Each class of sizes has its own freelist.
 */
struct size_descriptor {
	struct page_descriptor *firstfree;
	struct page_descriptor *dmafree;	/* DMA-able memory */
	int nblocks;

	int nmallocs;
	int nfrees;
	int nbytesmalloced;
	int npages;
	unsigned long gfporder;	/* number of pages in the area required */
};

/*
 * For now it is unsafe to allocate bucket sizes between n and
 * n-sizeof(page_descriptor) where n is PAGE_SIZE * any power of two
 *
 * The blocksize and sizes arrays _must_ match!
 */

#if PAGE_SIZE == 4096

static const unsigned int blocksize[] = {
	32,
	64,
	128,
	252,
	508,
	1020,
	2040,
	0xffffffff, /* big bad catch all allocs */
	0
};

static struct size_descriptor sizes[] =
{
	{NULL, NULL, 127, 0, 0, 0, 0, 0},
	{NULL, NULL, 63, 0, 0, 0, 0, 0},
	{NULL, NULL, 31, 0, 0, 0, 0, 0},
	{NULL, NULL, 16, 0, 0, 0, 0, 0},
	{NULL, NULL, 8, 0, 0, 0, 0, 0},
	{NULL, NULL, 4, 0, 0, 0, 0, 0},
	{NULL, NULL, 2, 0, 0, 0, 0, 0},
	{NULL, NULL, 1, 0, 0, 0, 0, 0},
	{NULL, NULL, 0, 0, 0, 0, 0, 0},
};

#elif PAGE_SIZE == 8192

static const unsigned int blocksize[] = {
	64,
	128,
	248,
	504,
	1016,
	2040,
	4080,
	0xffffffff,
	0
};

struct size_descriptor sizes[] =
{
	{NULL, NULL, 127, 0, 0, 0, 0, 0},
	{NULL, NULL, 63, 0, 0, 0, 0, 0},
	{NULL, NULL, 31, 0, 0, 0, 0, 0},
	{NULL, NULL, 16, 0, 0, 0, 0, 0},
	{NULL, NULL, 8, 0, 0, 0, 0, 0},
	{NULL, NULL, 4, 0, 0, 0, 0, 0},
	{NULL, NULL, 2, 0, 0, 0, 0, 0},
	{NULL, NULL, 1, 0, 0, 0, 0, 0},
	{NULL, NULL, 0, 0, 0, 0, 0, 0}
};

#else

#error you need to make a version for your pagesize

#endif

#define NBLOCKS(order)          (sizes[order].nblocks)
#define BLOCKSIZE(order)        (blocksize[order])
#define	NUM_PAGES(n)		(((n) + PAGE_SIZE - 1) >> PAGE_SHIFT)

/*
 * Create a small cache of page allocations: this helps a bit with
 * those pesky 8kB+ allocations for NFS when we're temporarily
 * out of memory..
 *
 * This is a _truly_ small cache, we just cache one single page
 * order (for orders 0, 1 and 2, that is  4, 8 and 16kB on x86).
 */
#define MAX_CACHE_ORDER 7
struct page_descriptor * kmalloc_cache[MAX_CACHE_ORDER];

/*
 * extern these for now rather than pollute the linux headers
 */

extern unsigned long	__get_contiguous_pages(int priority,
					unsigned long num_adjpages, int dma);
extern void		free_contiguous_pages(unsigned long addr,
					unsigned long num_adjpages);

static inline struct page_descriptor *
get_kmalloc_pages(unsigned long priority, unsigned long pages, int dma)
{
	return((struct page_descriptor *)
			__get_contiguous_pages(priority, pages, dma));
}

static inline void
free_kmalloc_pages(struct page_descriptor *page, int dma)
{
	int order = (page->order & ~PAGE_MASK);

	if (!dma && order < MAX_CACHE_ORDER) {
		page = xchg(kmalloc_cache + order, page);
		if (!page)
			return;
	}
	free_contiguous_pages((unsigned long) page, page->order >> PAGE_SHIFT);
}


long kmalloc_init(long start_mem, long end_mem)
{
	int order;

/*
 * Check the static info array. Things will blow up terribly if it's
 * incorrect. This is a late "compile time" check.....
 */
	for (order = 0; BLOCKSIZE(order); order++) {
		if (NBLOCKS(order) <= 1)
			continue; /* skip checking the catch all case */
		if ((NBLOCKS(order) * BLOCKSIZE(order) + sizeof(struct page_descriptor)) >
		    PAGE_SIZE) {
			printk("Cannot use %d bytes out of %d in order = %d block mallocs\n",
			       (int) (NBLOCKS(order) * BLOCKSIZE(order) +
				      sizeof(struct page_descriptor)),
			        (int) PAGE_SIZE,
			       BLOCKSIZE(order));
			panic("This only happens if someone messes with kmalloc");
		}
	}
	return start_mem;
}


/*
 * Ugh, this is ugly, but we want the default case to run
 * straight through, which is why we have the ugly goto's
 */
void *kmalloc(size_t size, int priority)
{
	unsigned long flags;
	unsigned long type;
	int order, dma;
	struct block_header *p;
	struct page_descriptor *page, **pg;
	struct size_descriptor *bucket = sizes;

	/* Get order */
	order = 0;
	{
		unsigned int realsize = size + sizeof(struct block_header);
		for (;;) {
			int ordersize = BLOCKSIZE(order);
			if (realsize <= ordersize)
				break;
			order++;
			bucket++;
			if (ordersize)
				continue;
			printk("kmalloc of too large a block (%d bytes).\n", (int) size);
			return NULL;
		}
	}

	dma = 0;
	type = MF_USED;
	pg = &bucket->firstfree;
	if (priority & GFP_DMA) {
		dma = 1;
		type = MF_DMA;
		pg = &bucket->dmafree;
	}

	priority &= GFP_LEVEL_MASK;

/* Sanity check... */
	if (intr_count && priority != GFP_ATOMIC) {
		static int count = 0;
		if (++count < 5) {
			printk("kmalloc called nonatomically from interrupt %p\n",
			       __builtin_return_address(0));
			priority = GFP_ATOMIC;
		}
	}

	save_flags(flags);
	cli();
	page = *pg;
	if (!page)
		goto no_bucket_page;

	p = page->firstfree;
	if (p->bh_flags != MF_FREE)
		goto not_free_on_freelist;

found_it:
	page->firstfree = p->bh_next;
	page->nfree--;
	if (!page->nfree)
		*pg = page->next;
	restore_flags(flags);
	bucket->nmallocs++;
	bucket->nbytesmalloced += size;
	p->bh_flags = type;	/* As of now this block is officially in use */
	p->bh_length = size;
#ifdef SADISTIC_KMALLOC
	memset(p+1, 0xf0, size);
#endif
	return p + 1;		/* Pointer arithmetic: increments past header */


no_bucket_page:
	/*
	 * If we didn't find a page already allocated for this
	 * bucket size, we need to get one..
	 *
	 * This can be done with ints on: it is private to this invocation
	 */
	restore_flags(flags);

	{
		int i, realpages;
		
		if (BLOCKSIZE(order) < PAGE_SIZE)
			realpages = 1;
		else
			realpages = NUM_PAGES(size + sizeof(struct block_header) +
					sizeof(struct page_descriptor));
			
		page = get_kmalloc_pages(priority, realpages, dma);
		if (!page)
			goto no_free_page;
found_cached_page:

		bucket->npages++; /* SLC - bucket->npages += realpages? */

		page->order = order | (realpages * PAGE_SIZE);
		/* Loop for all but last block: */
		i = (page->nfree = bucket->nblocks) - 1;
		p = BH(page + 1);
		while (i > 0) { /* doesn't happen except for small ^2 mallocs */
			i--;
			p->bh_flags = MF_FREE;
			p->bh_next = BH(((long) p) + BLOCKSIZE(order));
			p = p->bh_next;
		}
		/* Last block: */
		p->bh_flags = MF_FREE;
		p->bh_next = NULL;

		p = BH(page+1);
	}

	/*
	 * Now we're going to muck with the "global" freelist
	 * for this size: this should be uninterruptible
	 */
	cli();
	page->next = *pg;
	*pg = page;
	goto found_it;


no_free_page:
	/*
	 * No free pages, check the kmalloc cache of
	 * pages to see if maybe we have something available
	 */
	if (!dma && order < MAX_CACHE_ORDER) {
		page = xchg(kmalloc_cache+order, page);
		if (page)
			goto found_cached_page;
	}
	{
		static unsigned long last = 0;
		if (priority != GFP_BUFFER && priority != GFP_IO &&
		    (last + 10 * HZ < jiffies)) {
			last = jiffies;
			printk("Couldn't get a free page.....\n");
		}
		return NULL;
	}

not_free_on_freelist:
	restore_flags(flags);
	printk("Problem: block on freelist at %08lx isn't free.\n", (long) p);
	return NULL;
}

void kfree(void *__ptr)
{
	int dma;
	unsigned long flags;
	unsigned int order;
	struct page_descriptor *page, **pg;
	struct size_descriptor *bucket;

	if (!__ptr)
		goto null_kfree;
#define ptr ((struct block_header *) __ptr)
	page = PAGE_DESC(ptr);
	__ptr = ptr - 1;
	if (~PAGE_MASK & (unsigned long)page->next)
		goto bad_order;
	order = (page->order & ~PAGE_MASK);
	if (order >= sizeof(sizes) / sizeof(sizes[0]))
		goto bad_order;
	bucket = sizes + order;
	dma = 0;
	pg = &bucket->firstfree;
	if (ptr->bh_flags == MF_DMA) {
		dma = 1;
		ptr->bh_flags = MF_USED;
		pg = &bucket->dmafree;
	}
	if (ptr->bh_flags != MF_USED)
		goto bad_order;
	ptr->bh_flags = MF_FREE;	/* As of now this block is officially free */
#ifdef SADISTIC_KMALLOC
	memset(ptr+1, 0xe0, ptr->bh_length);
#endif
	save_flags(flags);
	cli();

	bucket->nfrees++;
	bucket->nbytesmalloced -= ptr->bh_length;

	ptr->bh_next = page->firstfree;
	page->firstfree = ptr;
	if (!page->nfree++) {
/* Page went from full to one free block: put it on the freelist. */
		if (bucket->nblocks == 1)
			goto free_page;
		page->next = *pg;
		*pg = page;
	}
/* If page is completely free, free it */
	if (page->nfree == bucket->nblocks) {
		for (;;) {
			struct page_descriptor *tmp = *pg;
			if (!tmp)
				goto not_on_freelist;
			if (tmp == page)
				break;
			pg = &tmp->next;
		}
		*pg = page->next;
free_page:
		bucket->npages--;
		free_kmalloc_pages(page, dma);
	}
	restore_flags(flags);
null_kfree:
	return;

bad_order:
	printk("kfree of non-kmalloced memory: %p, next= %p, order=%d\n",
	       ptr+1, page->next, page->order);
	return;

not_on_freelist:
	restore_flags(flags);
	printk("Ooops. page %p doesn't show on freelist.\n", page);
}

unsigned int ksize(void *__ptr)
{
	unsigned int order;
	struct page_descriptor *page;

	if (!ptr) return 0;
#define ptr ((struct block_header *) __ptr)
	page = PAGE_DESC(ptr);
	__ptr = ptr - 1;

	if (~PAGE_MASK & (unsigned long)page->next) return 0;

	order = (page->order & ~PAGE_MASK);
	if (order >= sizeof(sizes) / sizeof(sizes[0])) return 0;
/*
 *	This looks like we are cheating,  but we are actually just reporting
 *	what ksize would normally report,  the size of the allocated block
 *	that is available to the caller.
 */
	if (BLOCKSIZE(order) > PAGE_SIZE)
		return((page->order & PAGE_MASK) -
				(sizeof(struct block_header) + sizeof(struct page_descriptor)));

	return BLOCKSIZE(order);
}

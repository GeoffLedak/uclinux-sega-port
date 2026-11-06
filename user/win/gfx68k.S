/*
 * gfx68k.S -- the inner loops of the drawing, in 68000 assembly
 *
 * gfx.c clips and splits everything into bands that stay inside one row
 * of tiles, so in these routines the pixel rows of a tile are just
 * consecutive longs, and the next tile across is 32 bytes on. (The C
 * versions in gfx.c, used on the PC, show what each one does.)
 *
 * The C calling convention: arguments on the stack, d0-d1/a0-a1 free,
 * everything else saved. a5 (the data pointer of -msep-data) is never
 * touched.
 */

	.text
	.even

/*
 * void fb_fill(u32 *p, int nrows, int ntiles, u32 cv)
 * Set nrows longs (rows) in each of ntiles tiles, from p, to cv.
 */
	.globl	fb_fill
fb_fill:
	movem.l	d2-d7/a2-a3,-(sp)
	move.l	36(sp),a0
	move.l	40(sp),d0
	move.l	44(sp),d1
	move.l	48(sp),d2
	subq.l	#1,d1
	bmi	9f
	moveq	#8,d3
	cmp.l	d3,d0
	bne	2f
	/* whole tiles: 32 bytes each, 8 longs per movem */
	move.l	d2,d3
	move.l	d2,d4
	move.l	d2,d5
	move.l	d2,d6
	move.l	d2,d7
	movea.l	d2,a2
	movea.l	d2,a3
1:	movem.l	d2-d7/a2-a3,(a0)
	lea	32(a0),a0
	dbra	d1,1b
	bra	9f
	/* part of each tile: jump into a run of eight 2-byte moves */
2:	sub.l	d0,d3			/* d3 = moves to skip */
	move.l	d3,d4
	add.l	d4,d4
	add.l	d4,d4			/* d4 = bytes to step over after */
	add.l	d3,d3
	lea	3f(pc),a1
	adda.l	d3,a1
4:	jmp	(a1)
3:	move.l	d2,(a0)+
	move.l	d2,(a0)+
	move.l	d2,(a0)+
	move.l	d2,(a0)+
	move.l	d2,(a0)+
	move.l	d2,(a0)+
	move.l	d2,(a0)+
	move.l	d2,(a0)+
	adda.l	d4,a0
	dbra	d1,4b
9:	movem.l	(sp)+,d2-d7/a2-a3
	rts

/*
 * void fb_mask(u32 *p, int nrows, u32 m, u32 cv)
 * Set the pixels of mask m to cv in nrows consecutive longs.
 */
	.globl	fb_mask
fb_mask:
	movem.l	d2-d4,-(sp)
	move.l	16(sp),a0
	move.l	20(sp),d0
	move.l	24(sp),d1
	move.l	28(sp),d2
	and.l	d1,d2			/* d2 = cv & m */
	not.l	d1			/* d1 = ~m */
	subq.l	#1,d0
	bmi	9f
1:	move.l	(a0),d3
	and.l	d1,d3
	or.l	d2,d3
	move.l	d3,(a0)+
	dbra	d0,1b
9:	movem.l	(sp)+,d2-d4
	rts

/*
 * void fb_strip(u32 *p, const unsigned char *b, int n, u32 cv,
 *		 const u32 *expand)
 * One pixel row of a text strip: byte k of b (bit 7 the leftmost
 * pixel) gives the pixels set to cv in the long of tile k from p.
 */
	.globl	fb_strip
fb_strip:
	movem.l	d2-d3/a2,-(sp)
	move.l	16(sp),a0
	move.l	20(sp),a1
	move.l	24(sp),d3
	move.l	28(sp),d2
	move.l	32(sp),a2
	subq.l	#1,d3
	bmi	9f
1:	moveq	#0,d0
	move.b	(a1)+,d0
	beq	5f
	add.w	d0,d0
	add.w	d0,d0
	move.l	0(a2,d0.w),d0
	move.l	(a0),d1			/* *p ^= (*p ^ cv) & m */
	eor.l	d2,d1
	and.l	d0,d1
	eor.l	d1,(a0)
5:	lea	32(a0),a0
	dbra	d3,1b
9:	movem.l	(sp)+,d2-d3/a2
	rts

/*
 * void fb_blit_mv(u32 *p, const u32 *mv, int nrows, int sh,
 *		   u32 colmask, int stride)
 * Draw 8 pixels wide from mask/value pairs (an icon: mv[0] is the mask
 * of the pixels to draw, mv[1] their colours), one pair per row, stride
 * longs apart. colmask clips columns (before the shift); the 8 pixels
 * start sh/4 pixels into the tile at p and spill into the next tile.
 */
	.globl	fb_blit_mv
fb_blit_mv:
	movem.l	d2-d7/a2,-(sp)
	move.l	32(sp),a0
	move.l	36(sp),a1
	move.l	40(sp),d7
	move.l	44(sp),d5
	move.l	48(sp),d4
	move.l	52(sp),d0
	add.l	d0,d0
	add.l	d0,d0
	subq.l	#8,d0
	movea.l	d0,a2			/* bytes from one pair to the next */
	moveq	#-1,d3
	lsr.l	d5,d3
	subq.l	#1,d7
	bmi	9f
1:	move.l	(a1)+,d0		/* mask */
	move.l	(a1)+,d1		/* value */
	adda.l	a2,a1
	and.l	d4,d0
	beq	5f
	ror.l	d5,d0
	ror.l	d5,d1
	move.l	d0,d6
	and.l	d3,d0
	eor.l	d0,d6
	move.l	(a0),d2
	eor.l	d1,d2
	and.l	d0,d2
	eor.l	d2,(a0)
	tst.l	d6
	beq	5f
	move.l	32(a0),d2
	eor.l	d1,d2
	and.l	d6,d2
	eor.l	d2,32(a0)
5:	addq.l	#4,a0
	dbra	d7,1b
9:	movem.l	(sp)+,d2-d7/a2
	rts

/*
 * void fb_sync(const u32 *fb, u32 *shadow, u32 *dirty, int nwords)
 * For each tile marked in dirty[] (nwords longs of bits, bit 0 of
 * dirty[0] is tile 0): if it is the same as in shadow (what the screen
 * already shows), unmark it; if not, copy it to shadow. So only tiles
 * that really changed get sent to the video memory.
 */
	.globl	fb_sync
fb_sync:
	movem.l	d2-d7/a2-a4,-(sp)
	move.l	40(sp),a0
	move.l	44(sp),a1
	move.l	48(sp),a2
	move.l	52(sp),d7
	subq.l	#1,d7
	bmi	9f
1:	move.l	a0,d0			/* this word's 32 tiles */
	move.l	a1,d1
	move.l	(a2),d6
	beq	7f
	moveq	#0,d5			/* the tiles that changed */
	moveq	#1,d4
2:	lsr.l	#1,d6
	bcc	6f
	movea.l	a0,a3
	movea.l	a1,a4
	moveq	#7,d3
3:	cmpm.l	(a3)+,(a4)+
	dbne	d3,3b
	beq	6f			/* all 8 rows the same */
	or.l	d4,d5
	movea.l	a0,a3
	movea.l	a1,a4
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
	move.l	(a3)+,(a4)+
6:	lea	32(a0),a0
	lea	32(a1),a1
	add.l	d4,d4
	tst.l	d6
	bne	2b
	move.l	d5,(a2)
7:	addq.l	#4,a2
	movea.l	d0,a0
	movea.l	d1,a1
	lea	1024(a0),a0
	lea	1024(a1),a1
	dbra	d7,1b
9:	movem.l	(sp)+,d2-d7/a2-a4
	rts

/*
 * void fb_copy(u32 *dst, const u32 *src, int n, u32 m0, u32 m1, int sstride)
 * Copy n longs to dst, dst + 32 bytes ... from src, src + sstride ...;
 * only the pixels of m0 in the first and of m1 in the last (n > 1).
 */
	.globl	fb_copy
fb_copy:
	movem.l	d2-d5,-(sp)
	move.l	20(sp),a0
	move.l	24(sp),a1
	move.l	28(sp),d3
	move.l	32(sp),d4
	move.l	36(sp),d5
	move.l	40(sp),d2		/* source step */
	subq.l	#1,d3
	bmi	9f
	move.l	(a0),d0			/* the first: masked */
	move.l	(a1),d1
	eor.l	d0,d1
	and.l	d4,d1
	eor.l	d1,d0
	move.l	d0,(a0)
	lea	32(a0),a0
	adda.l	d2,a1
	subq.l	#1,d3
	bmi	9f
	subq.l	#1,d3			/* the middle ones */
	bmi	2f
	moveq	#32,d0
	cmp.l	d0,d2
	bne	3f
	addq.l	#1,d3			/* d3: how many */
	moveq	#7,d0
	and.l	d3,d0			/* d0: the odd ones */
	lsr.l	#3,d3			/* d3: blocks of 8 */
	bra	5f
4:	move.l	(a1),(a0)		/* 8 at a time */
	move.l	32(a1),32(a0)
	move.l	64(a1),64(a0)
	move.l	96(a1),96(a0)
	move.l	128(a1),128(a0)
	move.l	160(a1),160(a0)
	move.l	192(a1),192(a0)
	move.l	224(a1),224(a0)
	lea	256(a0),a0
	lea	256(a1),a1
5:	dbra	d3,4b
	bra	7f
6:	move.l	(a1),(a0)
	lea	32(a0),a0
	lea	32(a1),a1
7:	dbra	d0,6b
	bra	2f
3:	move.l	(a1)+,(a0)		/* (from the row buffer) */
	lea	32(a0),a0
	dbra	d3,3b
2:	move.l	(a0),d0			/* the last: masked */
	move.l	(a1),d1
	eor.l	d0,d1
	and.l	d5,d1
	eor.l	d1,d0
	move.l	d0,(a0)
9:	movem.l	(sp)+,d2-d5
	rts

/*
 * void fb_gather(u32 *buf, const u32 *src, int n, int sh)
 * n longs of pixels that start sh/4 pixels into the tile at src, into
 * buf, one after another: buf[k] = src[8k] << sh | src[8k + 8] >> (32 - sh).
 * sh is 4 - 28.
 */
	.globl	fb_gather
fb_gather:
	movem.l	d2-d5,-(sp)
	move.l	20(sp),a0
	move.l	24(sp),a1
	move.l	28(sp),d3
	move.l	32(sp),d2
	moveq	#32,d4
	sub.l	d2,d4			/* 32 - sh */
	subq.l	#1,d3
	bmi	9f
	move.l	(a1),d0
1:	lea	32(a1),a1
	move.l	(a1),d1
	move.l	d1,d5			/* the next tile's long, for next time */
	lsl.l	d2,d0
	lsr.l	d4,d1
	or.l	d1,d0
	move.l	d0,(a0)+
	move.l	d5,d0
	dbra	d3,1b
9:	movem.l	(sp)+,d2-d5
	rts

/*
 * void fb_shift(u32 *dst, const u32 *src, int n, int sh, u32 m0, u32 m1,
 *		 u32 first)
 * Like fb_copy, but the pixels come from sh/4 pixels further on: each
 * long written is (a << sh) | (b >> (32 - sh)), a the long before b,
 * starting with a = first and b = *src (then src + 32 bytes, ...).
 * sh is 4 - 28. Every long is rotated once, by sh (or the other way,
 * by 32 - sh, which is quicker when sh > 16), and split with a mask.
 */
	.macro	SHSTEP rot
	move.l	(a1),d1			/* b */
	lea	32(a1),a1
	\rot	d2,d1
	move.l	d1,d3
	and.l	d6,d3			/* its left pixels, at the right */
	and.l	d5,d0			/* a's right pixels, at the left */
	or.l	d0,d3			/* d3: the long to write */
	move.l	d1,d0			/* b is the next a */
	.endm

	.macro	SHBODY rot
	SHSTEP	\rot
	move.l	(a0),d1			/* the first: masked with m0 */
	eor.l	d3,d1
	and.l	d4,d1
	eor.l	d1,(a0)
	lea	32(a0),a0
	subq.l	#1,d7
	bmi	9f
	subq.l	#1,d7
	bmi	2f
1:	SHSTEP	\rot			/* the middle */
	move.l	d3,(a0)
	lea	32(a0),a0
	dbra	d7,1b
2:	SHSTEP	\rot			/* the last: masked with m1 */
	move.l	a2,d4
	move.l	(a0),d1
	eor.l	d3,d1
	and.l	d4,d1
	eor.l	d1,(a0)
9:
	.endm

	.globl	fb_shift
fb_shift:
	movem.l	d2-d7/a2,-(sp)
	move.l	32(sp),a0
	move.l	36(sp),a1
	move.l	40(sp),d7
	move.l	44(sp),d2
	move.l	48(sp),d4
	move.l	52(sp),a2
	move.l	56(sp),d0
	moveq	#1,d6
	lsl.l	d2,d6
	subq.l	#1,d6			/* d6: the low sh bits */
	move.l	d6,d5
	not.l	d5
	subq.l	#1,d7
	bmi	8f
	moveq	#16,d1
	cmp.l	d1,d2
	bgt	5f
	rol.l	d2,d0
	SHBODY	rol.l
	bra	8f
5:	neg.l	d2
	add.l	#32,d2			/* rotating right by 32 - sh is the same */
	ror.l	d2,d0
	SHBODY	ror.l
8:	movem.l	(sp)+,d2-d7/a2
	rts

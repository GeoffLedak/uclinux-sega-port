/* SEGA MegaDrive support code */
/* by Chilly Willy */
/* Adapted for uClinux by conversion from Sega-Genesis-Keyboard-Test */

        .text
        .align  2


/* Define Constants */
        .equ VDP_DATA,                  0xC00000
        .equ VDP_CONTROL,               0xC00004
        .equ VDP_WRITE_SPRITE_TABLE,    0x60000003

/* Text console layout (software rendered, 8x8 or 6x8 font)                */
/*                                                                          */
/* The screen shows 24 text rows (plane B rows 2-25) with 16 pixels of      */
/* padding all round. Each text row is drawn into a "block" of 36 tiles;    */
/* the name table row for screen row r points at block rowBlock[r]          */
/* (text_map_row). Scrolling just rotates which block each screen row uses  */
/* and rewrites the name table, so no pixels are copied.                    */
/* text_charmap holds the characters of each block, and characters are      */
/* drawn into the tiles in software (text_refresh):                         */
/*   8x8 font (font_cga.c):       36 columns, one character = one tile.     */
/*   6x8 font (font_cp437_6x8.c): 48 columns. The VDP can only place whole  */
/*     8x8 tiles, so a character overlaps 1 or 2 tiles, which are rebuilt   */
/*     from text_charmap. 4 characters fit exactly in 3 tiles (24 pixels).  */
/* text_attrmap has one byte per character too: nonzero = reverse video,   */
/* drawn by swapping the text colour (1) and the background (0).            */
/* The terminal logic (cursor, escape sequences, scrolling) is in           */
/* genesis_term.c; the constants marked (*) must match it.                  */
        .equ TEXT_COLS_8X8,             36              /* (320 - 2*16) / 8 */
        .equ TEXT_COLS_6X8,             48              /* (320 - 2*16) / 6 */
        .equ TEXT_COLS,                 48              /* (*) text_charmap row length (widest mode) */
        .equ TEXT_ROWS,                 24              /* (*) text rows = tile blocks */
        .equ TEXT_TILE_COLS,            36              /* text tiles per row (288 px) */
        .equ TEXT_FIRST_PLANE_ROW,      2               /* plane B row of text row 0 (16 px) */
        .equ TEXT_FIRST_PLANE_COL,      2               /* plane B column of the first text tile (16 px) */
        .equ TEXT_FIRST_TILE,           1               /* tile 0 stays blank */
        .equ CURSOR_TILES,              TEXT_FIRST_TILE + TEXT_TILE_COLS * TEXT_ROWS   /* (*) 865 */
                                        /* +0 block 8px, +1 block 6px, +2 underline 8px, */
                                        /* +3 underline 6px, +4 bar                       */

/* Boot text colour, Genesis CRAM format 0000 BBB0 GGG0 RRR0 (each 0x0 - 0xE) */
/* 0x0AAA = light gray, 0x0EEE = white, 0x00C0 = dim green.                   */
/* The boot font and colour are set in genesis_term.c (TERM_DEFAULT_FONT /   */
/* TERM_DEFAULT_COLOR); keep TEXT_COLOR the same as TERM_DEFAULT_COLOR.      */
        .equ TEXT_COLOR,                0x0EEE




/* Initialize the hardware & load font tiles */
        .global init_hardware
init_hardware:
        movem.l d2-d7/a2-a6,-(sp)

/* init joyports */
        lea     0xA10000,a5
        move.b  #0x40,0x09(a5)
        move.b  #0x40,0x0B(a5)
        move.b  #0x40,0x03(a5)
        move.b  #0x40,0x05(a5)

        lea     0xC00000,a3             /* VDP data reg */
        lea     0xC00004,a4             /* VDP cmd/sts reg */

/* wait on VDP DMA (in case we reset in the middle of DMA) */
        move.w  #0x8114,(a4)            /* display off, dma enabled */
0:
        move.w  (a4),d0                 /* read VDP status */
        btst    #1,d0                   /* DMA busy? */
        bne.b   0b                      /* yes */

        moveq   #0,d0
        move.w  #0x8000,d5              /* set VDP register 0 */
        move.w  #0x0100,d7

/* Set VDP registers */
        lea     InitVDPRegs(pc),a5
        moveq   #18,d1
1:
        move.b  (a5)+,d5                /* lower byte = register data */
        move.w  d5,(a4)                 /* set VDP register */
        add.w   d7,d5                   /* + 0x0100 = next register */
        dbra    d1,1b

/* clear VRAM */
        move.w  #0x8F02,(a4)            /* set INC to 2 */
        move.l  #0x40000000,(a4)        /* write VRAM address 0 */
        move.w  #0x7FFF,d1              /* 32K - 1 words */
2:
        move.w  d0,(a3)                 /* clear VRAM */
        dbra    d1,2b

/* The VDP state at this point is: Display disabled, ints disabled, Name Tbl A at 0xC000, */
/* Name Tbl B at 0xE000, Name Tbl W at 0xB000, Sprite Attr Tbl at 0xA800, HScroll Tbl at 0xAC00, */
/* H40 V28 mode, and Scroll size is 64x32. */

/* Clear CRAM */
        lea     InitVDPRAM(pc),a5
        move.l  (a5)+,(a4)              /* set reg 1 and reg 15 */
        move.l  (a5)+,(a4)              /* write CRAM address 0 */
        moveq   #31,d3
3:
        move.l  d0,(a3)
        dbra    d3,3b

/* Clear VSRAM */
        move.l  (a5)+,(a4)              /* write VSRAM address 0 */
        moveq   #19,d4
4:
        move.l  d0,(a3)
        dbra    d4,4b

/* halt Z80 and init FM chip */
        /* Allow the 68k to access the FM chip */
        move.w  #0x100,0xA11100
        move.w  #0x100,0xA11200

/* reset YM2612 */
        lea     FMReset(pc),a5
        lea     0xA00000,a0
        move.w  #0x4000,d1
        moveq   #26,d2
5:
        move.b  (a5)+,d1                /* FM reg */
        move.b  (a5)+,0(a0,d1.w)        /* FM data */
        nop
        nop
        dbra    d2,5b

        moveq   #0x30,d0
        moveq   #0x5F,d2
6:
        move.b  d0,0x4000(a0)           /* FM reg */
        nop
        nop
        move.b  #0xFF,0x4001(a0)        /* FM data */
        nop
        nop
        move.b  d0,0x4002(a0)           /* FM reg */
        nop
        nop
        move.b  #0xFF,0x4003(a0)        /* FM data */
        nop
        nop
        addq.b  #1,d0
        dbra    d2,6b

/* reset PSG */
        lea     PSGReset(pc),a5
        lea     0xC00000,a0
        move.b  (a5)+,0x0011(a0)
        move.b  (a5)+,0x0011(a0)
        move.b  (a5)+,0x0011(a0)
        move.b  (a5),0x0011(a0)

/* set up plane B: text row r (plane row r+2) shows block r, i.e. tiles     */
/* TEXT_FIRST_TILE + r*TEXT_TILE_COLS + t; everything else is blank. The     */
/* tiles themselves are already blank because VRAM was cleared.             */
        move.w  #0x8F02,(a4)            /* INC = 2 */
        move.l  #0x60000003,(a4)        /* write VRAM 0xE000 (plane B name table) */
        moveq   #0,d0                   /* blank name table entry */
        move.w  #TEXT_FIRST_PLANE_ROW*64-1,d3
7:
        move.w  d0,(a3)                 /* blank rows above the text */
        dbra    d3,7b
        moveq   #TEXT_FIRST_TILE,d1     /* next text tile number */
        moveq   #TEXT_ROWS-1,d2
8:
        moveq   #TEXT_FIRST_PLANE_COL-1,d3
9:
        move.w  d0,(a3)                 /* left padding: blank */
        dbra    d3,9b
        moveq   #TEXT_TILE_COLS-1,d3
10:
        move.w  d1,(a3)                 /* text tile, palette 0 */
        addq.w  #1,d1
        dbra    d3,10b
        moveq   #64-TEXT_FIRST_PLANE_COL-TEXT_TILE_COLS-1,d3
11:
        move.w  d0,(a3)                 /* right padding and off-screen columns: blank */
        dbra    d3,11b
        dbra    d2,8b
        move.w  #(32-TEXT_FIRST_PLANE_ROW-TEXT_ROWS)*64-1,d3
12:
        move.w  d0,(a3)                 /* blank rows below the text */
        dbra    d3,12b

/* cursor tiles (consecutive): block 8px, block 6px, underline 8px, underline 6px, bar */
        move.l  #CURSOR_TILES*32,d0
        bsr     vram_write_cmd
        move.l  d0,(a4)
        lea     CursorTileRows(pc),a5
        moveq   #5-1,d2
13:
        moveq   #8-1,d3
        move.l  (a5)+,d0                /* pixel row for rows 0-5 */
        move.l  (a5)+,d1                /* pixel row for rows 6-7 */
14:
        cmpi.w  #1,d3
        bhi.s   15f
        move.l  d1,(a3)
        bra.s   16f
15:
        move.l  d0,(a3)
16:
        dbra    d3,14b
        dbra    d2,13b

/* set the default palette for text */
        move.l  #0xC0000000,(a4)        /* write CRAM address 0 */
        move.l  #TEXT_COLOR,(a3)        /* entry 0 (black) and 1 (text and cursor colour) */
        move.l  #0xC0200000,(a4)        /* write CRAM address 32 */
        move.l  #0x000000A0,(a3)        /* entry 16 (black) BGR and 17 (green) */
        move.l  #0xC0400000,(a4)        /* write CRAM address 64 */
        move.l  #0x0000000A,(a3)        /* entry 32 (black) BGR and 33 (red) */

        move.w  #0x8174,(a4)            /* display on, vblank ENABLED */
        move    #0x2000,sr              /* enable CPU interrupts */

        movem.l (sp)+,d2-d7/a2-a6
        rts

/* cursor tile pixel rows: (rows 0-5, rows 6-7) for each cursor tile */
CursorTileRows:
        .long   0x11111111, 0x11111111  /* block, 8 px */
        .long   0x11111100, 0x11111100  /* block, 6 px */
        .long   0x00000000, 0x11111111  /* underline, 8 px */
        .long   0x00000000, 0x11111100  /* underline, 6 px */
        .long   0x11000000, 0x11000000  /* bar */

/* VDP register initialization values */
InitVDPRegs:
        .byte   0x04    /* 8004 => write reg 0 = /IE1 (no HBL INT), /M3 (enable read H/V cnt) */
        .byte   0x14    /* 8114 => write reg 1 = /DISP (display off), /IE0 (no VBL INT), M1 (DMA enabled), /M2 (V28 mode) */
        .byte   0x30    /* 8230 => write reg 2 = Name Tbl A = 0xC000 */
        .byte   0x2C    /* 832C => write reg 3 = Name Tbl W = 0xB000 */
        .byte   0x07    /* 8407 => write reg 4 = Name Tbl B = 0xE000 */
        .byte   0x54    /* 8554 => write reg 5 = Sprite Attr Tbl = 0xA800 */
        .byte   0x00    /* 8600 => write reg 6 = always 0 */
        .byte   0x00    /* 8700 => write reg 7 = BG color */
        .byte   0x00    /* 8800 => write reg 8 = always 0 */
        .byte   0x00    /* 8900 => write reg 9 = always 0 */
        .byte   0x00    /* 8A00 => write reg 10 = HINT = 0 */
        .byte   0x00    /* 8B00 => write reg 11 = /IE2 (no EXT INT), full scroll */
        .byte   0x81    /* 8C81 => write reg 12 = H40 mode, no lace, no shadow/hilite */
        .byte   0x2B    /* 8D2B => write reg 13 = HScroll Tbl = 0xAC00 */
        .byte   0x00    /* 8E00 => write reg 14 = always 0 */
        .byte   0x01    /* 8F01 => write reg 15 = data INC = 1 */
        .byte   0x01    /* 9001 => write reg 16 = Scroll Size = 64x32 */
        .byte   0x00    /* 9100 => write reg 17 = W Pos H = left */
        .byte   0x00    /* 9200 => write reg 18 = W Pos V = top */

        .align  2

/* VDP Commands */
InitVDPRAM:
        .word   0x8104, 0x8F01  /* set registers 1 (display off) and 15 (INC = 1) */
        .word   0xC000, 0x0000  /* write CRAM address 0 */
        .word   0x4000, 0x0010  /* write VSRAM address 0 */

FMReset:
        /* disable LFO */
        .byte   0,0x22
        .byte   1,0x00
        /* disable timer & set channel 6 to normal mode */
        .byte   0,0x27
        .byte   1,0x00
        /* all KEY_OFF */
        .byte   0,0x28
        .byte   1,0x00
        .byte   1,0x04
        .byte   1,0x01
        .byte   1,0x05
        .byte   1,0x02
        .byte   1,0x06
        /* disable DAC */
        .byte   0,0x2A
        .byte   1,0x80
        .byte   0,0x2B
        .byte   1,0x00
        /* turn off channels */
        .byte   0,0xB4
        .byte   1,0x00
        .byte   0,0xB5
        .byte   1,0x00
        .byte   0,0xB6
        .byte   1,0x00
        .byte   2,0xB4
        .byte   3,0x00
        .byte   2,0xB5
        .byte   3,0x00
        .byte   2,0xB6
        .byte   3,0x00

/* PSG register initialization values */
PSGReset:
        .byte   0x9f    /* set ch0 attenuation to max */
        .byte   0xbf    /* set ch1 attenuation to max */
        .byte   0xdf    /* set ch2 attenuation to max */
        .byte   0xff    /* set ch3 attenuation to max */

        .align  2

/* short set_sr(short new_sr); */
/* set SR, return previous SR */
/* entry: arg = SR value */
/* exit:  d0 = previous SR value */
        .global set_sr
set_sr:
        moveq   #0,d0
        move.w  sr,d0
        move.l  4(sp),d1
        move.w  d1,sr
        rts

/* short get_pad(short pad); */
/* return buttons for selected pad */
/* entry: arg = pad index (0 or 1) */
/* exit:  d0 = pad value (0 0 0 1 M X Y Z S A C B R L D U) or (0 0 0 0 0 0 0 0 S A C B R L D U) */
        .global get_pad
get_pad:
        move.l  d2,-(sp)
        move.l  8(sp),d0        /* first arg is pad number */
        cmpi.w  #1,d0
        bhi     no_pad
        add.w   d0,d0
        addi.l  #0xA10003,d0    /* pad control register */
        movea.l d0,a0
        bsr.b   get_input       /* - 0 s a 0 0 d u - 1 c b r l d u */
        move.w  d0,d1
        andi.w  #0x0C00,d0
        bne.b   no_pad
        bsr.b   get_input       /* - 0 s a 0 0 d u - 1 c b r l d u */
        bsr.b   get_input       /* - 0 s a 0 0 0 0 - 1 c b m x y z */
        move.w  d0,d2
        bsr.b   get_input       /* - 0 s a 1 1 1 1 - 1 c b r l d u */
        andi.w  #0x0F00,d0      /* 0 0 0 0 1 1 1 1 0 0 0 0 0 0 0 0 */
        cmpi.w  #0x0F00,d0
        beq.b   common          /* six button pad */
        move.w  #0x010F,d2      /* three button pad */
common:
        lsl.b   #4,d2           /* - 0 s a 0 0 0 0 m x y z 0 0 0 0 */
        lsl.w   #4,d2           /* 0 0 0 0 m x y z 0 0 0 0 0 0 0 0 */
        andi.w  #0x303F,d1      /* 0 0 s a 0 0 0 0 0 0 c b r l d u */
        move.b  d1,d2           /* 0 0 0 0 m x y z 0 0 c b r l d u */
        lsr.w   #6,d1           /* 0 0 0 0 0 0 0 0 s a 0 0 0 0 0 0 */
        or.w    d1,d2           /* 0 0 0 0 m x y z s a c b r l d u */
        eori.w  #0x1FFF,d2      /* 0 0 0 1 M X Y Z S A C B R L D U */
        move.w  d2,d0
        move.l  (sp)+,d2
        rts

/* 3-button/6-button pad not found */
no_pad:
        move.w  #0xF000,d0      /* SEGA_CTRL_NONE */
        move.l  (sp)+,d2
        rts

/* read single phase from controller */
get_input:
        move.b  #0x00,(a0)
        nop
        nop
        move.b  (a0),d0
        move.b  #0x40,(a0)
        lsl.w   #8,d0
        move.b  (a0),d0
        rts

/* ============================================================ */
/* Software text renderer                                        */
/* Internal routines expect a1 = 0xC00000 (VDP data), INC = 2.   */
/* "block" = one text row's worth of tiles and text_charmap.     */
/* ============================================================ */

/* vram_write_cmd */
/* entry: d0.l = VRAM byte address (0 - 0xFFFF) */
/* exit:  d0.l = VDP "write VRAM" command for that address */
/* trashes d1 */
vram_write_cmd:
        move.l  d0,d1
        andi.l  #0x3FFF,d0
        swap    d0                      /* (addr & 0x3FFF) << 16 */
        ori.l   #0x40000000,d0          /* VRAM write */
        rol.w   #2,d1                   /* addr bits 15-14 -> bits 1-0 */
        andi.w  #3,d1
        or.w    d1,d0
        rts

/* tile_write_cmd */
/* entry: d2.w = block, d3.w = tile column -> sets the VDP to write that tile */
/* trashes d0-d1 */
tile_write_cmd:
        move.w  d2,d0
        mulu.w  #TEXT_TILE_COLS,d0
        add.w   d3,d0
        addq.w  #TEXT_FIRST_TILE,d0     /* tile number */
        lsl.l   #5,d0                   /* * 32 bytes per tile */
        bsr     vram_write_cmd
        move.l  d0,4(a1)
        rts

/* text_render_tile8 */
/* 8x8 font: draw the character in column d3 of block d2 (one tile). */
/* trashes d0-d1, d4-d6, a2 */
text_render_tile8:
        bsr     tile_write_cmd
        move.w  d2,d0
        mulu.w  #TEXT_COLS,d0
        add.w   d3,d0
        moveq   #0,d6                   /* d6 = pixels to flip: none, */
        lea     text_attrmap,a2
        tst.b   0(a2,d0.w)
        beq.s   0f
        move.l  #0x11111111,d6          /* or all of them (reverse video) */
0:
        lea     text_charmap,a2
        moveq   #0,d4
        move.b  0(a2,d0.w),d4           /* character */
        moveq   #7,d5
        tst.b   d4
        beq.s   2f
        cmpi.b  #0x20,d4
        beq.s   2f
        lea     font_data,a2
        lsl.w   #5,d4                   /* 8 longs (32 bytes) per glyph */
        adda.w  d4,a2
        move.l  #0x11111111,d1          /* glyph rows are 0xF per lit pixel; use colour 1 */
1:
        move.l  (a2)+,d0
        and.l   d1,d0
        eor.l   d6,d0
        move.l  d0,(a1)                 /* write tile row */
        dbra    d5,1b
        rts
2:
        move.l  d6,d0                   /* blank character: clear (or fill) the tile */
3:
        move.l  d0,(a1)
        dbra    d5,3b
        rts

/* text_render_tile6 */
/* 6x8 font: rebuild one tile from the two characters that overlap it. */
/* entry: d2.w = block, d3.w = tile column (0 - 35) */
/* trashes d0-d1, d4-d7, a0, a2-a5 (d2, d3 preserved) */
/*                                                             */
/* The first character in tile t is k = 4t/3 and it starts      */
/* s = -2 * (4t mod 3) pixels from the tile's left edge         */
/* (0, -2 or -4); character k+1 starts at s+6. A glyph row      */
/* expanded by font_6x8_expand has its leftmost pixel in bits   */
/* 23-20, so it is shifted left by 8 - 4s for character k and   */
/* right by 16 + 4s for character k+1.                          */
text_render_tile6:
        bsr     tile_write_cmd

        moveq   #0,d4
        move.w  d3,d4
        lsl.w   #2,d4                   /* 4t */
        divu.w  #3,d4                   /* low word = k, high word = 4t mod 3 */
        move.l  d4,d6
        swap    d6
        lsl.w   #3,d6                   /* 8 * (4t mod 3) */
        moveq   #16,d7
        sub.w   d6,d7                   /* d7 = right shift for character k+1 */
        addq.w  #8,d6                   /* d6 = left shift for character k */

        lea     text_charmap,a2
        move.w  d2,d0
        mulu.w  #TEXT_COLS,d0
        add.w   d4,d0
        adda.w  d0,a2                   /* a2 = &text_charmap[block][k] */
        lea     text_attrmap,a0
        adda.w  d0,a0                   /* a0 = &text_attrmap[block][k] */

        moveq   #0,d0
        move.b  (a2)+,d0                /* character k */
        moveq   #0,d1
        move.b  (a2),d1                 /* character k+1 */
        moveq   #7,d5                   /* 8 pixel rows */

        /* both characters blank (0 or space) and not reversed: clear the tile */
        move.b  (a0),d4
        or.b    1(a0),d4
        bne.s   2f
        move.b  d0,d4
        andi.b  #0xDF,d4                /* 0x20 -> 0x00 */
        bne.s   2f
        move.b  d1,d4
        andi.b  #0xDF,d4
        bne.s   2f
        moveq   #0,d0
3:
        move.l  d0,(a1)
        dbra    d5,3b
        rts
2:
        lea     font_6x8,a3
        lsl.w   #3,d0
        adda.w  d0,a3                   /* a3 = glyph of character k */
        lea     font_6x8,a4
        lsl.w   #3,d1
        adda.w  d1,a4                   /* a4 = glyph of character k+1 */

        /* d4 = pixels to flip for reverse video, shifted like the glyphs */
        moveq   #0,d4
        tst.b   (a0)+
        beq.s   4f
        move.l  #0x00111111,d4          /* character k */
        lsl.l   d6,d4
4:
        tst.b   (a0)
        beq.s   5f
        move.l  #0x00111111,d0          /* character k+1 */
        lsr.l   d7,d0
        or.l    d0,d4
5:
        lea     font_6x8_expand,a5
1:
        moveq   #0,d0
        move.b  (a3)+,d0
        add.w   d0,d0
        add.w   d0,d0
        move.l  0(a5,d0.w),d0           /* character k row as 4bpp pixels */
        lsl.l   d6,d0
        moveq   #0,d1
        move.b  (a4)+,d1
        add.w   d1,d1
        add.w   d1,d1
        move.l  0(a5,d1.w),d1           /* character k+1 row as 4bpp pixels */
        lsr.l   d7,d1
        or.l    d1,d0
        eor.l   d4,d0
        move.l  d0,(a1)                 /* write tile row */
        dbra    d5,1b
        rts

/* text_refresh_internal */
/* Redraw columns [d3, d4) of block d2 from text_charmap in the current font. */
/* trashes d0-d1, d3-d7, a0, a2-a5 */
text_refresh_internal:
        cmp.w   d3,d4
        bls.s   9f                      /* empty range */
        cmpi.l  #6,fontWidth
        beq.s   5f

        /* 8x8 font: one tile per column */
        move.w  d4,-(sp)
1:
        bsr     text_render_tile8
        addq.w  #1,d3
        cmp.w   (sp),d3
        bcs.s   1b
        addq.l  #2,sp
        rts

        /* 6x8 font: tiles (from*6)/8 .. (to*6-1)/8 */
5:
        mulu.w  #6,d3
        lsr.w   #3,d3
        mulu.w  #6,d4
        subq.w  #1,d4
        lsr.w   #3,d4
        move.w  d4,-(sp)
6:
        bsr     text_render_tile6
        addq.w  #1,d3
        cmp.w   (sp),d3
        bls.s   6b
        addq.l  #2,sp
9:
        rts

/* ============================================================ */
/* C-callable text primitives (used by genesis_term.c)           */
/* Standard C calling convention: args on the stack, d0-d1/a0-a1 */
/* may be trashed.                                               */
/* ============================================================ */

/* void text_refresh(int block, int from, int to); */
/* Redraw columns [from, to) of a block from text_charmap. */
        .global text_refresh
text_refresh:
        movem.l d2-d7/a2-a5,-(sp)       /* 40 bytes */
        move.l  44(sp),d2               /* block */
        move.l  48(sp),d3               /* from */
        move.l  52(sp),d4               /* to */
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        bsr     text_refresh_internal
        movem.l (sp)+,d2-d7/a2-a5
        rts

/* void text_zero_block(int block); */
/* Blank a block: clear its text_charmap and text_attrmap rows and all of its tiles. */
        .global text_zero_block
text_zero_block:
        move.l  4(sp),d0                /* block */
        mulu.w  #TEXT_COLS,d0
        lea     text_charmap,a0
        adda.w  d0,a0
        lea     text_attrmap,a1
        adda.w  d0,a1
        moveq   #TEXT_COLS/4-1,d1
        moveq   #0,d0
1:
        move.l  d0,(a0)+                /* TEXT_COLS is a multiple of 4 and rows are long aligned */
        move.l  d0,(a1)+
        dbra    d1,1b

        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        move.l  4(sp),d0
        mulu.w  #TEXT_TILE_COLS,d0
        addq.w  #TEXT_FIRST_TILE,d0
        lsl.l   #5,d0
        bsr     vram_write_cmd
        move.l  d0,4(a1)                /* write VRAM at the block's first tile */
        moveq   #0,d0
        move.w  #TEXT_TILE_COLS*8-1,d1  /* the block's tiles are contiguous */
2:
        move.l  d0,(a1)
        dbra    d1,2b
        rts

/* void text_map_row(int row, int block); */
/* Point screen text row `row` (0 - 23) at the tiles of `block`. */
        .global text_map_row
text_map_row:
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        move.l  4(sp),d0                /* row */
        addq.w  #TEXT_FIRST_PLANE_ROW,d0
        lsl.w   #6,d0                   /* 64 cells per plane row */
        addq.w  #TEXT_FIRST_PLANE_COL,d0
        add.w   d0,d0                   /* 2 bytes per cell */
        andi.l  #0xFFFF,d0
        addi.l  #0xE000,d0              /* plane B name table */
        bsr     vram_write_cmd
        move.l  d0,4(a1)
        move.l  8(sp),d1                /* block */
        mulu.w  #TEXT_TILE_COLS,d1
        addq.w  #TEXT_FIRST_TILE,d1     /* first tile of the block */
        moveq   #TEXT_TILE_COLS-1,d0
1:
        move.w  d1,(a1)
        addq.w  #1,d1
        dbra    d0,1b
        rts

/* void put_str(char *str, int color, int x, int y); */
/* Draw a string directly (used by crt0 before the kernel runs). No wrapping, */
/* no cursor; x = column, y = text row (block). The color arg is ignored.    */
        .global put_str
put_str:
        movem.l d2-d7/a2-a5,-(sp)       /* 40 bytes */
        movea.l 44(sp),a0               /* string pointer */
        move.l  52(sp),d3               /* x (column) */
        move.l  56(sp),d2               /* y (row) */
        cmpi.l  #TEXT_ROWS,d2
        bcc.s   9f
        lea     text_charmap,a2
        move.w  d2,d0
        mulu.w  #TEXT_COLS,d0
        add.w   d3,d0
        adda.w  d0,a2
        move.w  d3,d4
1:
        cmp.w   textCols+2,d4
        bcc.s   2f
        move.b  (a0)+,d0
        beq.s   2f
        move.b  d0,(a2)+
        addq.w  #1,d4
        bra.s   1b
2:
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        bsr     text_refresh_internal   /* columns [x, end) */
9:
        movem.l (sp)+,d2-d7/a2-a5
        rts

/* void put_chr(char chr, int color, int x, int y); */
        .global put_chr
put_chr:
        movem.l d2-d7/a2-a5,-(sp)       /* 40 bytes */
        move.l  52(sp),d3               /* x (column) */
        move.l  56(sp),d2               /* y (row) */
        cmpi.l  #TEXT_ROWS,d2
        bcc.s   9f
        cmp.l   textCols,d3
        bcc.s   9f
        lea     text_charmap,a2
        move.w  d2,d0
        mulu.w  #TEXT_COLS,d0
        add.w   d3,d0
        move.b  47(sp),0(a2,d0.w)       /* character (low byte of the first arg) */
        move.w  d3,d4
        addq.w  #1,d4
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        bsr     text_refresh_internal
9:
        movem.l (sp)+,d2-d7/a2-a5
        rts

/* void delay(int count); */
/* wait count number of vertical blank periods - relies on vblank running */
/* entry: arg = count */
        .global delay
delay:
        move.l  4(sp),d0                /* count */
        add.l   gTicks,d0               /* add current vblank count */
0:
        cmp.l   gTicks,d0
        bgt.b   0b
        rts

/* void genesis_vblank_handler(struct pt_regs *regs); */
/* Called by genesis_vblank_interrupt (entry.S) during the vblank interrupt,
 * with all registers saved there; regs is the interrupted context. */
        .global genesis_vblank_handler
genesis_vblank_handler:
        jsr     genesis_read_keyboard   /* read keyboard input from port 2 */
        jsr     genesis_process_keyboard /* process keyboard and feed to TTY */
        jsr     genesis_term_vblank     /* console: settings, output, cursor, bell */
        move.l  4(sp),-(sp)             /* regs */
        jsr     genesis_timer_callback  /* call kernel timer (increments jiffies) */
        addq.l  #4,sp
        rts


        .data

        .global gTicks
gTicks:
        .long   0

/* Current font, set by genesis_term.c: width in pixels (8 or 6) and columns */
        .global fontWidth
fontWidth:
        .long   8

        .global textCols
textCols:
        .long   TEXT_COLS_8X8

        .section .bss
        .align  2

/* Characters of each block, [TEXT_ROWS][TEXT_COLS] (zeroed at boot = blank) */
        .global text_charmap
text_charmap:
        .space  TEXT_COLS*TEXT_ROWS

/* Attributes of each character, same layout: nonzero = reverse video */
        .global text_attrmap
text_attrmap:
        .space  TEXT_COLS*TEXT_ROWS

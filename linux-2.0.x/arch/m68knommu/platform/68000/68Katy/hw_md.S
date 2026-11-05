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
/* Each text cell of plane B gets its own tile (the name table is filled    */
/* once at boot and never changes), a RAM copy of the characters            */
/* (text_charmap) is kept, and characters are drawn into the tiles in       */
/* software.                                                                */
/*   8x8 font (font_cga.c):      36 columns, one character = one tile.      */
/*   6x8 font (font_cp437_6x8.c): 48 columns. The VDP can only place whole  */
/*     8x8 tiles, so a character overlaps 1 or 2 tiles, which are rebuilt   */
/*     from text_charmap. 4 characters fit exactly in 3 tiles (24 pixels). */
/* Both modes have 16 pixels of padding on the left and right.              */
/*                                                                          */
/* xCursor is a character column (0 - textCols-1), yCursor is a plane B     */
/* row (0 - 31). The font is switched at run time through pendingFont and   */
/* the text colour through pendingColor (see genesis_vdp.c ioctls).         */
        .equ SCREEN_LEFT_PIXEL,         16              /* left/right padding in pixels */
        .equ SCREEN_LEFT_EDGE,          0               /* first text column */
        .equ SCREEN_TOP_EDGE,           2
        .equ SCREEN_BOTTOM_EDGE,        25
        .equ PLANE_VERTICAL_EDGE,       31              /* 0 - 31 */

        .equ TEXT_COLS_8X8,             36              /* (320 - 2*16) / 8 */
        .equ TEXT_COLS_6X8,             48              /* (320 - 2*16) / 6 */
        .equ TEXT_COLS,                 48              /* text_charmap row length (widest mode) */
        .equ TEXT_ROWS,                 32              /* every row of the 64x32 plane (it wraps when scrolling) */
        .equ TEXT_TILE_COLS,            36              /* text tiles per row (288 px) */
        .equ TEXT_FIRST_PLANE_COL,      2               /* plane B column of the first text tile (16 px) */
        .equ TEXT_FIRST_TILE,           1               /* tile 0 stays blank */
        .equ CURSOR_TILE_8X8,           TEXT_FIRST_TILE + TEXT_TILE_COLS * TEXT_ROWS   /* 1153, 8 px wide block */
        .equ CURSOR_TILE_6X8,           CURSOR_TILE_8X8 + 1                            /* 1154, 6 px wide block */

/* Boot defaults */
        .equ DEFAULT_FONT_WIDTH,        8               /* 8 = 8x8 font, 6 = 6x8 font */
/* Text colour, Genesis CRAM format 0000 BBB0 GGG0 RRR0 (each 0x0 - 0xE) */
/* 0x0AAA = light gray, 0x0EEE = white, 0x00C0 = dim green */
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

/* set up plane B as a text bitmap: every text cell gets its own tile */
/* row r, tile column t uses tile TEXT_FIRST_TILE + r*TEXT_TILE_COLS + t */
/* (the tiles themselves are already blank because VRAM was cleared)     */
        move.w  #0x8F02,(a4)            /* INC = 2 */
        move.l  #0x60000003,(a4)        /* write VRAM 0xE000 (plane B name table) */
        moveq   #0,d0                   /* blank name table entry */
        moveq   #TEXT_FIRST_TILE,d1     /* next text tile number */
        moveq   #TEXT_ROWS-1,d2
7:
        moveq   #TEXT_FIRST_PLANE_COL-1,d3
8:
        move.w  d0,(a3)                 /* left padding: blank */
        dbra    d3,8b
        moveq   #TEXT_TILE_COLS-1,d3
9:
        move.w  d1,(a3)                 /* text tile, palette 0 */
        addq.w  #1,d1
        dbra    d3,9b
        moveq   #64-TEXT_FIRST_PLANE_COL-TEXT_TILE_COLS-1,d3
10:
        move.w  d0,(a3)                 /* right padding and off-screen columns: blank */
        dbra    d3,10b
        dbra    d2,7b

/* cursor tiles: an 8 pixel wide and a 6 pixel wide solid block (consecutive tiles) */
        move.l  #CURSOR_TILE_8X8*32,d0
        bsr     vram_write_cmd
        move.l  d0,(a4)
        moveq   #7,d2
11:
        move.l  #0x11111111,(a3)
        dbra    d2,11b
        moveq   #7,d2
12:
        move.l  #0x11111100,(a3)
        dbra    d2,12b

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
/* 6x8 software text renderer (internal routines)               */
/* All of these expect a1 = 0xC00000 (VDP data) and VDP INC = 2 */
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

/* text_render_tile */
/* 6x8 font only: rebuild one text tile from the two characters that overlap it. */
/* entry: d2.w = row (0 - 31), d3.w = tile column (0 - 35) */
/* trashes d0-d1, d4-d7, a2-a5 (d2, d3 preserved) */
/*                                                             */
/* The first character in tile t is k = 4t/3 and it starts      */
/* s = -2 * (4t mod 3) pixels from the tile's left edge         */
/* (0, -2 or -4); character k+1 starts at s+6. A glyph row      */
/* expanded by font_6x8_expand has its leftmost pixel in bits   */
/* 23-20, so it is shifted left by 8 - 4s for character k and   */
/* right by 16 + 4s for character k+1.                          */
text_render_tile:
        move.w  d2,d0
        mulu.w  #TEXT_TILE_COLS,d0
        add.w   d3,d0
        addq.w  #TEXT_FIRST_TILE,d0     /* tile number */
        lsl.l   #5,d0                   /* * 32 bytes per tile */
        bsr     vram_write_cmd
        move.l  d0,4(a1)                /* write VRAM at the tile */

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
        adda.w  d0,a2                   /* a2 = &text_charmap[row][k] */

        lea     font_6x8,a3
        moveq   #0,d0
        move.b  (a2)+,d0
        lsl.w   #3,d0
        adda.w  d0,a3                   /* a3 = glyph of character k */
        lea     font_6x8,a4
        moveq   #0,d0
        move.b  (a2),d0
        lsl.w   #3,d0
        adda.w  d0,a4                   /* a4 = glyph of character k+1 */

        lea     font_6x8_expand,a5
        moveq   #7,d5                   /* 8 pixel rows */
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
        move.l  d0,(a1)                 /* write tile row */
        dbra    d5,1b
        rts

/* text_draw_char */
/* entry: d0.b = character, d2.w = row (0 - 31), d3.w = column (0 - textCols-1) */
/* trashes d0-d1 */
text_draw_char:
        movem.l d3-d7/a2-a5,-(sp)
        moveq   #0,d4
        move.b  d0,d4                   /* d4 = character */
        lea     text_charmap,a2
        move.w  d2,d1
        mulu.w  #TEXT_COLS,d1
        add.w   d3,d1
        move.b  d4,0(a2,d1.w)           /* text_charmap[row][col] = character */

        cmpi.l  #6,fontWidth
        beq.s   2f

        /* 8x8 font: the character is exactly one tile (tile column = text column) */
        move.w  d2,d0
        mulu.w  #TEXT_TILE_COLS,d0
        add.w   d3,d0
        addq.w  #TEXT_FIRST_TILE,d0
        lsl.l   #5,d0
        bsr     vram_write_cmd
        move.l  d0,4(a1)                /* write VRAM at the tile */
        lea     font_data,a2
        lsl.w   #5,d4                   /* 8 longs (32 bytes) per glyph */
        adda.w  d4,a2
        move.l  #0x11111111,d1          /* glyph rows are 0xF per lit pixel; use colour 1 */
        moveq   #7,d5
3:
        move.l  (a2)+,d0
        and.l   d1,d0
        move.l  d0,(a1)                 /* write tile row */
        dbra    d5,3b
        bra.s   1f

        /* 6x8 font */
2:
        move.w  d3,d4
        mulu.w  #6,d4                   /* pixel x within the text area */
        move.w  d4,-(sp)
        lsr.w   #3,d4
        move.w  d4,d3                   /* first tile the character touches */
        bsr     text_render_tile
        move.w  (sp)+,d4
        andi.w  #7,d4                   /* pixel offset inside that tile */
        cmpi.w  #8-6,d4
        bls.s   1f                      /* character fits inside one tile */
        addq.w  #1,d3
        bsr     text_render_tile        /* it spills into the next tile */
1:
        movem.l (sp)+,d3-d7/a2-a5
        rts

/* text_clear_row */
/* entry: d2.w = row (0 - 31) */
/* trashes d0-d1 */
text_clear_row:
        move.l  a2,-(sp)
        lea     text_charmap,a2
        move.w  d2,d0
        mulu.w  #TEXT_COLS,d0
        adda.w  d0,a2
        moveq   #TEXT_COLS-1,d1
1:
        clr.b   (a2)+
        dbra    d1,1b
        move.l  (sp)+,a2

        move.w  d2,d0
        mulu.w  #TEXT_TILE_COLS,d0
        addq.w  #TEXT_FIRST_TILE,d0
        lsl.l   #5,d0
        bsr     vram_write_cmd
        move.l  d0,4(a1)                /* write VRAM at the row's first tile */
        moveq   #0,d0
        move.w  #TEXT_TILE_COLS*8-1,d1  /* the row's tiles are contiguous */
2:
        move.l  d0,(a1)
        dbra    d1,2b
        rts

/* void clear_screen(void); */
/* clear every text row of plane B */
        .global clear_screen
clear_screen:
        move.l  d2,-(sp)
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        moveq   #TEXT_ROWS-1,d2
1:
        bsr     text_clear_row
        dbra    d2,1b
        move.l  (sp)+,d2
        rts

/* void apply_console_settings(void); */
/* Apply a font or colour change requested by the console driver (genesis_vdp.c */
/* sets pendingFont / pendingColor). Called from the vblank handler before the  */
/* console buffer is flushed, so the VDP is only touched from vblank.           */
/* A font change clears the screen and puts the cursor back at the top.         */
        .global apply_console_settings
apply_console_settings:
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */

        move.l  pendingColor,d0
        beq.s   1f                      /* 0 = no change */
        clr.l   pendingColor
        move.l  #0xC0020000,4(a1)       /* write CRAM address 2 (palette 0, entry 1) */
        move.w  d0,(a1)                 /* text and cursor colour (low word) */
1:
        move.l  pendingFont,d0
        beq.w   9f                      /* 0 = no change */
        clr.l   pendingFont

        cmpi.l  #6,d0
        beq.s   2f
        move.l  #8,fontWidth            /* 8x8 font */
        move.l  #TEXT_COLS_8X8,textCols
        move.l  #CURSOR_TILE_8X8,cursorTile
        bra.s   3f
2:
        move.l  #6,fontWidth            /* 6x8 font */
        move.l  #TEXT_COLS_6X8,textCols
        move.l  #CURSOR_TILE_6X8,cursorTile
3:
        move.l  d2,-(sp)
        moveq   #TEXT_ROWS-1,d2
4:
        bsr     text_clear_row          /* clear the whole plane */
        dbra    d2,4b
        move.l  (sp)+,d2

        move.l  #SCREEN_LEFT_EDGE,xCursor
        move.l  #SCREEN_TOP_EDGE,yCursor
        clr.l   scrollFlag
        clr.l   vScrollPos
        move.l  #0x40020010,4(a1)       /* write VSRAM address 2 (plane B vertical scroll) */
        move.w  #0,(a1)
9:
        rts

/* void put_str(char *str, int color, int x, int y); */
/* put string characters to the screen (no wrapping, stops at the right edge) */
/* entry: first arg = string address */
/*        second arg = color (currently ignored) */
/*        third arg = text column at which to start printing (0 - 47) */
/*        fourth arg = row at which to start printing (0 - 31) */
        .global put_str
put_str:
        movem.l d2-d3,-(sp)
        movea.l 12(sp),a0               /* string pointer */
        move.l  20(sp),d3               /* x (column) */
        move.l  24(sp),d2               /* y (row) */
        andi.w  #TEXT_ROWS-1,d2
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
1:
        moveq   #0,d0
        move.b  (a0)+,d0
        beq.s   2f
        cmpi.w  #TEXT_COLS,d3
        bcc.s   2f
        bsr     text_draw_char
        addq.w  #1,d3
        bra.s   1b
2:
        movem.l (sp)+,d2-d3
        rts

/* void put_chr(char chr, int color, int x, int y); */
/* put a character to the screen */
/* entry: first arg = character */
/*        second arg = color (currently ignored) */
/*        third arg = text column (0 - 47) */
/*        fourth arg = row (0 - 31) */
        .global put_chr
put_chr:
        movem.l d2-d3,-(sp)
        move.l  12(sp),d0               /* character */
        move.l  20(sp),d3               /* x (column) */
        move.l  24(sp),d2               /* y (row) */
        andi.w  #TEXT_ROWS-1,d2
        cmpi.w  #TEXT_COLS,d3
        bcc.s   1f
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */
        bsr     text_draw_char
1:
        movem.l (sp)+,d2-d3
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



/* void syscall_PRINT_STRING(char *str, int color); */

/* entry: first arg = string address */
/*        second arg = 0 for normal color font, N * 0x0200 for alternate color font */

        .global syscall_PRINT_STRING
syscall_PRINT_STRING:
        movem.l d2-d3,-(sp)
        movea.l 12(sp),a0               /* string pointer (color arg at 16(sp) is currently ignored) */
        lea     0xC00000,a1
        move.w  #0x8F02,4(a1)           /* set INC to 2 */

printLoop:
        moveq   #0,d0
        move.b  (a0)+,d0
        beq.s   printDone               /* end of string */

        cmpi.b  #0x0A, d0               /* check if it is the enter key (0xA) */
        beq.s   newline                 /* if it is, then jump to newline subroutine */

        move.l  yCursor,d2
        move.l  xCursor,d3
        bsr     text_draw_char          /* draw character at the cursor */

        addq.l  #1, xCursor
        move.l  xCursor,d0
        cmp.l   textCols,d0                             /* if xCursor is past the last column */
        bcc.s   newline                                 /* jump to newline */
        bra.s   printLoop

printDone:
        movem.l (sp)+,d2-d3
        rts

newline:
        move.l  #SCREEN_LEFT_EDGE, xCursor              /*  set X cursor to the first column */
        addq.l  #1, yCursor                             /*  advance Y cursor         */

        tst.l   scrollFlag                              /* if scrollFlag is set */
        bne.s   scrollPlaneB                            /* jump to scrollPlaneB */

        cmpi.l  #SCREEN_BOTTOM_EDGE, yCursor            /* if yCursor is less than or equal to SCREEN_BOTTOM_EDGE */
        bls.s   shouldResetYcursor                      /* jump to shouldResetYcursor */
                                                        /* ..otherwise scroll the screen down */
scrollPlaneB:
        /* clear the row that is about to scroll into view: (yCursor + 8) mod 32 */
        move.l  yCursor,d2
        cmpi.l  #24,d2                  /* if yCursor is greater than or equal to 24 */
        bcc.s   sub24                   /* jump to sub24 */
        addq.l  #8,d2                   /* otherwise add 8... */
        bra.s   continueLineClear       /* and jump to continueLineClear */

sub24:
        subi.l  #24,d2

continueLineClear:
        bsr     text_clear_row

        addq.l  #8, vScrollPos                          /* Increment vertical scroll position */
        move.l  #0x40020010, 4(a1)                      /* put Scroll Plane B into VDP control */
        move.w  vScrollPos+2, (a1)                      /* put incremented scroll location into VDP data */
        move.l  #1, scrollFlag                          /* set scrollFlag to true */

shouldResetYcursor:
        cmpi.l  #PLANE_VERTICAL_EDGE, yCursor           /* if yCursor is less than or equal to PLANE_VERTICAL_EDGE */
        bls.w   printLoop                               /* keep printing */
        clr.l   yCursor                                 /* otherwise reset yCursor to zero */
        bra.w   printLoop




/* void syscall_SCROLL(); */

        .global syscall_SCROLL
syscall_SCROLL:
        addi.l  #8, vScrollPos                  /* Increment vertical scroll position */
        move.l  vScrollPos, d6
        move.l  #0x40020010, (VDP_CONTROL)      /* put Scroll Plane B into VDP control */
        move.w  d6, (VDP_DATA)                  /* put incremented scroll location into VDP data */
        rts



/* ============================================================ */
/* Console buffer functions for vblank-synchronized output      */
/* ============================================================ */

/* void console_putchar(char c); */
/* Add a character to the console buffer (called from kernel) */
        .global console_putchar
console_putchar:
        move.l  4(sp),d0                /* get character */
        move.l  console_buf_index,d1
        cmpi.l  #254,d1                 /* buffer full? */
        bge.s   1f                      /* yes, drop character */
        lea     console_buffer,a0
        move.b  d0,0(a0,d1.l)           /* store character */
        addq.l  #1,d1
        move.l  d1,console_buf_index
        clr.b   0(a0,d1.l)              /* null terminate */
1:
        rts

/* void console_flush(void); */
/* Flush the console buffer to screen - call during vblank */
        .global console_flush
console_flush:
        tst.l   console_buf_index       /* anything in buffer? */
        beq.s   1f                      /* no, return */
        
        /* Call syscall_PRINT_STRING(console_buffer, 0) */
        move.l  #0,-(sp)                /* color = 0 */
        pea     console_buffer          /* string pointer */
        jsr     syscall_PRINT_STRING
        addq.l  #8,sp                   /* clean up stack */
        
        /* Clear the buffer */
        clr.l   console_buf_index
        clr.b   console_buffer
1:
        rts

/* void genesis_vblank_handler(void); */
/* Called by crt0_rom.S during vblank interrupt */
/* Reads keyboard, flushes console, draws cursor, calls kernel timer */
        .global genesis_vblank_handler
genesis_vblank_handler:
        jsr     genesis_read_keyboard   /* read keyboard input from port 2 */
        jsr     genesis_process_keyboard /* process keyboard and feed to TTY */
        jsr     apply_console_settings  /* font / colour changes requested by the console driver */
        jsr     console_flush           /* flush any pending console output */
        jsr     draw_cursor_sprite      /* draw blinking cursor */
        jsr     genesis_timer_callback  /* call kernel timer (increments jiffies) */
        rts

/* Draw cursor sprite - blinks based on globalCounter */
draw_cursor_sprite:
        movem.l d0-d2/a0-a1,-(sp)

        /* Increment globalCounter (wraps at 16 for blink effect) */
        move.l  globalCounter,d0
        addq.l  #1,d0
        andi.l  #0x0F,d0                /* wrap at 16 */
        move.l  d0,globalCounter

        /* Calculate sprite Y position: (yCursor * 8) + 128 - vScrollPos */
        move.l  yCursor,d1
        lsl.l   #3,d1                   /* * 8 */
        addi.l  #128,d1                 /* + 128 (sprite offset) */
        sub.l   vScrollPos,d1           /* - vScrollPos */

        /* If Y < 128, cursor would be off screen, put at bottom */
        cmpi.l  #128,d1
        bge.s   1f
        move.l  #328,d1                 /* (25 * 8) + 128 */
1:
        /* Calculate sprite X position: (xCursor * fontWidth) + left padding + 128 */
        move.l  xCursor,d2
        mulu.w  fontWidth+2,d2          /* * 8 or 6 */
        addi.l  #SCREEN_LEFT_PIXEL+128,d2 /* + padding + 128 (sprite offset) */

        /* Set up VDP for sprite table write at 0xA800 */
        lea     0xC00004,a0             /* VDP control */
        lea     0xC00000,a1             /* VDP data */
        move.l  #0x68000002,(a0)        /* Write to sprite table at 0xA800 */

        /* Write sprite data - first longword: Y pos (10 bits), size, link */
        move.w  d1,(a1)                 /* Y position */
        move.w  #0x0000,(a1)            /* size=0 (8x8), link=0 (end of list) */

        /* Write sprite data - second longword: tile index, X pos */
        /* Blink: if globalCounter < 8, show the cursor tile, else show tile 0x00 (blank) */
        move.l  globalCounter,d0
        cmpi.l  #8,d0
        blt.s   2f
        move.w  #0x0000,(a1)            /* tile 0 (blank) */
        bra.s   3f
2:
        move.w  cursorTile+2,(a1)       /* solid block as wide as the font */
3:
        move.w  d2,(a1)                 /* X position */

        movem.l (sp)+,d0-d2/a0-a1
        rts


        .data

        .global gTicks
gTicks:
        .long   0

        .global globalCounter
globalCounter:
        .long   0

        .global xCursor
xCursor:
        .long   SCREEN_LEFT_EDGE

/* Current font: width in pixels (8 or 6), columns per row, cursor tile */
        .global fontWidth
fontWidth:
        .long   DEFAULT_FONT_WIDTH

        .if DEFAULT_FONT_WIDTH - 6
textCols:
        .long   TEXT_COLS_8X8
cursorTile:
        .long   CURSOR_TILE_8X8
        .else
textCols:
        .long   TEXT_COLS_6X8
cursorTile:
        .long   CURSOR_TILE_6X8
        .endif

/* Requests from the console driver, applied in apply_console_settings */
        .global pendingFont
pendingFont:
        .long   0                       /* 0 = none, 6 or 8 = switch to that font */

        .global pendingColor
pendingColor:
        .long   0                       /* 0 = none, else 0x10000 | new CRAM colour 0x0BGR */

        .global yCursor
yCursor:
        .long   2

        .global vScrollPos
vScrollPos:
        .long   0

        .global scrollFlag
scrollFlag:
        .long   0

/* Console output buffer */
        .global console_buf_index
console_buf_index:
        .long   0

        .global console_buffer
console_buffer:
        .space  256, 0                  /* 256 byte buffer for console output */

        .section .bss
        .align  2

/* Characters currently on screen, [TEXT_ROWS][TEXT_COLS] (zeroed at boot = blank) */
        .global text_charmap
text_charmap:
        .space  TEXT_COLS*TEXT_ROWS

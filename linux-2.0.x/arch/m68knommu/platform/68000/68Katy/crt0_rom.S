/* SEGA MegaDrive startup code for uClinux */
/* Based on code by Chilly Willy */
/* Adapted for uClinux kernel boot */

        .text

/* Initial exception vectors */

        .long   0x01000000,initialize,exception,exception,exception,exception,exception,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception
        .long   exception,exception,exception,exception,hblank,exception,vblank,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception

/* Standard MegaDrive ROM header at 0x100 */

        .ascii  "SEGA SSF       "       /* SEGA must be the first four chars for TMSS */
        .ascii  " "
        .ascii  "(C)2024         "
        .ascii  "uClinux for     "      /* export name */
        .ascii  "Sega Genesis    "
        .ascii  "                "
        .ascii  "uClinux for     "      /* domestic (Japanese) name */
        .ascii  "Sega Genesis    "
        .ascii  "                "
        .ascii  "GM MK-0000 -00"
        .word   0x0000                  /* checksum - not needed */
        .ascii  "J6              "
        .long   0x00000000,0x001FFFFF   /* ROM start, end (2MB) */
        .long   0x00FF0000,0x00FFFFFF   /* Work RAM start, end */

        .ascii  "RA"                    /* SRAM indicator */
        .byte   0xF8                    /* SRAM flags: backup RAM, odd+even bytes */
        .byte   0x20                    /* SRAM type */
        .long   0x00200000              /* SRAM start */
        .long   0x0027FFFF              /* SRAM end (512KB) */

        .ascii  "    "
        .ascii  "        "

        .ascii  "        "              /* memo */

        .ascii  "                "
        .ascii  "                "
        .ascii  "F               "      /* enable any hardware configuration */


/* Standard MegaDrive startup at 0x200 */

.global _stext
_stext:
initialize:
        move    #0x2700,sr              /* disable interrupts */

        tst.l   0xA10008                /* check CTRL1 and CTRL2 setup */
        bne.b   1f
        tst.w   0xA1000C                /* check CTRL3 setup */
1:
        bne.b   skip_tmss               /* if any controller control port is setup, skip TMSS handling */

/* Check Hardware Version Number */
        move.b  0xA10001,d0
        andi.b  #0x0F,d0                /* VERS */
        beq     2f                      /* 0 = original hardware, TMSS not present */
        move.l  #0x53454741,0xA14000    /* Store Sega Security Code "SEGA" to TMSS */
2:
        move.w  0xC00004,d0             /* read VDP Status reg */

skip_tmss:
        move.w  #0x8104,0xC00004        /* display off, vblank disabled */
        move.w  0xC00004,d0             /* read VDP Status reg */

/* Clear Work RAM */
        lea     0xFF0000,a0
        moveq   #0,d0
        move.w  #0x3FFF,d1
1:
        move.l  d0,(a0)+
        dbra    d1,1b

/* Enable Mega EverDrive X7 SSF mapper with write access */
/* Register 0xA130F0: [PXWL.... ...RRRRR] (16-bit, active high byte)
 *   P = Protection bit (must be 1 to write to this register)
 *   X = 32X mode (0 for normal)
 *   W = Write enable (1 = cartridge space is writable as RAM)
 *   L = LED (0 = off)
 *   R = Bank number for first 512KB slot (0 = bank 0)
 * 0xA0 = 10100000 binary = P=1, W=1
 */
        move.b  #0xA0,0xA130F0          /* Enable cartridge RAM writes: P=1, W=1 */

/* Set up SSF mapper bank for our RAM region */
/* Slot 4 (0x200000-0x27FFFF) is controlled by register 0xA130F8 */
/* Map bank 4 to slot 4 (this is the default, but be explicit) */
        move.w  #0x0004,0xA130F8        /* Bank 4 -> slot 4 (0x200000-0x27FFFF) */

/* Copy initialized data from ROM to cartridge RAM */
        lea     __data_rom_start,a0     /* source: ROM */
        lea     __data_start,a1         /* dest: cartridge RAM at 0x200000 */
        move.l  #__data_size,d0         /* size in bytes */
        beq.b   2f                      /* skip if no data */
        addq.l  #3,d0                   /* round up to longword */
        lsr.l   #2,d0                   /* convert to longwords */
1:
        move.l  (a0)+,(a1)+
        subq.l  #1,d0
        bne.b   1b
2:

/* Clear BSS section in cartridge RAM */
        lea     __bss_start,a0
        move.l  #__bss_size,d0          /* size in bytes */
        beq.b   4f                      /* skip if no bss */
        addq.l  #3,d0                   /* round up to longword */
        lsr.l   #2,d0                   /* convert to longwords */
        moveq   #0,d1
3:
        move.l  d1,(a0)+
        subq.l  #1,d0
        bne.b   3b
4:

/* Set stack pointer to top of Work RAM */
        lea     0x01000000,a0
        movea.l a0,sp
        link.w  a6,#-8                  /* set up initial stack frame */

/* Initialize the console hardware */
        jsr     init_hardware

/* Sanity check - display "test" on screen */
        move.l  #2,-(sp)                /* y = 2 */
        move.l  #2,-(sp)                /* x = 2 */
        move.l  #0,-(sp)                /* color = 0 (normal) */
        pea     test_string             /* string pointer */
        jsr     put_str
        lea     16(sp),sp               /* clean up stack (4 args * 4 bytes) */

/* Wait for a few frames so "test" is visible before kernel potentially crashes */
        move.w  #5,d0                   /* wait for 5 vblanks */
1:      move.w  0xC00004,d1             /* read VDP status */
        btst    #3,d1                   /* check vblank flag */
        beq.b   1b                      /* wait for vblank */
2:      move.w  0xC00004,d1             /* read VDP status */
        btst    #3,d1                   /* check vblank flag */
        bne.b   2b                      /* wait for vblank to end */
        dbra    d0,1b                   /* repeat for N frames */

/* Jump to Linux kernel */
        jmp     start_kernel


/* Test string in ROM */
        .text
test_string:
        .asciz  "test"
        .align  2

/* Data section - vectors and tick counter */

        .data

        .global exception_vector
exception_vector:
        .long   0
        .global hblank_vector
hblank_vector:
        .long   0
        .global vblank_vector
vblank_vector:
        .long   0


/* Exception handlers */

        .text

exception:
        move.l  exception_vector,-(sp)
        beq.b   1f
        rts
1:
        addq.l  #4,sp
        rte

hblank:
        move.l  hblank_vector,-(sp)
        beq.b   1f
        rts
1:
        addq.l  #4,sp
        rte

vblank:
        /* VBlank handler - flush console buffer and increment tick counter */
        movem.l d0-d7/a0-a6,-(sp)       /* save all registers (console_flush uses several) */
        
        /* Call genesis_vblank_handler to flush console buffer */
        jsr     genesis_vblank_handler
        
        /* Check if kernel installed a custom vblank handler */
        move.l  vblank_vector,d0
        beq.b   1f
        movea.l d0,a0
        jsr     (a0)
1:
        addq.l  #1,gTicks
        movem.l (sp)+,d0-d7/a0-a6       /* restore all registers */
        rte

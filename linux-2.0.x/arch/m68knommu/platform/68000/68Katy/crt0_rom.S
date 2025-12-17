/* SEGA MegaDrive startup code for uClinux */
/* Based on code by Chilly Willy */
/* Adapted for uClinux kernel boot */

        .text

/* Import exception handlers from kernel entry.S */
        .extern system_call
        .extern buserr
        .extern trap3, trap4, trap5, trap6, trap7
        .extern trap8, trap9, trap10, trap11
        .extern inthandler1, inthandler2, inthandler3
        .extern inthandler5, inthandler7

/* Initial exception vectors */
/* Vector 32 (TRAP #0) must point to system_call for kernel syscalls */

        .long   0x01000000,initialize,buserr,trap3,trap4,trap5,trap6,trap7
        .long   trap8,trap9,trap10,trap11,exception,exception,exception,exception
        .long   exception,exception,exception,exception,exception,exception,exception,exception
        .long   exception,inthandler1,inthandler2,inthandler3,hblank,inthandler5,vblank,inthandler7
        .long   system_call,exception,exception,exception,exception,exception,exception,exception
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

/* ===========================================================================
 * DEBUG STEP 2: Test EverDrive mapper and RAM write
 * ===========================================================================
 */
        move.w  #0x8104,0xC00004        /* display off, vblank disabled */
        move.w  0xC00004,d0             /* read VDP Status reg */

        /* Wait for DMA to complete (in case of reset during DMA) */
        lea     0xC00004,a4
0:      move.w  (a4),d0
        btst    #1,d0                   /* DMA busy? */
        bne.b   0b

        /* Set VDP registers for minimal display */
        move.w  #0x8004,(a4)            /* Reg 0: no H-blank int */
        move.w  #0x8104,(a4)            /* Reg 1: display off, no V-blank int */
        move.w  #0x8230,(a4)            /* Reg 2: Name Table A at 0xC000 */
        move.w  #0x832C,(a4)            /* Reg 3: Name Table W at 0xB000 */
        move.w  #0x8407,(a4)            /* Reg 4: Name Table B at 0xE000 */
        move.w  #0x8554,(a4)            /* Reg 5: Sprite Table at 0xA800 */
        move.w  #0x8700,(a4)            /* Reg 7: Background color = 0 */
        move.w  #0x8C81,(a4)            /* Reg 12: H40 mode */
        move.w  #0x8F02,(a4)            /* Reg 15: Auto-increment = 2 */
        move.w  #0x9001,(a4)            /* Reg 16: Scroll size 64x32 */

        /* Clear VRAM */
        move.l  #0x40000000,(a4)        /* Write to VRAM address 0 */
        lea     0xC00000,a3             /* VDP data port */
        move.w  #0x7FFF,d1
1:      move.w  #0,(a3)
        dbra    d1,1b

        /* Set palette: entry 0 = black, entry 1 = white, entry 2 = green, entry 3 = red */
        move.l  #0xC0000000,(a4)        /* Write to CRAM address 0 */
        move.w  #0x0000,(a3)            /* Color 0: black */
        move.w  #0x0EEE,(a3)            /* Color 1: white */
        move.w  #0x00E0,(a3)            /* Color 2: green */
        move.w  #0x000E,(a3)            /* Color 3: red */

        /* Load font into VRAM (we need letters for RAM OK / RAM FAIL) */
        /* Copy font_data to VRAM at address 0 */
        move.l  #0x40000000,(a4)        /* Write to VRAM address 0 */
        lea     font_data,a0
        move.w  #256*8-1,d2             /* 256 characters * 8 longwords each */
font_load_loop:
        move.l  (a0)+,d0                /* font fg mask */
        move.l  d0,d1
        not.l   d1                      /* font bg mask */
        andi.l  #0x11111111,d0          /* set font fg color (color 1) */
        andi.l  #0x00000000,d1          /* set font bg color (color 0) */
        or.l    d1,d0
        move.l  d0,(a3)                 /* write tile line */
        dbra    d2,font_load_loop

        /* Turn display ON so we can see what happens */
        move.w  #0x8144,(a4)            /* Reg 1: display ON, no V-blank int */

        /* ---- Now test EverDrive mapper ---- */
        
        /* First, let's test Work RAM (0xFF0000) to make sure basic writes work */
        move.l  #0x12345678,0xFF0000    /* Write to Work RAM */
        move.l  0xFF0000,d0             /* Read it back */
        cmp.l   #0x12345678,d0
        bne     workram_fail            /* If this fails, something is very wrong */

        /* Work RAM OK - show "W" */
        move.l  #0x60800003,(a4)        /* Row 2, col 2 */
        move.w  #'W',(a3)
        move.w  #'R',(a3)
        move.w  #'A',(a3)
        move.w  #'M',(a3)
        move.w  #':',(a3)
        move.w  #'O',(a3)
        move.w  #'K',(a3)

        /* Enable Mega EverDrive X7 SSF mapper with write access */
        /* Try writing the full word to 0xA130F0 */
        move.w  #0xA000,0xA130F0        /* Enable cartridge RAM writes: P=1, W=1 (word write) */
        
        /* Add delay - real hardware might need time */
        move.w  #1000,d1
delay1: dbra    d1,delay1
        
        /* Map bank 4 to slot 4 (0x200000-0x27FFFF) */
        move.w  #0x0004,0xA130F8        /* Bank 4 -> slot 4 */

        /* Add delay */
        move.w  #1000,d1
delay2: dbra    d1,delay2

        /* Test 1: Try writing to slot 4 (0x200000) */
        move.l  #0xDEADBEEF,0x200000    /* Write test pattern */
        nop
        nop
        move.l  0x200000,d0             /* Read it back */
        cmp.l   #0xDEADBEEF,d0          /* Compare */
        beq     ram_test_ok

        /* Test 2: Try slot 2 (0x100000) instead */
        move.w  #0x0002,0xA130F4        /* Bank 2 -> slot 2 */
        move.w  #100,d1
delay3: dbra    d1,delay3
        move.l  #0xCAFEBABE,0x100000    /* Write test pattern */
        nop
        nop
        move.l  0x100000,d0             /* Read it back */
        cmp.l   #0xCAFEBABE,d0
        beq     ram_test_ok_slot2

        /* Test 3: Try slot 1 (0x080000) */
        move.w  #0x0001,0xA130F2        /* Bank 1 -> slot 1 */
        move.w  #100,d1
delay4: dbra    d1,delay4
        move.l  #0xFEEDFACE,0x080000    /* Write test pattern */
        nop
        nop
        move.l  0x080000,d0             /* Read it back */
        cmp.l   #0xFEEDFACE,d0
        beq     ram_test_ok_slot1

        /* All failed - show what we read back from 0x200000 */
        move.l  0x200000,d7             /* Save what we read */
        bra     ram_test_fail

ram_test_ok:
        /* RAM OK at slot 4! Show brief message then continue booting */
        move.l  #0x61000003,(a4)        /* Row 4, col 2 */
        move.w  #'R',(a3)
        move.w  #'A',(a3)
        move.w  #'M',(a3)
        move.w  #' ',(a3)
        move.w  #'O',(a3)
        move.w  #'K',(a3)
        move.w  #' ',(a3)
        move.w  #'-',(a3)
        move.w  #' ',(a3)
        move.w  #'B',(a3)
        move.w  #'O',(a3)
        move.w  #'O',(a3)
        move.w  #'T',(a3)
        move.w  #'I',(a3)
        move.w  #'N',(a3)
        move.w  #'G',(a3)
        bra     continue_boot

ram_test_ok_slot2:
ram_test_ok_slot1:
        /* Unexpected - we want slot 4, halt for debug */
        bra     ram_test_fail

workram_fail:
        /* Work RAM failed - very bad! */
        move.l  #0x60800003,(a4)
        move.w  #'W',(a3)
        move.w  #'R',(a3)
        move.w  #'A',(a3)
        move.w  #'M',(a3)
        move.w  #' ',(a3)
        move.w  #'F',(a3)
        move.w  #'A',(a3)
        move.w  #'I',(a3)
        move.w  #'L',(a3)
        bra     debug_halt

ram_test_fail:
        /* All cart RAM tests failed - show diagnostic */
        move.l  #0x61000003,(a4)        /* Row 4, col 2 */
        move.w  #'C',(a3)
        move.w  #'A',(a3)
        move.w  #'R',(a3)
        move.w  #'T',(a3)
        move.w  #' ',(a3)
        move.w  #'R',(a3)
        move.w  #'A',(a3)
        move.w  #'M',(a3)
        move.w  #' ',(a3)
        move.w  #'F',(a3)
        move.w  #'A',(a3)
        move.w  #'I',(a3)
        move.w  #'L',(a3)

/* DEBUG: Loop forever on failure */
debug_halt:
        bra.b   debug_halt

/* ===========================================================================
 * Continue booting - RAM test passed
 * ===========================================================================
 */
continue_boot:

/* Copy initialized data from ROM to cartridge RAM */
        lea     __data_rom_start,a0     /* source: ROM */
        lea     __data_start,a1         /* dest: cartridge RAM at 0x200000 */
        move.l  #__data_size,d0         /* size in bytes */
        beq.b   data_copy_done          /* skip if no data */
        addq.l  #3,d0                   /* round up to longword */
        lsr.l   #2,d0                   /* convert to longwords */
data_copy_loop:
        move.l  (a0)+,(a1)+
        subq.l  #1,d0
        bne.b   data_copy_loop
data_copy_done:

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

/* Initialize the console hardware (re-init VDP with proper font/colors) */
        jsr     init_hardware

/* Wait for a few frames so "RAM OK - BOOTING" is visible */
        move.w  #30,d0                  /* wait for 30 vblanks (~0.5 sec) */
wait_boot_msg:
        move.w  0xC00004,d1             /* read VDP status */
        btst    #3,d1                   /* check vblank flag */
        beq.b   wait_boot_msg           /* wait for vblank */
wait_boot_msg2:
        move.w  0xC00004,d1             /* read VDP status */
        btst    #3,d1                   /* check vblank flag */
        bne.b   wait_boot_msg2          /* wait for vblank to end */
        dbra    d0,wait_boot_msg        /* repeat for N frames */

/* Jump to Linux kernel */
        jmp     start_kernel

/* Test strings in ROM */
        .text
test_string:
        .asciz  "test"
        .align  2
ram_ok_string:
        .asciz  "RAM OK"
        .align  2
ram_fail_string:
        .asciz  "RAM FAIL!"
        .align  2
argv0_label:
        .asciz  "argv[0] after copy saved in d6"
        .align  2
argv0_now:
        .asciz  "argv[0] after init_hw read again"
        .align  2
jumping_string:
        .asciz  "Jumping to kernel..."
        .align  2
returned_string:
        .asciz  "ERROR: kernel returned!"
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

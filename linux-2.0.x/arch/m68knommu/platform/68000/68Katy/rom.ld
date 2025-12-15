/*
 * Sega Genesis / MegaDrive Memory Map for uClinux
 * Configured for Mega EverDrive X7 with SSF mapper (extended_ssf-v2)
 *
 * SSF Mapper Setup:
 *   - ROM header contains "SEGA SSF" to enable mapper
 *   - Register 0xA130F0 with P=1, W=1 enables write access
 *   - Slot 4 (0x200000-0x27FFFF) mapped to bank 4 via 0xA130F8
 *
 * +--------------------+ <- 0x00000000
 * | .text              |   ROM (cartridge slot 0, bank 0)
 * | (code + rodata)    |
 * |        __text_end  |
 * | __data_rom_start   |   (initialized data stored here in ROM)
 * +--------------------+ <- ~0x00080000 (ROM fits in first 512KB)
 * +--------------------+ <- 0x00200000
 * | .data              |   SSF Bank 4 (writable PSRAM)
 * |      __data_start  |
 * |        __data_end  |
 * +--------------------+
 * | .bss               |
 * |       __bss_start  |
 * |         __bss_end  |
 * +--------------------+
 * |       (heap)       |
 * |             _end   |
 * .                    .
 * +--------------------+ <- 0x00280000 (end of 512KB bank)
 * .                    .
 * +--------------------+ <- 0x00FF0000
 * | Work RAM (64KB)    |   Used for stack (fast on-chip RAM)
 * |           _stack   | <- stack grows down from top
 * +--------------------+ <- 0x01000000
 *
 * The SSF mapper provides 512KB banks that can be used as RAM.
 * Bank 4 at slot 4 (0x200000-0x27FFFF) is used for kernel data/bss/heap.
 * Work RAM (0xFF0000-0xFFFFFF) is used for the stack.
 */

MEMORY
{
    rom (rx)   : ORIGIN = 0x00000000, LENGTH = 0x00200000  /* 2MB ROM max */
    cartram (rwx) : ORIGIN = 0x00200000, LENGTH = 0x00080000  /* 512KB cartridge RAM */
    workram (rwx) : ORIGIN = 0x00FF0000, LENGTH = 0x00010000  /* 64KB Work RAM */
}

/*
 * Stack at top of Work RAM (fast internal RAM).
 * The 68000 pre-decrements SP on push, so SP=0x01000000 means
 * first push writes to 0xFFFFFC (valid Work RAM).
 */
PROVIDE (__stack = 0x01000000);

SECTIONS
{
    .text 0x00000000 :
    {
        _romvec = .;        /* Vector table at start of ROM */
        __rom_start = .;
        __text_start = .;
        
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t.*)

        . = ALIGN(16);
        __INIT_SECTION__ = .;
        KEEP (*(.init))
        SHORT (0x4E75)  /* rts */
        
        . = ALIGN(16);
        __FINI_SECTION__ = .;
        KEEP (*(.fini))
        SHORT (0x4E75)  /* rts */

        *(.eh_frame_hdr)
        KEEP (*(.eh_frame))
        *(.gcc_except_table)
        KEEP (*(.jcr))

        . = ALIGN(16);
        __CTOR_LIST__ = .;
        ___CTOR_LIST__ = .;
        LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
        LONG(0)
        __CTOR_END__ = .;

        . = ALIGN(16);
        __DTOR_LIST__ = .;
        ___DTOR_LIST__ = .;
        LONG((__DTOR_END__ - __DTOR_LIST__) / 4 - 2)
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
        LONG(0)
        __DTOR_END__ = .;

        /* Read-only data stays in ROM */
        *(.rdata)
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r.*)
        
        . = ALIGN(4);
        __text_end = .;
        _etext = .;
        _stext = .;
        
        /* Mark where .data content is stored in ROM */
        __data_rom_start = .;
    } > rom

    __text_size = __text_end - __text_start;

    /*
     * Initialized data - lives in cartridge RAM at runtime, but initial
     * values are stored in ROM. Startup code must copy from __data_rom_start
     * to __data_start.
     */
    .data 0x00200000 : AT (__data_rom_start)
    {
        __data_start = .;
        __ram_start = .;
        _ramvec = .;        /* RAM vector table starts here if needed */
        
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d.*)
        CONSTRUCTORS

        *(.lit8)
        *(.lit4)
        *(.sdata)
        *(.sdata.*)
        *(.gnu.linkonce.s.*)
        
        . = ALIGN(4);
        __data_end = .;
        _edata = .;
    } > cartram

    __data_size = __data_end - __data_start;

    /*
     * Uninitialized data - cleared to zero by startup code
     */
    .bss :
    {
        __bss_start = .;
        
        *(.bss)
        *(.bss.*)
        *(.gnu.linkonce.b.*)
        *(.sbss)
        *(.sbss.*)
        *(.gnu.linkonce.sb.*)
        *(.scommon)
        *(COMMON)
        
        . = ALIGN(4);
        __bss_end = .;
        _end = .;
        __end = .;
        end = .;
    } > cartram

    __bss_size = __bss_end - __bss_start;

    /* End of cartridge RAM */
    _ramend = 0x00280000;       /* End of 512KB cartridge RAM */
    _boot_stack = 0x00FFFFFC;   /* Stack in Work RAM */
}

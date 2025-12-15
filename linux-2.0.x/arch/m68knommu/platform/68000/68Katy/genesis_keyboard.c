/*
 * genesis_keyboard.c - XBAND Keyboard and Controller input for Sega Genesis
 * 
 * Ported from Sega-Genesis-Keyboard-Test project
 * For uClinux on Sega Genesis
 */

#include <linux/tty.h>
#include <linux/tty_flip.h>

/* Hardware registers */
#define kData2      0xA10005
#define kCtl2       0xA1000B
#define kSerial2    0xA10019

/* Control lines */
#define kTH         0x40
#define kTR         0x20
#define kTL         0x10
#define kDataLines  0x0F

/* Key codes */
#define kUpArrowKey         0xFD
#define kDownArrowKey       0xFC
#define kRightArrowKey      0xFB
#define kLeftArrowKey       0xFA
#define kAltKey             0xF9
#define kControlKey         0xF8
#define kEnterKey           0xF7
#define kNoKey              0x00

/* Keyboard state flags */
#define kReturnIsLF         0x01000000
#define kControlDown        0x00000040
#define kAltDown            0x00000020
#define kShiftDown          0x00000010
#define kCapsLockDown       0x00000008
#define kCapsLocked         0x00000004
#define kNumLocked          0x00000002
#define kScrollLocked       0x00000001

/* FIFO masks */
#define kKeybdDataFifoMask      0xF
#define kSysKeysFifoMask        0xF
#define kKeybdCmdStatusFifoMask 0x7

/* Special keymap values */
#define kShiftKey           0xFF
#define kCapsLockKey        0xFE

/* Data type identifiers */
#define kESKeycodeData          0
#define kESControllerEcho       1
#define kESControllerVersion    2

typedef unsigned char UChar;
typedef unsigned long ULong;

/* Control globals structure */
typedef struct ControlGlobals {
    short           controlValues[5];
    unsigned char   sysKeysBuf[16];
    unsigned char   sysKeysHead, sysKeysTail;
    unsigned char   keycodeBuf[16];
    unsigned char   keycodeHead, keycodeTail;
    unsigned char   statusBuf[8];
    unsigned char   statusHead, statusTail;
    unsigned char   cmdBuf[8];
    unsigned char   cmdHead, cmdTail;
    char            keyboardPresent;
    char            pad;
    unsigned long   keyboardID;
    unsigned long   keyboardFlags;
    long            samplePhase;
} ControlGlobals;

/* Global state */
static ControlGlobals ControlGlobalz;
static char keyboardConnected = 0;

/* External TTY driver reference */
extern struct tty_driver genesis_serial_driver;
extern struct tty_struct *genesis_tty_table[];

/* Scancode to ASCII table */
static const unsigned char scancodeToAscii[] = {
    0,     /* 00, unused */
    0,     /* 01, F9 */
    0,     /* 02, unused */
    0,     /* 03, F5 */
    0,     /* 04, F3 */
    0,     /* 05, F1 */
    0,     /* 06, F2 */
    0,     /* 07, F12 */
    0,     /* 08, unused */
    0,     /* 09, F10 */
    0,     /* 0A, F8 */
    0,     /* 0B, F6 */
    0,     /* 0C, F4 */
    '\t',  /* 0D, TAB */
    0x60,  /* 0E, ` */
    0,     /* 0F, unused */
    0,     /* 10, unused */
    0,     /* 11, L ALT */
    kShiftKey,     /* 12, L SHFT */
    0,     /* 13, unused */
    0,     /* 14, L CTRL */
    'q',   /* 15, q */
    '1',   /* 16, 1 */
    0,     /* 17, unused */
    0,     /* 18, unused */
    0,     /* 19, unused */
    'z',   /* 1A, z */
    's',   /* 1B, s */
    'a',   /* 1C, a */
    'w',   /* 1D, w */
    '2',   /* 1E, 2 */
    0,     /* 1F, unused */
    0,     /* 20, unused */
    'c',   /* 21, c */
    'x',   /* 22, x */
    'd',   /* 23, d */
    'e',   /* 24, e */
    '4',   /* 25, 4 */
    '3',   /* 26, 3 */
    0,     /* 27, unused */
    0,     /* 28, unused */
    ' ',   /* 29, space */
    'v',   /* 2A, v */
    'f',   /* 2B, f */
    't',   /* 2C, t */
    'r',   /* 2D, r */
    '5',   /* 2E, 5 */
    0,     /* 2F, unused */
    0,     /* 30, unused */
    'n',   /* 31, n */
    'b',   /* 32, b */
    'h',   /* 33, h */
    'g',   /* 34, g */
    'y',   /* 35, y */
    '6',   /* 36, 6 */
    0,     /* 37, unused */
    0,     /* 38, unused */
    0,     /* 39, unused */
    'm',   /* 3A, m */
    'j',   /* 3B, j */
    'u',   /* 3C, u */
    '7',   /* 3D, 7 */
    '8',   /* 3E, 8 */
    0,     /* 3F, unused */
    0,     /* 40, unused */
    ',',   /* 41, , */
    'k',   /* 42, k */
    'i',   /* 43, i */
    'o',   /* 44, o */
    '0',   /* 45, 0 */
    '9',   /* 46, 9 */
    0,     /* 47, unused */
    0,     /* 48, unused */
    '.',   /* 49, . */
    '/',   /* 4A, / */
    'l',   /* 4B, l */
    ';',   /* 4C, ; */
    'p',   /* 4D, p */
    '-',   /* 4E, - */
    0,     /* 4F, unused */
    0,     /* 50, unused */
    0,     /* 51, unused */
    '\'',  /* 52, ' */
    0,     /* 53, unused */
    '[',   /* 54, [ */
    '=',   /* 55, = */
    0,     /* 56, unused */
    0,     /* 57, unused */
    kCapsLockKey,  /* 58, caps */
    kShiftKey,     /* 59, R SHFT */
    0x0A,  /* 5A, ENTER */
    ']',   /* 5B, ] */
    0,     /* 5C, unused */
    '\\',  /* 5D, backslash */
    0,     /* 5E, unused */
    0,     /* 5F, unused */
    0, 0, 0, 0, 0, 0,  /* 60-65 */
    0x08,  /* 66, BKSP */
    0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 67-6F */
    0, 0, 0, 0, 0, 0,  /* 70-75 */
    0x1B,  /* 76, ESC */
    0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 77-7F */
    0, 0, 0, 0  /* 80-83 */
};

static const unsigned char scancodeToAsciiShifted[] = {
    0,     /* 00, unused */
    0,     /* 01, F9 */
    0,     /* 02, unused */
    0,     /* 03, F5 */
    0,     /* 04, F3 */
    0,     /* 05, F1 */
    0,     /* 06, F2 */
    0,     /* 07, F12 */
    0,     /* 08, unused */
    0,     /* 09, F10 */
    0,     /* 0A, F8 */
    0,     /* 0B, F6 */
    0,     /* 0C, F4 */
    '\t',  /* 0D, TAB */
    '~',   /* 0E, ~ */
    0,     /* 0F, unused */
    0,     /* 10, unused */
    0,     /* 11, L ALT */
    kShiftKey,     /* 12, L SHFT */
    0,     /* 13, unused */
    0,     /* 14, L CTRL */
    'Q',   /* 15, Q */
    '!',   /* 16, ! */
    0,     /* 17, unused */
    0,     /* 18, unused */
    0,     /* 19, unused */
    'Z',   /* 1A, Z */
    'S',   /* 1B, S */
    'A',   /* 1C, A */
    'W',   /* 1D, W */
    '@',   /* 1E, @ */
    0,     /* 1F, unused */
    0,     /* 20, unused */
    'C',   /* 21, C */
    'X',   /* 22, X */
    'D',   /* 23, D */
    'E',   /* 24, E */
    '$',   /* 25, $ */
    '#',   /* 26, # */
    0,     /* 27, unused */
    0,     /* 28, unused */
    ' ',   /* 29, space */
    'V',   /* 2A, V */
    'F',   /* 2B, F */
    'T',   /* 2C, T */
    'R',   /* 2D, R */
    '%',   /* 2E, % */
    0,     /* 2F, unused */
    0,     /* 30, unused */
    'N',   /* 31, N */
    'B',   /* 32, B */
    'H',   /* 33, H */
    'G',   /* 34, G */
    'Y',   /* 35, Y */
    '^',   /* 36, ^ */
    0,     /* 37, unused */
    0,     /* 38, unused */
    0,     /* 39, unused */
    'M',   /* 3A, M */
    'J',   /* 3B, J */
    'U',   /* 3C, U */
    '&',   /* 3D, & */
    '*',   /* 3E, * */
    0,     /* 3F, unused */
    0,     /* 40, unused */
    '<',   /* 41, < */
    'K',   /* 42, K */
    'I',   /* 43, I */
    'O',   /* 44, O */
    ')',   /* 45, ) */
    '(',   /* 46, ( */
    0,     /* 47, unused */
    0,     /* 48, unused */
    '>',   /* 49, > */
    '?',   /* 4A, ? */
    'L',   /* 4B, L */
    ':',   /* 4C, : */
    'P',   /* 4D, P */
    '_',   /* 4E, _ */
    0,     /* 4F, unused */
    0,     /* 50, unused */
    0,     /* 51, unused */
    '"',   /* 52, " */
    0,     /* 53, unused */
    '{',   /* 54, { */
    '+',   /* 55, + */
    0,     /* 56, unused */
    0,     /* 57, unused */
    kCapsLockKey,  /* 58, caps */
    kShiftKey,     /* 59, R SHFT */
    0x0A,  /* 5A, ENTER */
    '}',   /* 5B, } */
    0,     /* 5C, unused */
    '|',   /* 5D, | */
    0,     /* 5E, unused */
    0,     /* 5F, unused */
    0, 0, 0, 0, 0, 0,  /* 60-65 */
    0x08,  /* 66, BKSP */
    0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 67-6F */
    0, 0, 0, 0, 0, 0,  /* 70-75 */
    0x1B,  /* 76, ESC */
    0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 77-7F */
    0, 0, 0, 0  /* 80-83 */
};

#define SCANCODE_TABLE_SIZE (sizeof(scancodeToAscii) / sizeof(unsigned char))

/* Inline nop macro */
#define nop() __asm__ __volatile__ ("nop\n\t")

/* Forward declarations */
static short GetHandshakeNibblePort2(short *hshkState);
static void PutHandshakeNibblePort2(short *hshkState, unsigned char byteToSend);
static unsigned char GetNextESKeyboardRawcode(void);
static void BackUpKeycodeTail(void);

/*
 * Find the ES (Eric Smith) keyboard on port 2
 */
static int FindESKeyboard(void)
{
    UChar readBuf[4];
    register UChar *readScan = readBuf;
    volatile register UChar *reg = (UChar *)kData2;
    short hshkState;
    register long timeout = 100;
    register ULong kbID = 0xC030609;

    *reg = kTH + kTR;
    *(char *)kSerial2 = 0;
    *(char *)kCtl2 = kTH + kTR;

    nop(); nop();
    *readScan++ = *reg & 0x0F;

    *reg = kTR;

    do {
        *readScan = *reg & 0x0F;
    } while ((*readScan != ((kbID >> 16) & 0xF)) && --timeout);

    if (!timeout) {
        *reg = kTH + kTR;
        return 0;
    }

    readScan++;

    hshkState = 0;
    *readScan++ = GetHandshakeNibblePort2(&hshkState);
    *readScan = GetHandshakeNibblePort2(&hshkState);

    *reg |= kTH;

    for (timeout = 0; timeout != 50; timeout++);

    *reg = kTH + kTR;

    if (*(ULong *)readBuf == kbID)
        return 1;
    else
        return 0;
}

/*
 * Read from ES keyboard
 */
static void ReadESKeyboard(void)
{
    UChar readBuf[4];
    register UChar *readScan = readBuf;
    volatile register UChar *reg = (UChar *)kData2;
    short hshkState;
    UChar len;
    UChar temp;
    register long timeout = 100;
    register ULong kbID = 0xC030609;

    *reg = kTH + kTR;
    *(char *)kSerial2 = 0;
    *(char *)kCtl2 = kTH + kTR;

    nop(); nop();
    *readScan++ = *reg & 0x0F;

    *reg = kTR;

    do {
        *readScan = *reg & 0x0F;
    } while ((*readScan != ((kbID >> 16) & 0xF)) && --timeout);

    if (!timeout) {
        *reg = kTH + kTR;
        return;
    }

    readScan++;

    hshkState = 0;
    *readScan++ = GetHandshakeNibblePort2(&hshkState);
    *readScan++ = GetHandshakeNibblePort2(&hshkState);

    if (*(ULong *)readBuf == kbID) {
        len = GetHandshakeNibblePort2(&hshkState);

        if (len) {
            temp = GetHandshakeNibblePort2(&hshkState);
            temp <<= 4;
            temp |= GetHandshakeNibblePort2(&hshkState);

            len--;

            if (temp == kESKeycodeData) {
                readScan = ControlGlobalz.keycodeBuf;
                while (len) {
                    ControlGlobalz.keycodeHead++;
                    ControlGlobalz.keycodeHead &= kKeybdDataFifoMask;
                    temp = GetHandshakeNibblePort2(&hshkState);
                    temp <<= 4;
                    temp |= GetHandshakeNibblePort2(&hshkState);
                    readScan[ControlGlobalz.keycodeHead] = temp;
                    len--;
                }
            } else {
                readScan = ControlGlobalz.statusBuf;
                while (len) {
                    ControlGlobalz.statusHead++;
                    ControlGlobalz.statusHead &= kKeybdCmdStatusFifoMask;
                    temp = GetHandshakeNibblePort2(&hshkState);
                    temp <<= 4;
                    temp |= GetHandshakeNibblePort2(&hshkState);
                    readScan[ControlGlobalz.statusHead] = temp;
                    len--;
                }
            }
        }

        *reg = kTH + kTR;
    }
}

/*
 * Write to ES keyboard (for LED control, etc.)
 */
static void WriteESKeyboard(void)
{
    UChar readBuf[4];
    register UChar *readScan = readBuf;
    volatile register UChar *reg = (UChar *)kData2;
    short hshkState;
    register long timeout = 100;
    register ULong kbID = 0xC030609;
    UChar byteToSend;

    if (ControlGlobalz.cmdTail != ControlGlobalz.cmdHead) {
        *reg = kTH + kTR;
        *(char *)kSerial2 = 0;
        *(char *)kCtl2 = kTH + kTR;

        nop(); nop();
        *readScan++ = *reg & 0x0F;

        *reg = kTR;

        do {
            *readScan = *reg & 0x0F;
        } while ((*readScan != ((kbID >> 16) & 0xF)) && --timeout);

        if (!timeout) {
            *reg = kTH + kTR;
            return;
        }

        readScan++;

        hshkState = 0;
        *readScan++ = GetHandshakeNibblePort2(&hshkState);

        if ((*(ULong *)readBuf & 0xFFFFFF00) == (kbID & 0xFFFFFF00)) {
            *reg &= 0xF0;
            *(char *)kCtl2 |= kDataLines;

            ControlGlobalz.cmdTail++;
            ControlGlobalz.cmdTail &= kKeybdCmdStatusFifoMask;
            byteToSend = ControlGlobalz.cmdBuf[ControlGlobalz.cmdTail];

            PutHandshakeNibblePort2(&hshkState, 0);
            PutHandshakeNibblePort2(&hshkState, 2);
            PutHandshakeNibblePort2(&hshkState, ((kESKeycodeData & 0xF0) >> 4));
            PutHandshakeNibblePort2(&hshkState, (kESKeycodeData & 0x0F));
            PutHandshakeNibblePort2(&hshkState, ((byteToSend & 0xF0) >> 4));
            PutHandshakeNibblePort2(&hshkState, (byteToSend & 0x0F));

            *(char *)kCtl2 &= ~kDataLines;
            *reg = kTH + kTR;
        }
    }
}

static short GetHandshakeNibblePort2(short *hshkState)
{
    register long timeout = 100;
    volatile register UChar *reg = (UChar *)kData2;

    if (*hshkState == -1)
        return 0x0F;

    if ((*hshkState ^= 1) == 0) {
        *reg |= kTR;
        nop(); nop();
        do {} while (!(*reg & kTL) && --timeout);
        if (timeout) {
            nop(); nop();
            return *reg & 0x0F;
        }
    } else {
        *reg &= ~kTR;
        nop(); nop();
        do {} while ((*reg & kTL) && --timeout);
        if (timeout) {
            nop(); nop();
            return *reg & 0x0F;
        }
    }

    *hshkState = -1;
    return 0xFF;
}

static void PutHandshakeNibblePort2(short *hshkState, unsigned char byteToSend)
{
    register long timeout = 100;
    volatile register UChar *reg = (UChar *)kData2;

    if (*hshkState == -1)
        return;

    *reg = (*reg & 0xF0) | byteToSend;

    if ((*hshkState ^= 1) == 0) {
        *reg |= kTR;
        nop(); nop();
        do {} while (!(*reg & kTL) && --timeout);
        if (timeout)
            return;
    } else {
        *reg &= ~kTR;
        nop(); nop();
        do {} while ((*reg & kTL) && --timeout);
        if (timeout)
            return;
    }

    *hshkState = -1;
}

static void SendCmdToESKeyboard(unsigned char *cmdBuf, unsigned char cmdLen)
{
    while (cmdLen) {
        ControlGlobalz.cmdHead++;
        ControlGlobalz.cmdHead &= kKeybdCmdStatusFifoMask;
        ControlGlobalz.cmdBuf[ControlGlobalz.cmdHead] = *cmdBuf;
        cmdBuf++;
        cmdLen--;
    }
}

static void BackUpKeycodeTail(void)
{
    if (ControlGlobalz.keycodeTail == 0)
        ControlGlobalz.keycodeTail = kKeybdDataFifoMask;
    else
        ControlGlobalz.keycodeTail--;
}

static unsigned char GetNextESKeyboardRawcode(void)
{
    if (ControlGlobalz.keycodeTail != ControlGlobalz.keycodeHead) {
        ControlGlobalz.keycodeTail++;
        ControlGlobalz.keycodeTail &= kKeybdDataFifoMask;
        return ControlGlobalz.keycodeBuf[ControlGlobalz.keycodeTail];
    } else {
        return 0xFF;
    }
}

/*
 * Get next character from keyboard (handles scancodes, shift, caps lock)
 */
static unsigned char GetNextESKeyboardChar(void)
{
    unsigned char raw;
    unsigned char map;
    char specialPending;
    unsigned char ledCmd[2];

    specialPending = 0;
    map = kNoKey;

    raw = GetNextESKeyboardRawcode();

    /* BREAK (KEYUP) CODES */
    if (raw == 0xF0) {
        raw = GetNextESKeyboardRawcode();

        if (raw == 0xFF) {
            BackUpKeycodeTail();
            return kNoKey;
        }

        if (raw < SCANCODE_TABLE_SIZE) {
            map = scancodeToAscii[raw];

            if (map == kShiftKey)
                ControlGlobalz.keyboardFlags &= ~kShiftDown;

            if (map == kAltKey)
                ControlGlobalz.keyboardFlags &= ~kAltDown;

            if (map == kControlKey)
                ControlGlobalz.keyboardFlags &= ~kControlDown;

            if (map == kCapsLockKey)
                ControlGlobalz.keyboardFlags &= ~kCapsLockDown;
        }

        return kNoKey;
    }

    /* 101-STYLE CODES (extended keys) */
    if (raw == 0xE0) {
        raw = GetNextESKeyboardRawcode();

        if (raw == 0xFF) {
            BackUpKeycodeTail();
            return kNoKey;
        } else {
            if (raw == 0xF0) {
                raw = GetNextESKeyboardRawcode();
                if (raw == 0xFF) {
                    BackUpKeycodeTail();
                    BackUpKeycodeTail();
                }
                return kNoKey;
            }
            specialPending = 1;
        }
    }

    /* NORMAL CODES */
    if (raw < SCANCODE_TABLE_SIZE) {
        map = scancodeToAscii[raw];

        if (ControlGlobalz.keyboardFlags & kShiftDown) {
            if ((map >= 0x20) && (map <= 0x7E)) {
                map = scancodeToAsciiShifted[raw];
            }
        } else if (ControlGlobalz.keyboardFlags & kCapsLocked) {
            if ((map >= 'a') && (map <= 'z')) {
                map = scancodeToAsciiShifted[raw];
            }
        }
    }

    /* META KEYS */
    switch (map) {
        case kShiftKey:
            ControlGlobalz.keyboardFlags |= kShiftDown;
            return kNoKey;

        case kAltKey:
            ControlGlobalz.keyboardFlags |= kAltDown;
            return kNoKey;

        case kControlKey:
            ControlGlobalz.keyboardFlags |= kControlDown;
            return kNoKey;
    }

    if ((map == kCapsLockKey) && !(ControlGlobalz.keyboardFlags & kCapsLockDown)) {
        ControlGlobalz.keyboardFlags |= kCapsLockDown;
        ControlGlobalz.keyboardFlags ^= kCapsLocked;
        ledCmd[0] = 0xED;
        ledCmd[1] = ControlGlobalz.keyboardFlags & kCapsLocked;
        SendCmdToESKeyboard(ledCmd, 2);
        return kNoKey;
    }

    /* Special keys (arrow keys, etc.) */
    if (specialPending) {
        switch (raw) {
            case 0x75: map = kUpArrowKey; break;
            case 0x72: map = kDownArrowKey; break;
            case 0x6B: map = kLeftArrowKey; break;
            case 0x74: map = kRightArrowKey; break;
            case 0x5A: map = kEnterKey; break;
            default: map = kNoKey; break;
        }
    }

    return map;
}

/*
 * Emulate joypad with keyboard - puts characters in system key buffer
 */
static void EmulateJoypadWithKeyboard(void)
{
    unsigned char key;

    key = GetNextESKeyboardChar();

    ControlGlobalz.sysKeysHead++;
    ControlGlobalz.sysKeysHead &= kSysKeysFifoMask;
    ControlGlobalz.sysKeysBuf[ControlGlobalz.sysKeysHead] = key;
}

/*
 * Get next hardware keyboard character
 */
static unsigned char GetNextHardwareKeyboardChar(void)
{
    if (ControlGlobalz.sysKeysTail != ControlGlobalz.sysKeysHead) {
        ControlGlobalz.sysKeysTail++;
        ControlGlobalz.sysKeysTail &= kSysKeysFifoMask;
        return ControlGlobalz.sysKeysBuf[ControlGlobalz.sysKeysTail];
    } else {
        return kNoKey;
    }
}

/* Debug: track if keyboard was ever found */
static int keyboard_found_once = 0;

/*
 * Read keyboard during vblank
 */
void genesis_read_keyboard(void)
{
    if (FindESKeyboard()) {
        if (!keyboardConnected) {
            unsigned char resetCmd = 0xFF;
            SendCmdToESKeyboard(&resetCmd, 1);
            WriteESKeyboard();
            keyboardConnected = 1;
            ControlGlobalz.keyboardFlags = 0L;
            if (!keyboard_found_once) {
                printk("XBAND keyboard detected!\n");
                keyboard_found_once = 1;
            }
        }

        ReadESKeyboard();
        WriteESKeyboard();
        EmulateJoypadWithKeyboard();
    } else {
        keyboardConnected = 0;
    }
}

/*
 * Process keyboard input and feed to TTY
 * Called from main loop or timer context
 */
void genesis_process_keyboard(void)
{
    unsigned char ch;
    struct tty_struct *tty;

    ch = GetNextHardwareKeyboardChar();

    if (ch != kNoKey && ch != 0) {
        /* Debug: echo the character we got */
        if (ch >= 0x20 && ch <= 0x7E) {
            printk("%c", ch);  /* printable char */
        } else if (ch == '\n' || ch == 0x0A) {
            printk("\n");
        } else if (ch == 0x08) {
            printk("<BS>");
        }

        /* Get the TTY for ttyS0 */
        tty = genesis_tty_table[0];

        if (tty) {
            /* Feed character to TTY input buffer */
            tty_insert_flip_char(tty, ch, 0);
            tty_schedule_flip(tty);
        }
    }
}

/*
 * Initialize keyboard subsystem
 */
void genesis_keyboard_init(void)
{
    /* Clear control globals */
    ControlGlobalz.sysKeysHead = 0;
    ControlGlobalz.sysKeysTail = 0;
    ControlGlobalz.keycodeHead = 0;
    ControlGlobalz.keycodeTail = 0;
    ControlGlobalz.statusHead = 0;
    ControlGlobalz.statusTail = 0;
    ControlGlobalz.cmdHead = 0;
    ControlGlobalz.cmdTail = 0;
    ControlGlobalz.keyboardFlags = 0;

    keyboardConnected = 0;

    printk("Genesis keyboard driver initialized\n");
}


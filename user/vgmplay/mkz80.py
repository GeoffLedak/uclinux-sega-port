#!/usr/bin/env python3
"""A small Z80 assembler: enough for the music player (player.z80).

    python3 mkz80.py player.z80 z80prog.h

It writes the program as a C array, z80_prog[], plus a #define for each
label whose name is in capitals (the addresses vgmplay.c needs to know).

The source: one instruction or directive per line, ';' starts a comment,
"name:" defines a label and "name equ value" a constant. Numbers are
decimal, 0x1F or 1Fh. Expressions may use + - * / & | << >> and ( ).
Directives: org (pads up to the address), db, dw, ds (zeros).

Only the instructions listed in encode() are known; anything else is an
error, so a typo can't slip through as something different.
"""
import re
import sys

R8 = {'b': 0, 'c': 1, 'd': 2, 'e': 3, 'h': 4, 'l': 5, '(hl)': 6, 'a': 7}
R16 = {'bc': 0, 'de': 1, 'hl': 2, 'sp': 3}
R16AF = {'bc': 0, 'de': 1, 'hl': 2, 'af': 3}
CC = {'nz': 0, 'z': 1, 'nc': 2, 'c': 3, 'po': 4, 'pe': 5, 'p': 6, 'm': 7}
ALU = {'add': 0, 'adc': 1, 'sub': 2, 'sbc': 3, 'and': 4, 'xor': 5, 'or': 6, 'cp': 7}
ROT = {'rlc': 0, 'rrc': 1, 'rl': 2, 'rr': 3, 'sla': 4, 'sra': 5, 'srl': 7}
SIMPLE = {'nop': [0x00], 'halt': [0x76], 'di': [0xF3], 'ei': [0xFB],
          'ret': [0xC9], 'rlca': [0x07], 'rrca': [0x0F], 'rla': [0x17],
          'rra': [0x1F], 'cpl': [0x2F], 'scf': [0x37], 'ccf': [0x3F],
          'neg': [0xED, 0x44], 'exx': [0xD9], 'im1': [0xED, 0x56]}


class AsmError(Exception):
    pass


def number(tok):
    tok = tok.strip()
    if re.fullmatch(r'0x[0-9a-fA-F]+', tok):
        return int(tok, 16)
    if re.fullmatch(r'[0-9][0-9a-fA-F]*[hH]', tok):
        return int(tok[:-1], 16)
    if re.fullmatch(r'[0-9]+', tok):
        return int(tok)
    raise AsmError('bad number ' + tok)


def evaluate(expr, syms, final):
    """Value of expr; while not final, unknown labels count as 0."""
    def name(m):
        n = m.group(0)
        if re.fullmatch(r'0x[0-9a-fA-F]+|[0-9][0-9a-fA-F]*[hH]|[0-9]+', n):
            return str(number(n))
        if n in syms:
            return str(syms[n])
        if final:
            raise AsmError('unknown name ' + n)
        return '0'
    py = re.sub(r'[A-Za-z_0-9]+', name, expr)
    if not re.fullmatch(r'[0-9+\-*/&|<>() ]*', py):
        raise AsmError('bad expression ' + expr)
    return int(eval(py.replace('/', '//')))


def split_args(s):
    return [a.strip() for a in s.split(',')] if s.strip() else []


def is_mem(a):
    return a.startswith('(') and a.endswith(')') and a.lower() not in ('(hl)', '(bc)', '(de)')


def encode(op, args, pc, val):
    """The bytes of one instruction. val(expr) evaluates an expression."""
    la = [a.lower() for a in args]
    n = len(args)

    def imm8(e):
        v = val(e)
        if not -128 <= v <= 255:
            raise AsmError('%s does not fit in a byte' % e)
        return v & 0xFF

    def imm16(e):
        v = val(e) & 0xFFFF
        return [v & 0xFF, v >> 8]

    def rel(e):
        d = val(e) - (pc + 2)
        if FINAL[0] and not -128 <= d <= 127:
            raise AsmError('jump to %s is too far' % e)
        return d & 0xFF

    if op in SIMPLE and n == 0:
        return SIMPLE[op]
    if op == 'ld' and n == 2:
        d, s = la
        if d in R8 and s in R8 and not (d == s == '(hl)'):
            return [0x40 | R8[d] << 3 | R8[s]]
        if d in R8 and not is_mem(args[1]) and s not in R16 and s not in ('(bc)', '(de)'):
            return [0x06 | R8[d] << 3, imm8(args[1])]
        if d == 'a' and s == '(bc)':
            return [0x0A]
        if d == 'a' and s == '(de)':
            return [0x1A]
        if d == '(bc)' and s == 'a':
            return [0x02]
        if d == '(de)' and s == 'a':
            return [0x12]
        if d == 'a' and is_mem(args[1]):
            return [0x3A] + imm16(args[1][1:-1])
        if is_mem(args[0]) and s == 'a':
            return [0x32] + imm16(args[0][1:-1])
        if d == 'hl' and is_mem(args[1]):
            return [0x2A] + imm16(args[1][1:-1])
        if is_mem(args[0]) and s == 'hl':
            return [0x22] + imm16(args[0][1:-1])
        if d in R16 and is_mem(args[1]):
            return [0xED, 0x4B | R16[d] << 4] + imm16(args[1][1:-1])
        if is_mem(args[0]) and s in R16:
            return [0xED, 0x43 | R16[s] << 4] + imm16(args[0][1:-1])
        if d == 'sp' and s == 'hl':
            return [0xF9]
        if d in R16:
            return [0x01 | R16[d] << 4] + imm16(args[1])
    if op in ('inc', 'dec') and n == 1:
        a = la[0]
        if a in R8:
            return [(0x04 if op == 'inc' else 0x05) | R8[a] << 3]
        if a in R16:
            return [(0x03 if op == 'inc' else 0x0B) | R16[a] << 4]
    if op in ALU:
        if n == 2 and la[0] == 'a':
            la, args, n = la[1:], args[1:], 1
        if n == 1:
            if la[0] in R8:
                return [0x80 | ALU[op] << 3 | R8[la[0]]]
            return [0xC6 | ALU[op] << 3, imm8(args[0])]
        if n == 2 and la[0] == 'hl' and la[1] in R16:
            if op == 'add':
                return [0x09 | R16[la[1]] << 4]
            if op == 'adc':
                return [0xED, 0x4A | R16[la[1]] << 4]
            if op == 'sbc':
                return [0xED, 0x42 | R16[la[1]] << 4]
    if op == 'jr':
        if n == 1:
            return [0x18, rel(args[0])]
        if n == 2 and la[0] in ('nz', 'z', 'nc', 'c'):
            return [0x20 | CC[la[0]] << 3, rel(args[1])]
    if op == 'djnz' and n == 1:
        return [0x10, rel(args[0])]
    if op == 'jp':
        if n == 1 and la[0] == '(hl)':
            return [0xE9]
        if n == 1:
            return [0xC3] + imm16(args[0])
        if n == 2 and la[0] in CC:
            return [0xC2 | CC[la[0]] << 3] + imm16(args[1])
    if op == 'call':
        if n == 1:
            return [0xCD] + imm16(args[0])
        if n == 2 and la[0] in CC:
            return [0xC4 | CC[la[0]] << 3] + imm16(args[1])
    if op == 'ret' and n == 1 and la[0] in CC:
        return [0xC0 | CC[la[0]] << 3]
    if op in ('push', 'pop') and n == 1 and la[0] in R16AF:
        return [(0xC5 if op == 'push' else 0xC1) | R16AF[la[0]] << 4]
    if op == 'ex' and la == ['de', 'hl']:
        return [0xEB]
    if op in ('bit', 'res', 'set') and n == 2 and la[1] in R8:
        b = val(args[0])
        if not 0 <= b <= 7:
            raise AsmError('bad bit number')
        return [0xCB, {'bit': 0x40, 'res': 0x80, 'set': 0xC0}[op] | b << 3 | R8[la[1]]]
    if op in ROT and n == 1 and la[0] in R8:
        return [0xCB, ROT[op] << 3 | R8[la[0]]]
    raise AsmError('unknown instruction: %s %s' % (op, ', '.join(args)))


FINAL = [False]


def assemble(lines):
    syms = {}
    for final in (False, True, True):
        FINAL[0] = final
        out = []
        pc = 0
        pass_labels = {}
        for num, raw in enumerate(lines, 1):
            line = raw.split(';', 1)[0].rstrip()
            try:
                m = re.match(r'\s*([A-Za-z_][A-Za-z_0-9]*):(.*)', line)
                if m:
                    if not final and pass_labels.get(m.group(1), pc) != pc:
                        raise AsmError('label twice: ' + m.group(1))
                    pass_labels[m.group(1)] = pc
                    syms[m.group(1)] = pc
                    line = m.group(2)
                m = re.match(r'\s*([A-Za-z_][A-Za-z_0-9]*)\s+equ\s+(.*)', line, re.I)
                if m:
                    syms[m.group(1)] = evaluate(m.group(2), syms, final)
                    continue
                if not line.strip():
                    continue
                m = re.match(r'\s*(\S+)\s*(.*)', line)
                op, rest = m.group(1).lower(), m.group(2)
                val = lambda e: evaluate(e, syms, final)
                if op == 'org':
                    to = val(rest)
                    if to < pc:
                        raise AsmError('org 0x%X is behind 0x%X' % (to, pc))
                    out += [0] * (to - pc)
                    pc = to
                    continue
                if op == 'db':
                    b = [val(a) & 0xFF for a in split_args(rest)]
                elif op == 'dw':
                    b = []
                    for a in split_args(rest):
                        v = val(a) & 0xFFFF
                        b += [v & 0xFF, v >> 8]
                elif op == 'ds':
                    b = [0] * val(rest)
                elif op == 'im' and rest.strip() == '1':
                    b = SIMPLE['im1']
                else:
                    b = encode(op, split_args(rest), pc, val)
                out += b
                pc += len(b)
            except AsmError as e:
                sys.exit('line %d: %s\n  %s' % (num, e, raw.rstrip()))
    return out, syms


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    code, syms = assemble(open(src).read().splitlines())
    with open(dst, 'w') as f:
        f.write('/* GENERATED by mkz80.py from %s - do not edit. */\n' % src)
        for name in sorted(syms, key=lambda k: syms[k]):
            if name.isupper():
                f.write('#define Z80_%s\t0x%04X\n' % (name, syms[name]))
        f.write('\nstatic const unsigned char z80_prog[%d] = {\n' % len(code))
        for i in range(0, len(code), 12):
            f.write('\t' + ' '.join('0x%02X,' % b for b in code[i:i + 12]) + '\n')
        f.write('};\n')
    print('%s: %d bytes' % (dst, len(code)))


if __name__ == '__main__':
    main()

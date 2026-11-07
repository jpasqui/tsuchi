"""Runs a patch's instructions on the host, enough of ARM64 for what tools/gen/stubs.py and scatter.py
write and for the branches and address loads a native route can displace.

A result here says what the instructions do. It is not a result in the game. An instruction listed in
`opaque` does nothing here and is counted, which is how a test sees a replaced instruction run once.
"""


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


MASK = (1 << 64) - 1


class Machine:
    def __init__(self, words, memory=None, opaque=()):
        self.words = words                     # {address: instruction}
        self.memory = dict(memory or {})       # {address: byte}
        self.opaque = set(opaque)
        self.ran = []
        self.x = [0x1000 + 0x111 * n for n in range(31)]
        self.sp = 0x7F000000
        self.nzcv = 0
        self.pc = 0

    def state(self):
        return (tuple(self.x), self.sp, self.nzcv)

    def read(self, address, size):
        return int.from_bytes(bytes(self.memory.get(address + i, 0) for i in range(size)), 'little')

    def write(self, address, value, size):
        for i, byte in enumerate(int(value & ((1 << (8 * size)) - 1)).to_bytes(size, 'little')):
            self.memory[address + i] = byte

    def get(self, n, sp=False):
        return (self.sp if sp else 0) if n == 31 else self.x[n]

    def put(self, n, value, sp=False):
        if n == 31:
            if sp:
                self.sp = value & MASK
        else:
            self.x[n] = value & MASK

    def compare(self, left, right, bits):
        mask = (1 << bits) - 1
        left, right = left & mask, right & mask
        result = (left - right) & mask
        n = result >> (bits - 1)
        z = int(result == 0)
        c = int(left >= right)
        v = ((left ^ right) & (left ^ result)) >> (bits - 1)
        self.nzcv = (n << 31) | (z << 30) | (c << 29) | (v << 28)

    def holds(self, condition):
        n, z, c, v = (self.nzcv >> 31) & 1, (self.nzcv >> 30) & 1, (self.nzcv >> 29) & 1, (self.nzcv >> 28) & 1
        result = {0: z, 1: c, 2: n, 3: v, 4: c and not z, 5: n == v, 6: n == v and not z, 7: True}[condition >> 1]
        return bool(result) != bool(condition & 1 and condition != 15)

    def step(self):
        pc = self.pc
        if pc not in self.words:
            raise ValueError(f'nothing to run at {pc:#x}')
        word = self.words[pc]
        self.pc = pc + 4
        if word in self.opaque:
            self.ran.append(word)
            return
        if word == 0xD503201F or (word & 0xFFFFF01F) == 0xD503201F:             # nop and other hints
            return
        rd, rn, rt2 = word & 31, (word >> 5) & 31, (word >> 10) & 31
        if (word & 0x7C000000) == 0x14000000:                                    # b, bl
            if word >> 31:
                self.x[30] = pc + 4
            self.pc = pc + 4 * signed(word & 0x3FFFFFF, 26)
        elif (word & 0xFF000010) == 0x54000000:                                  # b.cond
            if self.holds(word & 15):
                self.pc = pc + 4 * signed((word >> 5) & 0x7FFFF, 19)
        elif (word & 0x7E000000) == 0x34000000:                                  # cbz, cbnz
            value = self.get(rd) & (MASK if word >> 31 else 0xFFFFFFFF)
            if (value == 0) != bool(word & 0x01000000):
                self.pc = pc + 4 * signed((word >> 5) & 0x7FFFF, 19)
        elif (word & 0x7E000000) == 0x36000000:                                  # tbz, tbnz
            bit = ((word >> 31) << 5) | ((word >> 19) & 31)
            if bool((self.get(rd) >> bit) & 1) == bool(word & 0x01000000):
                self.pc = pc + 4 * signed((word >> 5) & 0x3FFF, 14)
        elif (word & 0xFFFFFC1F) == 0xD63F0000:                                  # blr
            target = self.get(rn)
            self.x[30] = pc + 4
            self.pc = target
        elif (word & 0xFFFFFC1F) in (0xD61F0000, 0xD65F0000):                    # br, ret
            self.pc = self.get(rn)
        elif (word & 0x1F000000) == 0x10000000:                                  # adr, adrp
            offset = signed(((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2), 21)
            self.put(rd, ((pc & ~4095) + (offset << 12)) if word >> 31 else pc + offset)
        elif (word & 0xFF800000) in (0x91000000, 0xD1000000):                    # add, sub with an immediate
            amount = ((word >> 10) & 0xFFF) * (-1 if word >> 30 & 1 else 1)
            self.put(rd, self.get(rn, True) + amount, True)
        elif (word & 0xFFE0E000) == 0x8B204000:                                  # add with an extended w register
            self.put(rd, self.get(rn, True) + ((self.get((word >> 16) & 31) & 0xFFFFFFFF) << ((word >> 10) & 7)), True)
        elif (word & 0xFFE0FFE0) == 0xAA0003E0:                                  # mov
            self.put(rd, self.get((word >> 16) & 31))
        elif (word & 0x7F800000) == 0x52800000 or (word & 0x7F800000) == 0x72800000:  # movz, movk
            shift = ((word >> 21) & 3) * 16
            value = ((word >> 5) & 0xFFFF) << shift
            if word & 0x20000000:
                value |= self.get(rd) & ~(0xFFFF << shift)
            self.put(rd, value if word >> 31 else value & 0xFFFFFFFF)
        elif (word & 0x7F80001F) == 0x7100001F:                                  # cmp with an immediate
            self.compare(self.get(rn), (word >> 10) & 0xFFF, 64 if word >> 31 else 32)
        elif (word & 0x7FE0FC1F) == 0x6B00001F:                                  # cmp with a register
            self.compare(self.get(rn), self.get((word >> 16) & 31), 64 if word >> 31 else 32)
        elif (word & 0xFFFFFFE0) == 0xD53B4200:                                  # mrs nzcv
            self.put(rd, self.nzcv)
        elif (word & 0xFFFFFFE0) == 0xD51B4200:                                  # msr nzcv
            self.nzcv = self.get(rd) & 0xF0000000
        elif (word & 0xFFC00000) in (0xA9800000, 0xA9C00000, 0xA8800000, 0xA8C00000, 0xA9000000, 0xA9400000):  # stp, ldp
            offset = signed((word >> 15) & 0x7F, 7) * 8
            base = self.get(rn, True)
            at = base + offset if word & 0x01000000 else base
            if word & 0x00400000:
                first, second = self.read(at, 8), self.read(at + 8, 8)
                self.put(rd, first)
                self.put(rt2, second)
            else:
                self.write(at, self.get(rd), 8)
                self.write(at + 8, self.get(rt2), 8)
            if word & 0x00800000:
                self.put(rn, base + offset, True)
        elif (word & 0xFFC00000) in (0xF9400000, 0xB9400000, 0xF9000000):        # ldr and str with an unsigned offset
            size = 8 if word >> 30 == 3 else 4
            at = self.get(rn, True) + ((word >> 10) & 0xFFF) * size
            if word & 0x00400000:
                self.put(rd, self.read(at, size))
            else:
                self.write(at, self.get(rd), size)
        elif (word & 0xBF000000) == 0x18000000:                                  # ldr from a literal
            size = 8 if word >> 30 else 4
            self.put(rd, self.read(pc + 4 * signed((word >> 5) & 0x7FFFF, 19), size))
        else:
            raise ValueError(f'{word:#010x} at {pc:#x} is not one of the instructions this runs')

    def run(self, until, most=400):
        """Runs until the program counter is in `until`, or says where it went instead."""
        for _ in range(most):
            if self.pc in until:
                return self.pc
            self.step()
        raise ValueError(f'still running after {most} instructions, at {self.pc:#x}')

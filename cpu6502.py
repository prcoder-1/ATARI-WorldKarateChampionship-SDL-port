"""Minimal but complete 6502 emulator, enough to run the game's sprite-decode
routines over a RAM snapshot and read back the rendered output.
No hardware — plain 64K RAM. Used to faithfully render the ROM's shape data."""

class CPU:
    def __init__(self, mem):
        self.m = bytearray(mem)          # 64K
        self.a = self.x = self.y = 0
        self.sp = 0xFD
        self.pc = 0
        self.C=self.Z=self.I=self.D=self.B=self.V=self.N=0
        self.cycles = 0

    # --- flag helpers ---
    def setzn(self, v):
        v &= 0xFF; self.Z = 1 if v == 0 else 0; self.N = 1 if v & 0x80 else 0; return v
    def push(self, v): self.m[0x100 + self.sp] = v & 0xFF; self.sp = (self.sp - 1) & 0xFF
    def pop(self): self.sp = (self.sp + 1) & 0xFF; return self.m[0x100 + self.sp]
    def rd(self, a): return self.m[a & 0xFFFF]
    def wr(self, a, v): self.m[a & 0xFFFF] = v & 0xFF
    def rd16(self, a): return self.rd(a) | (self.rd((a & 0xFF00) | ((a + 1) & 0xFF)) << 8)  # zp wrap

    # --- addressing ---
    def imm(self): a = self.pc; self.pc += 1; return a
    def zp(self): a = self.rd(self.pc); self.pc += 1; return a
    def zpx(self): a = (self.rd(self.pc) + self.x) & 0xFF; self.pc += 1; return a
    def zpy(self): a = (self.rd(self.pc) + self.y) & 0xFF; self.pc += 1; return a
    def ab(self): a = self.rd(self.pc) | (self.rd(self.pc + 1) << 8); self.pc += 2; return a
    def abx(self): return (self.ab() + self.x) & 0xFFFF
    def aby(self): return (self.ab() + self.y) & 0xFFFF
    def indx(self): p = (self.rd(self.pc) + self.x) & 0xFF; self.pc += 1; return self.rd16(p)
    def indy(self): p = self.rd(self.pc); self.pc += 1; return (self.rd16(p) + self.y) & 0xFFFF

    def flags(self):
        return (self.N<<7)|(self.V<<6)|0x20|(self.B<<4)|(self.D<<3)|(self.I<<2)|(self.Z<<1)|self.C
    def setflags(self, p):
        self.N=(p>>7)&1; self.V=(p>>6)&1; self.D=(p>>3)&1; self.I=(p>>2)&1; self.Z=(p>>1)&1; self.C=p&1

    def branch(self, cond):
        off = self.rd(self.pc); self.pc += 1
        if cond:
            if off & 0x80: off -= 0x100
            self.pc = (self.pc + off) & 0xFFFF

    def adc(self, v):
        if self.D:
            al=(self.a&0xF)+(v&0xF)+self.C
            if al>9: al+=6
            ah=(self.a>>4)+(v>>4)+(1 if al>0xF else 0)
            self.Z=1 if ((self.a+v+self.C)&0xFF)==0 else 0
            self.N=(ah>>3)&1; self.V=0
            if ah>9: ah+=6
            self.C=1 if ah>0xF else 0
            self.a=((ah<<4)|(al&0xF))&0xFF
        else:
            s=self.a+v+self.C
            self.V=1 if (~(self.a^v)&(self.a^s)&0x80) else 0
            self.C=1 if s>0xFF else 0; self.a=self.setzn(s)
    def sbc(self, v): self.adc(v ^ 0xFF)
    def cmp_(self, r, v):
        t=(r-v)&0x1FF; self.C=1 if r>=v else 0; self.setzn(t&0xFF)

    def step(self):
        op = self.rd(self.pc); self.pc = (self.pc + 1) & 0xFFFF
        m = self
        # ---- big dispatch ----
        if op==0xEA: pass
        elif op==0xA9: m.a=m.setzn(m.rd(m.imm()))
        elif op==0xA5: m.a=m.setzn(m.rd(m.zp()))
        elif op==0xB5: m.a=m.setzn(m.rd(m.zpx()))
        elif op==0xAD: m.a=m.setzn(m.rd(m.ab()))
        elif op==0xBD: m.a=m.setzn(m.rd(m.abx()))
        elif op==0xB9: m.a=m.setzn(m.rd(m.aby()))
        elif op==0xA1: m.a=m.setzn(m.rd(m.indx()))
        elif op==0xB1: m.a=m.setzn(m.rd(m.indy()))
        elif op==0xA2: m.x=m.setzn(m.rd(m.imm()))
        elif op==0xA6: m.x=m.setzn(m.rd(m.zp()))
        elif op==0xB6: m.x=m.setzn(m.rd(m.zpy()))
        elif op==0xAE: m.x=m.setzn(m.rd(m.ab()))
        elif op==0xBE: m.x=m.setzn(m.rd(m.aby()))
        elif op==0xA0: m.y=m.setzn(m.rd(m.imm()))
        elif op==0xA4: m.y=m.setzn(m.rd(m.zp()))
        elif op==0xB4: m.y=m.setzn(m.rd(m.zpx()))
        elif op==0xAC: m.y=m.setzn(m.rd(m.ab()))
        elif op==0xBC: m.y=m.setzn(m.rd(m.abx()))
        elif op==0x85: m.wr(m.zp(),m.a)
        elif op==0x95: m.wr(m.zpx(),m.a)
        elif op==0x8D: m.wr(m.ab(),m.a)
        elif op==0x9D: m.wr(m.abx(),m.a)
        elif op==0x99: m.wr(m.aby(),m.a)
        elif op==0x81: m.wr(m.indx(),m.a)
        elif op==0x91: m.wr(m.indy(),m.a)
        elif op==0x86: m.wr(m.zp(),m.x)
        elif op==0x96: m.wr(m.zpy(),m.x)
        elif op==0x8E: m.wr(m.ab(),m.x)
        elif op==0x84: m.wr(m.zp(),m.y)
        elif op==0x94: m.wr(m.zpx(),m.y)
        elif op==0x8C: m.wr(m.ab(),m.y)
        elif op==0xAA: m.x=m.setzn(m.a)
        elif op==0xA8: m.y=m.setzn(m.a)
        elif op==0x8A: m.a=m.setzn(m.x)
        elif op==0x98: m.a=m.setzn(m.y)
        elif op==0xBA: m.x=m.setzn(m.sp)
        elif op==0x9A: m.sp=m.x
        elif op==0x48: m.push(m.a)
        elif op==0x68: m.a=m.setzn(m.pop())
        elif op==0x08: m.push(m.flags()|0x10)
        elif op==0x28: m.setflags(m.pop())
        elif op==0x18: m.C=0
        elif op==0x38: m.C=1
        elif op==0x58: m.I=0
        elif op==0x78: m.I=1
        elif op==0xB8: m.V=0
        elif op==0xD8: m.D=0
        elif op==0xF8: m.D=1
        elif op==0x69: m.adc(m.rd(m.imm()))
        elif op==0x65: m.adc(m.rd(m.zp()))
        elif op==0x75: m.adc(m.rd(m.zpx()))
        elif op==0x6D: m.adc(m.rd(m.ab()))
        elif op==0x7D: m.adc(m.rd(m.abx()))
        elif op==0x79: m.adc(m.rd(m.aby()))
        elif op==0x61: m.adc(m.rd(m.indx()))
        elif op==0x71: m.adc(m.rd(m.indy()))
        elif op==0xE9: m.sbc(m.rd(m.imm()))
        elif op==0xE5: m.sbc(m.rd(m.zp()))
        elif op==0xF5: m.sbc(m.rd(m.zpx()))
        elif op==0xED: m.sbc(m.rd(m.ab()))
        elif op==0xFD: m.sbc(m.rd(m.abx()))
        elif op==0xF9: m.sbc(m.rd(m.aby()))
        elif op==0xE1: m.sbc(m.rd(m.indx()))
        elif op==0xF1: m.sbc(m.rd(m.indy()))
        elif op==0xC9: m.cmp_(m.a,m.rd(m.imm()))
        elif op==0xC5: m.cmp_(m.a,m.rd(m.zp()))
        elif op==0xD5: m.cmp_(m.a,m.rd(m.zpx()))
        elif op==0xCD: m.cmp_(m.a,m.rd(m.ab()))
        elif op==0xDD: m.cmp_(m.a,m.rd(m.abx()))
        elif op==0xD9: m.cmp_(m.a,m.rd(m.aby()))
        elif op==0xC1: m.cmp_(m.a,m.rd(m.indx()))
        elif op==0xD1: m.cmp_(m.a,m.rd(m.indy()))
        elif op==0xE0: m.cmp_(m.x,m.rd(m.imm()))
        elif op==0xE4: m.cmp_(m.x,m.rd(m.zp()))
        elif op==0xEC: m.cmp_(m.x,m.rd(m.ab()))
        elif op==0xC0: m.cmp_(m.y,m.rd(m.imm()))
        elif op==0xC4: m.cmp_(m.y,m.rd(m.zp()))
        elif op==0xCC: m.cmp_(m.y,m.rd(m.ab()))
        elif op==0x29: m.a=m.setzn(m.a & m.rd(m.imm()))
        elif op==0x25: m.a=m.setzn(m.a & m.rd(m.zp()))
        elif op==0x35: m.a=m.setzn(m.a & m.rd(m.zpx()))
        elif op==0x2D: m.a=m.setzn(m.a & m.rd(m.ab()))
        elif op==0x3D: m.a=m.setzn(m.a & m.rd(m.abx()))
        elif op==0x39: m.a=m.setzn(m.a & m.rd(m.aby()))
        elif op==0x21: m.a=m.setzn(m.a & m.rd(m.indx()))
        elif op==0x31: m.a=m.setzn(m.a & m.rd(m.indy()))
        elif op==0x09: m.a=m.setzn(m.a | m.rd(m.imm()))
        elif op==0x05: m.a=m.setzn(m.a | m.rd(m.zp()))
        elif op==0x15: m.a=m.setzn(m.a | m.rd(m.zpx()))
        elif op==0x0D: m.a=m.setzn(m.a | m.rd(m.ab()))
        elif op==0x1D: m.a=m.setzn(m.a | m.rd(m.abx()))
        elif op==0x19: m.a=m.setzn(m.a | m.rd(m.aby()))
        elif op==0x01: m.a=m.setzn(m.a | m.rd(m.indx()))
        elif op==0x11: m.a=m.setzn(m.a | m.rd(m.indy()))
        elif op==0x49: m.a=m.setzn(m.a ^ m.rd(m.imm()))
        elif op==0x45: m.a=m.setzn(m.a ^ m.rd(m.zp()))
        elif op==0x55: m.a=m.setzn(m.a ^ m.rd(m.zpx()))
        elif op==0x4D: m.a=m.setzn(m.a ^ m.rd(m.ab()))
        elif op==0x5D: m.a=m.setzn(m.a ^ m.rd(m.abx()))
        elif op==0x59: m.a=m.setzn(m.a ^ m.rd(m.aby()))
        elif op==0x41: m.a=m.setzn(m.a ^ m.rd(m.indx()))
        elif op==0x51: m.a=m.setzn(m.a ^ m.rd(m.indy()))
        elif op in (0x0A,0x06,0x16,0x0E,0x1E):  # ASL
            if op==0x0A: m.C=(m.a>>7)&1; m.a=m.setzn(m.a<<1)
            else:
                a={0x06:m.zp,0x16:m.zpx,0x0E:m.ab,0x1E:m.abx}[op](); v=m.rd(a); m.C=(v>>7)&1; m.wr(a,m.setzn(v<<1))
        elif op in (0x4A,0x46,0x56,0x4E,0x5E):  # LSR
            if op==0x4A: m.C=m.a&1; m.a=m.setzn(m.a>>1)
            else:
                a={0x46:m.zp,0x56:m.zpx,0x4E:m.ab,0x5E:m.abx}[op](); v=m.rd(a); m.C=v&1; m.wr(a,m.setzn(v>>1))
        elif op in (0x2A,0x26,0x36,0x2E,0x3E):  # ROL
            if op==0x2A: c=m.C; m.C=(m.a>>7)&1; m.a=m.setzn((m.a<<1)|c)
            else:
                a={0x26:m.zp,0x36:m.zpx,0x2E:m.ab,0x3E:m.abx}[op](); v=m.rd(a); c=m.C; m.C=(v>>7)&1; m.wr(a,m.setzn((v<<1)|c))
        elif op in (0x6A,0x66,0x76,0x6E,0x7E):  # ROR
            if op==0x6A: c=m.C; m.C=m.a&1; m.a=m.setzn((m.a>>1)|(c<<7))
            else:
                a={0x66:m.zp,0x76:m.zpx,0x6E:m.ab,0x7E:m.abx}[op](); v=m.rd(a); c=m.C; m.C=v&1; m.wr(a,m.setzn((v>>1)|(c<<7)))
        elif op in (0xE6,0xF6,0xEE,0xFE):  # INC
            a={0xE6:m.zp,0xF6:m.zpx,0xEE:m.ab,0xFE:m.abx}[op](); m.wr(a,m.setzn(m.rd(a)+1))
        elif op in (0xC6,0xD6,0xCE,0xDE):  # DEC
            a={0xC6:m.zp,0xD6:m.zpx,0xCE:m.ab,0xDE:m.abx}[op](); m.wr(a,m.setzn(m.rd(a)-1))
        elif op==0xE8: m.x=m.setzn(m.x+1)
        elif op==0xCA: m.x=m.setzn(m.x-1)
        elif op==0xC8: m.y=m.setzn(m.y+1)
        elif op==0x88: m.y=m.setzn(m.y-1)
        elif op in (0x24,0x2C):  # BIT
            v=m.rd(m.zp() if op==0x24 else m.ab()); m.Z=1 if (m.a&v)==0 else 0; m.N=(v>>7)&1; m.V=(v>>6)&1
        elif op==0x10: m.branch(m.N==0)
        elif op==0x30: m.branch(m.N==1)
        elif op==0x50: m.branch(m.V==0)
        elif op==0x70: m.branch(m.V==1)
        elif op==0x90: m.branch(m.C==0)
        elif op==0xB0: m.branch(m.C==1)
        elif op==0xD0: m.branch(m.Z==0)
        elif op==0xF0: m.branch(m.Z==1)
        elif op==0x4C: m.pc=m.ab()
        elif op==0x6C: a=m.ab(); m.pc=m.rd(a)|(m.rd((a&0xFF00)|((a+1)&0xFF))<<8)
        elif op==0x20:  # JSR
            a=m.ab(); r=(m.pc-1)&0xFFFF; m.push(r>>8); m.push(r&0xFF); m.pc=a
        elif op==0x60:  # RTS
            lo=m.pop(); hi=m.pop(); m.pc=((hi<<8)|lo)+1
        elif op==0x40:  # RTI
            m.setflags(m.pop()); lo=m.pop(); hi=m.pop(); m.pc=(hi<<8)|lo
        elif op==0x00:  # BRK - treat as halt marker
            raise StopIteration("BRK")
        else:
            raise ValueError("unimplemented opcode $%02X at $%04X" % (op, (m.pc-1)&0xFFFF))
        self.cycles += 1

    def call(self, addr, max_steps=2_000_000):
        """JSR-style call: run until RTS returns to the sentinel."""
        SENT = 0xFFF0
        self.push((SENT-1)>>8); self.push((SENT-1)&0xFF)
        self.pc = addr
        for _ in range(max_steps):
            if self.pc == SENT:
                return True
            self.step()
        return False

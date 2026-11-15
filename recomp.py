"""Static recompiler: i386 ELF (v8mnu22b) -> C.

Every reachable guest instruction is translated to C operating on a CPU context
(registers, lazy flags, x87 stack).  Guest addresses are host addresses (the image is
mapped at its link address in a 32-bit Windows process), so memory operands become plain
pointer dereferences.  Imports (PLT stubs) become calls into the runtime (rt/*.c).

usage: python recomp.py v8mnu22b.elf outdir
"""
import os, sys, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import Program, NORETURN
from capstone import x86_const as X

# guest functions replaced by runtime C implementations (hardware / protection glue)
OVERRIDES = {
    0x806B4B0: "ovr_dongle_read",     # sub_806B4B0(port, addr): EEPROM word read (LPT bit-bang)
    0x806B5D0: "ovr_dongle_write",    # sub_806B5D0(port, addr, val)
    0x80675FE: "ovr_prn_init",        # prn_init(): no printer hardware
    0x8067CCB: "ovr_prn_status",      # prn_status(pr): always ready
    0x807AAA1: "ovr_uprn_open",       # uprn_server_open(): no fork; in-process uprn stand-in
    0x807A7D9: "ovr_vsend_cmd",       # vsend_cmd(cmd, fmt, va, reply, n): answered natively
    0x8083C33: "ovr_dict_key",        # dict.db string key from dongle string
    0x80A50D4: "ovr_run_child",       # run_in_child(mode, execfn, path, argv): vfork+exec -> CreateProcess
    0x808D630: "ovr_vt1_setup",       # -vt1 (XImage) driver setup: also install the camera ops below
    0x80938D3: "ovr_cam_ctl",         # bttv capture mode (live/freeze/stop) -> Windows webcam
    0x8093A1F: "ovr_cam_adj",         # bttv picture adjustments
    0x8093A80: "ovr_cam_win",         # bttv video window rect
    0x8093D4B: "ovr_cam_src",         # bttv input select
    0x809E88A: "ovr_snd_play",        # snd_play(files, n, music): vfork+exec wavplay -> waveOut thread
    0x809F240: "ovr_gmalloc",         # malloc wrapper: banner-save buffers sized for any video mode
}
GAME_OVERRIDES = dict(OVERRIDES)
TRACE_FNS = set(int(x, 16) for x in os.environ.get("RECOMP_TRACE", "").split(",") if x)

REG32 = {"eax": 0, "ecx": 1, "edx": 2, "ebx": 3, "esp": 4, "ebp": 5, "esi": 6, "edi": 7}
REG16 = {"ax": 0, "cx": 1, "dx": 2, "bx": 3, "sp": 4, "bp": 5, "si": 6, "di": 7}
REG8L = {"al": 0, "cl": 1, "dl": 2, "bl": 3}
REG8H = {"ah": 0, "ch": 1, "dh": 2, "bh": 3}


def creg(name):
    if name in REG32:
        return "c->r[%d].d" % REG32[name]
    if name in REG16:
        return "c->r[%d].w" % REG16[name]
    if name in REG8L:
        return "c->r[%d].b.l" % REG8L[name]
    if name in REG8H:
        return "c->r[%d].b.h" % REG8H[name]
    raise ValueError("reg " + name)


SZ = {1: 0, 2: 1, 4: 2}
UT = {1: "u8", 2: "u16", 4: "u32"}
ST = {1: "s8", 2: "s16", 4: "s32"}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}

# flag op kinds (must match rt/cpu.h)
K_ADD, K_ADC, K_SUB, K_SBB, K_LOGIC, K_INC, K_DEC, K_SHL, K_SHR, K_SAR, K_MUL, K_BT, K_FLAGS = range(13)

FLAG_READERS = {"adc", "sbb"}
CC = {"je": "E", "jne": "NE", "jb": "B", "jae": "AE", "jbe": "BE", "ja": "A", "jl": "L", "jge": "GE",
      "jle": "LE", "jg": "G", "js": "S", "jns": "NS", "jp": "P", "jnp": "NP"}
SETCC = {"set" + k[1:]: v for k, v in CC.items()}
FULL_WRITERS = {"add", "sub", "cmp", "test", "and", "or", "xor", "neg", "imul", "mul", "sbb", "adc",
                "repe cmpsb", "repne scasb", "bt", "bts", "sahf"}
PARTIAL_WRITERS = {"inc", "dec", "shl", "shr", "sar"}   # inc/dec keep CF; shifts by 0 keep all
NONFLAG = {"mov", "lea", "push", "pop", "movzx", "movsx", "not", "cdq", "cwde", "cwd", "nop", "cld",
           "leave", "xchg", "rep movsd", "rep stosd", "movsb", "movsw", "stosb", "stosw", "fld", "fstp",
           "fst", "fild", "fistp", "fldcw", "fnstcw", "fnstsw", "fxch", "fmul", "fadd", "faddp", "fmulp",
           "fsub", "fsubp", "fdiv", "fdivr", "fdivrp", "fidivr", "fidiv", "fimul", "fiadd", "fisubr",
           "fucompp", "fucomp", "fucom", "fldz", "fld1", "fabs", "fchs", "fsqrt", "fist", "div", "idiv", "in", "out", "rol"} | set(SETCC) - set()


class Gen:
    def __init__(self, p):
        self.p = p
        self.ins = p.insns
        self.imports = p.imports
        # strong entries: real call targets and data-section function pointers
        strong = {p.entry}
        for a, i in self.ins.items():
            if i.mnemonic == "call" and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm & 0xffffffff
                if t not in self.imports:
                    strong.add(t)
        for a, v in p.data_ptrs:
            if a % 4 == 0:
                strong.add(v)
        for n in (".init", ".fini"):
            if n in p.secs:
                strong.add(p.secs[n][0])
        self.strong = strong & set(p.funcs)
        self.funcs = sorted(p.funcs)
        self.pic_tables = {}
        self._resolve_pic()

    # ebx-relative PIC switch:  cmp edx,N ; ja ; mov eax,[ebx+edx*4+d] ; add eax,ebx ; jmp eax
    def _resolve_pic(self):
        p = self.p
        got = p.secs[".got"][0]
        for a, i in list(self.ins.items()):
            if i.mnemonic == "jmp" and i.operands[0].type == X.X86_OP_REG:
                prev = [x for x in sorted(self.ins) if a - 24 <= x < a]
                mv = [self.ins[x] for x in prev if self.ins[x].mnemonic == "mov" and "ebx +" in self.ins[x].op_str]
                cm = [self.ins[x] for x in prev if self.ins[x].mnemonic == "cmp"]
                if mv and cm:
                    m = mv[-1].operands[1].mem
                    n = cm[-1].operands[1].imm + 1
                    tg = []
                    for k in range(n):
                        off = p.u32((got + (m.disp & 0xffffffff) + 4 * k) & 0xffffffff)
                        tg.append((got + off) & 0xffffffff)
                    if all(p.is_code(t) for t in tg):
                        self.pic_tables[a] = tg
                        print("PIC table at %x: %s" % (a, [hex(t) for t in tg]), file=sys.stderr)

    def fname(self, a):
        if a in OVERRIDES:
            return OVERRIDES[a]
        return "F_%08x" % a

    # ---------------------------------------------------------------- function bodies
    def body(self, entry):
        """intra-procedural reachability from entry."""
        seen = set()
        targets = set()   # addresses needing labels
        tails = set()
        work = [entry]
        while work:
            a = work.pop()
            while a not in seen:
                i = self.ins.get(a)
                if i is None:
                    raise RuntimeError("no insn at %x (func %x)" % (a, entry))
                seen.add(a)
                m = i.mnemonic
                nxt = a + i.size
                if m == "call":
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t in self.imports and self.imports[t] in NORETURN:
                            break
                    a = nxt
                    continue
                if m in ("ret", "hlt"):
                    break
                if m == "jmp":
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t in self.imports or (t in self.strong and t != entry):
                            tails.add(a)
                            break
                        targets.add(t)
                        work.append(t)
                    else:
                        tg = self.p.jtables.get(a) or self.pic_tables.get(a) or []
                        for t in tg:
                            targets.add(t)
                            work.append(t)
                    break
                if m in CC:
                    t = i.operands[0].imm & 0xffffffff
                    targets.add(t)
                    work.append(t)
                    a = nxt
                    continue
                a = nxt
        return sorted(seen), targets

    # ---------------------------------------------------------------- operands
    def addr(self, i, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(creg(i.reg_name(m.base)))
        if m.index:
            r = creg(i.reg_name(m.index))
            parts.append(r if m.scale == 1 else "%s*%d" % (r, m.scale))
        d = m.disp & 0xffffffff
        if d or not parts:
            parts.append("0x%xu" % d)
        return "(u32)(" + " + ".join(parts) + ")"

    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return creg(i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return "0x%xu" % (op.imm & int(MASK[size][:-1], 16))
        return "M%d(%s)" % (size * 8, self.addr(i, op))

    def wr(self, i, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return "%s = (%s)(%s);" % (creg(i.reg_name(op.reg)), UT[size], val)
        return "M%d(%s) = (%s)(%s);" % (size * 8, self.addr(i, op), UT[size], val)

    # ---------------------------------------------------------------- flag liveness
    def flags_live(self, a, body_set, labels):
        """are flags produced by insn at a possibly read later?"""
        i = self.ins[a]
        n = a + i.size
        steps = 0
        while steps < 64:
            steps += 1
            if n not in body_set:
                return True
            if n in labels:
                return True
            j = self.ins[n]
            m = j.mnemonic
            if m in CC or m in SETCC or m in FLAG_READERS or m in ("inc", "dec"):
                return True
            if m in FULL_WRITERS:
                return False
            if m in ("call", "ret"):
                return False
            if m == "jmp":
                return True
            if m in ("shl", "shr", "sar"):
                # count may be zero (register count) -> flags pass through
                if j.operands[1].type == X.X86_OP_IMM and (j.operands[1].imm & 31):
                    return False
                return True
            n = n + j.size
        return True

    # ---------------------------------------------------------------- codegen
    def setf(self, kind, size, a, b, r, live, cf=None):
        if not live:
            return ""
        s = "fop=%d; fa=(u32)(%s); fb=(u32)(%s); fr=(u32)(%s);" % (kind * 4 + SZ[size], a, b, r)
        if cf is not None:
            s += " fcf=(%s);" % cf
        return s

    def cond(self, cc):
        return "CC_%s(fop,fa,fb,fr,fcf)" % cc

    def gen_func(self, entry):
        self.cur_fn = entry
        if entry in OVERRIDES:
            return None
        addrs, targets = self.body(entry)
        body_set = set(addrs)
        labels = set(targets)
        # pre-pass: fallthroughs into non-adjacent addresses need explicit gotos + labels
        fall_goto = {}
        for k, a in enumerate(addrs):
            i = self.ins[a]
            m = i.mnemonic
            nxt = a + i.size
            ends = m in ("ret", "hlt", "jmp") or (m == "call" and i.operands[0].type == X.X86_OP_IMM and self.imports.get(i.operands[0].imm & 0xffffffff) in NORETURN)
            if not ends and (k + 1 >= len(addrs) or addrs[k + 1] != nxt):
                if nxt not in body_set:
                    raise RuntimeError("fallthrough out of body %x" % a)
                fall_goto[a] = nxt
                labels.add(nxt)
        out = []
        out.append("void %s(CPU *c) {" % self.fname(entry))
        out.append("  u32 fop=0, fa=0, fb=0, fr=0, fcf=0; (void)fop; (void)fa; (void)fb; (void)fr; (void)fcf;")
        out.append("  ENTER(c, 0x%08xu);" % entry)
        if addrs and addrs[0] != entry:  # body is emitted in address order; entry may not be the lowest block
            labels.add(entry)
            out.append("  goto L_%08x;" % entry)
        for a in addrs:
            i = self.ins[a]
            if a in labels:
                out.append("L_%08x:;" % a)
            code = self.gen_insn(entry, a, i, body_set, labels)
            out.append("  /* %08x: %s %s */ %s" % (a, i.mnemonic, i.op_str.replace("*/", "* /"), code))
            if a in fall_goto:
                out.append("  goto L_%08x;" % fall_goto[a])
        out.append("}")
        return "\n".join(out)

    def gen_insn(self, entry, a, i, body_set, labels):
        m = i.mnemonic
        ops = i.operands
        nxt = a + i.size
        live = lambda: self.flags_live(a, body_set, labels)

        if m == "mov":
            if ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg) not in REG32 and i.reg_name(ops[0].reg) not in REG16 and i.reg_name(ops[0].reg) not in REG8L and i.reg_name(ops[0].reg) not in REG8H:
                return "/* segment load ignored */"
            if ops[1].type == X.X86_OP_REG and i.reg_name(ops[1].reg) in ("ds", "es", "cs", "ss", "fs", "gs"):
                return self.wr(i, ops[0], "0x2b")
            return self.wr(i, ops[0], self.rd(i, ops[1]))
        if m == "lea":
            return self.wr(i, ops[0], self.addr(i, ops[1]))
        if m == "movzx":
            return self.wr(i, ops[0], "(%s)%s" % (UT[ops[1].size], self.rd(i, ops[1])))
        if m == "movsx":
            return self.wr(i, ops[0], "(%s)(%s)(%s)%s" % (UT[ops[0].size], ST[ops[0].size], ST[ops[1].size], self.rd(i, ops[1])))
        if m == "push":
            return "{ u32 v = %s; c->r[4].d -= 4; M32(c->r[4].d) = v; }" % self.rd(i, ops[0], 4)
        if m == "pop":
            return "{ u32 v = M32(c->r[4].d); c->r[4].d += 4; %s }" % self.wr(i, ops[0], "v")
        if m == "leave":
            return "c->r[4].d = c->r[5].d; c->r[5].d = M32(c->r[4].d); c->r[4].d += 4;"
        if m in ("add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"):
            s = ops[0].size
            ut = UT[s]
            A = self.rd(i, ops[0])
            B = self.rd(i, ops[1], s)
            lv = live()
            if m == "add":
                return "{ %s x = %s, y = %s, r = x + y; %s %s }" % (ut, A, B, self.wr(i, ops[0], "r"), self.setf(K_ADD, s, "x", "y", "r", lv))
            if m == "sub":
                return "{ %s x = %s, y = %s, r = x - y; %s %s }" % (ut, A, B, self.wr(i, ops[0], "r"), self.setf(K_SUB, s, "x", "y", "r", lv))
            if m == "cmp":
                return "{ %s x = %s, y = %s, r = x - y; %s }" % (ut, A, B, self.setf(K_SUB, s, "x", "y", "r", lv))
            if m == "adc":
                return "{ %s x = %s, y = %s; u32 ci = CF(fop,fa,fb,fr,fcf); %s r = x + y + ci; %s %s }" % (ut, A, B, ut, self.wr(i, ops[0], "r"), self.setf(K_ADC, s, "x", "y", "r", lv, "ci"))
            if m == "sbb":
                return "{ %s x = %s, y = %s; u32 ci = CF(fop,fa,fb,fr,fcf); %s r = x - y - ci; %s %s }" % (ut, A, B, ut, self.wr(i, ops[0], "r"), self.setf(K_SBB, s, "x", "y", "r", lv, "ci"))
            op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
            # xor r,r idiom
            if m == "xor" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return "{ %s %s }" % (self.wr(i, ops[0], "0"), self.setf(K_LOGIC, s, "0", "0", "0", lv))
            if m == "test":
                return "{ %s r = %s %s %s; %s }" % (ut, A, op, B, self.setf(K_LOGIC, s, "0", "0", "r", lv))
            return "{ %s r = %s %s %s; %s %s }" % (ut, A, op, B, self.wr(i, ops[0], "r"), self.setf(K_LOGIC, s, "0", "0", "r", lv))
        if m in ("inc", "dec"):
            s = ops[0].size
            ut = UT[s]
            lv = live()
            k, o = (K_INC, "+") if m == "inc" else (K_DEC, "-")
            pre = "u32 oc = CF(fop,fa,fb,fr,fcf); " if lv else ""
            return "{ %s%s r = %s %s 1; %s %s }" % (pre, ut, self.rd(i, ops[0]), o, self.wr(i, ops[0], "r"), self.setf(k, s, "0", "0", "r", lv, "oc"))
        if m == "neg":
            s = ops[0].size
            ut = UT[s]
            return "{ %s y = %s, r = (%s)(0 - y); %s %s }" % (ut, self.rd(i, ops[0]), ut, self.wr(i, ops[0], "r"), self.setf(K_SUB, s, "0", "y", "r", live()))
        if m == "not":
            return self.wr(i, ops[0], "~%s" % self.rd(i, ops[0]))
        if m in ("shl", "shr", "sar"):
            s = ops[0].size
            ut, st = UT[s], ST[s]
            bits = s * 8
            n = self.rd(i, ops[1], 1) if ops[1].type != X.X86_OP_IMM else "%du" % (ops[1].imm & 31)
            lv = live()
            if m == "shl":
                expr, kind = "(%s)(x << n)" % ut, K_SHL
            elif m == "shr":
                expr, kind = "(%s)(x >> n)" % ut, K_SHR
            else:
                expr, kind = "(%s)((%s)x >> n)" % (ut, st), K_SAR
            # for 8/16-bit, counts up to 31 are allowed: shifting a promoted int is fine
            fl = self.setf(kind, s, "x", "n", "r", lv)
            return "{ u32 n = (%s) & 31; if (n) { %s x = %s; %s r = %s; %s %s } }" % (n, ut, self.rd(i, ops[0]), ut, expr, self.wr(i, ops[0], "r"), fl)
        if m == "rol":
            s = ops[0].size
            ut = UT[s]
            bits = s * 8
            n = self.rd(i, ops[1], 1) if ops[1].type != X.X86_OP_IMM else "%du" % (ops[1].imm & 31)
            return "{ u32 n = ((%s) & 31) %% %d; %s x = %s; if (n) { %s r = (%s)((x << n) | (x >> (%d - n))); %s } }" % (n, bits, ut, self.rd(i, ops[0]), ut, ut, bits, self.wr(i, ops[0], "r"))
        if m == "imul":
            lv = live()
            if len(ops) == 1:
                if ops[0].size != 4:
                    raise ValueError("imul8/16")
                return "{ s64 p = (s64)(s32)c->r[0].d * (s32)%s; c->r[0].d = (u32)p; c->r[2].d = (u32)(p >> 32); %s }" % (self.rd(i, ops[0]), self.setf(K_MUL, 4, "0", "0", "(u32)p", lv, "p != (s64)(s32)p"))
            s = ops[0].size
            if len(ops) == 2:
                x, y = self.rd(i, ops[0]), self.rd(i, ops[1])
            else:
                x, y = self.rd(i, ops[1]), self.rd(i, ops[2], s)
            st = ST[s]
            return "{ s64 p = (s64)(%s)%s * (s64)(%s)%s; %s %s }" % (st, x, st, y, self.wr(i, ops[0], "p"), self.setf(K_MUL, s, "0", "0", "(u32)p", lv, "p != (s64)(%s)p" % st))
        if m == "mul":
            if ops[0].size != 4:
                raise ValueError("mul8/16")
            return "{ u64 p = (u64)c->r[0].d * %s; c->r[0].d = (u32)p; c->r[2].d = (u32)(p >> 32); %s }" % (self.rd(i, ops[0]), self.setf(K_MUL, 4, "0", "0", "(u32)p", live(), "(p >> 32) != 0"))
        if m in ("div", "idiv"):
            s = ops[0].size
            if s == 4:
                if m == "div":
                    return "{ u32 d = %s; if (!d) rt_divzero(c, 0x%xu); u64 n = ((u64)c->r[2].d << 32) | c->r[0].d; c->r[0].d = (u32)(n / d); c->r[2].d = (u32)(n %% d); }" % (self.rd(i, ops[0]), a)
                return "{ s32 d = (s32)%s; if (!d) rt_divzero(c, 0x%xu); s64 n = (s64)(((u64)c->r[2].d << 32) | c->r[0].d); if (d == -1 && n == (s64)(s32)0x80000000) rt_divzero(c, 0x%xu); c->r[0].d = (u32)(s32)(n / d); c->r[2].d = (u32)(s32)(n %% d); }" % (self.rd(i, ops[0]), a, a)
            if s == 2 and m == "idiv":
                return "{ s16 d = (s16)%s; if (!d) rt_divzero(c, 0x%xu); s32 n = (s32)(((u32)c->r[2].w << 16) | c->r[0].w); c->r[0].w = (u16)(n / d); c->r[2].w = (u16)(n %% d); }" % (self.rd(i, ops[0]), a)
            raise ValueError("div form")
        if m == "cdq":
            return "c->r[2].d = (u32)((s32)c->r[0].d >> 31);"
        if m == "cwde":
            return "c->r[0].d = (u32)(s32)(s16)c->r[0].w;"
        if m == "cwd":
            return "c->r[2].w = (u16)((s16)c->r[0].w >> 15);"
        if m == "xchg":
            return "{ u32 t = %s; %s %s }" % (self.rd(i, ops[0]), self.wr(i, ops[0], self.rd(i, ops[1])), self.wr(i, ops[1], "t"))
        if m in ("bt", "bts"):
            # bt [mem], reg : bit string addressing
            base = self.addr(i, ops[0])
            r = self.rd(i, ops[1])
            setb = " M32(p) |= (1u << (b & 31));" if m == "bts" else ""
            return "{ s32 b = (s32)%s; u32 p = %s + (u32)((b >> 5) * 4); u32 bit = (M32(p) >> (b & 31)) & 1;%s %s }" % (r, base, setb, self.setf(K_BT, 4, "0", "0", "1", True, "bit"))
        if m in SETCC:
            return self.wr(i, ops[0], self.cond(SETCC[m]))
        if m in ("nop", "cld"):
            return ";"
        if m == "hlt":
            return "rt_hlt(c, 0x%xu); return;" % a
        if m == "in":
            return "c->r[0].b.l = rt_inb(c, c->r[2].w);"
        if m == "out":
            return "rt_outb(c, c->r[2].w, c->r[0].b.l);"
        # string ops (DF assumed 0)
        if m == "rep movsd":
            return "{ u32 n = c->r[1].d; u32 *d = (u32*)c->r[7].d, *s = (u32*)c->r[6].d; while (n--) *d++ = *s++; c->r[7].d += c->r[1].d*4; c->r[6].d += c->r[1].d*4; c->r[1].d = 0; }"
        if m == "rep stosd":
            return "{ u32 n = c->r[1].d; u32 *d = (u32*)c->r[7].d, v = c->r[0].d; while (n--) *d++ = v; c->r[7].d += c->r[1].d*4; c->r[1].d = 0; }"
        if m == "movsb":
            return "M8(c->r[7].d) = M8(c->r[6].d); c->r[7].d++; c->r[6].d++;"
        if m == "movsw":
            return "M16(c->r[7].d) = M16(c->r[6].d); c->r[7].d += 2; c->r[6].d += 2;"
        if m == "stosb":
            return "M8(c->r[7].d) = c->r[0].b.l; c->r[7].d++;"
        if m == "stosw":
            return "M16(c->r[7].d) = c->r[0].w; c->r[7].d += 2;"
        if m == "repne scasb":
            return ("{ while (c->r[1].d) { u8 x = c->r[0].b.l, y = M8(c->r[7].d); c->r[7].d++; c->r[1].d--; "
                    "fop=%d; fa=x; fb=y; fr=(u8)(x-y); if (x == y) break; } }" % (K_SUB * 4 + 0))
        if m == "repe cmpsb":
            return ("{ while (c->r[1].d) { u8 x = M8(c->r[6].d), y = M8(c->r[7].d); c->r[6].d++; c->r[7].d++; c->r[1].d--; "
                    "fop=%d; fa=x; fb=y; fr=(u8)(x-y); if (x != y) break; } }" % (K_SUB * 4 + 0))
        # control flow
        if m in CC:
            t = ops[0].imm & 0xffffffff
            return "if (%s) goto L_%08x;" % (self.cond(CC[m]), t)
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t in self.imports:
                    return "IMP_%s(c); return;" % self.imports[t]
                if t in self.strong and t != entry:
                    return "SIGCHK(c); %s(c); return;" % self.fname(t)
                bk = "SIGCHK(c); " if t <= a else ""
                return "%sgoto L_%08x;" % (bk, t)
            tg = self.p.jtables.get(a) or self.pic_tables.get(a)
            tv = self.rd(i, op)
            if tg:
                cases = " ".join("case 0x%08xu: goto L_%08x;" % (t, t) for t in sorted(set(tg)))
                return "{ u32 t = %s; switch (t) { %s default: rt_jmp_ind(c, t, 0x%xu); return; } }" % (tv, cases, a)
            return "{ u32 t = %s; rt_jmp_ind(c, t, 0x%xu); return; }" % (tv, a)
        if m == "call":
            op = ops[0]
            push = "c->r[4].d -= 4; M32(c->r[4].d) = 0x%xu; " % nxt
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t == nxt:
                    return push + "/* call $+5 (pc thunk) */"
                if t in self.imports:
                    name = self.imports[t]
                    if name in ("_setjmp", "__sigsetjmp"):
                        return push + "{ rt_jb *jb = rt_setjmp_prep(c, %d); if (RT_SETJMP(jb)) { rt_setjmp_back(c); } else { c->r[0].d = 0; } }" % (name == "__sigsetjmp")
                    s = push + "IMP_%s(c);" % name
                    if name in NORETURN:
                        s += " return;"
                    return s
                return push + "%s(c);" % self.fname(t)
            return push + "rt_call_ind(c, %s);" % self.rd(i, op)
        if m == "ret":
            if self.cur_fn in TRACE_FNS:  # RECOMP_TRACE=addr,addr: log return values (debug aid)
                return ('rt_logf("R %%08x -> %%d\\n", 0x%08xu, (int)c->r[0].d); ' % self.cur_fn) + \
                    ("c->r[4].d += 4 + %d; return;" % ops[0].imm if ops else "c->r[4].d += 4; return;")
            if ops:
                return "c->r[4].d += 4 + %d; return;" % ops[0].imm
            return "c->r[4].d += 4; return;"
        if m == "sahf":
            return "fop=%d; fa=0; fb=0; fr=c->r[0].b.h; fcf=fr & 1;" % (K_FLAGS * 4 + 2)
        # ---------------- x87
        if m.startswith("f"):
            return self.gen_fpu(i, a)
        raise ValueError("unhandled %s %s at %x" % (m, i.op_str, a))

    def fsrc(self, i, op):
        """value of an x87 source operand (memory float / st(i))."""
        if op.type == X.X86_OP_REG:
            return "ST(%d)" % self.stn(i, op)
        if op.size == 4:
            return "(f80)MF32(%s)" % self.addr(i, op)
        if op.size == 8:
            return "(f80)MF64(%s)" % self.addr(i, op)
        if op.size == 10:
            return "MF80(%s)" % self.addr(i, op)
        raise ValueError("fsrc size")

    def stn(self, i, op):
        n = i.reg_name(op.reg)
        assert n.startswith("st"), n
        return int(n[3]) if "(" in n else 0

    def gen_fpu(self, i, a):
        m = i.mnemonic
        ops = i.operands
        b = i.bytes
        if m == "fld":
            if ops[0].type == X.X86_OP_REG:
                return "{ f80 v = ST(%d); FPUSH(v); }" % self.stn(i, ops[0])
            return "{ f80 v = %s; FPUSH(v); }" % self.fsrc(i, ops[0])
        if m == "fldz":
            return "FPUSH(0.0L);"
        if m == "fld1":
            return "FPUSH(1.0L);"
        if m == "fild":
            sz = ops[0].size
            t = {2: "s16", 4: "s32", 8: "s64"}[sz]
            return "{ f80 v = (f80)*(%s*)%s; FPUSH(v); }" % (t, self.addr(i, ops[0]))
        if m in ("fst", "fstp"):
            pop = " FPOP();" if m == "fstp" else ""
            op = ops[0]
            if op.type == X.X86_OP_REG:
                return "ST(%d) = ST(0);%s" % (self.stn(i, op), pop)
            if op.size == 4:
                return "MF32(%s) = (float)ST(0);%s" % (self.addr(i, op), pop)
            if op.size == 8:
                return "MF64(%s) = (double)ST(0);%s" % (self.addr(i, op), pop)
            raise ValueError("fst size")
        if m == "fistp":
            sz = ops[0].size
            if sz == 2:
                return "{ s32 v = rt_fist32(c, ST(0)); M16(%s) = (u16)(v > 32767 || v < -32768 ? 0x8000 : v); FPOP(); }" % self.addr(i, ops[0])
            if sz == 8:
                return "*(s64 *)(uintptr_t)(%s) = rt_fist64(c, ST(0)); FPOP();" % self.addr(i, ops[0])
            if sz != 4:
                raise ValueError("fistp size")
            return "M32(%s) = (u32)rt_fist32(c, ST(0)); FPOP();" % self.addr(i, ops[0])
        if m == "fldcw":
            return "c->fcw = M16(%s);" % self.addr(i, ops[0])
        if m == "fnstcw":
            return "M16(%s) = c->fcw;" % self.addr(i, ops[0])
        if m == "sahf":
            return "fop=%d; fa=0; fb=0; fr=c->r[0].b.h; fcf=fr & 1;" % (K_FLAGS * 4 + 2)
        if m == "fnstsw":
            return "c->r[0].w = FSW();"
        if m == "fxch":
            n = self.stn(i, ops[1]) if len(ops) > 1 else 1
            if len(ops) > 1 and self.stn(i, ops[0]) != 0:
                n = self.stn(i, ops[0])
            return "{ f80 t = ST(0); ST(0) = ST(%d); ST(%d) = t; }" % (n, n)
        if m == "fchs":
            return "ST(0) = -ST(0);"
        if m == "fsqrt":
            return "ST(0) = sqrtl(ST(0));"
        if m == "fist":
            sz = ops[0].size
            if sz == 2:
                return "{ s32 v = rt_fist32(c, ST(0)); M16(%s) = (u16)(v > 32767 || v < -32768 ? 0x8000 : v); }" % self.addr(i, ops[0])
            if sz == 4:
                return "M32(%s) = (u32)rt_fist32(c, ST(0));" % self.addr(i, ops[0])
            raise ValueError("fist size")
        if m == "fabs":
            return "ST(0) = ST(0) < 0 ? -ST(0) : ST(0);"
        if m in ("fucom", "fucomp", "fucompp"):
            n = self.stn(i, ops[0]) if ops else 1
            pops = {"fucom": "", "fucomp": " FPOP();", "fucompp": " FPOP(); FPOP();"}[m]
            return "FCOM(ST(0), ST(%d));%s" % (n, pops)
        # integer-memory arithmetic
        if m in ("fiadd", "fimul", "fidiv", "fidivr", "fisubr"):
            v = "(f80)*(s32*)%s" % self.addr(i, ops[0])
            e = {"fiadd": "ST(0) = ST(0) + %s;", "fimul": "ST(0) = ST(0) * %s;", "fidiv": "ST(0) = ST(0) / %s;",
                 "fidivr": "ST(0) = %s / ST(0);", "fisubr": "ST(0) = %s - ST(0);"}[m]
            return "{ f80 v = %s; %s }" % (v, e % "v")
        # arithmetic: decide by opcode bytes for register forms (capstone/llvm swap sub/subr div/divr naming)
        if m in ("fadd", "fmul", "fsub", "fdiv", "fdivr", "fsubr") and ops and ops[0].type == X.X86_OP_MEM:
            v = self.fsrc(i, ops[0])
            e = {"fadd": "ST(0) + v", "fmul": "ST(0) * v", "fsub": "ST(0) - v", "fdiv": "ST(0) / v",
                 "fdivr": "v / ST(0)", "fsubr": "v - ST(0)"}[m]
            return "{ f80 v = %s; ST(0) = %s; }" % (v, e)
        # register forms: D8 /r: st0 = st0 op st(i);  DC /r: st(i) = st(i) op st0;  DE /r: same + pop
        op0 = b[0]
        modrm = b[1]
        reg = (modrm >> 3) & 7
        sti = modrm & 7
        OPS = {0: "+", 1: "*", 4: "-", 5: "-r", 6: "/", 7: "/r"}
        if op0 == 0xD8:
            o = OPS[reg]
            if o.endswith("r"):
                return "ST(0) = ST(%d) %s ST(0);" % (sti, o[0])
            return "ST(0) = ST(0) %s ST(%d);" % (o, sti)
        if op0 in (0xDC, 0xDE):
            # Intel: DC/DE E0+i FSUBR(P) st(i)=st0-st(i); E8+i FSUB(P) st(i)=st(i)-st0;
            #        F0+i FDIVR(P) st(i)=st0/st(i); F8+i FDIV(P) st(i)=st(i)/st0
            o = {0: "+", 1: "*", 4: "subr", 5: "sub", 6: "divr", 7: "div"}[reg]
            if o in ("+", "*"):
                e = "ST(%d) = ST(%d) %s ST(0);" % (sti, sti, o)
            elif o == "sub":
                e = "ST(%d) = ST(%d) - ST(0);" % (sti, sti)
            elif o == "subr":
                e = "ST(%d) = ST(0) - ST(%d);" % (sti, sti)
            elif o == "div":
                e = "ST(%d) = ST(%d) / ST(0);" % (sti, sti)
            else:
                e = "ST(%d) = ST(0) / ST(%d);" % (sti, sti)
            if op0 == 0xDE:
                e += " FPOP();"
            return e
        raise ValueError("fpu %s %s %s" % (m, i.op_str, b.hex()))


def main():
    elf, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    is_game = os.path.basename(elf).startswith("v8mnu22b")
    if not is_game:
        OVERRIDES.clear()   # the overrides are addresses in the game binary only
    p = Program(elf).analyze()
    g = Gen(p)
    # extra roots: PIC table targets must be reachable in insns (they were decoded? ensure)
    missing = [t for tg in g.pic_tables.values() for t in tg if t not in p.insns]
    if missing:
        p2 = Program(elf).analyze(extra_entries=missing)
        p = p2
        g = Gen(p)
    funcs = [f for f in g.funcs]
    print("functions:", len(funcs), "strong:", len(g.strong), file=sys.stderr)
    # emit
    hdr = ['#include "../rt/cpu.h"']
    decls = []
    for f in funcs:
        if f not in OVERRIDES:
            decls.append("void %s(CPU *c);" % g.fname(f))
    for name in sorted(set(OVERRIDES.values())):
        decls.append("void %s(CPU *c);" % name)
    for name in sorted(set(p.imports.values())):
        decls.append("void IMP_%s(CPU *c);" % name)
    with open(os.path.join(outdir, "guest.h"), "w") as fh:
        fh.write("#pragma once\n" + "\n".join(decls) + "\n")
    NFILES = 48
    chunks = [[] for _ in range(NFILES)]
    sizes = [0] * NFILES
    errors = 0
    for f in funcs:
        try:
            t = g.gen_func(f)
        except Exception as e:
            print("ERROR func %x: %s" % (f, e), file=sys.stderr)
            errors += 1
            t = "void %s(CPU *c) { rt_unimpl(c, 0x%xu); }" % (g.fname(f), f)
        if t is None:
            continue
        k = sizes.index(min(sizes))
        chunks[k].append(t)
        sizes[k] += len(t)
    for k, ch in enumerate(chunks):
        with open(os.path.join(outdir, "guest_%02d.c" % k), "w") as fh:
            fh.write('#include "../rt/cpu.h"\n#include "guest.h"\n\n')
            fh.write("\n\n".join(ch))
            fh.write("\n")
    # dispatch table: every function entry + PLT stubs + overrides
    ents = []
    for f in funcs:
        ents.append((f, g.fname(f)))
    for a, name in p.imports.items():
        ents.append((a, "IMP_" + name))
    ents.sort()
    with open(os.path.join(outdir, "dispatch.c"), "w") as fh:
        fh.write('#include "../rt/cpu.h"\n#include "guest.h"\n')
        fh.write("const DispEnt guest_dispatch[] = {\n")
        for a, n in ents:
            fh.write("  {0x%08xu, %s},\n" % (a, n))
        fh.write("};\nconst int guest_dispatch_n = %d;\n" % len(ents))
        # image info
        fh.write("const u32 guest_entry = 0x%08xu;\n" % p.entry)
        fh.write("const int guest_is_game = %d;\n" % is_game)
        fh.write('const char guest_name[] = "%s";\n' % os.path.basename(elf).split(".")[0])
        # GOT plt slots -> stub addresses
        fh.write("const GotEnt guest_got[] = {\n")
        stub_by_name = {}
        for a, name in p.imports.items():
            stub_by_name[name] = a
        for got, name in sorted(p.got_to_name.items()):
            fh.write('  {0x%08xu, 0x%08xu, "%s"},\n' % (got, stub_by_name.get(name, 0), name))
        fh.write("};\nconst int guest_got_n = %d;\n" % len(p.got_to_name))
        fh.write("const DataImp guest_dataimp[] = {\n")
        for a, (name, typ, size) in sorted(p.data_imports.items()):
            fh.write('  {0x%08xu, %d, "%s"},\n' % (a, typ, name))
        fh.write("};\nconst int guest_dataimp_n = %d;\n" % len(p.data_imports))
    # raw image segments
    with open(os.path.join(outdir, "image.c"), "w") as fh:
        fh.write('#include "../rt/cpu.h"\n')
        for k, (va, fs, ms, data, fl) in enumerate(p.segs):
            fh.write("static const unsigned char seg%d[%d] = {" % (k, max(fs, 1)))
            fh.write(",".join(str(x) for x in data[:fs]) or "0")
            fh.write("};\n")
        fh.write("const ImgSeg guest_segs[] = {\n")
        for k, (va, fs, ms, data, fl) in enumerate(p.segs):
            fh.write("  {0x%08xu, %d, %d, seg%d},\n" % (va, fs, ms, k))
        fh.write("};\nconst int guest_nsegs = %d;\n" % len(p.segs))
    print("done, errors:", errors, file=sys.stderr)


if __name__ == "__main__":
    main()

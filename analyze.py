"""Static analysis of v8mnu22b for recompilation: recursive-descent disassembly.

Produces a Program object with:
  insns   : addr -> capstone insn (every reachable instruction)
  funcs   : set of function entry addresses
  jtables : addr of indirect jmp -> list of target addrs
  imports : plt stub addr -> symbol name
"""
import struct, sys, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
import capstone
from capstone import x86_const as X

NORETURN = {"exit", "_exit", "abort", "longjmp", "siglongjmp", "__assert_fail", "pthread_exit"}


class Program:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.elf = ELFFile(self.f)
        self.segs = []  # (vaddr, filesz, memsz, data, flags)
        for s in self.elf.iter_segments():
            if s["p_type"] == "PT_LOAD":
                self.segs.append((s["p_vaddr"], s["p_filesz"], s["p_memsz"], s.data(), s["p_flags"]))
        self.secs = {}
        for s in self.elf.iter_sections():
            if s["sh_addr"]:
                self.secs[s.name] = (s["sh_addr"], s["sh_size"])
        self.text_lo, sz = self.secs[".text"]
        self.text_hi = self.text_lo + sz
        # code-ish sections: .init .plt .text .fini
        self.code_ranges = [self.secs[n] for n in (".init", ".plt", ".text", ".fini") if n in self.secs]
        self.entry = self.elf.header["e_entry"]
        self.imports = self._plt_imports()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    # ---- memory access on the file image
    def read(self, addr, n):
        for va, fs, ms, data, fl in self.segs:
            if va <= addr and addr + n <= va + ms:
                off = addr - va
                b = data[off:off + n]
                return b + b"\0" * (n - len(b))
        return None

    def u32(self, addr):
        b = self.read(addr, 4)
        return None if b is None else struct.unpack("<I", b)[0]

    def is_code(self, a):
        return any(lo <= a < lo + sz for lo, sz in self.code_ranges)

    def _plt_imports(self):
        """map PLT stub address -> imported symbol name, via .rel.plt + GOT."""
        imp = {}
        relplt = self.elf.get_section_by_name(".rel.plt")
        dynsym = self.elf.get_section_by_name(".dynsym")
        got_to_name = {}
        for r in relplt.iter_relocations():
            got_to_name[r["r_offset"]] = dynsym.get_symbol(r["r_info_sym"]).name
        self.got_to_name = got_to_name
        plt_lo, plt_sz = self.secs[".plt"]
        # stubs: jmp *GOT ; push n ; jmp plt0   (16 bytes each), first is PLT0
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for a in range(plt_lo + 16, plt_lo + plt_sz, 16):
            ins = next(md.disasm(self.read(a, 6), a))
            if ins.mnemonic == "jmp" and ins.op_str.startswith("dword ptr ["):
                got = int(ins.op_str[len("dword ptr ["):-1], 16)
                if got in got_to_name:
                    imp[a] = got_to_name[got]
        # data imports (R_386_COPY / GLOB_DAT) for reference
        self.data_imports = {}
        reldyn = self.elf.get_section_by_name(".rel.dyn")
        if reldyn:
            for r in reldyn.iter_relocations():
                sym = dynsym.get_symbol(r["r_info_sym"])
                self.data_imports[r["r_offset"]] = (sym.name, r["r_info_type"], sym["st_size"])
        return imp

    def decode(self, a):
        b = self.read(a, 16)
        try:
            return next(self.md.disasm(b, a))
        except StopIteration:
            return None

    # ---- recursive descent
    def analyze(self, extra_entries=()):
        self.insns = {}
        self.funcs = set()
        self.jtables = {}
        self.bad = []
        work = []

        def add_func(a):
            if self.is_code(a) and a not in self.funcs and a not in self.imports:
                self.funcs.add(a)
                work.append(a)

        add_func(self.entry)
        for n in (".init", ".fini"):
            if n in self.secs:
                add_func(self.secs[n][0])
        for n in (".ctors", ".dtors", ".init_array", ".fini_array"):
            if n in self.secs:
                lo, sz = self.secs[n]
                for a in range(lo, lo + sz, 4):
                    v = self.u32(a)
                    if v and self.is_code(v):
                        add_func(v)
        for a in extra_entries:
            add_func(a)
        # code pointers in data
        self.data_ptrs = set()
        for n, (lo, sz) in self.secs.items():
            if n in (".text", ".plt", ".init", ".fini", ".bss") or n.startswith(".rel") or n in (".dynsym", ".dynstr", ".hash", ".gnu.version", ".gnu.version_r", ".interp", ".note.ABI-tag", ".dynamic", ".got", ".got.plt"):
                continue
            for a in range(lo, lo + sz - 3):
                v = self.u32(a)
                if v and self.text_lo <= v < self.text_hi:
                    self.data_ptrs.add((a, v))
        for a, v in self.data_ptrs:
            if a % 4 == 0:
                add_func(v)

        seen_blocks = set()
        while work:
            fa = work.pop()
            stack = [fa]
            while stack:
                a = stack.pop()
                while True:
                    if a in self.insns:
                        break
                    if not self.is_code(a):
                        self.bad.append((a, "out of code"))
                        break
                    ins = self.decode(a)
                    if ins is None:
                        self.bad.append((a, "decode fail"))
                        break
                    self.insns[a] = ins
                    m = ins.mnemonic
                    nxt = a + ins.size
                    # immediates pointing into text => potential code pointers
                    for op in ins.operands:
                        if op.type == X.X86_OP_IMM and self.text_lo <= (op.imm & 0xffffffff) < self.text_hi and m not in ("call", "jmp") and not m.startswith("j") and not m.startswith("loop"):
                            add_func(op.imm & 0xffffffff)
                    if m == "call":
                        op = ins.operands[0]
                        if op.type == X.X86_OP_IMM:
                            t = op.imm & 0xffffffff
                            if t in self.imports:
                                if self.imports[t] in NORETURN:
                                    break
                            else:
                                add_func(t)
                        a = nxt
                        continue
                    if m in ("ret", "retf", "hlt", "iret", "iretd", "ud2"):
                        break
                    if m == "jmp":
                        op = ins.operands[0]
                        if op.type == X.X86_OP_IMM:
                            t = op.imm & 0xffffffff
                            if t in self.imports:
                                break
                            stack.append(t)
                        else:
                            tg = self._jtable(ins)
                            if tg:
                                self.jtables[a] = tg
                                stack.extend(tg)
                        break
                    if m.startswith("j") or m.startswith("loop") or m == "jecxz" or m == "jcxz":
                        stack.append(ins.operands[0].imm & 0xffffffff)
                        a = nxt
                        continue
                    a = nxt
        return self

    def _jtable(self, ins):
        op = ins.operands[0]
        if op.type != X.X86_OP_MEM:
            return None
        mem = op.mem
        if mem.base == 0 and mem.index != 0 and mem.scale == 4:
            tbl = mem.disp & 0xffffffff
            out = []
            a = tbl
            while True:
                v = self.u32(a)
                if v is None or not (self.text_lo <= v < self.text_hi):
                    break
                out.append(v)
                a += 4
                if len(out) > 1024:
                    break
            return out or None
        return None


if __name__ == "__main__":
    p = Program(sys.argv[1]).analyze()
    covered = sum(i.size for i in p.insns.values())
    print("sections:", {k: (hex(v[0]), hex(v[1])) for k, v in p.secs.items()})
    print("entry", hex(p.entry), "imports", len(p.imports), "data imports", p.data_imports)
    print("funcs", len(p.funcs), "insns", len(p.insns), "bytes covered", covered, "of text", p.text_hi - p.text_lo)
    print("jump tables", len(p.jtables))
    print("bad", p.bad[:30], len(p.bad))
    mn = collections.Counter(i.mnemonic for i in p.insns.values())
    print("mnemonics", len(mn))
    print(" ".join(f"{k}:{v}" for k, v in sorted(mn.items(), key=lambda x: -x[1])))
    # indirect jmps without tables / indirect calls
    ij = [hex(a) + " " + i.op_str for a, i in p.insns.items() if i.mnemonic == "jmp" and i.operands[0].type != X.X86_OP_IMM and a not in p.jtables]
    print("unresolved indirect jmps", len(ij), ij[:20])
    ic = collections.Counter(i.op_str for a, i in p.insns.items() if i.mnemonic == "call" and i.operands[0].type != X.X86_OP_IMM)
    print("indirect call forms", len(ic), list(ic.items())[:30])
    # uncovered gaps in .text
    cov = bytearray(p.text_hi - p.text_lo)
    for a, i in p.insns.items():
        if p.text_lo <= a < p.text_hi:
            for k in range(i.size):
                if a + k - p.text_lo < len(cov):
                    cov[a + k - p.text_lo] = 1
    gaps = []
    s = None
    for k, c in enumerate(cov):
        if not c and s is None:
            s = k
        if c and s is not None:
            gaps.append((p.text_lo + s, k - s)); s = None
    big = [(hex(a), n) for a, n in gaps if n >= 16]
    print("gaps>=16:", len(big), "total gap bytes", sum(n for _, n in gaps))
    print(big[:60])

#!/usr/bin/env python3
"""tvabi.py: link-time stand-ins for the TV's shared libraries, so the sysroot needs no file from a TV.

The linker reads only the interface of a shared library: its SONAME and DT_NEEDED list, the symbols it defines
(name, version, type, binding, visibility, size) and the symbols it references (name, version, the library that
version comes from). No code from the library ends up in the program. This tool writes that interface down as a small
text file per library (an ".abi" file) and builds a stub library from it that has the same interface and no code.
At run time the program still loads the TV's real libraries.

The references matter too: lld exports a symbol from the program when a linked library references it, and it tells a
versioned reference (malloc@GLIBC_2.4) from an unversioned one. So stubs reproduce the references exactly.

Commands (Python 3.8+, no extra modules; mkstubs needs GNU as/ld for ARM, see --as/--ld):
  tvabi.py dump LIB.so                         print the interface of one library
  tvabi.py extract SYSROOT LIST OUTDIR         write OUTDIR/<soname>.abi for each soname in LIST (first word per line,
                                               e.g. TV-REPLACED.txt); the libraries are looked up in SYSROOT
  tvabi.py mkstubs ABIDIR OUTDIR               build OUTDIR/<soname> from every ABIDIR/*.abi, then check each stub
  tvabi.py check ABIDIR STUBDIR                compare every stub's interface with its .abi file (exit 1 on a difference)
  tvabi.py compare A.so B.so                   compare the interfaces of two libraries

Only `extract` reads TV files (done once by the maintainer); the .abi files are plain text and contain no code.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ABI_VERSION = "1"

STT = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 5: "COMMON", 6: "TLS", 10: "IFUNC"}
STB = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK", 10: "UNIQUE"}
VIS = {0: "DEFAULT", 1: "INTERNAL", 2: "HIDDEN", 3: "PROTECTED"}
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
SHT_DYNAMIC, SHT_DYNSYM = 6, 11
SHT_VERDEF, SHT_VERNEED, SHT_VERSYM = 0x6FFFFFFD, 0x6FFFFFFE, 0x6FFFFFFF
SHF_EXECINSTR = 4
DT_NEEDED, DT_SONAME = 1, 14
VER_FLG_BASE, VER_FLG_WEAK = 1, 2


# ---------------------------------------------------------------------------------------------------------- reading
class Elf:
    """Minimal reader for 32-bit little-endian ELF shared libraries (all the TV's are ELF32 ARM)."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise SystemExit(f"{path}: not a 32-bit little-endian ELF file")
        (self.e_type, self.e_machine, _, _, _, shoff, self.e_flags, _, _, _, shentsize, shnum,
         shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        if not shoff or not shnum:
            raise SystemExit(f"{path}: no section headers")
        self.sh = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
        # (name, type, flags, addr, offset, size, link, info, addralign, entsize)

    def section(self, sht):
        for i, s in enumerate(self.sh):
            if s[1] == sht:
                return i, s
        return None, None

    def cstr(self, sec_index, off):
        s = self.sh[sec_index]
        start = s[4] + off
        end = self.d.index(b"\0", start)
        return self.d[start:end].decode("utf-8", "surrogateescape")


def read_abi(path):
    """Interface of a shared library as a dict (see format_abi for the fields)."""
    e = Elf(path)
    abi = {"soname": None, "needed": [], "verdefs": [], "defs": [], "unds": []}

    _, dyn = e.section(SHT_DYNAMIC)
    if dyn:
        for o in range(dyn[4], dyn[4] + dyn[5], 8):
            tag, val = struct.unpack_from("<iI", e.d, o)
            if tag == 0:
                break
            if tag == DT_NEEDED:
                abi["needed"].append(e.cstr(dyn[6], val))
            elif tag == DT_SONAME:
                abi["soname"] = e.cstr(dyn[6], val)

    # version definitions: index -> name; list of (index, flags, name, parents)
    vername = {}
    _, vd = e.section(SHT_VERDEF)
    if vd:
        o = vd[4]
        for _ in range(vd[7]):
            ver, flags, ndx, cnt, _, aux, nxt = struct.unpack_from("<HHHHIII", e.d, o)
            names, a = [], o + aux
            for _ in range(cnt):
                name, anext = struct.unpack_from("<II", e.d, a)
                names.append(e.cstr(vd[6], name))
                a += anext
            vername[ndx] = (names[0], False)
            abi["verdefs"].append((ndx, flags, names[0], names[1:]))
            if not nxt:
                break
            o += nxt

    # version needs: vna_other index -> (version name, file)
    verneed = {}
    _, vn = e.section(SHT_VERNEED)
    if vn:
        o = vn[4]
        for _ in range(vn[7]):
            ver, cnt, fname, aux, nxt = struct.unpack_from("<HHIII", e.d, o)
            fn, a = e.cstr(vn[6], fname), o + aux
            for _ in range(cnt):
                _, vflags, other, name, anext = struct.unpack_from("<IHHII", e.d, a)
                verneed[other] = (e.cstr(vn[6], name), fn)
                a += anext
            if not nxt:
                break
            o += nxt

    _, vs = e.section(SHT_VERSYM)
    dsi, ds = e.section(SHT_DYNSYM)
    if not ds:
        raise SystemExit(f"{path}: no .dynsym")
    strtab = ds[6]
    verdef_names = {v[2] for v in abi["verdefs"]}
    count = ds[5] // 16
    for i in range(1, count):
        st_name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", e.d, ds[4] + i * 16)
        bind, typ, vis = info >> 4, info & 15, other & 3
        if bind == 0:
            continue                                   # local entries (section symbols): not part of the interface
        name = e.cstr(strtab, st_name)
        vsym = struct.unpack_from("<H", e.d, vs[4] + i * 2)[0] if vs else 1
        hidden, vidx = bool(vsym & 0x8000), vsym & 0x7FFF
        tname, bname, vname = STT.get(typ, str(typ)), STB.get(bind, str(bind)), VIS.get(vis, str(vis))
        if shndx == SHN_UNDEF:
            ver, fn = verneed.get(vidx, ("-", "-")) if vidx >= 2 else ("-", "-")
            abi["unds"].append((name, ver, fn, tname, bname))
            continue
        if shndx == SHN_ABS and typ == 1 and size == 0 and name in verdef_names:
            continue                                   # GNU ld's version-name marker symbols
        if vidx >= 2 and vidx in vername:
            ver, mark = vername[vidx][0], ("@" if hidden else "@@")
        else:
            ver, mark = "-", "-"
        if shndx == SHN_ABS:
            kind = "abs:%d" % value
        elif shndx == SHN_COMMON:
            kind = "data"
        elif typ == 6:
            kind = "tls"
        elif shndx < len(e.sh) and e.sh[shndx][2] & SHF_EXECINSTR:
            kind = "code"
        else:
            kind = "data"
        abi["defs"].append((name, ver, mark, tname, bname, vname, size, kind))
    return abi


def format_abi(abi):
    out = [f"tvabi {ABI_VERSION}", f"soname {abi['soname']}"]
    out += [f"needed {n}" for n in abi["needed"]]
    for ndx, flags, name, parents in sorted(abi["verdefs"]):
        fl = "base" if flags & VER_FLG_BASE else ("weak" if flags & VER_FLG_WEAK else "-")
        out.append(" ".join(["verdef", str(ndx), fl, name] + list(parents)))
    out += ["def " + " ".join(map(str, d)) for d in sorted(abi["defs"])]
    out += ["und " + " ".join(u) for u in sorted(abi["unds"])]
    return "\n".join(out) + "\n"


def parse_abi(text, src="<abi>"):
    abi = {"soname": None, "needed": [], "verdefs": [], "defs": [], "unds": []}
    lines = text.splitlines()
    if not lines or lines[0] != f"tvabi {ABI_VERSION}":
        raise SystemExit(f"{src}: not a tvabi {ABI_VERSION} file")
    for ln in lines[1:]:
        w = ln.split(" ")
        if w[0] == "soname":
            abi["soname"] = w[1]
        elif w[0] == "needed":
            abi["needed"].append(w[1])
        elif w[0] == "verdef":
            fl = {"base": VER_FLG_BASE, "weak": VER_FLG_WEAK, "-": 0}[w[2]]
            abi["verdefs"].append((int(w[1]), fl, w[3], w[4:]))
        elif w[0] == "def":
            abi["defs"].append((w[1], w[2], w[3], w[4], w[5], w[6], int(w[7]), w[8]))
        elif w[0] == "und":
            abi["unds"].append(tuple(w[1:6]))
        elif ln.strip():
            raise SystemExit(f"{src}: bad line: {ln}")
    return abi


def key_view(abi):
    """What has to match between a library and its stub. Version indices and the order of version definitions are
    left out (the linker refers to versions by name); everything else is compared."""
    vd = sorted((name, flags & VER_FLG_BASE, tuple(parents)) for _, flags, name, parents in abi["verdefs"])
    return {"soname": abi["soname"], "needed": list(abi["needed"]), "verdefs": vd,
            "defs": sorted(abi["defs"]), "unds": sorted(abi["unds"])}


def diff_abi(a, b):
    ka, kb = key_view(a), key_view(b)
    msgs = []
    for k in ("soname", "needed", "verdefs"):
        if ka[k] != kb[k]:
            msgs.append(f"{k}: {ka[k]} != {kb[k]}")
    for k in ("defs", "unds"):
        sa, sb = set(ka[k]), set(kb[k])
        for x in sorted(sa - sb)[:20]:
            msgs.append(f"{k} only in first:  {' '.join(map(str, x))}")
        for x in sorted(sb - sa)[:20]:
            msgs.append(f"{k} only in second: {' '.join(map(str, x))}")
    return msgs


# ---------------------------------------------------------------------------------------------------------- writing
PLAIN = re.compile(r"^[A-Za-z_.$][A-Za-z0-9_.$]*$")


def q(name):
    """Symbol name for GNU as (quoted unless plain)."""
    return name if PLAIN.match(name) else '"' + name.replace("\\", "\\\\").replace('"', '\\"') + '"'


def vs_name(name):
    if not PLAIN.match(name):
        raise SystemExit(f"symbol name not usable in a version script: {name!r}")
    return name


ASTYPE = {"FUNC": "%function", "IFUNC": "%gnu_indirect_function", "OBJECT": "%object", "TLS": "%tls_object",
          "COMMON": "%object"}


def emit_def(lines, label, typ, bind, vis, size, kind):
    """Assembler lines that define `label` with the given attributes (no real content)."""
    if kind.startswith("abs:"):
        sec = None
    elif kind == "tls":
        sec = '.section .tbss,"awT",%nobits'
    elif kind == "code":
        sec = ".text"
    else:
        sec = ".data"
    if sec:
        lines.append(sec)
    if bind == "WEAK":
        lines.append(f".weak {q(label)}")
    else:
        lines.append(f".globl {q(label)}")
    if bind == "UNIQUE":
        lines.append(f".type {q(label)}, %gnu_unique_object")
    elif typ in ASTYPE:
        lines.append(f".type {q(label)}, {ASTYPE[typ]}")
    if vis == "PROTECTED":
        lines.append(f".protected {q(label)}")
    if sec:
        lines.append(".balign 4")
        lines.append(f"{q(label)}:")
    else:
        lines.append(f".set {q(label)}, {int(kind[4:])}")
    lines.append(f".size {q(label)}, {size}")


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r


class Tools:
    def __init__(self, asm, ld):
        self.asm, self.ld = asm, ld

    def assemble(self, src, obj):
        run([self.asm, "-mfloat-abi=softfp", "-meabi=5", "-march=armv7-a", "-o", obj, src])

    def link(self, out, soname, objs, libs=(), script=None):
        cmd = [self.ld, "-shared", "-m", "armelf_linux_eabi", "-soname", soname, "--no-as-needed", "--hash-style=both",
               "-z", "noexecstack", "-o", out] + list(objs) + list(libs)
        if script:
            cmd += ["--version-script", script]
        run(cmd)


def build_stub(abi, out, tools, tmp):
    """Build one stub library with the interface `abi`."""
    soname = abi["soname"]
    # 1. placeholders for every DT_NEEDED entry: same SONAME, and the versioned symbols this library takes from it
    wanted = {}                                        # file -> {version: [(name, type)]}
    for name, ver, fn, typ, bind in abi["unds"]:
        if ver != "-":
            wanted.setdefault(fn, {}).setdefault(ver, []).append((name, typ))
    for fn in wanted:
        if fn not in abi["needed"]:
            raise SystemExit(f"{soname}: version needed from {fn}, which is not in DT_NEEDED")
    ph = []
    for i, fn in enumerate(abi["needed"]):
        s, o, p = (os.path.join(tmp, f"ph{i}.{x}") for x in ("s", "o", "so"))
        lines, script = [".text", ".word 0", ".data", ".word 0", '.section .tbss,"awT",%nobits', ".space 4"], []
        for ver, syms in wanted.get(fn, {}).items():
            for name, typ in syms:
                emit_def(lines, name, "OBJECT" if typ == "OBJECT" else ("TLS" if typ == "TLS" else "FUNC"),
                         "GLOBAL", "DEFAULT", 4, "tls" if typ == "TLS" else ("data" if typ == "OBJECT" else "code"))
            script.append(f"{vs_name(ver)} {{ global: {' '.join(vs_name(n) + ';' for n, _ in syms)} }};")
        open(s, "w").write("\n".join(lines) + "\n")
        tools.assemble(s, o)
        vsf = None
        if script:
            vsf = os.path.join(tmp, f"ph{i}.map")
            open(vsf, "w").write("\n".join(script) + "\n")
        tools.link(p, fn, [o], script=vsf)
        ph.append(p)

    # 2. the stub: definitions, then references. Every section gets a few filler bytes first: GNU ld drops empty
    # output sections and moves their symbols to another section.
    lines = [".syntax unified", ".arm", ".text", ".word 0", ".data", ".word 0",
             '.section .tbss,"awT",%nobits', ".space 4"]
    nodes = {}                                         # version -> [default-version names]
    n = 0
    for name, ver, mark, typ, bind, vis, size, kind in abi["defs"]:
        if mark == "@@" and bind == "UNIQUE":          # .symver drops the unique binding: plain label + version
            emit_def(lines, name, typ, bind, vis, size, kind)       # script (C++ unique objects have one version)
            nodes.setdefault(ver, []).append(name)
        elif mark in ("@", "@@"):                      # versioned: a helper label renamed with .symver, because
            n += 1                                     # one name can exist in several versions (old + default)
            helper = f"__tvabi_{n}"
            emit_def(lines, helper, typ, bind, vis, size, kind)
            lines.append(f".symver {helper}, {q(name + mark + ver)}, remove")
        else:
            emit_def(lines, name, typ, bind, vis, size, kind)
    lines.append(".data")
    for name, ver, fn, typ, bind in abi["unds"]:
        lines.append(f".weak {q(name)}" if bind == "WEAK" else f".globl {q(name)}")
        if typ in ASTYPE and typ != "IFUNC":
            lines.append(f".type {q(name)}, {ASTYPE[typ]}")
        lines.append(f".word {q(name)}(tlsgd)" if typ == "TLS" else f".word {q(name)}")
    s, o = os.path.join(tmp, "stub.s"), os.path.join(tmp, "stub.o")
    open(s, "w").write("\n".join(lines) + "\n")
    tools.assemble(s, o)

    script = []
    for ndx, flags, ver, parents in sorted(abi["verdefs"]):
        if flags & VER_FLG_BASE:
            continue
        body = " ".join(vs_name(x) + ";" for x in sorted(nodes.get(ver, [])))
        deps = (" " + " ".join(vs_name(p) for p in parents)) if parents else ""
        script.append(f"{vs_name(ver)} {{ {'global: ' + body if body else ''} }}{deps};")
    vsf = None
    if script:
        vsf = os.path.join(tmp, "stub.map")
        open(vsf, "w").write("\n".join(script) + "\n")
    tools.link(out, soname, [o], libs=ph, script=vsf)


# ---------------------------------------------------------------------------------------------------------- commands
def find_lib(sysroot, soname):
    """The real file in the sysroot whose name or SONAME is `soname`."""
    for d in ("usr/lib/arm-linux-gnueabihf", "lib/arm-linux-gnueabihf", "usr/lib", "lib"):
        p = os.path.join(sysroot, d, soname)
        if os.path.exists(p):
            return os.path.realpath(p)
    return None


def cmd_extract(sysroot, listfile, outdir):
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for ln in open(listfile):
        w = ln.split()
        if not w:
            continue
        so = w[0]
        p = find_lib(sysroot, so)
        if not p:
            raise SystemExit(f"{so}: not found in {sysroot}")
        abi = read_abi(p)
        if abi["soname"] != so:
            print(f"note: {so}: file's SONAME is {abi['soname']}", file=sys.stderr)
        open(os.path.join(outdir, so + ".abi"), "w").write(format_abi(abi))
        n += 1
    print(f"wrote {n} .abi files to {outdir}")


def cmd_mkstubs(abidir, outdir, tools):
    os.makedirs(outdir, exist_ok=True)
    files = sorted(f for f in os.listdir(abidir) if f.endswith(".abi"))
    for f in files:
        abi = parse_abi(open(os.path.join(abidir, f)).read(), f)
        with tempfile.TemporaryDirectory() as tmp:
            build_stub(abi, os.path.join(outdir, abi["soname"]), tools, tmp)
    print(f"built {len(files)} stubs in {outdir}")
    return cmd_check(abidir, outdir)


def cmd_check(abidir, stubdir):
    bad = 0
    files = sorted(f for f in os.listdir(abidir) if f.endswith(".abi"))
    for f in files:
        want = parse_abi(open(os.path.join(abidir, f)).read(), f)
        p = os.path.join(stubdir, want["soname"])
        if not os.path.exists(p):
            print(f"MISSING {p}")
            bad += 1
            continue
        msgs = diff_abi(want, read_abi(p))
        if msgs:
            bad += 1
            print(f"DIFFERS {want['soname']}:")
            for m in msgs:
                print("   " + m)
    print(f"checked {len(files)} stubs: {len(files) - bad} match, {bad} differ")
    return 1 if bad else 0


def main(argv):
    asm = os.environ.get("TVABI_AS", "arm-linux-gnueabi-as")
    ld = os.environ.get("TVABI_LD", "arm-linux-gnueabi-ld")
    args = []
    it = iter(argv[1:])
    for a in it:
        if a == "--as":
            asm = next(it)
        elif a == "--ld":
            ld = next(it)
        else:
            args.append(a)
    if not args:
        print(__doc__)
        return 2
    c = args[0]
    if c == "dump" and len(args) == 2:
        sys.stdout.write(format_abi(read_abi(args[1])))
        return 0
    if c == "extract" and len(args) == 4:
        cmd_extract(*args[1:])
        return 0
    if c == "mkstubs" and len(args) == 3:
        for t in (asm, ld):
            if not shutil.which(t):
                raise SystemExit(f"{t} not found (GNU binutils for ARM: apt install binutils-arm-linux-gnueabi)")
        return cmd_mkstubs(args[1], args[2], Tools(asm, ld))
    if c == "check" and len(args) == 3:
        return cmd_check(args[1], args[2])
    if c == "compare" and len(args) == 3:
        msgs = diff_abi(read_abi(args[1]), read_abi(args[2]))
        print("\n".join(msgs) if msgs else "same interface")
        return 1 if msgs else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

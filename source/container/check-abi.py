#!/usr/bin/env python3
"""check-abi.py DIR: is every ELF file under DIR loadable on the TV without root and without extra libraries?

Rules (webOS 4 name lists in container/webos4/):
  - 32-bit ARM, soft-float calling convention: no "Tag_ABI_VFP_args: AAPCS VFP" (that is hard-float)
  - program interpreter (executables) = TV_LOADER (/lib/ld-linux.so.3)
  - every DT_NEEDED is either shipped next to it in DIR or listed in webos4/TV-SONAMES-webos4.txt (the libraries
    the webOS 4.4.3 TV has)
  - a strong (not weak) undefined dynamic symbol listed in webos4/known-missing.txt (missing on the TV) must be
    defined by a shipped library, otherwise the TV cannot load the binary. The list is "<version> <name>" per line
    ("-" = unversioned), and a reference matches only with the same version: expf@GLIBC_2.27 is missing on the TV,
    expf@GLIBC_2.4 is not (a correct glibc 2.24 build uses the old version).
  - no glibc symbol version newer than TV_GLIBC_MAX (required, no default); no libstdc++ (GLIBCXX_/CXXABI_) dependency
Usage: check-abi.py DIR [SHIP_DIR]
  SHIP_DIR (optional) holds libraries that ship next to DIR at run time (the NSS and NSPR files in <app>/lib). Their
  names count as shipped for DT_NEEDED, and their exports count as providers for strong references. They are not
  checked here: build-nss.sh checks them.
Prints a report; exit 1 if any rule is broken, exit 2 if TV_GLIBC_MAX is not set or SHIP_DIR is not a folder.
"""
import os
import re
import subprocess
import sys

LLVM = os.environ.get("LLVM", "/work/c151/src/third_party/llvm-build/Release+Asserts/bin")
LOADER = os.environ.get("TV_LOADER", "/lib/ld-linux.so.3")
LISTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "webos4")
# readelf --dyn-syms -W row: Num: Value Size Type Bind Vis Ndx Name
DYNSYM = re.compile(r"^\s*\d+:\s+\S+\s+\d+\s+\S+\s+(\S+)\s+\S+\s+(\S+)(?:\s+(\S+))?")


def readelf(*a):
    return subprocess.run([f"{LLVM}/llvm-readelf", *a], capture_output=True, text=True).stdout


def is_elf(p):
    try:
        with open(p, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def dynsyms(p):
    """(name, version or "", bind, undefined) for each named dynamic symbol of p."""
    out = []
    for line in readelf("--dyn-syms", "-W", p).splitlines():
        m = DYNSYM.match(line)
        if m and m.group(3):
            name, _, ver = m.group(3).partition("@")
            out.append((name, ver.lstrip("@"), m.group(1), m.group(2) == "UND"))
    return out


def load_missing(path):
    """{(version or "-", name)} from known-missing.txt; '#' lines are comments."""
    out = set()
    for line in open(path):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        out.add((f[0], f[1]) if len(f) > 1 else ("-", f[0]))
    return out


def main():
    raw = os.environ.get("TV_GLIBC_MAX", "")
    if not raw:
        print("TV_GLIBC_MAX not set", file=sys.stderr)
        sys.exit(2)
    glibc_max = tuple(int(x) for x in raw.split("."))
    d = sys.argv[1]
    ship_dir = sys.argv[2] if len(sys.argv) > 2 else ""
    if ship_dir and not os.path.isdir(ship_dir):
        print(f"SHIP_DIR {ship_dir} is not a folder", file=sys.stderr)
        sys.exit(2)
    tv = set(open(os.path.join(LISTS, "TV-SONAMES-webos4.txt")).read().split())
    missing = load_missing(os.path.join(LISTS, "known-missing.txt"))
    elfs = [os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs if is_elf(os.path.join(r, f))]
    ship_elfs = ([os.path.join(r, f) for r, _, fs in os.walk(ship_dir) for f in fs if is_elf(os.path.join(r, f))]
                 if ship_dir else [])
    shipped = {os.path.basename(p) for p in elfs + ship_elfs}
    dyn = {p: dynsyms(p) for p in elfs + ship_elfs}
    # symbols defined by shipped libraries (executables do not count: they cannot satisfy another file's reference)
    provided = set()
    for p in elfs + ship_elfs:
        if p in ship_elfs or "interpreter:" not in readelf("-l", p):
            provided |= {n for n, v, bind, und in dyn[p] if not und and bind in ("GLOBAL", "WEAK")}
    bad = 0
    for p in sorted(elfs):
        rel = os.path.relpath(p, d)
        problems = []
        h = readelf("-h", p)
        if "ARM" not in h or "ELF32" not in h:
            problems.append("not 32-bit ARM")
        attrs = readelf("-A", p)
        if "AAPCS VFP" in attrs or "VFP registers" in attrs or "hard-float ABI" in h:
            problems.append("hard-float calling convention (Tag_ABI_VFP_args / ELF hard-float flag)")
        m = re.search(r"interpreter: ([^\]]+)\]", readelf("-l", p))
        if m and m.group(1) != LOADER:
            problems.append(f"interpreter {m.group(1)} (TV has {LOADER})")
        needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", readelf("-d", p))
        for n in needed:
            if n not in tv and n not in shipped:
                problems.append(f"needs {n}: not on the TV and not shipped")
        strong = {n for n, v, bind, und in dyn[p] if und and bind == "GLOBAL" and (v or "-", n) in missing}
        for n in sorted(strong - provided):
            problems.append(f"strong reference to {n}: missing on the TV and not defined by a shipped library")
        vers = set(re.findall(r"\b(GLIBC_[0-9.]+|GLIBCXX_[0-9.]+|CXXABI_[0-9.]+)\b", readelf("-V", p)))
        for v in sorted(vers):
            if v.startswith(("GLIBCXX_", "CXXABI_")):
                problems.append(f"needs libstdc++ ({v})")
            else:
                num = tuple(int(x) for x in v[6:].split(".") if x)
                if num > glibc_max:
                    problems.append(f"needs {v} (TV has {'.'.join(map(str, glibc_max))})")
        size = os.path.getsize(p) / 1e6
        print(f"{'FAIL' if problems else 'ok  '} {rel} ({size:.1f} MB) needs: {', '.join(needed) or '-'}")
        for x in problems:
            print("       -", x)
        bad += bool(problems)
    print(f"\n{len(elfs)} ELF files, {bad} with problems")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

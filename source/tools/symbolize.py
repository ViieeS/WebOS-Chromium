#!/usr/bin/env python3
"""Turn a Chrome crash stack from the TV (lines with "chrome+0x...") into function names.

usage: symbolize.py chrome.syms.zst browser.log
chrome.syms.zst comes from artifacts/<build-id>/ (collect.sh, `llvm-nm -n -C --defined-only`); it must be from the
same build that crashed. Thumb code addresses have bit 0 set in the stack; it is cleared before the lookup.
"""
import bisect
import re
import subprocess
import sys

syms_path, log_path = sys.argv[1], sys.argv[2]
nm = subprocess.run(["zstd", "-dc", syms_path], capture_output=True, text=True, check=True).stdout
addrs, names = [], []
for line in nm.splitlines():
    parts = line.split(" ", 2)
    if len(parts) == 3 and parts[1] in "tTwW":
        addrs.append(int(parts[0], 16))
        names.append(parts[2])
pairs = sorted(zip(addrs, names))
addrs = [a for a, _ in pairs]
names = [n for _, n in pairs]

for line in open(log_path, errors="replace"):
    m = re.search(r"chrome\+0x([0-9a-f]+)", line)
    if not m:
        continue
    pc = int(m.group(1), 16) & ~1
    i = bisect.bisect_right(addrs, pc) - 1
    where = f"{names[i]} +0x{pc - addrs[i]:x}" if i >= 0 else "?"
    print(line.rstrip()[:40], "->", where[:200])

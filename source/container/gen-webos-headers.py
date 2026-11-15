#!/usr/bin/env python3
"""gen-webos-headers.py SDK_INCLUDE_DIR OUT_INCLUDE_DIR

Writes the wayland-webos-*-protocol.h headers Chromium includes, generated from the protocol definitions of the
TV's OWN libwayland-webos-client (the XML files in /opt/cn/tvproto = tvproto/, checked against a TV's own
libraries). So the request opcodes and event order Chromium compiles in are exactly the TV's.

The TV-derived tv-stubs.xml has no <enum> blocks. Enums do not change the wire format, so C enum blocks are copied
from the openlgtv SDK header of the same name when the generated header lacks them.
Chromium code that uses a request/event the TV does not have then fails to COMPILE (good: we find it before running
on the TV) and gets a patch in patches/.
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

P = "/opt/cn/tvproto"
GROUPS = {  # header base name -> (source xml, interfaces or None for all, also server header?)
    "wayland-webos-shell": ("webos-shell.xml", None, True),
    "wayland-webos-input-manager": ("webos-input-manager.xml", None, False),
    "wayland-webos-foreign": ("webos-foreign.xml", None, False),
    "wayland-webos-surface-group": ("tv-stubs.xml", ["wl_webos_surface_group_compositor", "wl_webos_surface_group",
                                                     "wl_webos_surface_group_layer"], False),
    "wayland-webos-extension": ("tv-stubs.xml", ["text_model_factory", "text_model", "wl_webos_xinput",
                                                 "wl_webos_xinput_extension"], False),
}
ENUM_BLOCK = re.compile(r"#ifndef (\w+_ENUM)\n#define \1\n.*?\n#endif /\* \1 \*/\n", re.S)


def subset_xml(src, names, dst, proto_name):
    root = ET.parse(src).getroot()
    out = ET.Element("protocol", {"name": proto_name})
    for c in root:
        if c.tag == "copyright":
            out.append(c)
        if c.tag == "interface" and (names is None or c.get("name") in names):
            out.append(c)
    missing = set(names or []) - {i.get("name") for i in out.findall("interface")}
    if missing:
        sys.exit(f"{src}: interfaces not found: {sorted(missing)}")
    ET.ElementTree(out).write(dst, encoding="utf-8", xml_declaration=True)


def add_enums(gen_path, sdk_path):
    if not os.path.isfile(sdk_path):
        return 0
    gen = open(gen_path).read()
    have = set(re.findall(r"#ifndef (\w+_ENUM)\n", gen))
    extra = [m.group(0) for m in ENUM_BLOCK.finditer(open(sdk_path).read()) if m.group(1) not in have]
    if not extra:
        return 0
    marker = "#ifdef  __cplusplus\n}\n#endif"
    i = gen.rfind(marker)
    if i < 0:
        sys.exit(f"{gen_path}: end marker not found")
    open(gen_path, "w").write(gen[:i] + "\n/* enums copied from the openlgtv SDK header (no wire effect) */\n"
                              + "\n".join(extra) + "\n" + gen[i:])
    return len(extra)


def main():
    sdk_inc, out_inc = sys.argv[1], sys.argv[2]
    tmp = "/tmp/webos-proto"
    os.makedirs(tmp, exist_ok=True)
    for base, (xml, names, server) in GROUPS.items():
        x = os.path.join(tmp, base + ".xml")
        subset_xml(os.path.join(P, xml), names, x, base.replace("-", "_"))
        kinds = [("client-header", "client-protocol.h")] + ([("server-header", "server-protocol.h")] if server else [])
        for kind, suffix in kinds:
            h = os.path.join(out_inc, f"{base}-{suffix}")
            subprocess.run(["wayland-scanner", kind, x, h], check=True)
            n = add_enums(h, os.path.join(sdk_inc, f"{base}-{suffix}"))
            print(f"{os.path.basename(h)}: from TV {xml}, {n} enum blocks from SDK")


if __name__ == "__main__":
    main()

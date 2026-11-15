#!/bin/bash
# build-sysroot-compat.sh (2026-11-01): sysroot for the browser on older TVs (webOS 4.4 and newer: glibc 2.24-2.31),
# saved as snapshot "sysroot-compat" (same directory name sysroot-tv, so args.gn and ninja paths stay the same).
#
# Starts from the normal sysroot (snapshot "sysroot", TV webOS 10 libraries, glibc 2.35) and replaces ONLY glibc:
#   - headers, crt*.o, libc_nonshared.a and the link-time libc/libm/libpthread/... from Debian stretch's soft-float
#     armel cross packages (glibc 2.24, the oldest glibc of webOS 4.4). Soft-float objects = the softfp ABI's calling
#     convention, as in build-sysroot.sh step 2b.
# The webOS Wayland headers are regenerated with wayland-scanner 1.18 (step 2b); patch 0031 does the same for
# Chromium's own protocol code.
# A binary linked here asks only for glibc symbol versions <= 2.24, which webOS 4.4 and newer all have. The other
# libraries stay the webOS 10 TV's: older TVs have the same libraries (webosbrew elf-verify, webOS 5.3.1: no missing
# library, only glibc versions and wl_proxy_marshal_flags missing), and their glibc 2.34+ references are only
# link-time noise (args-compat.gn: --allow-shlib-undefined).
. /opt/cn/container/lib.sh
[ "$VARIANT" = compat ] || die "run this from the compat checkout (config.env VARIANT=compat)"
S=$SYSROOT
ML=arm-linux-gnueabihf
DEB=https://archive.debian.org/debian/pool/main/c/cross-toolchain-base
snap_restore sysroot "$W" || die "no sysroot snapshot (run build_sysroot first)"

# 1. remove jammy's libc6-dev files (headers, crt, static libs, dev symlinks)
ver=$(sed -n 's/^libc6-dev_\(.*\)$/\1/p' "$S/PACKAGES.txt" | head -1)
[ -n "$ver" ] || die "libc6-dev not in PACKAGES.txt"
G=$W/glibc-compat; rm -rf "$G"; mkdir -p "$G"
v=${ver//%3a/:}
if [ "${SYSROOT_SOURCE:-tv}" = public ]; then   # the pinned file from Launchpad (the ports pool drops old versions)
	h=$(awk -v f="libc6-dev_${ver}_armhf.deb" '$1==f{print $2}' "$CN/container/sysroot-pinned.txt")
	[ -n "$h" ] || die "libc6-dev_${ver}_armhf.deb not in sysroot-pinned.txt"
	curl -fsSL --retry 4 -o "$G/libc6-dev.deb" "https://launchpad.net/ubuntu/+archive/primary/+files/libc6-dev_${ver}_armhf.deb" ||
		die "cannot download libc6-dev_${ver}_armhf.deb"
	echo "$h  $G/libc6-dev.deb" | sha256sum -c --quiet - || die "libc6-dev sha256 mismatch"
else
curl -fsSL -o "$G/libc6-dev.deb" "http://ports.ubuntu.com/ubuntu-ports/pool/main/g/glibc/libc6-dev_${ver}_armhf.deb" ||
	die "cannot download libc6-dev_${ver}_armhf.deb"
fi
n=0
while read -r f; do
	f=${f#./}
	case "$f" in ""|usr/share/*|*/) continue ;; esac
	[ -L "$S/$f" ] || [ -f "$S/$f" ] || continue
	rm -f "$S/$f"; n=$((n + 1))
done < <(dpkg-deb -c "$G/libc6-dev.deb" | awk '{print $6}')
# build-sysroot.sh made gnu/stubs-soft.h from jammy's stubs-hard.h; it would shadow glibc 2.24's own
rm -f "$S/usr/include/$ML/gnu/stubs-soft.h"
log "removed $n jammy libc6-dev files ($v)"

# 2. glibc 2.24 from Debian stretch (soft-float armel cross packages)
for p in libc6-armel-cross libc6-dev-armel-cross; do
	curl -fsSL -o "$G/$p.deb" "$DEB/${p}_2.24-10cross1_all.deb" || die "cannot download ${p}_2.24-10cross1_all.deb"
	dpkg-deb -x "$G/$p.deb" "$G/x"
done
X=$G/x/usr/arm-linux-gnueabi
[ -f "$X/lib/libc-2.24.so" ] || die "stretch libc-2.24.so missing"
cp -a "$X/include/." "$S/usr/include/"
# runtime libraries (link time only; the TV's own glibc is used at run time)
mkdir -p "$S/lib/$ML" "$S/usr/lib/$ML"
for f in "$X"/lib/*.so.* "$X"/lib/*-2.24.so; do
	[ -e "$f" ] || [ -L "$f" ] || continue
	rm -f "$S/lib/$ML/$(basename "$f")"; cp -a "$f" "$S/lib/$ML/"
done
cp -L "$X/lib/ld-linux.so.3" "$S/lib/ld-linux.so.3"
# crt objects, static archives, dev links / linker scripts
for f in "$X"/lib/*.o "$X"/lib/*.a "$X"/lib/*.so; do
	[ -e "$f" ] || [ -L "$f" ] || continue
	b=$(basename "$f")
	rm -f "$S/usr/lib/$ML/$b"
	if [ -L "$f" ]; then
		ln -s "../../../lib/$ML/$(basename "$(readlink "$f")")" "$S/usr/lib/$ML/$b"
	elif grep -q "GNU ld script" "$f" 2>/dev/null || grep -q "^GROUP" "$f" 2>/dev/null; then
		sed -e "s#/usr/arm-linux-gnueabi/lib/\([a-z_]*_nonshared\.a\)#/usr/lib/$ML/\1#g" \
			-e "s#/usr/arm-linux-gnueabi/lib/ld-linux\.so\.3#/lib/ld-linux.so.3#g" \
			-e "s#/usr/arm-linux-gnueabi/lib/#/lib/$ML/#g" "$f" > "$S/usr/lib/$ML/$b"
	else
		cp -a "$f" "$S/usr/lib/$ML/"
	fi
done
grep -h "GROUP" "$S/usr/lib/$ML/libc.so" | tee -a "$LOG"
[ -f "$S/usr/include/gnu/stubs-soft.h" ] || die "no gnu/stubs-soft.h in the glibc 2.24 headers"

# 2b. webOS Wayland headers again, with wayland-scanner 1.18 (Ubuntu 20.04): the build-sysroot.sh ones come from
# jammy's scanner 1.20, whose request stubs call wl_proxy_marshal_flags (libwayland 1.20+). 1.18's stubs use
# wl_proxy_marshal* + wl_proxy_destroy, which every libwayland since 1.10 has. Enum blocks are taken over from the
# current headers (gen-webos-headers.py copies missing ones from its first argument).
mkdir -p "$G/scanner"
curl -fsSL -o "$G/wlbin.deb" http://archive.ubuntu.com/ubuntu/pool/main/w/wayland/libwayland-bin_1.18.0-1ubuntu0.1_amd64.deb ||
	die "cannot download libwayland-bin 1.18"
dpkg-deb -x "$G/wlbin.deb" "$G/scanner"
"$G/scanner/usr/bin/wayland-scanner" --version 2>&1 | tee -a "$LOG"
mkdir -p "$G/oldinc" && cp -a "$S"/usr/include/wayland-webos-* "$G/oldinc/"
PATH=$G/scanner/usr/bin:$PATH python3 "$CN/container/gen-webos-headers.py" "$G/oldinc" "$S/usr/include" | tee -a "$LOG"
# the core protocol header too (the webOS wl_shell surface code includes the sysroot's, not Chromium's): from the
# sysroot's own wayland.xml, so the protocol stays jammy's, only the stubs change
"$G/scanner/usr/bin/wayland-scanner" client-header "$S/usr/share/wayland/wayland.xml" "$S/usr/include/wayland-client-protocol.h"
grep -l wl_proxy_marshal_flags "$S"/usr/include/wayland-*-protocol.h && die "protocol headers still call wl_proxy_marshal_flags"

# 3. checks: glibc 2.24 headers and libs; no hard-float crt
grep -q "define\s*__GLIBC_MINOR__\s*24" "$S/usr/include/features.h" || die "headers are not glibc 2.24"
for f in crt1.o crti.o Scrt1.o libc_nonshared.a; do
	readelf -A "$S/usr/lib/$ML/$f" 2>/dev/null | grep -q 'Tag_ABI_VFP_args: VFP registers' && die "$f is hard-float"
done
{ strings -a "$(readlink -f "$S/lib/$ML/libc.so.6")" | grep -m1 "GNU C Library" | tee -a "$LOG"; } || true
rm -rf "$G"
echo "glibc-compat 2.24 (stretch 2.24-10cross1), webOS wayland headers from wayland-scanner 1.18" >> "$S/PACKAGES.txt"

$META set "$SNAP/sysroot-compat.json" id="$BUILD_ID" glibc=2.24 base_sysroot="$($META get "$SNAP/sysroot.json" id)"
snap_save sysroot-compat "$W" sysroot-tv
st status=done sysroot_id="$BUILD_ID"
push_log

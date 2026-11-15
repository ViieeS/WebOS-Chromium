#!/bin/bash
# collect.sh: copy what the browser needs at run time from out/tv into artifacts/<build-id>/, check it with
# check-abi.py, and pack it. Called by build.sh when the build succeeded and COLLECT=1 (build --collect).
# File lists per target: Chromium's usual Linux run-time files. A new Chromium version may need changes here.
. /opt/cn/container/lib.sh
[ -d "$OUT" ] || die "no out dir (collect.sh runs only inside build.sh)"
O=$ART/$BUILD_ID; B=$O/browser; rm -rf "$B"; mkdir -p "$B"
case " ${TARGETS:-chrome} " in
*" chrome "*)
	FILES="chrome chrome_crashpad_handler *.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin locales \
		libEGL.so libGLESv2.so libvk_swiftshader.so vk_swiftshader_icd.json libvulkan.so.1 \
		gen/components/resources/about_credits.html" ;;
*)
	FILES="browser_shell libcbe.so icudtl.dat snapshot_blob.bin v8_context_snapshot.bin cbe_data locales resources *.pak" ;;
esac
cd "$OUT"
for f in $FILES; do
	for g in $f; do [ -e "$g" ] && cp -a "$g" "$B/" ; done
done
find "$B" -type f | sed "s|^$B/||" | sort > "$O/files.txt"
# Symbol table of the unstripped chrome (2026-10-31): the IPK's chrome is stripped, so crash stacks on the TV show
# only offsets (chrome+0x...). chrome.syms.zst maps them back to function names (tools/symbolize.py).
if [ -f "$OUT/chrome" ]; then
	"$LLVM/llvm-nm" -n -C --defined-only "$OUT/chrome" 2>/dev/null | zstd -T0 -10 -q -o "$O/chrome.syms.zst" || true
	log "chrome symbols: $(stat -c %s "$O/chrome.syms.zst" 2>/dev/null || echo none) bytes"
fi
log "collected $(wc -l < "$O/files.txt") files, $(du -sh "$B" | cut -f1)"
for e in $(find "$B" -type f); do
	head -c4 "$e" | grep -q ELF && "$LLVM/llvm-strip" --strip-unneeded "$e" 2>/dev/null || true
done
# compat build: the browser must not need libwayland 1.20+ (webOS 8 and older)
if [ "${VARIANT:-main}" = compat ] && "$LLVM/llvm-nm" -D "$B/chrome" 2>/dev/null | grep -q wl_proxy_marshal_flags; then
	die "compat chrome still needs wl_proxy_marshal_flags (libwayland 1.20+)"
fi
# every build: wl_egl_window_* are found at run time (patch 0019, cn_wayland_egl.c); the file name differs per TV
if "$LLVM/llvm-readelf" -d "$B/chrome" 2>/dev/null | grep -q "libwayland-egl"; then
	die "chrome still needs libwayland-egl by name (an LG C1 has no libwayland-egl.so.1; patch 0019)"
fi
# the NSS and NSPR libraries ship in <app>/lib (mkbeta --lib-dir), not in this tree: check against the newest build_nss artifact
NSS_TAR=$(ls -t "$ART"/*/nss.tar.zst 2>/dev/null | head -1 || true)
[ -n "$NSS_TAR" ] || die "no nss.tar.zst under $ART: run build_nss first"
NSSD=$W/nss-check; rm -rf "$NSSD"; mkdir -p "$NSSD"
zstd -dc "$NSS_TAR" | tar -C "$NSSD" -xf - lib
log "check-abi: NSS/NSPR shipped set from $NSS_TAR"
set +e
LLVM=$LLVM SYSROOT=$SYSROOT TV_GLIBC_MAX=$TV_GLIBC_MAX TV_LOADER=$TV_LOADER python3 "$CN/container/check-abi.py" "$B" "$NSSD/lib" > "$O/abi-report.txt" 2>&1
abi=$?
set -e
cat "$O/abi-report.txt" | tee -a "$LOG"
cp "$SYSROOT/TV-SONAMES.txt" "$O/"
tar --format=posix -C "$O" -cf - browser | zstd -T0 -19 -q -o "$O/browser.tar.zst"
rm -rf "$B"
(cd "$O" && sha256sum browser.tar.zst > SHA256SUMS)
st abi_ok=$((abi == 0)) browser_tar_bytes="$(stat -c %s "$O/browser.tar.zst")"
[ $abi -eq 0 ] || die "check-abi found problems (artifacts/$BUILD_ID/abi-report.txt)"

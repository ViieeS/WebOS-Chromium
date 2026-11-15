#!/bin/bash
# build-nss-sdk.sh: NSS 3.125 and NSPR 4.38.2 for the webOS 4 TVs, built on the build host with the prebuilt openlgtv SDK.
# why: the buildroot source build (container/build-nss.sh) did not finish on Modal (glibc-polyfills pulled in by the
# toolchain), so this uses the released SDK 2026.08-webos instead. It has gcc 16.2.0 and the
# glibc 2.12 sysroot, but no NSS or NSPR. The two libraries are built here from the upstream Mozilla tarballs with the
# same versions, the buildroot package patches (container/nss/patches) and the buildroot make variables.
# Glibc polyfills stay off: -tno-lgtv-compat on every compile and link, and a check that no shipped file uses a symbol
# from the SDK's libglibc_polyfills.a. Output: nss.tar.zst (lib/ and nss-report.txt) and nss.sha256 under
# ~/nssjob/out/<id>/, and the same archive on the Modal Volume at artifacts/<id>/ (collect.sh reads it from there).
# Runs on the build host (no Modal compute). Usage: bash container/build-nss-sdk.sh
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)            # the folder with config.env
set -a; . "$HERE/config.env"; set +a

NJ=$HOME/nssjob
case "$(uname -m)" in
	aarch64) SDK_URL=$SDK_URL_AARCH64; SDK_SHA256=$SDK_SHA256_AARCH64; HOSTARCH=aarch64 ;;
	x86_64) SDK_URL=$SDK_URL_X86_64; SDK_SHA256=$SDK_SHA256_X86_64; HOSTARCH=x86_64 ;;
	*) echo "ERROR: no SDK for host $(uname -m)" >&2; exit 1 ;;
esac
SDK_DIR=$NJ/sdk-$HOSTARCH/arm-webos-linux-gnueabi_sdk-buildroot
SDK_TAR=$NJ/sdk-$HOSTARCH.tar.gz
SYSROOT=$SDK_DIR/arm-webos-linux-gnueabi/sysroot
WORK=$NJ/work
SHIP=$HERE/container/nss/ship-list.txt
TVLIST=$HERE/container/webos4/TV-SONAMES-webos4.txt
PATCHES=$HERE/container/nss/patches
GLIBC_LIMIT=2.24
ID="$(date -u +%Y%m%dT%H%M%SZ)-build_nss_sdk"
OUT=$NJ/out/$ID
LOG=$OUT/build.log

die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"; }

# ver_le A B: true when version A is less than or equal to version B.
ver_le() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }

mkdir -p "$OUT"; : >"$LOG"

# 0. inputs
for v in SDK_URL SDK_SHA256 NSS_TARBALL_URL NSS_TARBALL_SHA256 NSPR_TARBALL_URL NSPR_TARBALL_SHA256 NSS_VERSION NSPR_VERSION SDK_TAG; do
	[ -n "${!v:-}" ] || die "$v is not set in config.env"
done
[ "$(grep -c . "$SHIP")" -eq 10 ] || die "ship-list.txt must have 10 names"
for p in "$PATCHES"/*.patch; do [ -e "$p" ] || die "no patches in $PATCHES"; done

# 1. SDK: download once, verify the release sha256, extract, relocate
log "SDK $SDK_TAG"
if [ ! -f "$SDK_TAR" ]; then
	curl -fsSL -o "$SDK_TAR.part" "$SDK_URL" && mv "$SDK_TAR.part" "$SDK_TAR" || die "SDK download failed: $SDK_URL"
fi
echo "$SDK_SHA256  $SDK_TAR" | sha256sum -c - >>"$LOG" 2>&1 || die "SDK sha256 does not match config.env"
if [ ! -d "$SDK_DIR" ]; then mkdir -p "$NJ/sdk-$HOSTARCH" && tar -xzf "$SDK_TAR" -C "$NJ/sdk-$HOSTARCH"; fi
[ -d "$SDK_DIR" ] || die "SDK did not extract to $SDK_DIR"
file -L "$SDK_DIR/bin/arm-webos-linux-gnueabi-gcc" | tee -a "$LOG" | grep -qi "$( [ $HOSTARCH = aarch64 ] && echo aarch64 || echo x86-64 )" ||
	echo "WARNING: the SDK gcc is not a native $HOSTARCH binary" | tee -a "$LOG"
(cd "$SDK_DIR" && ./relocate-sdk.sh >>"$LOG" 2>&1) || die "relocate-sdk.sh failed (see $LOG)"
[ -x "$SDK_DIR/bin/arm-webos-linux-gnueabi-gcc" ] || die "cross gcc not found in the SDK"
[ -d "$SYSROOT/usr/lib" ] || die "sysroot not found at $SYSROOT"

# RESUME=1: keep the extracted, patched sources and the NSPR stage from the last run, and continue the NSS build
NSS_SRC=$WORK/src/nss-$NSS_VERSION
NSPR_SRC=$WORK/src/nspr-$NSPR_VERSION/nspr
STAGE=$WORK/stage
if [ "${RESUME:-0}" = 1 ] && [ -e "$STAGE/usr/local/lib/libnspr4.so" ] && [ -d "$NSS_SRC/nss" ]; then
	RESUMED=1; log "RESUME=1: reusing $WORK (sources, patches, NSPR stage)"
else
	RESUMED=0
fi
# 2. upstream tarballs, checked against the Mozilla SHA256SUMS values in config.env
if [ $RESUMED = 0 ]; then
rm -rf "$WORK"; mkdir -p "$WORK/dl" "$WORK/src"
fetch() {  # fetch URL SHA256 DEST
	curl -fsSL -o "$3" "$1" || die "download failed: $1"
	echo "$2  $3" | sha256sum -c - >>"$LOG" 2>&1 || die "sha256 mismatch for $1"
}
fetch "$NSS_TARBALL_URL" "$NSS_TARBALL_SHA256" "$WORK/dl/nss.tar.gz"
fetch "$NSPR_TARBALL_URL" "$NSPR_TARBALL_SHA256" "$WORK/dl/nspr.tar.gz"
tar -xzf "$WORK/dl/nss.tar.gz" -C "$WORK/src"
tar -xzf "$WORK/dl/nspr.tar.gz" -C "$WORK/src"
NSS_SRC=$WORK/src/nss-$NSS_VERSION
NSPR_SRC=$WORK/src/nspr-$NSPR_VERSION/nspr
[ -d "$NSS_SRC/nss" ] || die "NSS source tree not found"
[ -d "$NSPR_SRC" ] || die "NSPR source tree not found"

# 3. buildroot package patches (apply in name order, paths relative to the NSS source root)
for p in "$PATCHES"/*.patch; do
	patch -d "$NSS_SRC" -p1 -s <"$p" || die "patch failed: $(basename "$p")"
	log "patch applied: $(basename "$p")"
done
fi

# 4. cross environment: SDK gcc, no webOS compat (-tno-lgtv-compat), softfp, cortex-a9 with neon-fp16
export PATH="$SDK_DIR/bin:$PATH"
export CC=arm-webos-linux-gnueabi-gcc AR=arm-webos-linux-gnueabi-gcc-ar RANLIB=arm-webos-linux-gnueabi-gcc-ranlib
# -Dsecure_getenv=getenv: the SDK's glibc 2.12 has no secure_getenv (2.17), and NSS freebl calls it (run
# 20261104T132429Z). It only differs from getenv in setuid programs; the browser is not setuid. (The SDK's own fix
# for this would be glibc-polyfills, which we do not use.)
TCFLAGS="-mcpu=cortex-a9 -mfpu=neon-fp16 -mfloat-abi=softfp -O2 -tno-lgtv-compat -Dsecure_getenv=getenv"
TLDFLAGS="-tno-lgtv-compat"
# NSS builds its host helper nsinstall with NATIVE_CC=/usr/bin/gcc but the same LDFLAGS, and the host gcc rejects
# -tno-lgtv-compat (run 20261104T123450Z). So the NSS target flags go into a cross-compiler wrapper instead of Linux.mk.
mkdir -p "$WORK/bin"
printf '#!/bin/sh\nexec %s %s "$@"\n' "$SDK_DIR/bin/arm-webos-linux-gnueabi-gcc" "$TCFLAGS" >"$WORK/bin/cross-cc"
chmod +x "$WORK/bin/cross-cc"
STAGE=$WORK/stage
log "toolchain: $($CC --version | sed -n 1p)"
log "sysroot: $($CC -print-sysroot)"

# 5. NSPR (configure with the buildroot options for an ARM target, 32-bit)
if [ $RESUMED = 0 ]; then
log "NSPR $NSPR_VERSION configure"
# HOST_CC: NSPR builds nsinstall (a helper that runs on the build machine) with it. Without it, configure picks the
# cross compiler and the install step fails with "nsinstall: No such file or directory" (run 20261104T114748Z).
(cd "$NSPR_SRC" && CFLAGS="$TCFLAGS" LDFLAGS="$TLDFLAGS" HOST_CC=/usr/bin/gcc HOST_CFLAGS="-g -O2" HOST_LDFLAGS="-lc" \
	./configure --host=arm-webos-linux-gnueabi --disable-64bit >"$OUT/nspr-configure.log" 2>&1) ||
	die "NSPR configure failed (see $OUT/nspr-configure.log)"
log "NSPR build"
(cd "$NSPR_SRC" && make -j"$(nproc)" >"$OUT/nspr-build.log" 2>&1 && make install DESTDIR="$STAGE" >>"$OUT/nspr-build.log" 2>&1) ||
	die "NSPR build failed (see $OUT/nspr-build.log)"
NSPR_INC=$STAGE/usr/local/include/nspr
NSPR_LIB=$STAGE/usr/local/lib
fi
NSPR_INC=$STAGE/usr/local/include/nspr
NSPR_LIB=$STAGE/usr/local/lib
[ -e "$NSPR_LIB/libnspr4.so" ] || die "libnspr4.so was not built"

# 6. NSS: buildroot's libnss.mk make variables (cross build, system sqlite and zlib from the sysroot)
log "NSS $NSS_VERSION build"
# buildroot appends the target flags to coreconf/Linux.mk (LIBNSS_FIXUP_LINUX_MK)
# (target flags: in $WORK/bin/cross-cc, see section 4; host tools keep plain /usr/bin/gcc)
# CC through the environment, not the make command line: a command-line CC also overrides coreconf/nsinstall's
# CC = $(NATIVE_CC), and an ARM nsinstall cannot run on the build host (run 20261104T125314Z).
export CC=$WORK/bin/cross-cc
NSS_VARS=(BUILD_OPT=1 MOZILLA_CLIENT=1 "NSPR_INCLUDE_DIR=$NSPR_INC" "NSPR_LIB_DIR=$NSPR_LIB" NS_USE_GCC=1 NSS_DISABLE_GTESTS=1
	NSS_USE_SYSTEM_SQLITE=1 USE_SYSTEM_ZLIB=1 NATIVE_CC=/usr/bin/gcc OS_ARCH=Linux OS_RELEASE=2.6 OS_TEST=arm
	NSS_ENABLE_WERROR=0 FREEBL_NO_DEPEND=1 "SOURCE_MD_DIR=$NSS_SRC/dist" "DIST=$NSS_SRC/dist" CHECKLOC=)
# Serial, like buildroot's $(MAKE1) for libnss: with -j, coreconf's $(shell $(MAKE) get_objs) captured make's
# "Entering directory '...'" lines and rules.mk failed with "multiple target patterns" (run 20261104T130847Z).
(cd "$NSS_SRC/nss" && make -j1 --no-print-directory coreconf "${NSS_VARS[@]}" >"$OUT/nss-build.log" 2>&1 &&
	make -j1 --no-print-directory lib/dbm all "${NSS_VARS[@]}" NATIVE_FLAGS="-DLINUX" NATIVE_LDFLAGS="" >>"$OUT/nss-build.log" 2>&1) ||
	die "NSS build failed (see $OUT/nss-build.log)"
NSS_LIB=$NSS_SRC/dist/lib
log "NSS build done"

# 7. copy the ten shipped files (dereferenced). NSS files come from dist/lib, NSPR files from the NSPR stage.
rm -rf "$WORK/nss-out"; mkdir -p "$WORK/nss-out/lib"
while IFS= read -r n; do
	[ -n "$n" ] || continue
	if [ -e "$NSS_LIB/$n" ]; then src=$NSS_LIB/$n
	elif [ -e "$NSPR_LIB/$n" ]; then src=$NSPR_LIB/$n
	else die "$n was not built"; fi
	cp -L "$src" "$WORK/nss-out/lib/$n"
done <"$SHIP"
NO=$WORK/nss-out
CHECKS=$WORK/checks.txt; : >"$CHECKS"

# 8. checks (fatal on failure)
# 8a. ELF rules from design 5.2.1, the same as container/build-nss.sh
allg=""
while IFS= read -r n; do
	[ -n "$n" ] || continue
	f=$NO/lib/$n
	hdr=$(readelf -hW "$f") || die "readelf -h failed on $n"
	attr=$(readelf -A "$f") || die "readelf -A failed on $n"
	phdr=$(readelf -lW "$f") || die "readelf -l failed on $n"
	dyn=$(readelf -W --dyn-syms "$f") || die "readelf --dyn-syms failed on $n"
	ver=$(readelf -WV "$f") || die "readelf -V failed on $n"
	dsec=$(readelf -d "$f") || die "readelf -d failed on $n"
	grep -Eq 'Class: +ELF32' <<<"$hdr" || die "$n: not ELF32"
	grep -Eq 'Machine: +ARM' <<<"$hdr" || die "$n: not ARM"
	grep -q 'Version5 EABI' <<<"$hdr" || die "$n: not EABI5"
	grep -Eq 'Tag_ABI_VFP_args: (AAPCS )?VFP' <<<"$attr" && die "$n: hard-float VFP argument tag"
	grep -q 'INTERP' <<<"$phdr" && die "$n: has PT_INTERP (a shared library must not)"
	gl=$(printf '%s\n%s\n' "$dyn" "$ver" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV || true)
	maxg=${gl##*$'\n'}
	if [ -n "$maxg" ]; then ver_le "${maxg#GLIBC_}" "$GLIBC_LIMIT" || die "$n: needs $maxg, the TV has glibc $GLIBC_LIMIT"; fi
	allg="$allg $maxg"
	case "$dyn$ver" in *GLIBCXX_*|*CXXABI_*) die "$n: uses libstdc++ or libc++abi symbols" ;; esac
	needed=$(sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' <<<"$dsec")
	while IFS= read -r d; do
		[ -n "$d" ] || continue
		grep -qxF "$d" "$SHIP" && continue
		grep -qxF "$d" "$TVLIST" && continue
		die "$n: DT_NEEDED $d is neither shipped nor on the webOS 4 list"
	done <<<"$needed"
	printf '%s: ELF32 ARM soft-float, no PT_INTERP, newest GLIBC %s, needs: %s\n' "$n" "${maxg:-none}" \
		"$(tr '\n' ' ' <<<"$needed")" >>"$CHECKS"
	log "check ok: $n (newest GLIBC ${maxg:-none})"
done <"$SHIP"
maxall=$(printf '%s\n' $allg | sort -uV | tail -1)

# 8b. no polyfills: the symbols the SDK's glibc polyfill archive defines must not appear in any shipped file, and no
# shipped file may name a polyfill library.
POLY=$WORK/polyfill-syms.txt
nm --defined-only -g "$SYSROOT/usr/lib/libglibc_polyfills.a" 2>/dev/null | awk 'NF==3 {print $3}' | sort -u >"$POLY"
[ -s "$POLY" ] || die "no symbols read from libglibc_polyfills.a"
log "polyfill archive defines $(wc -l <"$POLY") symbols"
while IFS= read -r n; do
	[ -n "$n" ] || continue
	f=$NO/lib/$n
	defs=$(readelf -W --dyn-syms "$f" | awk '$7 != "UND" && $8 != "" {print $8}' | sed 's/@.*//' | sort -u)
	# strong references only: a WEAK undefined reference is optional and resolves to the TV's own glibc or to NULL.
	# libfreebl3 has a weak getauxval (glibc 2.16, on every webOS 4+ TV), used only to read the ARM CPU features.
	refs=$(readelf -W --dyn-syms "$f" | awk '$7 == "UND" && $5 != "WEAK" && $8 != "" {print $8}' | sed 's/@.*//' | sort -u)
	weak=$(readelf -W --dyn-syms "$f" | awk '$7 == "UND" && $5 == "WEAK" && $8 != "" {print $8}' | sed 's/@.*//' | sort -u)
	w=$(comm -12 "$POLY" <(printf '%s\n' "$weak" | sort -u) | tr '\n' ' ')
	[ -z "$w" ] || log "$n: weak (optional) references also defined by the polyfill archive, resolved by the TV's glibc: $w"
	hit=$(comm -12 "$POLY" <(printf '%s\n' "$defs" | sort -u) | head -3 | tr '\n' ' ')
	[ -z "$hit" ] || die "$n: defines polyfill symbols: $hit"
	hit=$(comm -12 "$POLY" <(printf '%s\n' "$refs" | sort -u) | head -3 | tr '\n' ' ')
	[ -z "$hit" ] || die "$n: needs polyfill symbols from the TV: $hit"
	readelf -d "$f" | grep -i 'NEEDED' | grep -qi 'polyfill\|_compat' && die "$n: NEEDED a polyfill or compat library"
done <"$SHIP"
log "polyfill check ok: no shipped file defines or needs a polyfill symbol"

# 9. report
nssh=$(find "$NSS_SRC/dist/public" -name nss.h -print -quit 2>/dev/null || true)
prh=$(find "$NSPR_INC" -name prinit.h -print -quit 2>/dev/null || true)   # NSPR has no prversion.h; PR_VERSION is in prinit.h
[ -n "$nssh" ] || die "nss.h not found"
[ -n "$prh" ] || die "prversion.h not found"
nssv=$(sed -n 's/^#define NSS_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' "$nssh" | sed -n '1p')
nsprv=$(sed -n 's/^#define PR_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' "$prh" | sed -n '1p')
[ "$nssv" = "$NSS_VERSION" ] || die "NSS in the build is '$nssv', config.env says $NSS_VERSION"
[ "$nsprv" = "$NSPR_VERSION" ] || die "NSPR in the build is '$nsprv', config.env says $NSPR_VERSION"
libc=$(readlink -f "$SYSROOT/lib/libc.so.6" 2>/dev/null || true)
glibc=unknown
if [ -n "$libc" ] && [ -e "$libc" ]; then
	glibc=$(grep -aoE 'release version [0-9.]+' "$libc" | sed -n '1p' | awk '{print $3}' || true)
	glibc=${glibc:-unknown}
fi
sqlv=$(sed -n 's/^#define SQLITE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SYSROOT/usr/include/sqlite3.h" | sed -n '1p')
zlibv=$(sed -n 's/^#define ZLIB_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$SYSROOT/usr/include/zlib.h" | sed -n '1p')
REPORT=$NO/nss-report.txt
sdksha=$(sha256sum "$SDK_TAR" | cut -c1-64)
{
	echo "nss-report: NSS and NSPR for webOS 4 (container/build-nss-sdk.sh)"
	echo "id: $ID"
	echo "SDK: $SDK_URL"
	echo "SDK release tag: $SDK_TAG, sha256 $sdksha (checked against config.env)"
	echo "toolchain: $($CC --version | sed -n 1p)"
	echo "sysroot glibc: $glibc (limit $GLIBC_LIMIT)"
	echo "sqlite (sysroot header): ${sqlv:-unknown}; zlib (sysroot header): ${zlibv:-unknown}"
	echo "NSS_VERSION: $nssv (config $NSS_VERSION), tarball sha256 $NSS_TARBALL_SHA256"
	echo "NSPR_VERSION: $nsprv (config $NSPR_VERSION), tarball sha256 $NSPR_TARBALL_SHA256"
	echo "flags: $TCFLAGS; link: $TLDFLAGS (-tno-lgtv-compat: no webOS compat code, no polyfills)"
	echo "patches (container/nss/patches): $(ls "$PATCHES" | tr '\n' ' ')"
	echo "newest GLIBC version in the set: ${maxall:-none} (limit $GLIBC_LIMIT)"
	echo "files:"
	cat "$CHECKS"
} >"$REPORT"
log "report: $REPORT"

# 10. pack, checksum, keep the output, upload to the Volume
tar -C "$NO" -cf - lib nss-report.txt | zstd -T0 -q -f -o "$OUT/nss.tar.zst"
(cd "$OUT" && sha256sum nss.tar.zst >nss.sha256)
log "artifact $OUT/nss.tar.zst ($(stat -c %s "$OUT/nss.tar.zst") bytes)"
cp "$REPORT" "$OUT/nss-report.txt"
# NSS_UPLOAD=0: keep the result in $OUT only (upload it yourself to <volume>/artifacts/<id>/nss.tar.zst)
if [ "${NSS_UPLOAD:-1}" = 1 ]; then
VOLNAME=${MODAL_VOLUME:-chromium-webos}
MODAL_PROFILE="${CN_PROFILE:-default}" modal volume put "$VOLNAME" "$OUT/nss.tar.zst" "artifacts/$ID/nss.tar.zst" --force >>"$LOG" 2>&1 ||
	die "upload to the Volume failed (see $LOG)"
MODAL_PROFILE="${CN_PROFILE:-default}" modal volume put "$VOLNAME" "$OUT/nss.sha256" "artifacts/$ID/nss.sha256" --force >>"$LOG" 2>&1 ||
	die "upload of nss.sha256 failed"
log "uploaded artifacts/$ID/ on the Volume $VOLNAME"
else
log "NSS_UPLOAD=0: not uploaded; artifact in $OUT"
fi

# 11. clean the big work tree (sources and build objects); keep $OUT
rm -rf "$WORK"
log "done: $ID"
echo "$ID"

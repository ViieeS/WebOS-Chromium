#!/bin/bash
# build-nss.sh: NSS and NSPR libraries for the webOS 4 TVs, built with openlgtv/buildroot-nc4.
# why: webOS 4.4 TVs have no libnss3, libnssutil3, libsmime3 or libnspr4 (TV Probe report from a webOS 4.4.3 TV),
# and the Beta browser needs them. The ten files in container/nss/ship-list.txt come from the buildroot toolchain of
# the TV SDK (soft-float ARM, glibc 2.12). The script checks each one: ELF32 soft-float ARM, GLIBC symbols at most
# 2.24, no libstdc++, and every DT_NEEDED name either shipped or on the webOS 4 list. Glibc polyfills stay off, and
# nothing from buildroot's glibc is shipped (no runtime shims).
# Output: nss.tar.zst (lib/ and nss-report.txt) and nss.sha256, copied to $ART/$BUILD_ID/. Runs inside the Modal container.
. /opt/cn/container/lib.sh
umask 022   # buildroot requires umask 022

GLIBC_LIMIT=2.24
BR=$W/buildroot
BROUT=$BR/output
NO=$W/nss-out
SHIP=$CN/container/nss/ship-list.txt
FRAG=$CN/container/nss/buildroot-nss.fragment
TVLIST=$CN/container/webos4/TV-SONAMES-webos4.txt
CHECKS=$W/nss-checks.txt

# ver_le A B: true when version A is less than or equal to version B.
ver_le() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }

# pick PATH_GLOB NAME: first file named NAME whose path matches PATH_GLOB, under the buildroot output.
# BR2_PER_PACKAGE_DIRECTORIES=y puts each package's target/ and staging/ under output/per-package/<pkg>/, so the
# paths are searched, not fixed. build/ directories are skipped (they hold sources and object files).
pick() { find "$BROUT" -name build -prune -o -path "$1" -name "$2" -print 2>/dev/null | sort | head -1; }

# 0. inputs
for v in BUILDROOT_REPO BUILDROOT_BRANCH BUILDROOT_COMMIT NSS_VERSION NSPR_VERSION; do
	[ -n "${!v:-}" ] || die "$v is not set in config.env"
done
[[ $BUILDROOT_COMMIT =~ ^[0-9a-f]{40}$ ]] || die "BUILDROOT_COMMIT is not a 40-character hash"
n_ship=$(grep -c . "$SHIP" || true)
[ "$n_ship" -eq 10 ] || die "ship-list.txt has $n_ship names, expected 10"

# 1. clone buildroot at the pinned commit (the branch head must be the pin)
st status=running stage=clone
log "clone $BUILDROOT_REPO branch $BUILDROOT_BRANCH"
rm -rf "$BR"
git clone -q --depth 1 --single-branch --branch "$BUILDROOT_BRANCH" "$BUILDROOT_REPO" "$BR" >>"$LOG" 2>&1 ||
	die "git clone of buildroot failed"
head=$(git -C "$BR" rev-parse HEAD)
[ "$head" = "$BUILDROOT_COMMIT" ] ||
	die "buildroot HEAD $head differs from BUILDROOT_COMMIT $BUILDROOT_COMMIT: the branch moved; check it, then update config.env"
log "buildroot at $head"
push_log

# 2. configuration: webos_tv_defconfig + fragment; polyfills must stay off
st status=running stage=config
mkdir -p /vol/cache/buildroot-ccache /vol/cache/buildroot-dl   # download and ccache survive a preemption (Volume)
make -C "$BR" webos_tv_defconfig >>"$LOG" 2>&1 || die "make webos_tv_defconfig failed"
cat "$FRAG" >>"$BROUT/.config"
make -C "$BR" olddefconfig >>"$LOG" 2>&1 || die "make olddefconfig failed"
grep -qx 'BR2_PACKAGE_GLIBC_POLYFILLS=y' "$BROUT/.config" && die "BR2_PACKAGE_GLIBC_POLYFILLS is on: glibc polyfills are not allowed"
# LGTV selects glibc-polyfills, so it must be off too (run 20261104T095128Z-build_nss built polyfills through it)
grep -qx 'BR2_PACKAGE_LGTV=y' "$BROUT/.config" && die "BR2_PACKAGE_LGTV is on: it selects glibc-polyfills"
log "fragment sha256 $(sha256sum "$FRAG" | cut -c1-16); config: $(grep -E 'BR2_PACKAGE_(LGTV|GLIBC_POLYFILLS)|BR2_PER_PACKAGE|BR2_JLEVEL' "$BROUT/.config" | tr '\n' ' ')"
grep -qx 'BR2_PACKAGE_LIBNSS=y' "$BROUT/.config" || die "BR2_PACKAGE_LIBNSS is not enabled after olddefconfig"
grep -qx 'BR2_PACKAGE_LIBNSPR=y' "$BROUT/.config" || die "BR2_PACKAGE_LIBNSPR is not enabled after olddefconfig"
push_log

# 3. build the toolchain, glibc, NSPR and NSS (output in $BROUT)
# The Modal container runs as root; buildroot's packages refuse to configure as root unless this is set.
# The container is disposable, so the override is safe here.
export FORCE_UNSAFE_CONFIGURE=1
st status=running stage=build
t0=$SECONDS
# Only the two packages we ship: buildroot builds their dependencies (toolchain, glibc, sqlite, zlib) and nothing else.
make -C "$BR" -j"$(nproc)" libnss libnspr >>"$LOG" 2>&1 || die "buildroot make failed (see the run log)"
size=$(du -sh "$BROUT" | cut -f1)
log "buildroot finished in $((SECONDS - t0)) s; output tree $size"
st build_seconds=$((SECONDS - t0)) output_size="$size"
push_log

# 4. copy the shipped set: only the names in ship-list.txt, dereferenced with cp -L
st status=running stage=copy
rm -rf "$NO"; mkdir -p "$NO/lib"
while IFS= read -r n; do
	[ -n "$n" ] || continue
	src=$(pick "*/target/usr/lib/*" "$n")
	[ -n "$src" ] || die "$n is not in buildroot's target usr/lib"
	cp -L "$src" "$NO/lib/$n"
done <"$SHIP"

# 5. checks (fatal on failure)
st status=running stage=check
push_log
nsshdr=$(pick "*/staging/usr/include/*" nss.h)
prhdr=$(pick "*/staging/usr/include/*" prversion.h)
[ -n "$nsshdr" ] || die "nss.h not found in buildroot staging"
[ -n "$prhdr" ] || die "prversion.h not found in buildroot staging"
nssv=$(sed -n 's/^#define NSS_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' "$nsshdr" | sed -n '1p')
nsprv=$(sed -n 's/^#define PR_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' "$prhdr" | sed -n '1p')
[ "$nssv" = "$NSS_VERSION" ] || die "NSS in the build is '$nssv', config.env says $NSS_VERSION"
[ "$nsprv" = "$NSPR_VERSION" ] || die "NSPR in the build is '$nsprv', config.env says $NSPR_VERSION"

: >"$CHECKS"
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
	grep -q 'hard-float' <<<"$hdr" && die "$n: hard-float ABI flag set"
	grep -Eq 'Tag_ABI_VFP_args: (AAPCS )?VFP' <<<"$attr" && die "$n: hard-float VFP argument tag"
	grep -q 'INTERP' <<<"$phdr" && die "$n: has PT_INTERP (a shared library must not)"

	gl=$(printf '%s\n%s\n' "$dyn" "$ver" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV || true)
	maxg=${gl##*$'\n'}   # gl is sorted, so the last line is the newest version (empty when there is none)
	if [ -n "$maxg" ]; then
		ver_le "${maxg#GLIBC_}" "$GLIBC_LIMIT" || die "$n: needs $maxg, the TV has glibc $GLIBC_LIMIT"
	fi
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

# 6. report: versions, toolchain and the checks above
st status=running stage=report
crossgcc_bin=$(pick "*/host/bin/*" "*-gcc")
crossgcc=none
if [ -x "$crossgcc_bin" ]; then crossgcc=$("$crossgcc_bin" --version | sed -n '1p'); fi
hostgcc_bin=$(pick "*/host/bin/*" gcc)
hostgcc=none
if [ -x "$hostgcc_bin" ]; then hostgcc=$("$hostgcc_bin" --version | sed -n '1p'); fi
libc_so=$(pick "*/target/lib/*" "libc.so.*")
glibc=unknown
if [ -n "$libc_so" ] && [ -e "$libc_so" ]; then
	banner=$(grep -aoE 'release version [0-9.]+' "$(readlink -f "$libc_so")" || true)
	first=${banner%%$'\n'*}
	glibc=${first##* }
	[ -n "$glibc" ] || glibc=unknown
fi
# buildroot builds NSS against its own sqlite and zlib (NSS_USE_SYSTEM_SQLITE=1). Record their versions so they can
# be compared with the TV's libsqlite3.so.0 and libz.so.1 (not checked by this script).
sqlh=$(pick "*/staging/usr/include/*" sqlite3.h)
zlibh=$(pick "*/staging/usr/include/*" zlib.h)
sqlv=unknown
zlibv=unknown
[ -n "$sqlh" ] && sqlv=$(sed -n 's/^#define SQLITE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$sqlh" | sed -n '1p')
[ -n "$zlibh" ] && zlibv=$(sed -n 's/^#define ZLIB_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$zlibh" | sed -n '1p')
sqlv=${sqlv:-unknown}
zlibv=${zlibv:-unknown}
REPORT=$NO/nss-report.txt
{
	echo "nss-report: NSS and NSPR for webOS 4 (container/build-nss.sh)"
	echo "build id: $BUILD_ID"
	echo "buildroot: $BUILDROOT_REPO branch $BUILDROOT_BRANCH commit $head"
	echo "config: webos_tv_defconfig + container/nss/buildroot-nss.fragment (glibc polyfills off)"
	echo "host gcc: $hostgcc"
	echo "cross gcc (target toolchain): $crossgcc"
	echo "target glibc: $glibc"
	echo "NSS_VERSION: $nssv (config $NSS_VERSION)"
	echo "NSPR_VERSION: $nsprv (config $NSPR_VERSION)"
	echo "buildroot sqlite version (NSS_USE_SYSTEM_SQLITE): $sqlv"
	echo "buildroot zlib version: $zlibv"
	echo "newest GLIBC version in the set: ${maxall:-none} (limit $GLIBC_LIMIT)"
	echo "files:"
	cat "$CHECKS"
} >"$REPORT"
log "report written: $REPORT"

# 7. pack and save the artifact
st status=running stage=pack
tar -C "$NO" -cf - lib nss-report.txt | zstd -T0 -q -f -o "$W/nss.tar.zst"
(cd "$W" && sha256sum nss.tar.zst >nss.sha256)
dest=$ART/$BUILD_ID
mkdir -p "$dest"
cp -f "$W/nss.tar.zst" "$W/nss.sha256" "$dest/"
log "artifact $dest/nss.tar.zst ($(stat -c %s "$W/nss.tar.zst") bytes)"
st status=done nss_artifact="$dest/nss.tar.zst" buildroot_commit="$head" nss_version="$nssv" nspr_version="$nsprv"
push_log

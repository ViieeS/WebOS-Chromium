#!/bin/bash
# build-sysroot.sh: the TV-faithful target sysroot /work/sysroot-tv, saved as snapshot "sysroot".
#   1. unpack the TV rootfs + its bsp /usr layer (exact TV libraries)
#   2. Ubuntu 22.04 armhf -dev packages (glibc 2.35 headers/crt, other headers, .pc files)
#   3. softfp fix-ups (gnu/stubs-soft.h), relative symlinks
#   4. replace every shared library that the TV also has with the TV's own file
#   5. TV-only libraries + webOS headers (luna-service2, pbnjson, wayland-webos) from the openlgtv SDK, with .pc files
#   6. lists for later checks: TV-SONAMES.txt, TV-REPLACED.txt, NOT-ON-TV.txt
#   7. softfp guard: no static object/archive with the hard-float tag may remain (found 2026-10-27)
#
# SYSROOT_SOURCE=public (config.env; for builders without a TV): no TV files at all. Step 1 is skipped, step 2 takes
# the exact package versions of container/sysroot-pinned.txt from Launchpad (sha256-checked), and every library that
# step 4/5 would take from the TV is a code-free stub built from container/tvabi/*.abi (same SONAME, symbols,
# versions and references; see container/tvabi/README.md). glibc and the loader come from the jammy armel cross
# packages (the same glibc 2.35, soft-float). The browser links the same way; at run time it loads the TV's libraries.
. /opt/cn/container/lib.sh
T=$W/tvroot
S=$SYSROOT
ML=arm-linux-gnueabihf
SDK_URL=https://github.com/openlgtv/buildroot-nc4/releases/download/2026.08-webos/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz
SOURCE=${SYSROOT_SOURCE:-tv}
case "$SOURCE" in tv|public) ;; *) die "SYSROOT_SOURCE must be tv or public, not '$SOURCE'" ;; esac
log "sysroot source: $SOURCE"
rm -rf "$T" "$S"; mkdir -p "$S"   # not $T: unsquashfs 4.5 refuses an existing -d directory
LP=https://launchpad.net/ubuntu/+archive/primary/+files
# pinned_get SECTION DIR: download the .deb files of one sysroot-pinned.txt section into DIR, checking each sha256
pinned_get() {
	mkdir -p "$2"
	awk -v s="[$1]" '/^\[/{on=($0==s); next} on && NF==2 && $1 !~ /^#/' "$CN/container/sysroot-pinned.txt" > "$2/.list"
	[ -s "$2/.list" ] || die "no packages in section [$1] of sysroot-pinned.txt"
	while read -r f h; do echo "$f $h"; done < "$2/.list" | xargs -P 8 -L 1 sh -c '
		curl -fsSL --retry 4 -o "$0/$1" "'"$LP"'/$1" || { echo "download failed: $1" >&2; exit 255; }
		echo "$2  $0/$1" | sha256sum -c --quiet - || { echo "sha256 mismatch: $1" >&2; exit 255; }' "$2" ||
		die "pinned package download failed (section $1)"
	log "downloaded $(wc -l < "$2/.list") pinned packages ([$1])"
}

if [ "$SOURCE" = public ]; then
log "public sysroot: no TV files; stubs from container/tvabi"
command -v arm-linux-gnueabi-as > /dev/null || { apt-get update -qq && apt-get install -y -qq --no-install-recommends binutils-arm-linux-gnueabi > /dev/null; } ||
	die "cannot install binutils-arm-linux-gnueabi (needed for the stub libraries)"
python3 "$CN/container/tvabi.py" mkstubs "$CN/container/tvabi" "$W/stubs" | tee -a "$LOG" | tail -1
[ "${PIPESTATUS[0]}" -eq 0 ] || die "stub libraries do not match container/tvabi (see the log)"
else
# 1. TV rootfs (LG proprietary: stays on the Volume and in this container only)
BUILD_ID=$BUILD_ID-tvcheck bash "$CN/container/tvdata-check.sh" || die "TV data check failed"
log "unpack TV rootfs"
# -ig: skip what a container cannot create (device nodes); messages go to the log
unsquashfs -q -n -no-xattrs -ig -d "$T" "$TVD/rootfs.sqfs" >> "$LOG" 2>&1 || log "unsquashfs exit $? (see messages above)"
[ -f "$T/lib/libc.so.6" ] || [ -f "$T/usr/lib/libc.so.6" ] || die "TV rootfs did not unpack (no libc.so.6)"
mkdir -p "$W/apps"
tar -C "$W/apps" -xf "$TVD/apps.tar" overlay/bsppart/bsp/usr
cp -a "$W/apps/overlay/bsppart/bsp/usr/." "$T/usr/"          # bsp layer wins, like on the TV
rm -rf "$W/apps"
strings "$T/lib/libc.so.6" 2>/dev/null | grep -m1 "GNU C Library" | tee -a "$LOG" || true
fi

# 2. Ubuntu jammy armhf packages
if [ "$SOURCE" = public ]; then
log "pinned jammy armhf packages (sysroot-pinned.txt)"
D=$W/debs; rm -rf "$D"
pinned_get armhf "$D"
n=0
for deb in "$D"/*_armhf.deb "$D"/*_all.deb; do
	[ -f "$deb" ] || continue
	dpkg -x "$deb" "$S"; n=$((n + 1))
done
log "unpacked $n packages"
ls "$D" | grep '\.deb$' | sed 's/_armhf.deb$//; s/_all.deb$//' > "$S/PACKAGES.txt"   # Launchpad names: no epochs
rm -rf "$D"
else
log "download jammy armhf packages"
sed -i 's/^deb http/deb [arch=amd64] http/' /etc/apt/sources.list
cat > /etc/apt/sources.list.d/armhf.list <<'EOF'
deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy main universe
deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy-updates main universe
EOF
dpkg --add-architecture armhf
apt-get update -qq
PKGS=$(grep -v '^\s*#' "$CN/container/sysroot-packages.txt" | awk 'NF{print $1":armhf"}')
D=$W/debs; rm -rf "$D"; mkdir -p "$D/partial"
apt-get -y -qq --no-install-recommends --download-only -o Dir::Cache::archives="$D" install $PKGS
n=0
for deb in "$D"/*_armhf.deb "$D"/*_all.deb; do
	[ -f "$deb" ] || continue
	dpkg -x "$deb" "$S"; n=$((n + 1))
done
log "unpacked $n packages"
ls "$D" | sed 's/_armhf.deb$//; s/_all.deb$//' > "$S/PACKAGES.txt"
rm -rf "$D"
fi

# 2b. Ubuntu's armhf crt*.o, libc_nonshared.a and libgcc.a are HARD-float: linking them marks every binary
# hard-float (abi-smoke run 20261027T032136Z). Replace them with the same glibc 2.35 / gcc 12 objects from the
# soft-float "armel" cross packages (no Tag_ABI_VFP_args = base AAPCS, which is what softfp code calls).
log "soft-float crt/libc_nonshared/libgcc from jammy armel cross packages"
AE=$W/armel; rm -rf "$AE"; mkdir -p "$AE/debs"
if [ "$SOURCE" = public ]; then
	pinned_get armel-cross "$AE/debs"
else
	(cd "$AE/debs" && apt-get download -qq libc6-dev-armel-cross libc6-armel-cross libgcc-12-dev-armel-cross)
fi
for deb in "$AE"/debs/*.deb; do dpkg -x "$deb" "$AE/x"; done
GCCD=$(ls -d "$S"/usr/lib/gcc/$ML/* | head -1)
n=0
for f in "$AE"/x/usr/arm-linux-gnueabi/lib/*.o "$AE"/x/usr/arm-linux-gnueabi/lib/*.a; do
	cp -f "$f" "$S/usr/lib/$ML/"; n=$((n + 1))
done
for f in "$AE"/x/usr/lib/gcc-cross/arm-linux-gnueabi/12/*.o "$AE"/x/usr/lib/gcc-cross/arm-linux-gnueabi/12/*.a; do
	cp -f "$f" "$GCCD/"; n=$((n + 1))
done
log "copied $n soft-float objects/archives (glibc lib dir + $GCCD)"
[ "$SOURCE" = public ] || rm -rf "$AE"   # public: steps 4 and 4b take glibc and the loader from here

# 3. softfp: with -mfloat-abi=softfp glibc's gnu/stubs.h includes stubs-soft.h; armhf only ships stubs-hard.h
INC=$S/usr/include/$ML/gnu
[ -f "$INC/stubs-soft.h" ] || cp "$INC/stubs-hard.h" "$INC/stubs-soft.h"
# absolute symlinks -> relative (inside the sysroot)
python3 - "$S" <<'EOF'
import os, sys
root = sys.argv[1]
for d, dirs, files in os.walk(root):
    for n in dirs + files:
        p = os.path.join(d, n)
        if os.path.islink(p):
            t = os.readlink(p)
            if t.startswith("/"):
                os.unlink(p)
                os.symlink(os.path.relpath(root + t, d), p)
EOF

# 4. replace libraries with the TV's own files, glibc included (same 2.35, but the TV's are softfp)
if [ "$SOURCE" = public ]; then
# public: the same libraries are replaced (those whose SONAME the TV has, by TV-SONAMES.txt), with a stub from
# container/tvabi, or for glibc with the jammy armel cross copy. Same lists as the TV mode.
log "replace libraries with stubs (container/tvabi) and armel glibc"
cp "$CN/container/tvabi/TV-SONAMES.txt" "$S/TV-SONAMES.txt"
python3 - "$S" "$W/stubs" "$AE/x/usr/arm-linux-gnueabi/lib" "$ML" "$CN/container/tvabi.py" <<'EOF' | tee -a "$LOG"
import hashlib, importlib.util, os, shutil, sys
S, STUBS, GLIBC_DIR, ML, TVABI = sys.argv[1:6]
spec = importlib.util.spec_from_file_location("tvabi", TVABI); tvabi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tvabi)
tv = set(open(os.path.join(S, "TV-SONAMES.txt")).read().split())
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()
replaced, missing, used = [], [], set()
for base in (os.path.join(S, "lib", ML), os.path.join(S, "usr/lib", ML)):
    if not os.path.isdir(base):
        continue
    for n in sorted(os.listdir(base)):
        p = os.path.join(base, n)
        if os.path.islink(p) or not os.path.isfile(p) or ".so" not in n:
            continue
        try:
            so = tvabi.read_abi(p)["soname"]
        except SystemExit:
            continue                             # not an ELF shared library (linker script etc.)
        if not so:
            continue
        if so not in tv:
            missing.append(so)
            continue
        src = os.path.join(STUBS, so)
        if not os.path.isfile(src):
            src = os.path.join(GLIBC_DIR, so)
            if not os.path.isfile(src):
                raise SystemExit(f"{so} is on the TV but has no stub (container/tvabi) and is not armel glibc")
        else:
            used.add(so)
        shutil.copyfile(src, p)
        replaced.append(f"{so} {sha(p)}")
open(os.path.join(S, "TV-REPLACED.txt"), "w").write("\n".join(replaced) + "\n")
open(os.path.join(S, "NOT-ON-TV.txt"), "w").write("\n".join(missing) + "\n")
open(os.path.join(S, ".stubs-used"), "w").write("\n".join(sorted(used)) + "\n")
print(f"replaced {len(replaced)} libraries ({len(used)} stubs, {len(replaced) - len(used)} armel glibc); "
      f"{len(missing)} not on the TV (must not be linked): " + " ".join(missing[:40]))
EOF
[ "${PIPESTATUS[0]}" -eq 0 ] || die "library replacement failed"
else
log "replace libraries with the TV's"
python3 - "$S" "$T" "$ML" <<'EOF' | tee -a "$LOG"
import hashlib, os, re, shutil, subprocess, sys
S, T, ML = sys.argv[1:4]
GLIBC = re.compile(r"^(ld-linux.*|libc|libm|libmvec|libpthread|libdl|librt|libresolv|libutil|libanl|libBrokenLocale|"
                   r"libnsl|libnss_[a-z0-9_]+|libthread_db|libcrypt|libmemusage|libpcprofile|libSegFault|libc_malloc_debug)\.so")
def tv_find(name):
    for d in ("usr/lib", "lib", "usr/lib/" + ML, "lib/" + ML):
        p = os.path.join(T, d, name)
        for _ in range(10):                      # resolve symlinks inside the TV tree
            if not os.path.islink(p):
                break
            t = os.readlink(p)
            p = os.path.join(T, t.lstrip("/")) if t.startswith("/") else os.path.join(os.path.dirname(p), t)
        if os.path.isfile(p):
            return p
    return None
def soname(p):
    out = subprocess.run(["readelf", "-d", p], capture_output=True, text=True).stdout
    m = re.search(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", out)
    return m.group(1) if m else None
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()
replaced, missing = [], []
for base in (os.path.join(S, "lib", ML), os.path.join(S, "usr/lib", ML)):
    if not os.path.isdir(base):
        continue
    for n in sorted(os.listdir(base)):
        p = os.path.join(base, n)
        if os.path.islink(p) or not os.path.isfile(p) or ".so" not in n:
            continue
        so = soname(p)
        if not so:
            continue
        tv = tv_find(so)
        if tv:
            shutil.copyfile(tv, p)
            replaced.append(f"{so} {sha(p)}")
        else:
            missing.append(so)
open(os.path.join(S, "TV-REPLACED.txt"), "w").write("\n".join(replaced) + "\n")
open(os.path.join(S, "NOT-ON-TV.txt"), "w").write("\n".join(missing) + "\n")
names = set()
for d in ("lib", "usr/lib"):
    for dp, _, fs in os.walk(os.path.join(T, d)):
        names.update(f for f in fs if re.search(r"\.so(\.|$)", f))
open(os.path.join(S, "TV-SONAMES.txt"), "w").write("\n".join(sorted(names)) + "\n")
print(f"replaced {len(replaced)} libraries with the TV's; {len(missing)} not on the TV (must not be linked): "
      + " ".join(missing[:40]))
EOF
fi

# 4b. the TV loader is /lib/ld-linux.so.3 (softfp); Ubuntu's libc.so linker script names ld-linux-armhf.so.3,
# which would end up in DT_NEEDED. Point the script at the TV loader and remove the armhf one.
# (public: the jammy armel cross loader, the same glibc 2.35 soft-float loader)
if [ "$SOURCE" = public ]; then
	cp -L "$AE/x/usr/arm-linux-gnueabi/lib/ld-linux.so.3" "$S/lib/ld-linux.so.3"
else
	cp -L "$T/lib/ld-linux.so.3" "$S/lib/ld-linux.so.3"
fi
rm -f "$S/lib/ld-linux-armhf.so.3" "$S/lib/$ML/ld-linux-armhf.so.3" "$S/usr/lib/$ML/ld-linux-armhf.so.3"
for f in "$S/usr/lib/$ML/libc.so" "$S/usr/lib/$ML/libm.so"; do
	[ -f "$f" ] && grep -q GROUP "$f" && sed -i 's|/lib/ld-linux-armhf\.so\.3|/lib/ld-linux.so.3|g' "$f"
done
grep -h GROUP "$S/usr/lib/$ML/libc.so" | tee -a "$LOG"
echo "ld-linux.so.3 $(sha256sum "$S/lib/ld-linux.so.3" | cut -d' ' -f1)" >> "$S/TV-REPLACED.txt"

# 5. TV-only libraries + webOS headers
log "openlgtv SDK (webOS headers only)"
if [ "$SOURCE" = public ]; then          # the same release tarball, checked against config.env
	[ "$SDK_URL" = "${SDK_URL_X86_64:-}" ] && [ -n "${SDK_SHA256_X86_64:-}" ] || die "config.env SDK_URL_X86_64/SDK_SHA256_X86_64 do not match $SDK_URL"
	curl -fsSL --retry 4 -o "$W/sdk.tar.gz" "$SDK_URL" || die "SDK download failed"
	echo "$SDK_SHA256_X86_64  $W/sdk.tar.gz" | sha256sum -c --quiet - || die "SDK sha256 mismatch"
	# only the headers used below (the whole SDK unpacks to >1 GB)
	tar -xzf "$W/sdk.tar.gz" -C "$W" --wildcards '*/sysroot/usr/include/luna-service2*' '*/sysroot/usr/include/pbnjson*' \
		'*/sysroot/usr/include/wayland-webos-*' || die "SDK unpack failed"
	rm -f "$W/sdk.tar.gz"
else
curl -fsSL "$SDK_URL" | tar -xz -C "$W"
fi
SDKS=$(dirname "$(find "$W" -maxdepth 6 -type d -path '*sysroot/usr/include/luna-service2' | head -1)")
[ -d "$SDKS/luna-service2" ] || die "luna-service2 headers not found in the SDK"
for h in luna-service2 pbnjson.h pbnjson pbnjson_c pbnjson_cxx; do
	[ -e "$SDKS/$h" ] && cp -a "$SDKS/$h" "$S/usr/include/"
done
# wayland-webos headers: generated from the TV's own protocol definitions (not the SDK's, which differ from the TV)
python3 "$CN/container/gen-webos-headers.py" "$SDKS" "$S/usr/include" | tee -a "$LOG"
ls "$S/usr/include" | grep -E '^(luna|pbnjson|wayland-webos)' | tee -a "$LOG"
LIBD=$S/usr/lib/$ML
for so in libwayland-webos-client.so.1 libluna-service2.so.3 libpbnjson_c.so.2 libpbnjson_cpp.so.2; do
	if [ "$SOURCE" = public ]; then
		f=$W/stubs/$so
		[ -f "$f" ] || die "no stub for $so (container/tvabi/$so.abi)"
		echo "$so" >> "$S/.stubs-used"
	else
		f=$(find "$T/usr/lib" "$T/lib" -maxdepth 1 -name "$so" | head -1)
	fi
	[ -n "$f" ] || { log "TV has no $so (skipped)"; continue; }
	cp -L "$f" "$LIBD/$so"
	ln -sf "$so" "$LIBD/${so%.so.*}.so"
	echo "$so $(sha256sum "$LIBD/$so" | cut -d' ' -f1)" >> "$S/TV-REPLACED.txt"
done
PC=$LIBD/pkgconfig; mkdir -p "$PC"
pc() { printf 'prefix=/usr\nlibdir=${prefix}/lib/%s\nincludedir=${prefix}/include\n\nName: %s\nDescription: %s (TV library)\nVersion: %s\nRequires: %s\nLibs: -L${libdir} %s\nCflags: -I${includedir}\n' \
	"$ML" "$1" "$1" "$2" "$3" "$4" > "$PC/$1.pc"; }
pc wayland-webos-client 1.0.0 "wayland-client" "-lwayland-webos-client"
pc luna-service2 3.21.2 "glib-2.0" "-lluna-service2"
pc pbnjson_c 2.0 "" "-lpbnjson_c"
rm -rf "$W"/arm-webos-linux-gnueabi_sdk-buildroot* "$T"

# 7. softfp guard: crt/libc/libgcc objects must be soft-float; other hard-float static archives (unused -dev
# leftovers) are removed so nothing can link them by accident.
log "softfp guard"
if [ "$SOURCE" = public ]; then
# public: the same files as the Beta sysroot (container/sysroot-hardfloat-removed.txt). The TV-mode loop below is not
# deterministic: under pipefail, `readelf | grep -q` fails when grep stops early (SIGPIPE on big archives), so some
# hard-float archives are kept at random. Kept archives are never linked (the .so next to them wins), but the list
# keeps the public sysroot identical. Core objects are checked without a pipe.
bad=0
while IFS= read -r f; do
	a=$(readelf -A "$f" 2>/dev/null || true)
	case "$a" in *"Tag_ABI_VFP_args: VFP registers"*) log "HARD-FLOAT core object: $f"; bad=1 ;; esac
done < <(find "$S" -type f \( -name 'crt*.o' -o -name libc_nonshared.a -o -name libgcc.a -o -name libgcc_eh.a -o -name libc.a -o -name libm.a \))
[ $bad -eq 0 ] || die "hard-float core objects remain in the sysroot"
cp "$CN/container/sysroot-hardfloat-removed.txt" "$S/HARDFLOAT-REMOVED.txt"
while IFS= read -r f; do
	[ -n "$f" ] || continue
	[ -f "$S$f" ] || die "listed hard-float archive not in the sysroot: $f"
	rm -f "$S$f"
done < "$S/HARDFLOAT-REMOVED.txt"
else
: > "$S/HARDFLOAT-REMOVED.txt"
bad=0
while IFS= read -r f; do
	readelf -A "$f" 2>/dev/null | grep -q 'Tag_ABI_VFP_args: VFP registers' || continue
	case "$(basename "$f")" in
	crt*.o|libc_nonshared.a|libgcc.a|libgcc_eh.a|libc.a|libm.a) log "HARD-FLOAT core object: $f"; bad=1 ;;
	*) echo "${f#$S}" >> "$S/HARDFLOAT-REMOVED.txt"; rm -f "$f" ;;
	esac
done < <(find "$S" -type f \( -name '*.o' -o -name '*.a' \))
[ $bad -eq 0 ] || die "hard-float core objects remain in the sysroot"
fi
log "removed $(grep -c . "$S/HARDFLOAT-REMOVED.txt" || true) hard-float static archives (HARDFLOAT-REMOVED.txt)"
grep -q "^libc.so.6 " "$S/TV-REPLACED.txt" || die "libc.so.6 was not replaced with the TV's"
if [ "$SOURCE" = public ]; then          # every stub must have been used exactly where the TV mode used a TV file
	nabi=$(ls "$CN"/container/tvabi/*.abi | wc -l); nused=$(sort -u "$S/.stubs-used" | grep -c .)
	[ "$nabi" -eq "$nused" ] || die "$nused of $nabi stubs used: $(ls "$CN"/container/tvabi/*.abi | xargs -n1 basename | sed 's/\.abi$//' | sort | comm -23 - <(sort -u "$S/.stubs-used") | tr '\n' ' ')"
	rm -f "$S/.stubs-used"; rm -rf "$AE" "$W/stubs"
	log "public sysroot: $nused stub libraries, no TV files"
fi

$META set "$SNAP/sysroot.json" id="$BUILD_ID" packages="$(wc -l < "$S/PACKAGES.txt")" \
	replaced="$(wc -l < "$S/TV-REPLACED.txt")" not_on_tv="$(grep -c . "$S/NOT-ON-TV.txt" || true)" source="$SOURCE"
snap_save sysroot "$W" sysroot-tv
st status=done sysroot_id="$BUILD_ID"
push_log

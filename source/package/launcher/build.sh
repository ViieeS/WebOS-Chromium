#!/bin/sh
# Builds cnrun (the app's bin/run) for the TV. Output: $OUT (default package/launcher/out/cnrun).
# Needs: zig 0.13 ($ZIG, default ~/zig/zig), libwayland-dev (wayland-scanner, headers), binutils-arm-linux-gnueabi
# (strip, readelf). The TV's libwayland-client.so.0 is used at run time; a stub .so with the same SONAME is linked
# here, so no LG/TV file is needed.
# PROTO = dir with webos-foreign.xml (Apache-2.0, LG webOS OSE); the default is set below.
#
# Built against glibc 2.17 with zig's per-version glibc stubs, so the launcher starts on every webOS TV and can tell
# users of TVs that are too old for the browser (cnrun.c tv_too_old) instead of the app closing silently. A normal
# cross gcc links against its own, much newer glibc.
set -e
cd "$(dirname "$0")"
P=${PROTO:-../../tvproto}
OUT=${OUT:-$PWD/out/cnrun}
ZIG=${ZIG:-$HOME/zig/zig}
W=$(mktemp -d); trap 'rm -rf $W' EXIT
# Protocol code from wayland-scanner 1.18 (Ubuntu 20.04). Its request stubs call
# wl_proxy_marshal* + wl_proxy_destroy, which every libwayland since 1.10 has; scanner 1.20+ writes calls to
# wl_proxy_marshal_flags, which webOS 8 and older lack. The core wayland-client-protocol.h is generated the same way.
SC=${WAYLAND_SCANNER:-$W/scanner/usr/bin/wayland-scanner}
if [ ! -x "$SC" ]; then
	mkdir -p $W/scanner
	curl -fsSL -o $W/wlbin.deb "http://ports.ubuntu.com/ubuntu-ports/pool/main/w/wayland/libwayland-bin_1.18.0-1ubuntu0.1_$(dpkg --print-architecture).deb" ||
		curl -fsSL -o $W/wlbin.deb "http://archive.ubuntu.com/ubuntu/pool/main/w/wayland/libwayland-bin_1.18.0-1ubuntu0.1_$(dpkg --print-architecture).deb"
	dpkg-deb -x $W/wlbin.deb $W/scanner
fi
"$SC" --version
mkdir -p $W/inc
"$SC" client-header $P/webos-foreign.xml $W/webos-foreign-client-protocol.h
"$SC" private-code $P/webos-foreign.xml $W/webos-foreign-protocol.c
"$SC" client-header /usr/share/wayland/wayland.xml $W/inc/wayland-client-protocol.h
"$SC" private-code /usr/share/wayland/wayland.xml $W/wayland-protocol.c
# only the wayland headers from the host (not its glibc headers), with the generated core protocol header above
cp /usr/include/wayland-client.h /usr/include/wayland-client-core.h /usr/include/wayland-util.h \
	/usr/include/wayland-version.h $W/inc/
CC="$ZIG cc -target arm-linux-gnueabi.2.17 -mcpu=cortex_a7 -mfpu=neon -mfloat-abi=softfp -U_FORTIFY_SOURCE"
cat > $W/stub.c <<'EOF'
void wl_display_connect(void) {} void wl_display_roundtrip(void) {} void wl_display_dispatch(void) {}
void wl_display_flush(void) {} void wl_display_get_fd(void) {} void wl_display_disconnect(void) {}
void wl_proxy_marshal(void) {} void wl_proxy_marshal_constructor(void) {} void wl_proxy_marshal_constructor_versioned(void) {}
void wl_proxy_add_listener(void) {} void wl_proxy_get_version(void) {} void wl_proxy_destroy(void) {}
void wl_display_get_error(void) {}
EOF
$CC -shared -fPIC -Wl,-soname,libwayland-client.so.0 -o $W/libwayland-client.so $W/stub.c
mkdir -p "$(dirname "$OUT")"
$CC -O2 -Wall -fPIE -pie -I$W -I$W/inc -o "$OUT" cnrun.c $W/webos-foreign-protocol.c $W/wayland-protocol.c \
	-L$W -lwayland-client -ldl
arm-linux-gnueabi-strip "$OUT"
v=$(arm-linux-gnueabi-readelf -V "$OUT" | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1)
interp=$(arm-linux-gnueabi-readelf -l "$OUT" | grep -o "/lib/ld-linux[^]]*")
echo "$OUT: $(stat -c %s "$OUT") bytes, newest glibc symbol $v, loader $interp"
case $v in GLIBC_2.1[89]*|GLIBC_2.[2-9][0-9]*) echo "ERROR: must not need glibc newer than 2.17"; exit 1;; esac
[ "$interp" = "/lib/ld-linux.so.3" ] || { echo "ERROR: loader must be /lib/ld-linux.so.3"; exit 1; }
arm-linux-gnueabi-nm -D "$OUT" 2>/dev/null | grep -q wl_proxy_marshal_flags && { echo "ERROR: needs wl_proxy_marshal_flags (libwayland 1.20+)"; exit 1; }
true

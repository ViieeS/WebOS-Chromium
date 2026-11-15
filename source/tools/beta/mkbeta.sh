#!/bin/sh
# mkbeta.sh BROWSER_TAR VERSION LAUNCHER OUTDIR [REQUIRES] [LIBDIR]: Chromium Beta IPK (app id org.nativechromium.browser.beta,
# badge icons) from this checkout's package/. LIBDIR holds the NSS and NSPR libraries (container/nss/ship-list.txt); it is
# shipped as <app>/lib. Installs next to the normal Chromium. Runs on the build host.
set -e
H=$(cd "$(dirname "$0")/../.." && pwd); B=$(mktemp -d); trap 'rm -rf $B' EXIT
LIBDIR=
if [ -n "$6" ]; then LIBDIR=$(cd "$6" && pwd); fi
cp -r "$H/package" "$B/package"; rm -rf "$B/package/start" "$B/package/__pycache__"
mkdir -p "$B/container/nss"; cp "$H/container/nss/ship-list.txt" "$B/container/nss/"
# BETA_APP_ID / BETA_TITLE / BETA_BADGE: a separate app for one tester group. Used for "Chromium Beta WebOS 4"
# (org.nativechromium.browser.webos4, 0.0.53-0.0.58); merged into Chromium Beta on 2026-11-05 (one build, webOS 4.4+),
# so normal Beta releases need none of them: mkbeta.sh BROWSER_TAR VERSION LAUNCHER OUT "glibc=2.24,webos=webOS 4.4" LIBDIR
AID=${BETA_APP_ID:-org.nativechromium.browser.beta}; ATI=${BETA_TITLE:-Chromium Beta}
sed -e "s/^APP_ID=.*/APP_ID=$AID/" -e "s/^APP_TITLE=.*/APP_TITLE=$ATI/" "$H/config.env" > "$B/config.env"
cd "$B/package"
python3 "$H/tools/beta/badge.py" icon.png icon.png "${BETA_BADGE:-}"; python3 "$H/tools/beta/badge.py" largeIcon.png largeIcon.png "${BETA_BADGE:-}"
python3 "$H/tools/beta/splash.py" splash.png splash.png
python3 mkipk.py --browser "$1" --version "$2" --launcher "$3" --out "$4" ${5:+--requires "$5"} ${LIBDIR:+--lib-dir "$LIBDIR"}

# Third-party notice: NSS and NSPR

The app ships the NSS and NSPR shared libraries in its `lib` folder. webOS 4 has no NSS, so every IPK brings its own
copy. The start script puts `<app>/lib` first in the library path, so the browser uses this copy on every TV, not the
TV's own.

## Sources

| Component | Licence | Version | Source |
|---|---|---|---|
| NSS (Network Security Services) | MPL-2.0 | 3.125 (`NSS_VERSION` in `config.env`) | https://ftp.mozilla.org/pub/mozilla.org/security/nss/releases/NSS_3_125_RTM/src/ |
| NSPR (Netscape Portable Runtime) | MPL-2.0 | 4.38.2 (`NSPR_VERSION` in `config.env`) | https://ftp.mozilla.org/pub/mozilla.org/nspr/releases/v4.38.2/src/ |
| openlgtv buildroot-nc4 SDK (compiler and sysroot used for the build) | GPL-2.0 and others | release `2026.08-webos` (`SDK_TAG` in `config.env`) | https://github.com/openlgtv/buildroot-nc4/releases/tag/2026.08-webos |

`config.env` also has the sha256 of each tarball and SDK, and the build checks them.

## How they are built

`container/build-nss-sdk.sh` builds both libraries from Mozilla's tarballs with the compiler from openlgtv's
prebuilt SDK. It uses buildroot's build settings for NSS and NSPR and two patches from buildroot's `package/libnss`,
copied into `container/nss/patches/`:

- `0001-Bug-1801182-Allow-overriding-OS_ARCH-OS_TEST-and-OS_.patch`
- `0002-Bug-2027768-Fix-build-failure-due-to-missing-gcm-stu.patch`

NSPR isn't patched (buildroot has no patches for it), and we don't add patches of our own to either.

`container/build-nss.sh` is an older way to build the same thing from buildroot itself (`BUILDROOT_COMMIT` in
`config.env`). We don't use it for releases.

## What ships

Only these ten files go into `<app>/lib` (the list is `container/nss/ship-list.txt`):

`libnss3.so`, `libnssutil3.so`, `libsmime3.so`, `libssl3.so`, `libsoftokn3.so`, `libfreebl3.so`, `libnssckbi.so`,
`libnspr4.so`, `libplc4.so`, `libplds4.so`

Nothing from the SDK's glibc or toolchain ships. `libfreeblpriv3.so` isn't needed: this NSS build has no
FREEBL_LOWHASH variant, so `libsoftokn3.so` loads `libfreebl3.so`.

## Licences

NSS and NSPR are under the Mozilla Public License 2.0. Their source is at the addresses above, which is what MPL-2.0
asks for when the libraries are shipped as binaries. The SDK is under the GPL-2.0 and other licences. We only use it
to build, and none of it ships.

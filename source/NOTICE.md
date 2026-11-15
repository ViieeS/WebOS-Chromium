# Third-party notices

| Part | Licence | Where from | Note |
|---|---|---|---|
| Chromium | BSD 3-Clause, plus the licences of its third-party code (Chromium's `about:credits`) | downloaded by `container/setup-source.sh` | not in this folder |
| webOS-ports/chromium151 (Chromium with LG's webOS OSE "neva" web runtime) | Chromium's licences; the webOS OSE parts are Apache-2.0 | https://github.com/webOS-ports/chromium151 at the commit in `config.env` | not in this folder; our patches apply on top of it |
| webOS Wayland protocol files (`tvproto/webos-*.xml`) | Apache-2.0 (`tvproto/LICENSE.webos-wayland-extensions`) | LG webOS OSE | included |
| `tvproto/tv-stubs.xml` | facts: request and event names, order, argument types | read from a retail TV's own client libraries | no LG code, see `tvproto/README.md` |
| `container/tvabi/*.abi`, `TV-SONAMES.txt` | facts: library names, symbol names, versions, types and sizes | read from the shared libraries of a retail webOS TV | interface lists for the link-time stub libraries, no code or data from the libraries, see `container/tvabi/README.md` |
| NSS 3.125, NSPR 4.38.2 (shipped in the app as shared libraries) | MPL-2.0 | Mozilla | see `NOTICE-nss.md` |
| openlgtv buildroot-nc4 SDK (compiler for NSS/NSPR) | GPL-2.0 and others | https://github.com/openlgtv/buildroot-nc4 | only used to build, none of it ships |
| wayland-scanner 1.18 (Ubuntu package) | MIT | Ubuntu | only used to build |

The app links against libraries the TV already has: glibc, libwayland, EGL/GLES, luna-service2 and LG's NDL media
library. None of LG's files are in this source or in the IPK.

"Chromium" here is the name of the browser engine. This project isn't affiliated with Google or LG.

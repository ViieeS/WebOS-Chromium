Licences and notices for WebOS Chromium
=======================================

LICENSE                   This project's own code (patches, start scripts, launcher, TV Tools): BSD 3-Clause.
chromium-credits.html.gz  Chromium and every third-party library in it, with their licence texts (FFmpeg,
                          OpenH264, dav1d, libvpx, BoringSSL, ICU, ...). The same list as chrome://credits.
NSS-NSPR.txt              The NSS and NSPR libraries in lib/ (Mozilla Public License 2.0) and where their
                          source is.
MPL-2.0.txt               The Mozilla Public License 2.0.
Apache-2.0.txt            The Apache License 2.0, for the webOS OSE parts of the engine (webOS-ports/chromium151)
                          and the webOS Wayland protocol files.

The engine is Chromium from https://github.com/webOS-ports/chromium151 with this project's patches on top.
Source, build steps and the full notice list: https://github.com/sarahahzub/WebOS-Chromium (folder source/).

This app uses libraries the TV already has (glibc, libwayland, EGL/GLES, luna-service2, LG's media library).
None of LG's files are in this app.

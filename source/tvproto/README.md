# tvproto/

Wayland protocol files for LG's webOS compositor. `container/gen-webos-headers.py` turns them into the
`wayland-webos-*-protocol.h` headers Chromium compiles against, and `package/launcher/build.sh` builds the launcher's
protocol code from `webos-foreign.xml`.

They match the TV's own libraries on purpose. webOS adds requests between releases without changing the interface
version, so the request numbers compiled into the browser have to be the TV's.

| File | Where it comes from |
|---|---|
| `webos-shell.xml`, `webos-foreign.xml`, `webos-input-manager.xml` | LG webOS Open Source Edition, Apache-2.0 (`LICENSE.webos-wayland-extensions`) |
| `tv-stubs.xml` | Interfaces the TV has but OSE doesn't define (`wl_webos_xinput`, `wl_starfish_*`, `text_model_factory`, linux-dmabuf). `gen-tv-stubs.py` wrote it from the interface tables in a retail TV's `libwayland-*-client` libraries. It has names, order and argument types only, no code. [webosbrew/wayland-protocols](https://github.com/webosbrew/wayland-protocols) publishes the same kind of data per webOS release. |
| `gen-tv-stubs.py` | Writes `tv-stubs.xml`. It runs on a TV, or on a copy of a TV's root filesystem. |

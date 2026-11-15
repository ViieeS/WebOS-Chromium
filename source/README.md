# WebOS Chromium source

This is the source of WebOS Chromium, a Chromium 151 browser that runs natively on LG webOS TVs (webOS 4.4 and
newer).

Only our own work is here: patches, build scripts, the launcher, TV Tools and the packaging. Chromium isn't
included, the build downloads it at a pinned commit. Nothing from LG's TV firmware is included either.

| Path | What |
|---|---|
| `config.env` | Versions and pins: the chromium151 commit, toolchains, NSS, app id |
| `args.gn` | GN build arguments |
| `patches/` | Our changes to Chromium, applied in name order. `patches/INDEX.md` has one line per patch. |
| `container/` | Build scripts: source, sysroot, gn, build, collect and the ABI checks |
| `modalapp/build.py` | Runs the build scripts on Modal |
| `tvproto/` | webOS Wayland protocol files |
| `package/` | The app: launcher (`bin/run`), start script, TV Tools, IPK builder, icons |
| `tools/` | Crash symbolizer and the Beta packaging script |

How to build it: [BUILDING.md](BUILDING.md).

## Credits

- [webOS-ports/chromium151](https://github.com/webOS-ports/chromium151): Chromium 151 with LG's
  [webOS OSE](https://www.webosose.org/) platform layer. Everything here is built on top of it.
- [Chromium](https://www.chromium.org/), the browser engine.
- [openlgtv buildroot-nc4](https://github.com/openlgtv/buildroot-nc4): the webOS SDK and the NSS/NSPR build recipes
  we use for webOS 4.
- [webOS Homebrew](https://www.webosbrew.org/): the NDL media headers from
  [webos-userland](https://github.com/webosbrew/webos-userland), used by the launcher.

## Licence

Our patches, scripts and app code are under the BSD 3-Clause licence (`LICENSE`), same as Chromium. Third-party
parts keep their own licences, see `NOTICE.md`. This project isn't affiliated with Google or LG.

# Building WebOS Chromium

The browser is cross-compiled on x86-64 Linux. You don't need a TV or any file from one: the browser links against
stub libraries (`container/tvabi/`) that have the same interface as the TV's own, and on the TV the real ones load.
That's what `SYSROOT_SOURCE=public` in `config.env` means.

The other mode, `SYSROOT_SOURCE=tv`, builds the sysroot from a copy of a TV's root filesystem instead. It's only for
people who have one: it needs `tv/rootfs.sqfs` and `tv/apps.tar` (each with a `.sha256` file) on the Modal Volume.

The build scripts run in containers on [Modal](https://modal.com), one `modal run` per step below. They also work in
any Ubuntu 22.04 container that uses the same paths.

## What you need

- A Modal account and the `modal` CLI (`pip install modal`, then `modal setup`). Create the Volume once with
  `modal volume create --version=2 chromium-webos`, or pick your own name and put it in `config.env`
  (`MODAL_VOLUME`).
- For NSS (step 4) and the app (steps 6 and 7), a Linux machine, x86-64 or arm64, with glibc 2.36 or newer. The
  SDK's compiler needs that, so Ubuntu 24.04 works and 22.04 doesn't. A plain `ubuntu:24.04` container is enough.
  Install `curl zstd make gcc g++ python3 python3-pil dpkg-dev binutils-arm-linux-gnueabi libwayland-dev file patch
  xz-utils` and [zig 0.13.0](https://ziglang.org/download/) (`ZIG=/path/to/zig`). It needs about 3 GB of disk.
- Money for Modal. Steps 1 to 3 cost well under $1. The first browser build (step 5) is a full Chromium build: a
  few hours on 64 cores, roughly $15 to $30. Later builds reuse its compile cache and take minutes.

## Paths inside the containers

| Path | What |
|---|---|
| `/opt/cn` | this folder (`config.env`, `args.gn`, `container/`, `patches/`, `tvproto/`) |
| `/vol` | the Modal Volume: `snap/` (snapshots), `logs/`, `artifacts/` |
| `/work` | scratch space |

Keep these paths: ninja and ccache remember absolute paths.

## Steps

Run them from this folder.

1. `modal run modalapp/build.py::setup_source` downloads chromium151 at the pinned commit with Chromium's clang, Rust
   and node, and saves it as the snapshot `src` (about 6 GB).
2. `modal run modalapp/build.py::build_sysroot`, then `modal run modalapp/build.py::build_sysroot_compat`, make the
   target sysroot: Ubuntu 22.04 packages at the versions in `container/sysroot-pinned.txt` (from Launchpad, sha256
   checked), the stub libraries from `container/tvabi/`, and glibc 2.24 from Debian. With the old glibc, one build
   runs on webOS 4.4 and newer.
3. `modal run modalapp/build.py::build_builtins` builds compiler-rt for ARMv7 softfp.
4. NSS and NSPR for webOS 4, which ships without them (see `NOTICE-nss.md`). This runs on your own machine:
   `bash container/build-nss-sdk.sh`. It uploads `artifacts/<id>/nss.tar.zst` to the Volume. With `NSS_UPLOAD=0` it
   stays local and you upload it yourself with `modal volume put`. Unpack its `lib/` for step 7.
5. The browser: `modal run modalapp/build.py::build --targets chrome --hours 6 --collect`. It applies `patches/` and
   builds in chunks, saving a checkpoint after each one. If it runs out of time, run the same command again and it
   continues. At the end it checks every binary with `container/check-abi.py` and writes
   `artifacts/<id>/browser.tar.zst`. Download it with
   `modal volume get chromium-webos artifacts/<id>/browser.tar.zst .`
6. The launcher: `cd package/launcher && OUT=/tmp/cnrun sh build.sh`.
7. The IPK, with absolute paths. For the normal app (`org.nativechromium.browser`):

   ```sh
   python3 package/mkipk.py --browser $PWD/browser.tar.zst --version VERSION --launcher /tmp/cnrun \
       --requires "glibc=2.24,webos=webOS 4.4" --lib-dir NSS_LIB_DIR --out OUTDIR
   ```

   For the Beta app (`org.nativechromium.browser.beta`, installs next to the normal one):
   `sh tools/beta/mkbeta.sh $PWD/browser.tar.zst VERSION /tmp/cnrun OUTDIR "glibc=2.24,webos=webOS 4.4" NSS_LIB_DIR`

   Install it with Homebrew Channel or `ares-install`.

## The stub libraries

`container/tvabi/README.md` says how the `.abi` files were made from a TV's libraries and how the stubs are checked
against them. Nothing else in the build reads TV files.

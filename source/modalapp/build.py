"""modalapp/build.py: every Modal function of the chromium151-for-webOS build. Run with `modal run modalapp/build.py::<function>`.

Each function starts a fresh container, runs one script from container/, and commits the Volume. Nothing on the
container's local disk survives; everything worth keeping is saved as a snapshot on the Volume (snap/: source, sysroots, out dir, ccache).

  hello          tiny check that the account works                       (0.25 core)
  tvdata_check   sha256 of the TV files on the Volume                    (2 cores)
  setup_source   clone chromium151 + toolchains -> snap/src              (8 cores)
  build_sysroot  TV-faithful sysroot -> snap/sysroot                     (4 cores)
  build_sysroot_compat glibc 2.24 sysroot for webOS 4.4 and newer -> snap/sysroot-compat (4 cores)
  build_builtins softfp compiler-rt builtins -> snap/builtins            (8 cores)
  build_nss      buildroot NSS/NSPR for the TV -> artifacts/<id>/nss.tar.zst (16 cores)
  abi_smoke      softfp test program -> artifacts/abi-smoke/             (4 cores)
  gn_gen         patches + gn gen + webOS protocol audit                 (8 cores)
  build          ninja in time-boxed chunks with checkpoints             (BUILD_CPU cores)
"""
import os
import pathlib
import subprocess
import threading

import modal

HERE = pathlib.Path(__file__).resolve().parent.parent  # the folder with config.env


def _config():
    for p in (HERE / "config.env", pathlib.Path("/opt/cn/config.env")):
        if p.is_file():
            out = {}
            for line in p.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip()] = v.strip()
            return out
    raise SystemExit("config.env not found")


CFG = _config()
app = modal.App(CFG["MODAL_APP"])
vol = modal.Volume.from_name(CFG["MODAL_VOLUME"], version=2)  # create it once: modal volume create --version=2 <name>

# Image: only things that never change between builds. Cached by Modal after the first build (a few minutes).
# The folder's own files are added last with copy=False, so editing a script does NOT rebuild the image.
image = (
    modal.Image.from_registry("ubuntu:22.04", add_python="3.12")
    .env({"DEBIAN_FRONTEND": "noninteractive", "TZ": "UTC"})
    # i386: V8's snapshot/generator tools for a 32-bit ARM target are i386 binaries; gVisor cannot exec them, so
    # patch 0006 runs them under qemu-i386-static with Ubuntu's i386 libc.
    .run_commands("dpkg --add-architecture i386")
    .apt_install("qemu-user-static", "libc6:i386", "libgcc-s1:i386")
    # x64 host tools (clang_x64/*) run during the build and link host NSS/NSPR/glib (e.g.
    # make_top_domain_list_variables needs libnspr4.so). Runtime libs for them. Others are common
    # Chromium host-tool deps, added so the next host tool does not stop the build.
    .apt_install("libnss3", "libnspr4", "libglib2.0-0", "libdbus-1-3", "libexpat1", "libfontconfig1", "libfreetype6",
                 "libharfbuzz0b", "libxkbcommon0", "libwayland-client0", "libdrm2", "libgbm1", "libasound2",
                 "libpulse0", "libudev1", "libxml2", "libxslt1.1")
    .apt_install(
        "git", "curl", "ca-certificates", "xz-utils", "zstd", "unzip", "ccache", "squashfs-tools", "file",
        "binutils", "rsync", "patch", "pkg-config", "lsb-release", "python3", "python3-pip", "gperf", "bison",
        "flex", "dpkg-dev", "cmake", "wayland-protocols", "libwayland-bin", "tar", "procps", "time", "gawk",
        # buildroot host tools (build_nss): its dependency check stops without them (run 20261104T083057Z-build_nss)
        "wget", "cpio", "bc",
    )
    .run_commands(
        f"curl -fsSL -o /tmp/ninja.zip https://github.com/ninja-build/ninja/releases/download/v{CFG['NINJA_VERSION']}/ninja-linux.zip",
        "unzip -o /tmp/ninja.zip -d /usr/local/bin && chmod 755 /usr/local/bin/ninja && rm /tmp/ninja.zip",
        "ninja --version",
    )
    .add_local_file(str(HERE / "config.env"), "/opt/cn/config.env", copy=False)
    .add_local_file(str(HERE / "args.gn"), "/opt/cn/args.gn", copy=False)
    .add_local_dir(str(HERE / "container"), "/opt/cn/container", copy=False)
    .add_local_dir(str(HERE / "patches"), "/opt/cn/patches", copy=False)
    # webOS Wayland protocol definitions matching the TV's libraries: source of the webOS headers
    .add_local_dir(str(HERE / "tvproto"), "/opt/cn/tvproto", copy=False)
)

COMMON = dict(image=image, volumes={"/vol": vol})


def _run(script, *args, build_id="", commit_every_min=0):
    env = dict(os.environ)
    if build_id:
        env["BUILD_ID"] = build_id
    cmd = ["bash", f"/opt/cn/container/{script}", *[str(a) for a in args]]
    print("+", " ".join(cmd), flush=True)
    stop = threading.Event()

    def _save_every():  # commit the Volume while the script runs, so a preemption keeps the caches so far
        while not stop.wait(commit_every_min * 60):
            try:
                vol.commit()
            except Exception as e:  # a failed periodic commit must not stop the build
                print("periodic Volume commit failed:", e, flush=True)

    if commit_every_min:
        threading.Thread(target=_save_every, daemon=True).start()
    rc = subprocess.run(cmd, env=env).returncode
    stop.set()
    vol.commit()
    if rc != 0:
        raise SystemExit(f"{script} failed with exit code {rc} (log: /logs/<build-id>.log on the Volume)")
    return rc


@app.function(**COMMON, cpu=0.25, memory=512, timeout=300)
def hello():
    out = subprocess.run(
        "echo hello from modal; uname -m; nproc; free -g | sed -n 2p; ls -la /vol",
        shell=True, capture_output=True, text=True,
    ).stdout
    print(out)
    return out


@app.function(**COMMON, cpu=2, memory=2048, timeout=1800)
def tvdata_check(build_id: str = ""):
    return _run("tvdata-check.sh", build_id=build_id)


@app.function(**COMMON, cpu=8, memory=16384, timeout=3 * 3600)
def setup_source(build_id: str = ""):
    return _run("setup-source.sh", build_id=build_id)


@app.function(**COMMON, cpu=4, memory=8192, timeout=2 * 3600)
def build_sysroot(build_id: str = ""):
    return _run("build-sysroot.sh", build_id=build_id)


@app.function(**COMMON, cpu=4, memory=8192, timeout=3600)
def build_sysroot_compat(build_id: str = ""):
    return _run("build-sysroot-compat.sh", build_id=build_id)


@app.function(**COMMON, cpu=8, memory=8192, timeout=3600)
def build_builtins(build_id: str = ""):
    return _run("build-builtins.sh", build_id=build_id)


# ephemeral_disk: Modal rejects requests under 512 GiB (run 20261104T082908Z-build_nss failed at submit, no charge).
# The real size is in nss-report.txt and the status file.
@app.function(**COMMON, cpu=64, memory=65536, timeout=6 * 3600, ephemeral_disk=512 * 1024)
def build_nss(build_id: str = ""):
    return _run("build-nss.sh", build_id=build_id, commit_every_min=5)


@app.function(**COMMON, cpu=4, memory=8192, timeout=3600)
def abi_smoke(build_id: str = ""):
    return _run("abi-smoke.sh", build_id=build_id)


@app.function(**COMMON, cpu=8, memory=16384, timeout=2 * 3600)
def gn_gen(build_id: str = ""):
    return _run("gn-gen.sh", build_id=build_id)


# ephemeral_disk (2026-10-30): the official (ThinLTO) build was evicted at ~24k of 63k steps for
# "exceeding its memory request" while the build tree, ccache and bitcode objects grew. Modal gives up to 3 TiB of
# real disk per container when asked; disk is billed at 1/20 of memory (1 TiB ~ 51 GiB), below our memory request.
# memory=(request, limit), limit == request (2026-10-30): with only a request, Modal counts the page cache of the
# 100+ GB build tree as usage and evicts the container under host pressure ("exceeding its memory request"; ThinLTO
# builds died at 24k and 30k of 63k steps). A hard limit makes the kernel drop clean cache instead; a test wrote 20 GB
# under an 8 GiB limit without a kill. Compilers + linker need far less than the limit.
@app.function(**COMMON, cpu=float(CFG["BUILD_CPU"]), memory=(int(CFG["BUILD_MEM_MIB"]), int(CFG["BUILD_MEM_MIB"])),
              timeout=24 * 3600, ephemeral_disk=int(CFG.get("BUILD_DISK_MIB", "1048576")))
def build(targets: str = "chrome", hours: float = 6.0, chunk_min: int = 30, clean: bool = False,
          collect: bool = False, build_id: str = ""):
    # Modal may preempt a long function and restart it with the same arguments. build.sh is safe to restart:
    # it always continues from the newest checkpoint on the Volume.
    return _run("build.sh", targets, hours, chunk_min, int(clean), int(collect), build_id=build_id)


# Small incremental builds (a few patched files + relink): 32 cores / 64 GiB, which Modal schedules much faster than
# the big size (2026-10-28). Same script and caches as build().
@app.function(**COMMON, cpu=32.0, memory=(65536, 65536), timeout=24 * 3600, ephemeral_disk=512 * 1024)
def build_small(targets: str = "chrome", hours: float = 2.0, chunk_min: int = 30, clean: bool = False,
                collect: bool = False, build_id: str = ""):
    return _run("build.sh", targets, hours, chunk_min, int(clean), int(collect), build_id=build_id)

# CI build

`.github/workflows/build.yml` builds the browser IPK from `source/` on GitHub-hosted runners, so anyone can see how
an IPK was made. It builds the main branch only, and only when started by hand.

## Release steps

1. Change `main`: push to it directly, or merge a pull request. Add a `## New in <version>` section to `README.md`
   for the release notes.
2. **Actions → Build WebOS Chromium → Run workflow** on `main`, with a new version (x.y.z). A version that is
   already a published release is refused.
3. The build ends with a **pre-release** `v<version>`: the IPK, its manifest, and the `## New in <version>` section
   of `README.md` as notes (no notes if there is no such section). A pre-release is never the latest release, so
   no TV is offered it yet. Running the build again with the same version replaces that pre-release.
4. Test the IPK from the pre-release on a TV.
5. **Actions → Publish update → Run workflow** with the version. It makes `v<version>` the latest release and
   points `webosbrew/apps.json` at it (one commit on `main` under your name). Homebrew Channel's main repository
   reads the latest release's manifest when it rebuilds, every 3 hours (at 41 minutes past) and on every change to
   it, so TVs see the update within about 3 hours; this project's own repository (`webosbrew/apps.json`) right
   away.

## Build jobs

The main branch is resolved to one commit at the start, and every job builds that commit:

| Job | What | Script from `source/` |
|---|---|---|
| Source commit | resolves the branch to a commit | |
| Chromium source | chromium151 at the pinned commit, Chromium's clang, Rust and node | `container/setup-source.sh` |
| Sysroot and builtins | the public sysroot (Ubuntu packages + stub libraries, no TV files), glibc 2.24, compiler-rt | `container/build-sysroot.sh`, `build-sysroot-compat.sh`, `build-builtins.sh` |
| NSS and NSPR | from the Mozilla tarballs with the openlgtv SDK | `container/build-nss-sdk.sh` |
| Launcher | `bin/run` | `package/launcher/build.sh` |
| Build part 1, 2, ... | the browser, about 5 hours per job (a hosted job stops at 6 hours); each part continues the previous one's build folder (at most 5 parts) | `container/build.sh` |
| IPK and provenance | the IPK, plus a signed build provenance attestation | `package/mkipk.py` |
| Pre-release | the IPK and its manifest on the pre-release `v<version>`, notes from `README.md` | `ci/release.sh` |
| Result | the end summary (IPK, sha256, how to verify); one logs artifact; deletes the hand-overs between jobs | |

`args.gn` is used as it is, except `cc_wrapper = ""` (no ccache: the build folder carries the state between jobs);
`ci/prep-cn.sh` makes that change in a copy and prints the diff in the log.

Source, sysroot and NSS are cached, so later runs of the same Chromium commit skip those jobs. The finished build
folder is cached too: the next run starts from it and recompiles only what changed. `seed_run` (optional) starts from
another run's last build-folder hand-over when no cached build folder fits. The container (`ci/Dockerfile`) has the
same contents as the image in `source/modalapp/build.py`.

A finished run keeps two artifacts: **webos-chromium-&lt;version&gt;** (the IPK) and **logs-&lt;version&gt;** (build logs,
SHA256SUMS, ABI and NSS reports, the launcher). Verify an IPK with `gh attestation verify <file> -R sarahahzub/WebOS-Chromium`.

`ci/gen-variant-workflow.py` writes `.github/workflows/variant.yml` (the chain of build parts); edit the script, not
the generated file.

# CI build

`.github/workflows/build.yml` builds the browser IPK from `source/` on GitHub-hosted runners, so anyone can see how
an IPK was made. It only runs when started by hand: **Actions → Build WebOS Chromium → Run workflow** (inputs: source
repo, branch or commit, IPK version).

The branch is resolved to one commit at the start, and every job builds that commit:

| Job | What | Script from `source/` |
|---|---|---|
| Source commit | resolves the branch to a commit | |
| Chromium source | chromium151 at the pinned commit, Chromium's clang, Rust and node | `container/setup-source.sh` |
| Sysroot and builtins | the public sysroot (Ubuntu packages + stub libraries, no TV files), glibc 2.24, compiler-rt | `container/build-sysroot.sh`, `build-sysroot-compat.sh`, `build-builtins.sh` |
| NSS and NSPR | from the Mozilla tarballs with the openlgtv SDK | `container/build-nss-sdk.sh` |
| Launcher | `bin/run` | `package/launcher/build.sh` |
| Build part 1, 2, ... | the browser, about 5 hours per job (a hosted job stops at 6 hours); each part continues the previous one's build folder (at most 5 parts) | `container/build.sh` |
| IPK and provenance | the IPK, plus a signed build provenance attestation | `package/mkipk.py` |
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

#!/bin/bash
# build.sh TARGETS HOURS CHUNK_MIN CLEAN COLLECT
# Restore caches -> apply patches -> ninja in chunks of CHUNK_MIN minutes -> after every chunk save a checkpoint
# (out dir, patch timestamps, ccache) -> stop when done, failed, or HOURS used. Safe to restart at any time.
#
# Two caches:
#   out-tv  : the whole out/tv dir. With it, ninja rebuilds only files whose inputs changed (minutes).
#   ccache  : compile results by content. Used when out-tv is missing or stale (new commit, new sysroot, clean build):
#             ninja then re-runs every compile, but most are answered from ccache instead of compiled.
. /opt/cn/container/lib.sh
TARGETS=$1 HOURS=$2 CHUNK=$3 CLEAN=$4 COLLECT=$5
TARGETS=${TARGETS//\'/}; TARGETS=${TARGETS//\"/}   # quotes that survived ssh/modal quoting
st targets="$TARGETS" hours="$HOURS" chunk_min="$CHUNK" clean="$CLEAN"
T0=$SECONDS
need_src
need_sysroot
need_builtins
SRC_COMMIT=$(git -C "$REPO" rev-parse HEAD)
SYSROOT_ID=$($META get "$SNAP/$SYSROOT_SNAP.json" id)
ARGS_SHA=$(sha256sum "$CN/args.gn" | cut -c1-16)
PATCH_SHA=$(cat /opt/cn/patches/*.patch 2>/dev/null | sha256sum | cut -c1-16)

# --- restore out dir + decide about ccache
# OFFICIAL (ThinLTO) builds (2026-10-30): 3/4 of the jobs (memory). ccache stays ON and is saved in the background
# every 3 minutes: after an eviction/preemption only the last ~3 minutes of compiles are lost.
OFFICIAL=0; grep -q '^is_official_build = true' "$CN/args.gn" && OFFICIAL=1
# Separate build-dir snapshots per build kind (2026-10-30): dev builds and official size builds would
# otherwise overwrite each other's out-tv and both fall back to a full recompile.
OUTSNAP=out-tv; [ $OFFICIAL = 1 ] && OUTSNAP=out-tv-official
OUTSNAP=$OUTSNAP$OUT_SUFFIX
want_ccache=1
if [ "$CLEAN" = 1 ]; then
	log "clean build requested: not restoring out-tv"
elif snap_restore $OUTSNAP "$SRC/out"; then
	o_commit=$($META get "$SNAP/$OUTSNAP.json" src_commit) o_sys=$($META get "$SNAP/$OUTSNAP.json" sysroot_id)
	o_args=$($META get "$SNAP/$OUTSNAP.json" args_sha)
	o_done=$($META get "$SNAP/$OUTSNAP.json" complete 0)
	# 2026-11-01: only a FINISHED out-tv means "small incremental build". A checkpoint of a build that was preempted
	# (Modal restarts it with the same input) still has thousands of steps to go: restore ccache and keep saving it
	# every CCACHE_SAVE_MIN, otherwise each preemption loses everything since the last 30-min checkpoint.
	if [ "$o_commit" = "$SRC_COMMIT" ] && [ "$o_sys" = "$SYSROOT_ID" ] && [ "$o_args" = "$ARGS_SHA" ] && [ "$o_done" = 1 ]; then
		want_ccache=0
		log "out-tv is a finished build of this commit/sysroot/args: incremental build, ccache not restored"
	elif [ "$o_commit" = "$SRC_COMMIT" ] && [ "$o_sys" = "$SYSROOT_ID" ] && [ "$o_args" = "$ARGS_SHA" ]; then
		log "out-tv is a checkpoint of an unfinished build: continuing it, restoring ccache too"
	else
		log "out-tv is from another commit/sysroot/args ($o_commit $o_sys $o_args): restoring ccache too"
	fi
fi
if [ $want_ccache = 1 ]; then snap_restore $CCACHE_SNAP "$W" || mkdir -p "$CCACHE_DIR"; else mkdir -p "$CCACHE_DIR"; fi
ccache -z > /dev/null
st restore_s=$((SECONDS - T0)) ccache_restored=$want_ccache

# --- patches (timestamps kept stable, see apply-patches.py)
REPO=$REPO W=$W SNAP=$SNAP python3 "$CN/container/apply-patches.py" 2>&1 | tee -a "$LOG"

# --- args + gn
mkdir -p "$OUT"
cmp -s "$CN/args.gn" "$OUT/args.gn" || cp "$CN/args.gn" "$OUT/args.gn"
cd "$SRC"
[ -f "$OUT/build.ninja" ] || buildtools/linux64/gn gen "$OUT" 2>&1 | tee -a "$LOG"
ninja -C "$OUT" build.ninja > "$W/gn-regen.txt" 2>&1 || { log "gn regen FAILED:"; grep -vE "^\[|^ninja: Entering" "$W/gn-regen.txt" | head -40 | tee -a "$LOG"; }   # re-run gn now if args/patches changed BUILD files
# CN_FORCE_REBUILD (config.env, space-separated dirs under out/tv/obj): objects that depend on SYSROOT headers, which
# ninja does not track (-MMD), and ccache's depend mode can't see either. Delete them and recompile without cache.
if [ -n "${CN_FORCE_REBUILD:-}" ]; then
	# objects only (the .ninja files there are build rules); gn gen restores rules a previous run may have removed
	(cd "$SRC" && buildtools/linux64/gn gen "$OUT" > /dev/null) || die "gn gen failed"
	for d in $CN_FORCE_REBUILD; do
		log "force rebuild: obj/$d ($(find "$OUT/obj/$d" -name '*.o' 2>/dev/null | wc -l) objects)"
		find "$OUT/obj/$d" -name '*.o' -delete 2>/dev/null || true
	done
	export CCACHE_RECACHE=1
fi
# Sysroot glibc lowered (2026-11-04, 2.28 -> 2.24): -MMD does not track sysroot headers, so objects compiled against
# the newer headers stay "up to date" and still call functions the older glibc lacks (fcntl64 from the 2.28
# _FILE_OFFSET_BITS=64 redirect, getentropy, ...). Once per sysroot id, delete every object whose bytes name one of
# the functions in container/glibc-newer-syms.txt and recompile it without ccache. A false match only costs a recompile.
symscan() {
	syms=$(grep -vE '^\s*(#|$)' "$CN/container/glibc-newer-syms.txt" | awk '{print $1}')
	stale=$(find "$OUT/obj" -name '*.o' -print0 | xargs -0 -r -P "$(nproc)" -n 200 grep -laF $(printf -- '-e %s ' $syms) || true)
	n=$(printf '%s\n' "$stale" | grep -c . || true)
	log "sysroot symbol scan: $n objects name a function newer than glibc ${TV_GLIBC_MAX:-?}; deleting them"
	printf '%s\n' "$stale" | grep . | sed "s|^$OUT/||" | head -40 | tee -a "$LOG" || true
	printf '%s\n' "$stale" | grep . | xargs -r rm -f || true
	[ "$n" = 0 ] || export CCACHE_RECACHE=1
}
# key = sysroot id + list hash (2026-11-13): a new name in the list also triggers one scan
SYMSCAN_KEY=$SYSROOT_ID
[ -f "$CN/container/glibc-newer-syms.txt" ] && SYMSCAN_KEY=$SYSROOT_ID-$(grep -vE '^\s*(#|$)' "$CN/container/glibc-newer-syms.txt" | sha256sum | cut -c1-12)
SYMSCAN_SYS=$($META get "$SNAP/$OUTSNAP.json" symscan_sysroot "")
if [ -f "$CN/container/glibc-newer-syms.txt" ] && [ "$SYMSCAN_SYS" != "$SYMSCAN_KEY" ] && [ -d "$OUT/obj" ]; then
	symscan
fi
SYMSCAN_SYS=$SYMSCAN_KEY
# 2026-11-13: a clean build (no out dir to scan) with a restored ccache can still get stale objects from ccache
# (compiled against older glibc headers, e.g. fcntl64), which then fail at link. On the first failure of a run,
# scan the objects ninja made, delete the stale ones and try once more (RESCANNED below).
RESCANNED=0
# float-ABI guard: every target compile must say -mfloat-abi=softfp, or objects come out hard-float
grep -q -- "-mfloat-abi=softfp" "$OUT/obj/base/base.ninja" || die "target cflags lack -mfloat-abi=softfp (patch 0012?)"

checkpoint() {
	local t0=$SECONDS
	wait_ccsaver_idle
	SNAP_TAR_OPTS=()
	snap_save $OUTSNAP "$SRC/out" tv
	$META set "$SNAP/$OUTSNAP.json" src_commit="$SRC_COMMIT" sysroot_id="$SYSROOT_ID" args_sha="$ARGS_SHA" patch_sha="$PATCH_SHA" \
		symscan_sysroot="$SYMSCAN_SYS" complete="$([ "${status:-}" = done ] && echo 1 || echo 0)"
	cp -f "$W/patch-state.json" "$SNAP/patch-state.json"
	local hits; hits=$(ccache -s 2>/dev/null | grep -iE 'hits|misses' | tr -s ' ' | tr '\n' ';')
	if [ "$STEPS_RUN" -gt 300 ] && [ $want_ccache = 1 ]; then
		ccache -c > /dev/null 2>&1 || true
		snap_save $CCACHE_SNAP "$W" ccache
	fi
	st checkpoint_s=$((SECONDS - t0)) ccache="$hits" steps_run="$STEPS_RUN" last_checkpoint="$(date -u +%FT%TZ)"
	push_log
}

# --- why would ninja rebuild? (dry run; first reasons go to the log: cache problems show up here)
WOULD=$(ninja -C "$OUT" -n $TARGETS 2>/dev/null | grep -cE '^\[[0-9]+/[0-9]+\]' || true)
log "dry run: $WOULD steps would run"
ninja -C "$OUT" -n -d explain $TARGETS 2>&1 | grep '^ninja explain:' | head -40 | tee -a "$LOG" || true
st dry_run_steps="$WOULD"

# --- resource sampler (to size the container: are 32 cores / 96 GiB too much?). Every 15 s: memory in use (MemTotal -
# MemAvailable) and CPU busy % over the interval; ninja-time peak and average go into the status JSON.
SAMPLES=$W/samples.txt; : > "$SAMPLES"
( read -r _ u n s i w x y z _ < /proc/stat; pb=$((u+n+s+x+y+z)); pt=$((pb+i+w))
  while sleep 15; do
	read -r _ u n s i w x y z _ < /proc/stat; b=$((u+n+s+x+y+z)); t=$((b+i+w))
	mt=$(awk '/^MemTotal/{print $2}' /proc/meminfo); ma=$(awk '/^MemAvailable/{print $2}' /proc/meminfo)
	[ $t -gt $pt ] && echo "$(( (mt-ma)/1024 )) $(( 100*(b-pb)/(t-pt) ))" >> "$SAMPLES"
	pb=$b; pt=$t
  done ) &
SAMPLER=$!
res_stats() {   # never fails the build
	[ -s "$SAMPLES" ] || return 0
	read -r pk av p95 < <(awk '{m=$1>m?$1:m; c+=$2; n++; a[n]=$1} END{asort(a); printf "%d %d %d\n", m, c/n, a[int(n*0.95)+0]}' "$SAMPLES")
	st mem_peak_mib="$pk" mem_p95_mib="$p95" cpu_busy_avg_pct="$av" res_samples="$(wc -l < "$SAMPLES")"
	cp -f "$SAMPLES" "$LOGS/$BUILD_ID.samples.txt" 2>/dev/null || true
}

# --- background ccache save every CCACHE_SAVE_MIN minutes while ninja runs (ninja is NOT stopped).
# Safe: ccache writes each entry to a temp file and renames it, so a copy only sees whole entries. After a preemption
# the build dir comes from the last 30-min checkpoint, but almost every compile since then is a ccache hit.
CCACHE_SAVE_MIN=${CCACHE_SAVE_MIN:-3}
ccache_bg_save() {
	local tmp=$SNAP/.$CCACHE_SNAP.bg.tar.zst
	tar --format=posix -C "$W" -cf - ccache 2>/dev/null | zstd -T4 -3 -q -f -o "$tmp" &&
		mv -f "$tmp" "$SNAP/$CCACHE_SNAP.tar.zst" &&
		$META set "$SNAP/$CCACHE_SNAP.json" saved="$(date -u +%FT%TZ)" bytes="$(stat -c %s "$SNAP/$CCACHE_SNAP.tar.zst")" build_id="$BUILD_ID" background=1 &&
		sync "$SNAP" 2>/dev/null
}
# Incremental builds (ccache not restored) never save it: their nearly empty ccache would replace the shared one
# that the other build kind (dev / official) restores (2026-10-31).
[ $want_ccache = 1 ] || CCACHE_SAVE_MIN=$((60 * 24 * 365))
( while sleep $(( CCACHE_SAVE_MIN * 60 )); do
	t0=$SECONDS; nice -n 10 bash -c "$(declare -f ccache_bg_save); W=$W SNAP=$SNAP META='$META' BUILD_ID=$BUILD_ID CCACHE_SNAP=$CCACHE_SNAP ccache_bg_save" &&
		echo "[$(date -u +%H:%M:%S)] background ccache save done in $((SECONDS - t0)) s" >> "$LOG"
  done ) &
CCSAVER=$!

wait_ccsaver_idle() { while pgrep -f "ccache_bg_save" > /dev/null 2>&1; do sleep 2; done; }
# --- ninja in chunks
# jobs: CPU+8, no memory cap (2026-10-27: memory is sized in config.env instead)
J=$(( $(nproc) + 8 )); [ $OFFICIAL = 1 ] && J=$(( $(nproc) * 3 / 4 )); [ "${BUILD_JOBS:-}" ] && J=$BUILD_JOBS
log "ninja jobs: $J"
st jobs=$J
DEADLINE=$(( T0 + $(python3 -c "print(int(float('$HOURS') * 3600))") ))
LOG0=$(wc -l < "$OUT/.ninja_log" 2>/dev/null || echo 0)
STEPS_RUN=0
status=running
rc=0
while :; do
	left=$(( DEADLINE - SECONDS ))
	[ $left -gt 900 ] || { status=out_of_time; break; }
	chunk=$(( CHUNK * 60 )); [ $chunk -lt $(( left - 900 )) ] || chunk=$(( left - 900 ))
	log "ninja -j $J $TARGETS (chunk ${chunk}s, ${left}s left)"
	st status=building
	set +e
	# full output goes to the log; the live Modal log (modal app logs) shows every 200th step and all errors
	timeout -s INT -k 300 "$chunk" ninja -C "$OUT" -j "$J" ${NINJA_KEEP_GOING:+-k 0} $TARGETS 2>&1 | tee -a "$LOG" |
		awk '/^\[[0-9]+\/[0-9]+\]/{n++; if (n % 200 == 0) {print; fflush()}} /^FAILED|error:/{print; fflush()}'
	rc=${PIPESTATUS[0]}
	set -e
	STEPS_RUN=$(( $(wc -l < "$OUT/.ninja_log" 2>/dev/null || echo 0) - LOG0 ))
	if [ $rc -eq 0 ]; then status=done; break; fi
	if [ $rc -eq 124 ]; then log "chunk time up: checkpoint"; res_stats; checkpoint; continue; fi
	if [ $RESCANNED = 0 ] && [ $want_ccache = 1 ] && [ -f "$CN/container/glibc-newer-syms.txt" ] && [ -d "$OUT/obj" ]; then
		RESCANNED=1; log "ninja failed: sysroot symbol scan of the new objects, then one more try"; symscan; continue
	fi
	status=failed; break
done
log "ninja finished: $status (exit $rc), $STEPS_RUN steps run"
kill $SAMPLER $CCSAVER 2>/dev/null || true
res_stats
# float-ABI guard on the output: no hard-float target object may exist
if [ "$status" = done ]; then
	hf=$(find "$OUT/obj" -name '*.o' -newer "$OUT/args.gn" 2>/dev/null | head -400 | xargs -r "$LLVM/llvm-readelf" -A 2>/dev/null | grep -c 'Tag_ABI_VFP_args: VFP registers' || true)
	st hardfloat_sample="$hf"
	[ "$hf" = 0 ] || { log "ERROR: $hf hard-float objects in a sample of 400"; status=failed; rc=99; }
fi
if [ "$status" = failed ]; then
	grep -nE '^FAILED: |error: ' "$LOG" | head -200 > "$LOGS/$BUILD_ID.errors.txt" || true
fi
checkpoint
st status="$status" ninja_exit=$rc wall_s=$((SECONDS - T0)) steps_run="$STEPS_RUN" \
	last_progress="$(grep -oE '^\[[0-9]+/[0-9]+\]' "$LOG" | tail -1)"

if [ "$status" = done ] && [ "$COLLECT" = 1 ]; then
	BUILD_ID=$BUILD_ID TARGETS="$TARGETS" bash "$CN/container/collect.sh" || { st status=collect_failed; push_log; exit 1; }
	st status=done
fi
push_log
# out_of_time is not an error: everything is saved, run the same command again to continue
[ "$status" = failed ] && exit 1
exit 0

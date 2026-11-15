#!/bin/bash
# gn-gen.sh: patches + gn gen on a throw-away out dir, then compile ONLY the webOS Wayland extension files
# (a few minutes) to find every place Chromium uses a webOS protocol message the TV does not have.
# Outputs in artifacts/<build-id>/: gn-gen.txt, args-effective.txt, webos-compile.txt. Saves nothing else.
. /opt/cn/container/lib.sh
need_src
need_sysroot
need_builtins
O=$ART/$BUILD_ID; mkdir -p "$O"
REPO=$REPO W=$W SNAP=$SNAP python3 "$CN/container/apply-patches.py" 2>&1 | tee -a "$LOG"
G=$SRC/out/gncheck; rm -rf "$G"; mkdir -p "$G"
cp "$CN/args.gn" "$G/args.gn"
sed -i 's/^cc_wrapper.*/cc_wrapper = ""/' "$G/args.gn"      # no ccache needed for this check
cd "$SRC"
set +e
buildtools/linux64/gn gen "$G" > "$O/gn-gen.txt" 2>&1
rc=$?
set -e
cat "$O/gn-gen.txt" | tee -a "$LOG"
[ $rc -eq 0 ] || die "gn gen failed (see artifacts/$BUILD_ID/gn-gen.txt)"
buildtools/linux64/gn args "$G" --list --short --overrides-only > "$O/args-effective.txt" 2>&1 || true
# the exact labels of the browser targets (including the ones that build browser_shell + libcbe.so)
buildtools/linux64/gn ls "$G" 2>/dev/null | grep -E '^//chrome:chrome$|browser_shell|neva/cbe|webos/install' > "$O/browser-targets.txt" || true
ninja -C "$G" -t targets all 2>/dev/null | grep -E '^(chrome|browser_shell|libcbe\.so|browser_shell_webos)[: ]' >> "$O/browser-targets.txt" || true

# compile the webOS ozone extension objects only (they include the generated wayland-webos headers)
mapfile -t OBJS < <(ninja -C "$G" -t targets all 2>/dev/null | grep -oE '^obj/ui/ozone/platform/wayland/extensions/webos/webos/[^:]+\.o' | sort -u)
# only source_set("webos") (object dir webos/webos/): that is what chrome links. test_support/ and
# webos_extension_unittests/ are testonly targets, never in chrome, so their errors do not count
log "webOS extension objects: ${#OBJS[@]}"
# Generated headers (mojom, protocol headers) are not always declared as deps of source_set("webos"), so a
# compile of only its objects can fail with "file not found" for a header ninja knows how to make. Build those
# headers and retry (up to 4 rounds). Only errors that remain after that count.
set +e
for round in 1 2 3 4; do
	ninja -C "$G" -k 0 -j "$(nproc)" "${OBJS[@]}" > "$O/webos-compile.txt" 2>&1
	rc=$?
	[ $rc -eq 0 ] && break
	mapfile -t GEN < <(grep -oE "fatal error: '[^']+' file not found" "$O/webos-compile.txt" | sed -E "s/fatal error: '(.*)' file not found/gen\/\1/" | sort -u |
		while read -r h; do ninja -C "$G" -t query "$h" > /dev/null 2>&1 && echo "$h"; done)
	[ ${#GEN[@]} -gt 0 ] || break
	log "round $round: building ${#GEN[@]} generated headers: ${GEN[*]}"
	ninja -C "$G" -j "$(nproc)" "${GEN[@]}" >> "$LOG" 2>&1
done
set -e
{ grep -E 'error:' "$O/webos-compile.txt" || true; } | sort -u > "$O/webos-errors.txt"
head -60 "$O/webos-errors.txt" | tee -a "$LOG"
st status=done gn_targets="$(grep -oE '(Generated|Made) [0-9]+ targets' "$O/gn-gen.txt" | grep -oE '[0-9]+' || true)" \
	webos_objects="${#OBJS[@]}" webos_compile_rc=$rc webos_errors="$(wc -l < "$O/webos-errors.txt")"
push_log

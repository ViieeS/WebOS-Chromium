#!/bin/bash
# tvdata-check.sh: check the TV files on the Volume (tv/rootfs.sqfs and tv/apps.tar, each with a .sha256 file, uploaded
# by the builder) against their sha256. Only for SYSROOT_SOURCE=tv; the default public sysroot needs no TV files.
. /opt/cn/container/lib.sh
for f in rootfs.sqfs apps.tar; do
	[ -f "$TVD/$f" ] || die "missing $TVD/$f (upload it and its .sha256 to the Volume's tv/ folder)"
	want=$(cut -d' ' -f1 < "$TVD/$f.sha256")
	have=$(sha256sum "$TVD/$f" | cut -d' ' -f1)
	[ "$want" = "$have" ] || die "$f sha256 mismatch: want $want have $have"
	log "$f OK $have ($(du -h "$TVD/$f" | cut -f1))"
	st "${f%%.*}_sha256=$have"
done
unsquashfs -s "$TVD/rootfs.sqfs" | head -5 | tee -a "$LOG"
st status=done
push_log

#!/bin/sh
# Collect bounded evidence from the isolated guest; no pool mutations.
set -eu
[ "$(uname -s)" = FreeBSD ] && [ "$(hostname)" = zfs-chunkdedup-lab ]
[ "$(sysctl -n hw.ncpu)" = 1 ] && [ "$(ifconfig -l)" = lo0 ]
lab=/root/lab
report="$lab/report"
[ ! -e "$report" ] || { echo 'Report directory already exists' >&2; exit 1; }
mkdir "$report" "$report/logs" "$report/results"
for log in "$lab"/*.log; do
    [ -f "$log" ] || continue
    tail -n 100 "$log" > "$report/logs/$(basename "$log")"
done
cp "$lab/conformance.log" "$report/logs/"
for directory in "$lab/tests/work" "$lab/tests"/attempt-*; do
    [ -d "$directory" ] || continue
    destination="$report/results/$(basename "$directory")"
    mkdir "$destination"
    for file in "$directory"/*.json "$directory"/*.log; do
        [ -f "$file" ] || continue
        cp "$file" "$destination/"
    done
done
{
    date -u
    uname -a
    freebsd-version
    kldstat
    sysctl hw.ncpu hw.physmem vfs.zfs.version.module
    /opt/chunkdedup/sbin/zfs version
    ldd /opt/chunkdedup/sbin/zfs
    /opt/chunkdedup/sbin/zpool status -P || true
    /opt/chunkdedup/sbin/zpool get feature@chunkdedup || true
    /opt/chunkdedup/sbin/zfs list -p -o name,used,usedbysnapshots,refer,quota,chunkdedup || true
    df -h /
    cat /boot/loader.conf /etc/rc.conf
    sha256 /boot/modules/openzfs.ko /opt/chunkdedup/sbin/zfs
} > "$report/guest-state.txt" 2>&1
if [ -f "$lab/source-paths.txt" ]; then
    (cd "$lab/openzfs"; xargs sha256 < "$lab/source-paths.txt") > "$report/source-sha256.txt"
fi
tar -czf "$lab/report.tar.gz" -C "$lab" report
sha256 "$lab/report.tar.gz"
wc -c "$lab/report.tar.gz"

#!/bin/sh
set -eu
lab_dataset=zroot/vm/zfs-chunkdedup-lab
lab_used=$(zfs get -Hp -o value used "$lab_dataset")
host_available=$(zfs get -Hp -o value available zroot/vm)
if [ "$lab_used" -ge 51539607552 ] || [ "$host_available" -lt 137438953472 ]; then
    echo 'LAB STORAGE LIMIT REACHED: stop this guest before further test writes.' >&2
    exit 1
fi
printf 'lab_used=%s host_available=%s\n' "$lab_used" "$host_available"

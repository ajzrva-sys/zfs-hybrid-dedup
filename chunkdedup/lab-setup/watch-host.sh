#!/bin/sh
# Run only on the BSD host while this isolated guest is running.
set -eu
lab=/vm/zfs-chunkdedup-lab
trap 'exit 0' INT TERM
while :; do
    if ! sh "$lab/host-guard.sh"; then
        date -u
        printf '%s\n' 'Stopping only zfs-chunkdedup-lab: storage guard failed.'
        vm stop zfs-chunkdedup-lab
        exit 1
    fi
    sleep 30 &
    wait $!
done

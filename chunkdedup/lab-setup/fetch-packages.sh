#!/bin/sh
# Fetch into the lab only; never install packages on the host.
set -eu
lab=/vm/zfs-chunkdedup-lab
[ "$(uname -s)" = FreeBSD ] || exit 1
[ "$(uname -m)" = amd64 ] || exit 1
case $(uname -r) in 15.1-RELEASE*) ;; *) exit 1 ;; esac
[ "$(id -u)" = 0 ] || exit 1
[ "$(zfs get -H -o value mountpoint zroot/vm/zfs-chunkdedup-lab)" = "$lab" ] || exit 1
sh "$lab/host-guard.sh"
mkdir -p "$lab/build/pkgdb" "$lab/build/pkgcache" "$lab/inputs/packages"
pkg -o PKG_DBDIR="$lab/build/pkgdb" \
    -o PKG_CACHEDIR="$lab/build/pkgcache" \
    fetch -d -y -o "$lab/inputs/packages" \
    bash autoconf automake libtool gmake gettext-tools gettext-runtime \
    pkgconf pkg python3 python312
find "$lab/inputs/packages" -name '*.pkg' -type f -exec sha256 {} + \
    > "$lab/logs/package-checksums.txt"

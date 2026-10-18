#!/bin/sh
# FreeBSD base tools only: verify the ISO before installing packages.
set -eu
media=${1:-/media/lab}
[ "$(cat "$media/LAB-MEDIA")" = 'ZFS chunkdedup offline media v2' ] || exit 1
[ -s "$media/SHA256SUMS" ] || exit 1
count=0
seen=' '
while IFS= read -r line; do
    digest=${line%%  *}
    relative=${line#*  }
    [ "$relative" != "$line" ] || exit 1
    [ "${#digest}" = 64 ] || exit 1
    case $digest in *[!0-9a-f]*) exit 1 ;; esac
    case $relative in
        LAB-MEDIA|SOURCE.json|source.patch|openzfs.tar.gz|conformance.tar.gz|tests.tar.gz|guest-build.sh|verify-media.sh) ;;
        packages/*.pkg)
            name=${relative#packages/}
            case $name in ''|*[!A-Za-z0-9._+-]*|*..*|*/*) exit 1 ;; esac ;;
        *) exit 1 ;;
    esac
    case $seen in *" $relative "*) exit 1 ;; esac
    seen="$seen$relative "
    [ -f "$media/$relative" ] && [ ! -L "$media/$relative" ] || exit 1
    [ "$(sha256 -q "$media/$relative")" = "$digest" ] || {
        printf 'Media checksum failed: %s\n' "$relative" >&2
        exit 1
    }
    count=$((count + 1))
done < "$media/SHA256SUMS"
[ "$count" -ge 10 ] || exit 1
for relative in LAB-MEDIA SOURCE.json source.patch openzfs.tar.gz conformance.tar.gz tests.tar.gz guest-build.sh verify-media.sh; do
    case $seen in *" $relative "*) ;; *) exit 1 ;; esac
done
for package in "$media"/packages/*.pkg; do
    [ -f "$package" ] && [ ! -L "$package" ] || exit 1
    relative=packages/${package##*/}
    case $seen in *" $relative "*) ;; *) exit 1 ;; esac
done
printf 'Verified %s offline media files\n' "$count"

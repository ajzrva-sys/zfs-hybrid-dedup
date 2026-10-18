#!/bin/sh
# Execute only inside the freshly created, offline disposable guest.
set -eu
[ "$(uname -s)" = FreeBSD ] || exit 1
[ "$(uname -m)" = amd64 ] || exit 1
case $(uname -r) in 15.1-RELEASE*) ;; *) exit 1 ;; esac
[ "$(id -u)" = 0 ] || exit 1
[ "$(hostname)" = zfs-chunkdedup-lab ] || exit 1
[ "$(sysctl -n hw.ncpu)" = 1 ] || exit 1
[ "$(ifconfig -l)" = lo0 ] || exit 1
for device in /dev/vtbd1 /dev/vtbd2; do
    [ "$(diskinfo "$device" | awk '{print $3}')" = 8589934592 ] || exit 1
done
[ "$(mount -p | awk '$2 == "/" {print $1 ":" $3}')" = /dev/vtbd0p1:ufs ] || exit 1
pool_status=0
pool_inventory=$(zpool list -H -o name 2>&1) || pool_status=$?
case $pool_status in 0|1) ;; *) exit 1 ;; esac
case $pool_inventory in
    ''|'no pools available') ;;
    *) echo 'Export the disposable test pools before building; refusing imported pools or an inventory error.' >&2
       exit 1 ;;
esac
[ ! -e /root/lab/openzfs ] && [ ! -e /root/lab/conformance ] && [ ! -e /root/lab/tests ] || {
    echo 'Refusing existing source/test directories; preserve results before a fresh build.' >&2
    exit 1
}
media=${1:-/media/lab}
case $media in /media/lab) ;; *) exit 1 ;; esac
mkdir -p "$media" /root/lab
if ! mount -p | awk '$2 == "/media/lab" {found=1} END {exit !found}'; then
    mount -t cd9660 -o ro /dev/cd0 "$media"
fi
mount -p | awk '$2 == "/media/lab" && $3 == "cd9660" && $4 ~ /(^|,)ro(,|$)/ {found=1} END {exit !found}' || exit 1
sh "$media/verify-media.sh" "$media"
set -- "$media"/packages/pkg-[0-9]*.pkg
[ "$#" = 1 ] && [ -f "$1" ] || exit 1
# Bootstrap into this guest only. Metadata entries are not filesystem paths.
tar --no-xattrs --no-acls -xpf "$1" -C / --exclude '+*'
mkdir -p /root/lab/empty-repos
env ASSUME_ALWAYS_YES=yes /usr/local/sbin/pkg-static -o REPOS_DIR=/root/lab/empty-repos add "$media"/packages/*.pkg
for command in bash gmake autoconf automake libtoolize pkgconf python3; do
    command -v "$command" >/dev/null || exit 1
done
[ -f /usr/local/lib/libintl.so ] || exit 1
for archive in openzfs conformance tests; do
    tar --no-xattrs --no-acls -xpf "$media/$archive.tar.gz" -C /root/lab
done
python3 /root/lab/conformance/verify_bundle.py
sh /root/lab/conformance/chunker/build.sh check
cd /root/lab/openzfs
./autogen.sh
export MAKE=gmake
CPPFLAGS="-I/usr/local/include" LDFLAGS="-L/usr/local/lib" LIBS="-L/usr/local/lib -lintl" ./configure --with-config=all --prefix=/opt/chunkdedup --enable-debug --enable-debuginfo --disable-pyzfs --disable-systemd
nice -n 20 gmake -j1
nice -n 20 gmake -j1 install
[ -s module/openzfs.ko ] || exit 1
mkdir -p /boot/modules
install -m 555 module/openzfs.ko /boot/modules/openzfs.ko
[ ! -e /root/lab/loader.conf.before ] || exit 1
cp -p /boot/loader.conf /root/lab/loader.conf.before
cat >> /boot/loader.conf <<'LOADER'

# BEGIN disposable chunkdedup lab module
module_path="/boot/modules;/boot/kernel"
zfs_load="NO"
openzfs_load="YES"
# END disposable chunkdedup lab module
LOADER
sha256 module/openzfs.ko /boot/modules/openzfs.ko
printf '%s\n' 'BUILD-COMPLETE: reboot only the lab guest to load openzfs.ko; then run the guarded guest tests.'

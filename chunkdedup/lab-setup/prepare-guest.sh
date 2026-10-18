#!/bin/sh
set -eu
lab=/vm/zfs-chunkdedup-lab
[ "$(uname -s)" = FreeBSD ] || exit 1
[ "$(id -u)" = 0 ] || exit 1
[ -f "$lab/LAB-STORAGE" ] || exit 1
sh "$lab/host-guard.sh"
release=https://download.freebsd.org/releases/amd64/amd64/15.1-RELEASE
[ "$(zfs get -H -o value mountpoint zroot/vm/zfs-chunkdedup-lab)" = "$lab" ] || exit 1
[ ! -e "$lab/build/root" ] || { echo 'Guest staging already exists' >&2; exit 1; }
cd "$lab/inputs"
curl --limit-rate 4m -fsSLo MANIFEST "$release/MANIFEST"
for dist in base.txz kernel.txz src.txz; do
    sh "$lab/host-guard.sh"
    curl --limit-rate 4m -fsSLo "$dist" "$release/$dist"
    expected=$(awk -v dist="$dist" '$1 == dist {print $2}' MANIFEST)
    [ -n "$expected" ] && [ "$(sha256 -q "$dist")" = "$expected" ] || exit 1
done
sh "$lab/host-guard.sh"
mkdir -p "$lab/build/root"
tar --no-xattrs --no-acls -xpf base.txz -C "$lab/build/root"
tar --no-xattrs --no-acls -xpf kernel.txz -C "$lab/build/root"
tar --no-xattrs --no-acls -xpf src.txz -C "$lab/build/root"
cat > "$lab/build/root/etc/rc.conf" <<'RC'
hostname="zfs-chunkdedup-lab"
zfs_enable="YES"
sshd_enable="NO"
sendmail_enable="NONE"
cron_enable="NO"
syslogd_enable="YES"
growfs_enable="YES"
RC
cat > "$lab/build/root/etc/fstab" <<'FSTAB'
/dev/vtbd0p1 / ufs rw 1 1
FSTAB
cat > "$lab/build/root/boot/loader.conf" <<'LOADER'
console="comconsole"
comconsole_speed="115200"
boot_serial="YES"
vfs.root.mountfrom="ufs:/dev/vtbd0p1"
LOADER
cat >> "$lab/build/root/etc/gettytab" <<'GETTY'

labautologin|lab isolated serial root console:\
    :al=root:tc=Pc:
GETTY
sed -i '' 's@^ttyu0.*@ttyu0 "/usr/libexec/getty labautologin" vt100 on secure@' "$lab/build/root/etc/ttys"
cat > "$lab/build/root/root/.profile" <<'PROFILE'
export TERM=vt100
export PS1='LAB# '
PROFILE
sh "$lab/host-guard.sh"
makefs -Z -t ffs -s 8g -o version=2 -B little "$lab/build/root.ufs" "$lab/build/root"
disk=/dev/zvol/zroot/vm/zfs-chunkdedup-lab/boot
[ "$disk" = /dev/zvol/zroot/vm/zfs-chunkdedup-lab/boot ] || exit 1
[ "$(zfs get -Hp -o value volsize zroot/vm/zfs-chunkdedup-lab/boot)" = 42949672960 ] || exit 1
if gpart show "$disk" >/dev/null 2>&1; then
    echo "Refusing a boot zvol with an existing partition table." >&2
    exit 1
fi
trap 'zfs set volmode=dev zroot/vm/zfs-chunkdedup-lab/boot' EXIT
trap 'exit 1' INT TERM HUP
sh "$lab/host-guard.sh"
zfs set volmode=geom zroot/vm/zfs-chunkdedup-lab/boot
gpart create -s gpt "$disk"
gpart add -t freebsd-ufs -a 1m -l chunkdedup-root "$disk"
dd if="$lab/build/root.ufs" of="${disk}p1" bs=1m speed=33554432 status=progress
sync
zfs set volmode=dev zroot/vm/zfs-chunkdedup-lab/boot
sh "$lab/host-guard.sh"
mkdir -p "$lab/inputs/payload"
printf '%s\n' 'ZFS chunkdedup lab offline payload; version 1' > "$lab/inputs/payload/LAB-MEDIA"
makefs -t cd9660 -o rockridge -o label=CHUNKDEDUP "$lab/inputs/payload.iso" "$lab/inputs/payload"
zfs get -Hp -o name,property,value used,available,volmode zroot/vm/zfs-chunkdedup-lab zroot/vm/zfs-chunkdedup-lab/boot

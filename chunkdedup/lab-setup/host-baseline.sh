#!/bin/sh
set -eu
uname -sr
zfs version
vm list
vm switch list
kldstat
zpool get -Hp all
zfs get -Hp all zroot/vm
for file in /etc/rc.conf /etc/rc.conf.local /etc/crontab /vm/.config/system.conf; do
    if [ -f "$file" ]; then sha256 "$file"; fi
done
service -e
service -e | while IFS= read -r rc_script; do
    printf 'service=%s\n' "${rc_script##*/}"
    timeout 10 service "${rc_script##*/}" onestatus || true
done
find /vm -maxdepth 2 -name '*.conf' ! -path '/vm/zfs-chunkdedup-lab/*' -exec sha256 {} \;
ps -axo pid,lstart,comm | awk '/transmission|rsync|bhyve/ {print}'
crontab -l 2>/dev/null | sha256

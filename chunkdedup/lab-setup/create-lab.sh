#!/bin/sh
# Only creates the named lab subtree. Never modifies an existing dataset.
set -eu
lab_dataset=zroot/vm/zfs-chunkdedup-lab
lab_path=/vm/zfs-chunkdedup-lab
[ "$(uname -s)" = FreeBSD ] || exit 1
[ "$(id -u)" = 0 ] || exit 1
autostart=$(sysrc -n vm_list 2>/dev/null || true)
case " $autostart " in
    *" ALL "*|*" zfs-chunkdedup-lab "*)
        echo "Refusing host autostart list that would include the lab." >&2
        exit 1 ;;
esac
[ "$(zfs get -Hp -o value available zroot/vm)" -ge 137438953472 ] || exit 1
if zfs list -H "$lab_dataset" >/dev/null 2>&1 || [ -e "$lab_path" ]; then
    echo 'Refusing existing lab path; inspect it before any reuse.' >&2
    exit 1
fi
zfs create -o mountpoint="$lab_path" -o quota=64G -o reservation=none \
    -o compression=off -o dedup=off -o atime=off "$lab_dataset"
for lab_disk in boot:40G test-a:8G test-b:8G; do
    lab_disk_name=${lab_disk%:*}
    lab_disk_size=${lab_disk#*:}
    zfs create -sV "$lab_disk_size" -o volblocksize=16K -o volmode=dev \
        -o refreservation=none -o dedup=off "$lab_dataset/$lab_disk_name"
done
mkdir -p "$lab_path/inputs" "$lab_path/logs" "$lab_path/build"
chmod 700 "$lab_path"
printf '%s\n' 'zfs-chunkdedup-lab dedicated disposable storage v1' > "$lab_path/LAB-STORAGE"
cat > "$lab_path/zfs-chunkdedup-lab.conf" <<'GUEST'
loader="bhyveload"
cpu=1
memory=4G
disk0_type="virtio-blk"
disk0_dev="custom"
disk0_name="/dev/zvol/zroot/vm/zfs-chunkdedup-lab/boot"
disk1_type="virtio-blk"
disk1_dev="custom"
disk1_name="/dev/zvol/zroot/vm/zfs-chunkdedup-lab/test-a"
disk2_type="virtio-blk"
disk2_dev="custom"
disk2_name="/dev/zvol/zroot/vm/zfs-chunkdedup-lab/test-b"
disk3_type="ahci-cd"
disk3_dev="custom"
disk3_name="/vm/zfs-chunkdedup-lab/inputs/payload.iso"
GUEST
if grep -q '^network' "$lab_path/zfs-chunkdedup-lab.conf"; then exit 1; fi
zfs get -Hp -o name,property,value quota,reservation,refreservation,dedup,volmode \
    "$lab_dataset" "$lab_dataset/boot" "$lab_dataset/test-a" "$lab_dataset/test-b"

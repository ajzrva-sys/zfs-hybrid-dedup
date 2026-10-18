# Guarded offline FreeBSD lab

Native execution is FreeBSD-first and uses a disposable `zfs-chunkdedup-lab`
guest, not the host kernel. The reference runner deliberately accepts only an
SSH host alias named `bsd`; that alias must point to your own reviewed lab host.
Review all paths and prerequisites before running host setup.

## Storage and isolation

The supplied `create-lab.sh` creates only a new dedicated parent under
`zroot/vm`, with a 64 GiB total quota and sparse 40/8/8 GiB zvols. It refuses
existing lab storage or host autostart settings that include the guest.
The guest has one CPU, 4 GiB RAM, VirtIO disks, no NIC and no shared directories.
Inputs arrive through a read-only ISO. Existing services, VMs, networking and
host ZFS modules must remain unchanged.

`host-guard.sh` stops lab writes at 48 GiB parent usage or below 128 GiB host
free space. Builds use one thread. Exhaustion tests use guest dataset quotas.
The normal native runner inventories all existing host configuration, modules,
VMs and service identities, then restores and compares them after shutdown.
Captured host inventories are private operational evidence; do not commit them.

## Initial setup

The scripts assume FreeBSD 15.1-RELEASE amd64, existing vm-bhyve facilities,
root access and `zroot/vm` mounted at `/vm`. Host setup is explicit; local
verification never creates storage or starts a VM.

On the lab host, inspect and then run `host-baseline.sh` (saving its output
privately), `create-lab.sh`, `prepare-guest.sh` and `fetch-packages.sh` from this
directory. Place the guard and guest setup scripts under the dedicated lab
path as required by their script headers. Package fetching uses isolated cache
and database directories; it does not install host packages. Guest preparation
uses only the dedicated boot zvol and verifies release archive checksums.

For initial offline media, the immutable `build-media.py` requires Git HEAD at
the pinned upstream commit with the feature patch applied as local changes.
Create that separate source checkout without changing this branch:

```sh
git worktree add --detach ../hybrid-media-source 71a9f9578616a90c3c14bb59629fb4d31bfd68d1
git -C ../hybrid-media-source apply "$PWD/chunkdedup/source.patch"
git -C ../hybrid-media-source add -N cmd/zfs/CHUNKDEDUP-MIT-LICENSE   cmd/zfs/chunkdedup_chunker.c cmd/zfs/chunkdedup_chunker.h   cmd/zfs/rabin64_table.h cmd/zfs/zfs_chunkdedup_user.c   cmd/zfs/chunkdedup_hybrid.c cmd/zfs/chunkdedup_hybrid.h   include/sys/zfs_chunkdedup.h module/zfs/zfs_chunkdedup.c
python3 chunkdedup/lab-setup/build-media.py --help
```

Supply that checkout, `chunkdedup/conformance`, `chunkdedup/lab-setup/tests` and
your fetched guest package cache to the media builder. Follow
`verify-media.sh` and the guest build/report scripts for initial offline
provisioning. They are kept as the original baseline inputs; the current
verification runner subsequently freezes the current branch itself.

## Native verification

Once the isolated lab is provisioned and stopped, run from the repository root:

```sh
./scripts/verify.sh --profile native --host bsd --output ../hybrid-native-verification
```

The native runner checks storage guards, attaches its own read-only ISO,
builds and installs only inside the guest, runs the preserved 21-case suite
and separate 12-case hybrid suite, captures evidence, restores the original
guest module and retained pools, restores the VM configuration, stops the
lab and compares host state. It never installs a host module.

The latest recorded run remains failed at the final hybrid harness mount.
The source publication does not repair that test or restart a repair round.
Crash and matched performance/RAM validation remain separate, incomplete work.

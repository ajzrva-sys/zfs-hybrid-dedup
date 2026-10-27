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
git -C ../hybrid-media-source add -N cmd include lib module
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

For a previously compiled, exactly matching source/module/tools set, the runner
also supports `--profile native-smoke --cached-build-receipt PATH`. The receipt
and actual cached binaries are checked before testing; reuse is recorded
explicitly and does not count as a fresh compilation.

Earlier revisions passed all 21 original and 12 hybrid cases with their exact
build/reuse and restoration scope recorded in the validation history. Later
optimizer changes have focused strict/sanitizer and native functional checks;
the new dirty-data gauge has separate direct-function tests, a kernel-module
build and ten checks in an isolated offline VM. The complete native suite has
not been rerun for the combined revision. See [current validation scope](../STATUS.md)
and [gauge validation](../DIRTY_DATA.md) for source identities and proof limits.

The gauge VM used a private UFS root image with the matching FreeBSD kernel,
one CPU, 4 GiB RAM, and no network adapter, shared filesystem or physical-disk
attachment. It verified current backlog and drain, pool isolation, read-only
field types, escaped/reserved pool names and concurrent-reader teardown before
clean shutdown. That run did not install the candidate on the running storage
host or change its boot configuration. Host activation and reboot remain held.

The one-CPU lab recipe isolates functional testing; it does not measure
multicore scaling. The hybrid optimizer itself defaults to online CPUs, capped
at 64, and accepts `--threads=1..64`. The separate block-only command is
currently serial. Matched performance/RAM and forced-power-loss qualification
remain incomplete; the 1.25x foreground-read target is unqualified.

## Deterministic crash qualification

The local gate includes strict and ASan/UBSan process-crash tests at payload
sync, manifest sync, atomic rename, directory sync, sort pipe-write and sort
merge boundaries. Each test stops a fresh child session at a named marker,
kills only that session and reopens the checkpoint in a separate process.
Previously committed prefixes remain authoritative; uncommitted tails replay.
These passing portable tests do not simulate power loss.

The disposable native runner also supports a fresh test-only build:

```sh
./scripts/verify.sh --profile native --host bsd --crash-recovery --output ../hybrid-crash-verification
```

Local verification never executes this option. It retains the existing offline
guest, storage, host-state and restoration guards. Eight additional native cases
cover the publication/sort points and before/after-share interruption, with
content, metadata, report, checkpoint and copy-on-write validation. Native crash
execution remains **NOT_RUN**; availability of the runner is not a native PASS.

Fault injection is enabled only by force-including `recovery_fault.h` in this
private test build. Normal `CD_RECOVERY_POINT` calls are no-ops with no
environment-controlled fault behavior. Fault builds must remain in the
disposable guest and cannot be reused through normal cached-build receipts.
Never modify or restart a production worker to adopt these test hooks.

Abrupt guest power-off/reboot qualification requires a separately reviewed
disposable lab and evidence for checkpoint recovery, source/metadata identity,
contents and copy-on-write behavior after boot. No automated host power-off is
added. Process-kill results, clean shutdowns and native loader authentication
do not qualify abrupt power loss; those cases remain **NOT_RUN**.

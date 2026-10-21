# Current dirty-data accounting

The FreeBSD kernel exposes three read-only per-pool `UINT64` fields:

| Field | Meaning |
| --- | --- |
| `kstat.zfs.<pool>.dirty_data.stats.pool_guid` | Pool identity; consumers must compare it with current inventory. |
| `kstat.zfs.<pool>.dirty_data.stats.dirty_bytes` | Current `dp_dirty_total`, in bytes. |
| `kstat.zfs.<pool>.dirty_data.stats.dirty_limit_bytes` | Current `zfs_dirty_data_max`, in bytes. |

`dirty_bytes` is ZFS dirty-space accounting across transaction groups, including
metadata. It is not physical or compressed device bytes waiting to write, ZIL
backlog, or historical completed-TXG `ndirty`. The global limit applies to each
pool independently; it is not a shared host-wide budget. Zero is a valid observed
value. A missing gauge is unavailable and must not be treated as zero. These are
point-in-time gauges, not counters or interval averages.

The interface uses FreeBSD's existing sysctl component escaping: literal dots in
a pool name appear as `%25` in the printed OID. Pool names are kept under their
own namespace, and the kstat class is constant, including for pools named
`dataset` or `dirty_data`. A module name that cannot fit `KSTAT_STRLEN` omits this
optional gauge instead of publishing a truncated identity.

The update callback copies dirty accounting under `dp_lock`. The kstat is owned
by the DSL pool, installed after initialization and removed at the beginning of
close. Existing FreeBSD sysctl teardown unregisters the OIDs and drains active
handlers before the DSL pool is freed. The close path does not hold `dp_lock`
while waiting for readers. No write-throttle accounting, tuning, on-disk format,
or storage policy changes are involved. Linux and userspace builds do not add
this interface.

## Validation

Run the portable direct-function tests from the repository root:

```sh
python3 chunkdedup/lab-setup/tests/test_dirty_data.py --repo .
```

The runner extracts the three actual functions from `module/zfs/dsl_pool.c` and
executes eight checks with userspace kstat and mutex stubs, both normally and
with address/undefined-behavior sanitizers:

1. Native field names and `UINT64` types.
2. Idle zero and full-width pool GUID.
3. Large current gauge without clamping to the limit.
4. Write rejection and preservation of dirty accounting.
5. Locked callback and simulated reader drain without lock inversion.
6. Allocation failure and absent/idempotent cleanup.
7. Reserved pool names and separate per-pool namespaces.
8. Name-length boundaries without truncation.

The exact source was also compiled as a FreeBSD kernel module with `-Werror`
and tested in an isolated VM using the matching FreeBSD 15.1-RELEASE-p4 kernel.
The guest had one CPU, 4 GiB RAM, a private UFS root image, no network adapter,
shared filesystem, or physical-disk attachment. Ten native checks passed:

1. Exact candidate identity, matching kernel and sole loaded ZFS module.
2. Main-pool GUID, limit, idle zero and write rejection.
3. The same checks for a pool named `dataset`.
4. The same checks for a pool named `dirty_data`.
5. The same checks for a pool name containing a literal dot.
6. All exported fields reported native `uint64_t` types.
7. Current backlog during writes, isolation from an idle second pool, and drain
   to zero afterward.
8. Five create/export/import/destroy cycles under three concurrent native
   sysctl pollers; each observed both present and removed OIDs without
   unexpected errors.
9. All disposable pools destroyed and their gauge nodes removed.
10. No guest panic, trap, or lock-order warning; clean guest shutdown.

The write test observed a 15,532,544-byte peak and 4,430 simultaneous checks in
which the main pool was dirty and the second pool remained at zero. These
numbers demonstrate the metric's behavior in this bounded test, not production
performance. Kernel module candidate SHA-256:
`7a836ada25ec533ae5af4f72f35e46b0ac7f0b0eb59c44f6cbb6cd5e56708401`.

The VM shut down and its runtime handle was removed after testing. The original base image and host
kernel modules, boot configuration, mounts, VM configurations, and pool identities
remained unchanged. Raw operational receipts and the module binary are retained
privately and are not publication artifacts. The candidate has **not been
installed on the running storage host**. Installation and reboot remain a
separate approval boundary; existing module-pinned observers must be revalidated
if the selected module name changes.

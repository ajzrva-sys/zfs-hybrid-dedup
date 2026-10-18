# ZFS hybrid background deduplication

An experimental FreeBSD-first OpenZFS 2.4.4 fork that writes files normally,
then deduplicates them in the background using native block sharing or
content-defined chunking (CDC).

Native block sharing is preferred. Automatic mode selects CDC only when its
conservative estimated live allocation is at least 20% lower. One method is
selected per file; existing chunked files retain their format.

## Current status

Fresh isolated VM and dedicated bare-metal tests passed **21/21 existing
regression cases and 12/12 hybrid cases**, including snapshot/clone and full/
incremental replication assertions. The replication mount-state harness error
and a strict media-inventory mismatch have been corrected; native code is unchanged.

The latest full native gate remains **INFRA_FAILURE** after a host-cleanup
SSH timeout. Separate recovery confirmed the guest stopped and restored its
exact VM configuration. A running unrelated service changed process identity,
so the strict host-state comparison is not a pass. The original failed
evaluation remains preserved.

Current-revision matched physical-allocation, pure-read and RAM comparisons,
and planned forced-power-loss validation, have **not run**. The foreground-read
target remains 1.25x pristine latency. This is a development prototype; run
experimental modules only inside a disposable guest.

See [validation status](chunkdedup/STATUS.md), [saved validation summary](chunkdedup/VALIDATION.json)
and [command/design documentation](chunkdedup/HYBRID.md).

## What is implemented

- `zfs chunkdedup optimize` and `watch` accept `--method=auto|block|chunk`;
  automatic selection is the default.
- Native sharing compares current bytes, validates identities and excludes
  competing opens/mappings, then shares ordinary blocks through BRT accounting.
- CDC preserves the Microsoft-derived regression chunker, chunk identifiers,
  immutable chunk objects and existing on-disk format.
- The worker analyzes the traversal before conversion, with temporary disk
  catalogs, bounded memory, allocation estimates and conservative fallbacks.
- `status --methods` reports per-file representation and labeled pool-wide BRT
  statistics. Plain six-field status remains compatible.

## Verify locally

Requirements: Python 3.9+, Git, Clang with working ASan/UBSan, and OpenSSL
headers/libraries (for example `libssl-dev` on Debian/Ubuntu).

```sh
git clone https://github.com/ajzrva-sys/zfs-hybrid-dedup.git
cd zfs-hybrid-dedup
./scripts/verify.sh --output ../hybrid-verification
```

This checks source integrity, pinned Microsoft fixtures, the actual chunker,
the native core model, worker batching, selection policy and packaging.
Local checks do not load a kernel module or prove native filesystem behavior.
Generated evidence belongs outside this checkout. For native reproduction,
read the [guarded offline lab recipe](chunkdedup/lab-setup/README.md).

## Source and provenance

The fork is based on upstream [`zfs-2.4.4`](https://github.com/openzfs/zfs/commit/71a9f9578616a90c3c14bb59629fb4d31bfd68d1).
The first feature commit exactly matches the 4,134-file tested source snapshot;
its digest and file manifest are in [SOURCE.json](chunkdedup/SOURCE.json) and
[tested-source-files.json](chunkdedup/tested-source-files.json). Later publication
changes add documentation, tests and a portable verification wrapper. They do
not change the native implementation.

The complete [source patch](chunkdedup/source.patch) applies to the pinned
upstream base. The [original upstream README](chunkdedup/upstream/README.md)
is preserved. Microsoft BuildXL chunker provenance is pinned to commit
`852a00e5b45ef8a8d06a2eb9040495c36243078f` in the conformance bundle.

## Licensing

Preserve OpenZFS's [LICENSE](LICENSE), [COPYRIGHT](COPYRIGHT), [NOTICE](NOTICE)
and third-party notices. The Microsoft-derived chunker/tables include their
[MIT notice](cmd/zfs/CHUNKDEDUP-MIT-LICENSE) and original attribution. Licensing
is described per file/component; the entire fork is not relicensed under MIT.

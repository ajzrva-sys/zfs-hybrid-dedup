# ZFS hybrid background deduplication

An experimental FreeBSD-first OpenZFS 2.4.4 fork that writes files normally,
then deduplicates them in the background using native block sharing or
content-defined chunking (CDC).

Native block sharing is preferred. Automatic mode selects CDC only when its
conservative estimated live allocation is at least 20% lower. One method is
selected per file; existing chunked files retain their format.

## Current status

Scanning and hashing now use multiple cores: `optimize` and `watch` default to
the online CPU count, capped at 64, with `--threads=1..64` available. Clean-file
inspection avoids a forced durability wait; dirty files share waits in batches
of up to 256. Selection and installation remain sequential.

The latest userspace revision passed local checks and focused strict/sanitizer
tests. Its kernel predecessor passed **21 original and 12 hybrid native cases**
through `native-smoke`, reusing an exactly verified build with guest and host
restoration. The full native suite was not rerun after the userspace-only
parallel scan change. A live 40-worker run confirmed multicore execution;
it does not establish a matched speedup.

Matched allocation, pure-read and RAM comparisons and forced-power-loss
qualification remain incomplete. The foreground-read target remains 1.25x
pristine latency and is unqualified. This is a development prototype; reproduce
native tests in a disposable guest.

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
  catalogs, bounded parallel admission, allocation estimates and conservative
  fallbacks.
- `status --methods` reports per-file representation and labeled pool-wide BRT
  statistics. Plain six-field status remains compatible.
- `zfs blockdedup` provides the separate block-only optimizer with a matching
  patched module. It uses native block sharing without enabling the CDC pool
  feature. That command is currently serial and has no `--threads` option;
  use `zfs chunkdedup optimize --method=block` for parallel scanning.

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
The latest tested development snapshot contains 4,138 source files. Its digest
and file manifest are in [SOURCE.json](chunkdedup/SOURCE.json) and
[tested-source-files.json](chunkdedup/tested-source-files.json). Publication
documentation and the portable verification wrapper use this repository's
layout; validation scope is recorded separately for each source revision.

The complete [source patch](chunkdedup/source.patch) applies to the pinned
upstream base. The [original upstream README](chunkdedup/upstream/README.md)
is preserved. Microsoft BuildXL chunker provenance is pinned to commit
`852a00e5b45ef8a8d06a2eb9040495c36243078f` in the conformance bundle.

## Licensing

Preserve OpenZFS's [LICENSE](LICENSE), [COPYRIGHT](COPYRIGHT), [NOTICE](NOTICE)
and third-party notices. The Microsoft-derived chunker/tables include their
[MIT notice](cmd/zfs/CHUNKDEDUP-MIT-LICENSE) and original attribution. Licensing
is described per file/component; the entire fork is not relicensed under MIT.

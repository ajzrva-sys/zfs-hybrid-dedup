# Hybrid background deduplication

The hybrid optimizer combines native block sharing with the Microsoft-derived
content-defined chunk format. [Validation status](STATUS.md) distinguishes the
latest parallel CLI checks from its kernel-tested predecessor.

## Commands and policy

`zfs chunkdedup optimize` and `watch` default to `--method=auto`. Use
`--method=block` to request native block sharing or `--method=chunk` for the
existing chunk conversion. An explicit method does not fall back to another.

The worker analyzes the complete requested traversal before converting files.
It uses private temporary on-disk catalogs, a disk-backed grouping/index, and a
4 MiB external-sort buffer rather than retaining a dataset-sized RAM index.
Catalogs are rebuilt each pass. Source reads preserve access time.

Scanning and hashing use the online CPU count by default, capped at 64 workers.
Set `--threads=N` to select 1 through 64 workers:

```sh
zfs chunkdedup optimize --method=block --threads=16 -r tank/data
```

At most twice the worker count is admitted at once. Workers own their file
descriptors, chunkers and private disk-backed output. The coordinator merges
results in traversal order, assigns global identities and handles hardlinks.
Cancellation stops admission and joins workers before releasing their resources
or applying a conversion. Selection and installation remain sequential.

Inspection of durable files avoids a forced sync per file. Dirty candidates
are queued in same-dataset batches of up to 256, sharing one pool transaction-
group durability wait. Each is retried once against its saved identity;
still-dirty, changed or busy files defer. Catalogs remain temporary, including
in forced block mode; no persistent CDC index is retained.

Files connected by matching chunks or native blocks are costed together. The
native estimate charges each distinct native block once. The chunk estimate
charges the first new stored chunk, object/index/reference metadata and file
recipes, while recognizing existing stored chunks. Candidates that remain
native are not treated as already-stored chunk objects. Recipe counts use the
file's actual native block size, including partial final records.

Automatic mode selects chunking when its estimated live allocation is at most
80% of the native estimate. Otherwise it prefers native block sharing. File
size is not a hard cutoff. Estimates are conservative admission decisions,
not a guarantee of an observed percentage reduction. Unsupported geometry,
existing external sharing and inconsistent allocation accounting favor native
storage. In particular, RAIDZ accounting/deflation can trigger that fallback.

Existing chunked files retain their representation and can resume interrupted
conversion. To change them to native sharing, run `unoptimize`, then optimize
again. Native-shared ordinary files can be selected for later chunk conversion;
other files and snapshots retain their native references.

Eligibility remains unchanged: the dataset property must be on, files default
to three days without modification, and competing opens/mappings defer work.
The watcher defaults to hourly passes. Ordinary foreground writes continue to
use native copy-on-write for block-shared files and existing record hydration
for chunked files. Dataset disablement stops new installations.

## Native sharing

The new same-dataset administrative operation verifies both file identities,
actual block geometry, current bytes and eligibility under vnode/range locks.
It settles dirty writes outside installation locks and revalidates before
sharing. Requests cover at most one native block, bounded by OpenZFS's 16 MiB
maximum. Nonoverlapping blocks within one file are supported; short tails must
reach EOF in both files. Holes and embedded blocks are retained as native data.

New sharing uses existing BRT/DMU transactions without changing file size,
timestamps, permissions or other attributes. Already-shared physical blocks
are no-ops. The operation adds no copy-style ZIL record: a crash can discard
optimization progress, while ordinary transaction atomicity protects content.
This follows the metadata-neutral approach in upstream OpenZFS commit
`afb4010` (Linux FIDEDUPERANGE), with byte comparison before every new share.

Block-shared files keep ordinary block-pointer reads. Chunked files retain
their existing recipe reads and buffered direct-I/O fallback. Block cloning
of a CDC-active file is still rejected. One optimization method is selected
per file; interrupted CDC conversion can contain the existing mix of ordinary
and converted records while it resumes.

The current unencrypted, regular-file, same-dataset restrictions remain.
Native inline deduplication must remain off. CDC retains its existing 128 KiB
record and 65,536-entry metadata limits. Block sharing requires the existing
`block_cloning` pool feature; the CDC pool/stream format is unchanged.

The separate `zfs blockdedup` command exposes the block-only port. It requires
the matching patched module and the `block_cloning` feature, but does not
require enabling the CDC pool feature. That command is currently serial and
does not accept `--threads`; the hybrid command's `--method=block` mode provides
parallel analysis while retaining hybrid admission rules.

## Reporting and reproducibility

Plain `status` preserves its six CDC fields. `status --methods` adds per-file
representation and clearly labels BRT usage/savings as pool-wide figures.
Selection output includes the chosen method, group estimates and reason.
Snapshot-retained allocation is reported separately from live estimates.
`unoptimize` restores CDC records to ordinary storage; it does not promise
exclusive physical ownership of native blocks.

The required profiles remain:

```sh
./scripts/verify.sh
./scripts/verify.sh --profile native --host bsd
```

The native runner also accepts `--profile native-smoke` with an explicit
`--cached-build-receipt` from a prior successful build. It validates the frozen
source, cached module/tools and their hashes before rerunning the native tests.
Its result records build reuse separately from fresh compilation.

Focused queue and parallel checks compile current production C functions with
strict warnings and AddressSanitizer/UndefinedBehaviorSanitizer:

```sh
sh chunkdedup/lab-setup/tests/test_sync_batches.sh "$PWD"
sh chunkdedup/lab-setup/tests/test_parallel_scan.sh "$PWD"
```

The parallel check covers concurrent execution, bounded admission, ordered
results, callback errors, slot reuse and joining active work before caller
cleanup. Extracted CLI coordinator/catalog checks cover global IDs, dirty
retry handoff, hardlinks, skipped/stale results and cancellation.

The original 21-case CDC suite and fixture hashes are unchanged. Its temporary
adapter explicitly selects chunk mode; the separate hybrid suite exercises the
real automatic default and both forced modes.

Matched allocation/read/RAM comparisons and forced sharing power-loss
validation remain pending. They require a passing full native attempt.
A matched comparison must include pristine OpenZFS, forced native sharing,
forced chunking and automatic selection on aligned, shifted and unique data.
Pure-read timing must exclude hash verification and retain every individual
1.25x target outcome, with allocation, ARC and worker memory reported
separately. Cache conditions and any host/device-cache limitations must be
recorded. Crash validation must check content, metadata, surviving references
and scrub health after recovery, while documenting the interruption point.
The earlier development runner used a cached lab-specific pristine build and
is not included in this portable publication.

Native verification runs in the guarded offline guest. The
runner temporarily attaches its input ISO as a read-only optical device and restores
the original VM configuration and guest module before shutdown. Host storage,
service-preservation and lab-usage guards remain in force.

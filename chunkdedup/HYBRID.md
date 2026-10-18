# Hybrid background deduplication

The hybrid revision adds native block sharing alongside the existing
Microsoft-derived content-defined chunk format. Verification results for this
revision belong in its dated hybrid report; historical prototype reports do
not establish that the new native operation passed.

## Commands and policy

`zfs chunkdedup optimize` and `watch` default to `--method=auto`. Use
`--method=block` to request native block sharing or `--method=chunk` for the
existing chunk conversion. An explicit method does not fall back to another.

The worker analyzes the complete requested traversal before converting files.
It uses private temporary on-disk catalogs, a disk-backed grouping/index, and a
4 MiB external-sort buffer rather than retaining a dataset-sized RAM index.
Catalogs are rebuilt each pass. Source reads preserve access time.

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

All native execution is confined to the existing guarded offline guest. The
runner temporarily attaches its input ISO as a read-only optical device and restores
the original VM configuration and guest module before shutdown. Host storage,
service-preservation and lab-usage guards remain in force.

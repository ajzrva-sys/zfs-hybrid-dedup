# Validation status — 21 October 2026

The current publication includes parallel native sharing, bounded catalog
selection and saved reports, parallel catalog sorting, and the FreeBSD
read-only per-pool dirty-data gauge. The OpenZFS source projection contains
4,138 files; [SOURCE.json](SOURCE.json) and
[tested-source-files.json](tested-source-files.json) identify it separately
from the complete repository snapshots used by local verification.

Local verification covers the actual production chunker and core model,
141 immutable baseline files, 35 Microsoft conformance fixtures, worker
batching, bounded parallel execution, catalog selection/reporting, sorting and
packaging. Strict and ASan/UBSan tests cover concurrency, admission limits,
catalog identity, dirty retries, hardlinks, stale/skipped files and joined
cancellation cleanup. Current run outcomes and their source identities are
recorded in [VALIDATION.json](VALIDATION.json).

## Optimizer changes and focused native checks

Native sharing now uses bounded workers while preserving current-file
identity/content checks and copy-on-write semantics. Catalog selection uses a
group index capped at 256 MiB, retaining the disk-backed path when necessary.
The optimize-only `--report` option publishes complete sharing-group estimates
before sharing, refuses an existing output, and cleans up partial output on
analysis failure. See [the report format](CATALOG_REPORT.md).

Catalog sorting distributes whole records to up to 64 workers, then performs
an ordered serial merge with the original keys and C locale. Per-worker wrapper
buffers are bounded to 64 KiB; each sort process retains the existing 4 MiB
sort-buffer setting, which is not a hard RSS cap. Temporary runs and spills are
removed on success, failure or cancellation.

Focused native optimizer runs at one and 40 threads used 196 fixture files and
observed 637,534,208 bytes of savings while preserving file hashes, metadata and
copy-on-write isolation. These are bounded functional checks against the
existing matching kernel, not the complete native suite. A synthetic 1 GiB
catalog-sort comparison observed 29.809 seconds serial and 16.573 seconds with
40 sort children, or about 1.80x faster. This measures that synthetic sorting
stage, not production optimizer throughput.

## Current dirty-data gauge

The new gauge reports current ZFS dirty-space accounting, its per-pool limit
and pool GUID. Eight direct-function checks passed in strict and ASan/UBSan
builds. The exact candidate compiled as a FreeBSD kernel module with `-Werror`
and passed ten native checks in an isolated offline VM using the matching
kernel. Checks covered pool identity, idle zero, read-only fields, active
backlog and drain, pool isolation, reserved/dotted names, concurrent readers
during pool teardown, and clean guest shutdown.

The gauge candidate remains uninstalled on the running storage host;
activation and reboot are held. This is separate gauge-specific runtime proof,
not a full native regression pass for the combined source. Interface semantics,
candidate identity and reproduction details are in [DIRTY_DATA.md](DIRTY_DATA.md).

## Remaining scope

Earlier revisions passed the preserved 21-original and 12-hybrid native suites,
including snapshot/clone independence, logical replication and reference
preservation. Their exact source identities, build reuse and retained failures
remain in [VALIDATION.json](VALIDATION.json). Those earlier results do not
qualify the combined revision. No current full-suite pass is claimed.

The separate `zfs blockdedup` command remains serial; parallel `--threads`
support belongs to `zfs chunkdedup`. Block-only operation needs the matching
patched module and native `block_cloning` feature without enabling CDC.

Current-revision matched allocation, pure-read and RAM comparisons and forced
sharing power-loss qualification remain **NOT_RUN**. The **1.25x** foreground-read
target is unqualified. Functional checks and synthetic sort timing do not
establish that target or production readiness.

This public summary excludes host inventories, private paths, device identities
and raw operational logs. Reproduce native behavior only in a guarded
disposable guest.

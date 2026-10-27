# Validation status — 26 October 2026

The current publication includes parallel native sharing, bounded catalog
selection and saved reports, parallel catalog sorting, durable sharing checkpoints,
and the FreeBSD read-only per-pool dirty-data gauge. [SOURCE.json](SOURCE.json) and
[tested-source-files.json](tested-source-files.json) identify it separately
from the complete repository snapshots used by local verification.

Local verification covers the actual production chunker and core model,
141 immutable baseline files, 35 Microsoft conformance fixtures, worker
batching, bounded parallel execution, catalog selection/reporting, sorting and
packaging. Strict and ASan/UBSan tests cover concurrency, admission limits,
catalog identity, dirty retries, hardlinks, stale/skipped files, operational
error propagation, checkpoint integrity/recovery and joined cancellation cleanup.
Current run outcomes and their source identities are
recorded in [VALIDATION.json](VALIDATION.json).

## Optimizer changes and focused native checks

Native sharing now uses bounded workers while preserving current-file
identity/content checks and copy-on-write semantics. Catalog selection uses a
group index capped at 256 MiB, retaining the disk-backed path when necessary.
The immutable file snapshot for parallel sharing is capped at 8 GiB. The cap
replaces the earlier 1 GiB limit; the snapshot allocates only the bytes its
records need. A native large-fixture check loaded 4,131,172,296 bytes of actual
file records and verified cleanup and rejection above the cap without allocation.
That fixture proves capacity and cleanup, with no sharing ioctls or throughput
claim. Over-cap catalogs and failed snapshot allocations retain the serial
fallback; this cap does not change the
catalog format or kernel behavior.
The optimize-only `--report` option publishes complete sharing-group estimates
before sharing, refuses an unrelated existing output, and cleans up partial output on
analysis failure. See [the report format](CATALOG_REPORT.md).

Catalog sorting distributes whole records to up to 64 workers, then performs
an ordered serial merge with the original keys and C locale. Per-worker wrapper
buffers are bounded to 64 KiB; each sort process retains the existing 4 MiB
sort-buffer setting, which is not a hard RSS cap. Temporary runs and spills are
removed on success, failure or cancellation.

Preceding native optimizer runs at one and 40 threads used 196 fixture files and
observed 637,534,208 bytes of savings while preserving file hashes, metadata and
copy-on-write isolation. These are bounded functional checks against the
existing matching kernel, not the complete native suite. A prior synthetic 1 GiB
catalog-sort comparison observed 29.809 seconds serial and 16.573 seconds with
40 sort children, or about 1.80x faster. This measures that synthetic sorting
stage, not production optimizer throughput.

Operational `ENOENT` from opening a source/destination or performing native
sharing now remains an error. Only an immediate catalog-reader EOF is normalized
to successful completion. Focused actual-driver tests cover parallel and serial
paths, watch behavior, first-error preservation, cancellation and concurrent
drain/cleanup; EOF and empty catalogs still complete normally.

## Durable checkpoint proof

`optimize --checkpoint DIR` retains committed scan records, raw catalogs and
finished sorts. Sharing checkpoints retain the selected plan, deferred state and
a drained, target-pool-synced sharing frontier. A partial scan rewalks metadata and skips committed file content;
a complete scan bypasses discovery. Resume validates checkpoint integrity,
options, target identity and saved source metadata before reuse. The same
published report is preserved. See [checkpoint and resume](CATALOG_REPORT.md#checkpoint-and-resume)
for lifecycle, storage requirements and compatibility limits, and
[recovery guidance](RECOVERY.md) for operating boundaries.

The preceding FreeBSD userspace candidate passed an isolated native test with
196 files containing 939,524,096 bytes. The writer analyzed the source, executed
82 real sort children, and stopped before any kernel share. A separate process
then resumed with source-inspection/read/chunk-lookup calls and sort execution
hard-vetoed. It made zero such attempts and completed 4,864 native shares with
637,534,208 bytes of savings. Contents, metadata, copy-on-write isolation,
published report and sorted payload identities remained intact. The owned
pool, backing file and checkpoint workspace were removed afterward.

This proves complete-scan and cached-sort recovery in the isolated fixture.
The newer reader also accepts separately admitted, authenticated whole-group
`sharing.proven` ranges, retaining uncertain groups for normal processing and
rejecting missing, changed or incompatible proof. Portable actual-driver checks
cover selected-plan reload, committed sharing/deferred state and sparse-proof
validation. Committed frontiers represent completed work; cursor and issued
operation counters do not.

Focused strict and ASan/UBSan tests also cover committed-prefix recovery,
corruption, incompatible options, locking and cancellation. Production sharing resume,
production sharing throughput and forced-power-loss recovery remain **NOT_RUN**.
Existing workers retain their running executable and cannot adopt checkpoints
retroactively; future invocations must start with `--checkpoint` and stable
checkpoint/report paths.

## Restore validation and interruption scope

`optimize --restore-validate-only` uses the native checkpoint loader with the
original target/options/report bindings and refuses incomplete scan/selection
state. It validates catalog digests, every saved source-file identity, selected
state, report and required authenticated sparse proof, then exits without source-content
analysis or sharing. This mode requires a proof-bearing legacy import and
refuses ordinary checkpoints without `sharing.proven`. The loader requires a writable private copy-on-write
checkpoint copy: it locks/open streams for recovery and may discard uncommitted
tails after committed data validates. The held/original checkpoint is preserved.

An actual production-scale native loader run returned exit zero and
`restore-validation: PASS` after approximately 35.2 minutes. It authenticated
roughly 505 GiB of catalog payloads and 2.57 million source-file identities.
This establishes full-size loader authentication before shutdown. It does not
establish resumed production sharing, a quick cold restart, or abrupt-power-loss
recovery. Subsequent small-file readback is separate from repeating this full
loader validation.

Deterministic portable process-crash checks run in strict and ASan/UBSan builds.
They stop test-only child sessions at named publication/sort boundaries, kill
those sessions and reopen checkpoints in separate processes. Committed prefixes
survive and uncommitted work replays. Normal builds compile the fault hooks to
no-ops. The optional disposable-guest native crash runner is available but its
native interruption and abrupt-power-off cases remain **NOT_RUN**. These source
checks and the full-size loader result do not replace the combined native suite.

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

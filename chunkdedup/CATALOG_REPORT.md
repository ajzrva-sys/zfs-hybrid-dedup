# Catalog comparisons and saved reports

`zfs chunkdedup optimize --method=block --threads=40 --report /var/tmp/chunk-estimates.jsonl DATASET`

Both native-block and chunk estimates are calculated even when a method is forced.
Selection uses a compact group index in memory, capped at 256 MiB, with the
original disk-backed calculation as a fallback. Block/chunk catalogs remain
streamed from disk. This removes per-block random accesses to the large file
table without changing the cost model or automatic method choices.

`--threads` also controls catalog sorting. Each catalog is distributed by whole
records to up to 64 concurrent sort workers, followed by an ordered merge using
the same keys and C locale. With `--threads=40`, both native and chunk catalogs
use 40 workers in turn. Wrapper buffers are bounded to 64 KiB per worker; each
sort worker retains the existing 4 MiB sort-buffer setting (not a hard RSS cap).
Temporary sorted runs and spills stay in a private operation directory and are
removed after success, failure, or cancellation. Start/end messages identify each
sort stage; the final merge is serial.

`--report` is an optimize-only option. It reserves a private sibling temporary
file before scanning and publishes the complete JSON Lines report before sharing.
It never overwrites an existing output. Interrupted/failed analysis removes its
partial output; a completed report survives sharing cancellation and catalog
cleanup. Store reports outside the data being optimized.

The report contains metadata, one `group` row per connected sharing group, and
`file` rows identifying that group's members and their selected methods. Group
rows contain `native_estimate`, `chunk_estimate`, `chunk_savings_estimate`,
`uncertain`, and `recommended_method`. Recommendations use the existing conservative
20-percent policy and can differ from the forced method actually selected.
Existing chunked files are identified separately.

Compare and total **group** estimates, not repeated per-file values. These are
estimates before sharing, not measured savings or permission to convert files.
Review whole sharing groups and revalidate their current files before a later
chunking run. Paths use UTF-8 with surrogateescape for undecodable filename bytes;
Python's `os.fsencode()` recovers their original bytes.

## Checkpoint and resume

Add `--checkpoint /var/tmp/my-dataset-checkpoint` to `optimize` to create or
resume a private durable checkpoint. Use the same target, method, minimum age,
recursive setting and report path when resuming; the worker count may change.
The option accepts one dataset or path and is unavailable for `watch`. Keep the
checkpoint outside the scanned tree, with an existing parent directory.

The coordinator commits complete file records and both raw catalogs about once
per minute while scanning, and commits the complete scan before sorting. After
a partial scan, restart walks metadata again but reuses committed files without
reading or hashing their contents. In-flight files and work since the last
commit may be repeated. A complete scan skips discovery entirely. Each finished
native/chunk sort is committed independently and reused after restart. Catalog
selection uses a disposable file-table copy so interrupted decisions cannot
modify the saved scan or double-charge estimates. Once selection is committed,
the immutable selected table, report binding and deferred dispositions are
reused. Native-sharing progress is committed only after earlier requests drain
and the target pool is synced. Recovery skips this completed prefix, preserving
canonical/member position inside giant groups. Work beyond the last commit may
replay. The usual 60-second publication interval can be delayed by active reads
or sharing operations.

Payloads and the manifest are flushed and synced before atomic publication.
Resume checks the schema, ABI, options, pool/dataset identity, target identity,
committed sizes and SHA-256 digests, then validates saved source metadata. It
reads the saved catalogs for integrity checking, not source file contents.
Uncommitted tails are discarded only after all committed data validates. A
missing committed payload or manifest, corrupt data or incompatible checkpoint
fails without silently rescanning or replacing evidence. A nonexistent directory
creates a new checkpoint on first use; during recovery, verify that the expected
checkpoint directory still exists before launching. Changed source metadata
also refuses reuse. Mount/device
identity changes require a fresh checkpoint; this format is host-local.

Checkpoint files are owner-only and locked against simultaneous use. They
survive success, failure and cancellation; remove the directory explicitly when
it is no longer needed. Retain enough disk space for both raw and sorted
catalogs, sort spills, the working file table and reports. A published report is
preserved on resume. Scan/sort-only checkpoints still repeat selection and sharing
traversal. New sharing checkpoints reuse the committed plan/frontier; completed
shares survive replay and source identity/kernel comparison checks still apply.

An optional authenticated `sharing.proven` file can bind separately qualified
completed whole-group ranges in a legacy catalog. The reader verifies their
checkpoint/payload/selected bindings and group boundaries, skips only proven
ranges and processes uncertain groups normally. Proof is retained across later
sharing commits; missing or changed proof refuses recovery. Group or read
counters alone cannot create a safe recovery frontier.

`--restore-validate-only` requires a proof-bearing legacy import with authenticated
`sharing.proven`; ordinary checkpoints without this proof are refused. It validates
an already complete checkpoint without
starting sharing. Its loader hashes committed catalogs and validates source
metadata, report and sharing/proof state, so a large checkpoint still takes time
to authenticate. It requires a private writable copy-on-write checkpoint copy
because normal loader recovery may truncate uncommitted tails. See
[restore validation](RECOVERY.md#validate-a-restore-without-starting-sharing).

An already running executable cannot gain checkpoint support in place. Leave
such a worker running to preserve its existing progress; this option protects
invocations started with the new executable.

# Recovery before a long run

Use `zfs chunkdedup optimize --checkpoint DIR` for an expensive single-target
run. Keep `DIR` outside both the scanned target and disposable controller/job
storage, with stable dataset identity and a stable report path. The directory
survives success, cancellation and failure; preserve it during job reconstruction.
The driver validates committed payloads and binding before reuse. An invalid or
incompatible checkpoint fails closed and must not trigger an automatic rescan.

A partial checkpoint reuses committed complete-file records during a metadata
walk. An interrupted file and work since the last commit may repeat. A complete
scan bypasses discovery, and completed native/chunk sorts are reused separately.
Sharing checkpoints also retain the selected file table, deferred dispositions
and a completed native-sharing frontier. Recovery reuses this plan and skips
the committed prefix; work after the last commit may repeat. Before publishing
a frontier, the coordinator drains earlier jobs and syncs the target pool.
Giant groups retain their canonical source and member position across commits.
Publication normally starts after 60 seconds but waits for active jobs, so this
is not a guaranteed shutdown or replay-time bound. Read/issued counters alone
never establish completed work. Completed native shares survive replay.
Source metadata, mount/device identity, options and format compatibility still
matter. Never modify a checkpoint belonging to a live worker.

A separately admitted legacy import may include `sharing.proven`, an immutable,
bounded list of completed whole-group ranges in the native catalog. Its header
binds the base checkpoint, native payload and selected table, and the sharing
state authenticates the entire file. The reader validates group boundaries,
skips only proven ranges and processes every uncertain group normally,
including singleton source deferrals. Later commits retain the proof alongside
the contiguous frontier. Missing, added, changed or uncommitted proof fails
closed; removing it is not a supported migration. Older readers cannot load a
proof-bearing state. A saved read position cannot substitute for completion,
deferred-disposition and durability evidence.

An installed replacement does not change an active process. A legacy process
without checkpoint/export support cannot acquire it in flight. Preserve healthy
legacy work; carrying successful dataset receipts forward is not the same as
recovering an unfinished scan.

A qualified legacy import can retain a completed scan and both sorts without
changing the running process. The preflight's optional `external_checkpoint`
facts require a stable launch path, matching process generation, source/options
and payload bindings, verified source metadata and implementation-bound native
recovery proof. Keep `active_checkpoint=false` and the real restart reason;
unknown or incompatible evidence preserves the restart hold. These supplied
facts do not authenticate receipts or authorize a stop. Scan/sort-only imports
still replay selection and sharing; a sharing-proof import requires the
additional evidence described above.

Before launch, a performance restart and each expensive phase transition:

1. Refresh the worker/executable generation, dataset identity, committed work,
   successful receipts, pool health, free space and actual phase progress.
2. Compare actual file-table and group-index sizes with their in-memory ceilings.
   Worker count alone cannot establish that a large workload will take the
   parallel path. Label intentionally serial coordination and merge stages.
3. Forecast scratch space from actual raw catalogs. The existing sort lower
   bound requires an additional native catalog plus two chunk catalogs beyond
   retained raw inputs; checkpointed selection also needs a working file table.
   Sharing checkpoints additionally retain an immutable selected table and
   per-file deferred state; include their actual sizes.
   Add explicit headroom for system-sort spills, indexes, reports and other
   consumers. A controller's low-space floor is not a phase forecast.
4. Run `python3 chunkdedup/workflow/recovery_preflight.py --help`, then supply its
   JSON observations and save its decision. The `facts` object in
   [the preflight selftest](workflow/test_recovery_preflight.py) gives a complete
   input example; replace every example path, size, limit and state with fresh
   observations. Unknown inputs hold readiness.
   A routine performance restart that loses an existing uncheckpointed scan
   stays on hold. Safety stops require their own concrete justification.
5. Bind source tests and recovery proof to the candidate. The required local
   gate checks actual-driver separate-process complete/partial recovery,
   corruption/binding/locking refusal and cached-sort reuse. Isolated native
   proof and production proof must be stated separately.

The preflight checks supplied facts and resource lower bounds. It does not
discover host state, validate a checkpoint's contents, authorize unsafe stopping
or establish production throughput. Continue observing actual next-stage work
instead of treating frozen scan counters or phase labels as a hang.

## Validate a restore without starting sharing

`optimize --restore-validate-only --checkpoint PRIVATE_COPY` authenticates an
already complete scan and selected plan from a proof-bearing legacy import.
It validates actual target/source metadata, the saved report, sharing state and
required authenticated sparse proof, then exits without
scanning source contents or starting sharing. Ordinary checkpoints without
`sharing.proven` are refused. Use the original target, options
and report path. An incomplete or incompatible checkpoint fails closed.

Use a separately admitted writable copy-on-write checkpoint copy, never the
held backup or a live worker's directory. The native loader takes an exclusive
lock, opens checkpoint streams for recovery and may truncate uncommitted tails
after authenticating committed data. Validation is therefore not completely
filesystem read-only. It hashes full committed catalogs and checks every saved
source-file identity; large checkpoints can take substantial time even without
a source-content rescan.

A production-scale native loader check authenticated approximately 505 GiB of
catalog payloads and 2.57 million source-file identities in 35.2 minutes, with
exit zero and `restore-validation: PASS`. This was restore authentication before
shutdown, not resumed production sharing or abrupt-power-loss qualification.
See [validation scope](STATUS.md) and the
[disposable crash-qualification guide](lab-setup/README.md#deterministic-crash-qualification).

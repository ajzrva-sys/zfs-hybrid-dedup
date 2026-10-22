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
Selection/report work and sharing may replay; completed native shares survive.
Source metadata, mount/device identity, options and format compatibility still
matter. Never modify a checkpoint belonging to a live worker.

An installed replacement does not change an active process. A legacy process
without checkpoint/export support cannot acquire it in flight. Preserve healthy
legacy work; carrying successful dataset receipts forward is not the same as
recovering an unfinished scan.

Before launch, a performance restart and each expensive phase transition:

1. Refresh the worker/executable generation, dataset identity, committed work,
   successful receipts, pool health, free space and actual phase progress.
2. Compare actual file-table and group-index sizes with their in-memory ceilings.
   Worker count alone cannot establish that a large workload will take the
   parallel path. Label intentionally serial coordination and merge stages.
3. Forecast scratch space from actual raw catalogs. The existing sort lower
   bound requires an additional native catalog plus two chunk catalogs beyond
   retained raw inputs; checkpointed selection also needs a working file table.
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

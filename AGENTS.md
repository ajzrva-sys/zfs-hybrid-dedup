# Long-running dedupe changes

Read `chunkdedup/RECOVERY.md` before launching or restarting expensive work.
Use `chunkdedup/workflow/recovery_preflight.py` with fresh observations; unknown
facts, scale-triggered serial fallback and insufficient scratch space must be
resolved before declaring a parallel run ready. A preflight is a lower-bound
resource check, not proof of end-to-end production throughput.

Preserve a healthy active scan/catalog generation. Before a performance restart,
establish whether that active executable actually writes usable checkpoints,
what work is committed and what will replay. A new installed CLI does not alter
an existing process. Do not discard legacy uncheckpointed work just to enable a
new feature. Safety stops are separate from performance restarts.

Future long runs must use a stable checkpoint directory outside disposable job
storage. Preserve it and successful dataset receipts during job reconstruction.
Never turn a missing expected recovery checkpoint or an invalid checkpoint into
an automatic rescan. First use may create a new checkpoint. Explain the
distinction between unfinished-scan recovery and carrying completed datasets.

The local gate must retain actual-driver, separate-process recovery evidence
with zero source analysis and zero cached-sort execution on complete resume,
plus partial-prefix and integrity/identity/lock refusal coverage. Bind that
evidence to current sources; do not substitute descriptive PASS strings.

Use focused checks while iterating, then the full local gate after freezing the
patch. Reuse valid native proof when production implementation bytes are
unchanged. Keep source, installed, isolated-native and production claims
separate. Publish sanitized summaries, never private operational receipts.

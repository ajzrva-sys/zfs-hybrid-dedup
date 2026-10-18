# Validation status — 18 October 2026

Two fresh isolated VM runs and a dedicated bare-metal rerun passed **21/21
original cases and 12/12 hybrid cases**, including snapshot/clone independence,
received-content verification and full/incremental logical replication.
The latest full native gate remains **INFRA_FAILURE**, with cleanup and host
state acceptance incomplete. Functional passes are reported separately.

The replication harness checks whether a received dataset is mounted, mounts
only when necessary, and asserts the resulting state before continuing. All
pre-existing hybrid assertions remain present. The evaluator now includes the
separately pinned hybrid archive in its exact media inventory when hybrid tests
are declared; archive coverage and hash checks remain enforced. Neither change
modifies the native implementation, source patch, chunker or immutable fixtures.

The latest VM attempt completed all 33 functional cases and restored the guest
module and test pools, then its host-cleanup SSH probe exceeded a five-second
timeout. A separate recovery confirmed the VM stopped, restored its exact
configuration and released only the run's lock. Configuration, module and pool
properties matched the baseline. One existing host service was running before
and after with a different process identity; the strict comparison therefore
records a state difference. The cause is not established by this evidence.
Recovery did not target services or interrupt unrelated replication. The
original failure and recovery evidence remain separate.

The bare-metal rerun passed on disposable file-backed pools using a dedicated
SSD. Restoration of the normal installation and original pool/mount intent
passed. This is functional evidence for that layout, not raw-device or
production qualification, and does not replace the VM-specific native gate.

The required local profile passed with **35 Microsoft fixtures and 141 immutable
baseline files**. The unchanged 4,134-file native source digest and current case
inventories are recorded in [VALIDATION.json](VALIDATION.json). That file retains
the earlier failed native evaluation as history. The 17 October report remains
unchanged in the development evidence.

Matched hybrid allocation, pure-read and RAM comparisons and planned sharing
power-loss validation remain **NOT_RUN**. The 1.25x foreground-read target is
unverified. Existing regression timings do not establish a matched hybrid
performance pass. Experimental modules belong in disposable test environments.

This public summary excludes host inventories, private paths, device identities
and raw operational logs. No full native gate or production-readiness pass is
claimed.

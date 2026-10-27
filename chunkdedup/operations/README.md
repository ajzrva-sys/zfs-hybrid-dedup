# Recovered deduplication operations

These are sanitized references recovered on 2026-10-25 from the deployed
FreeBSD deduplication workflow. They preserve the controller, dashboard snapshot
and userspace progress observer alongside the implementation already published.
They are not a portable installer or an instruction to restart a running job.

- `run.sh.example` carries completed dataset receipts, binds unfinished recovery
  to a required checkpoint, verifies admitted tools/module and host identities,
  guards resources, and restores original dataset properties and mounts.
- `snapshot.sh.example` reads an atomic controller generation, checks process
  liveness, reports persisted dataset outcomes and reads only the log tail.
- `progress_probe.c` observes the administrative API through `LD_PRELOAD`, counts
  successful reads/inspections and publishes an atomic progress file. Set
  `CD_PROGRESS_PATH` to its destination. It preserves the wrapped return value
  and errno; it does not change deduplication decisions.

The `.example` files retain their original prepared-state requirements. Host
UUIDs, pool/dataset GUIDs, private names and paths have been replaced consistently.
The controller refuses the placeholder host identity. Before adapting it, review
all paths, identity checks, expected checkpoint file sizes, admitted hashes,
prepared queue/property inventories, worker count and space floors. The
host-specific checkpoint sizes and resource settings are retained as examples,
not universal defaults. Required preparation receipts are intentionally private
and are not supplied here. Do not bypass those checks to launch the example.

On FreeBSD, the observer can be built with:

```sh
cc -std=c11 -Wall -Wextra -Werror -fPIC -shared -pthread \
  -o progress_probe.so progress_probe.c
```

See [recovery guidance](../RECOVERY.md) before any launch or restart. Do not
modify a checkpoint owned by a live worker. The snapshot and observer are
operational evidence tools; their counters do not prove a resumed checkpoint
or completed sharing. Production recovery, portable deployment of these
examples and the combined native suite are not qualified by this publication.

[Recovery provenance](RECOVERY.json) records original and published hashes.
Raw recovery files remain outside Git. Syntax/build checks and the repository
local gate do not constitute execution of this controller against a pool.

## Isolated controller regression tests

Run `python3 chunkdedup/operations/tests/test_controller.py` from the repository
root (Python 3, POSIX shell and standard text utilities). The local verification
profile runs this suite against its frozen controller and test sources too.
`--report FILE` records the controller SHA-256, source stability and test result;
`--case METHOD` selects one regression for diagnosis.

Tests execute the complete shell entry point in a temporary directory. Only
path/identity bindings and command lookup are adapted in the temporary copy;
there is no operational test mode or guard bypass in the published example.
Strict command doubles model ZFS properties, property sources and mount state,
reject unknown commands and capture worker/watchdog activity. Fixture preparation
files and checkpoint-shaped files exist only inside that temporary directory.
They are not private preparation receipts or optimizer checkpoint qualification.
The unadapted placeholder example is also checked for refusal at its first
identity gate, without invoking real identity or storage commands.

Coverage includes host/UUID, module version/test mode, admitted module and CLI
hashes, target/scratch pool and scratch dataset identities; missing checkpoint
payloads and invalid metadata; invalid completed receipts; resource refusal
before mutation; carried receipt preservation; optimizer failure, SIGTERM and
watchdog termination; local/default/inherited/received property restoration,
private unmount and existing-mount mode restoration; and distinct `FAILED`,
`RESTORATION_FAILED` and `PASS` outcomes. An interrupted dataset never receives a
successful receipt. Model readback checks original property values **and sources**
and mount state, while completed receipts are compared byte for byte.

The tests exposed a launch race: the original resource watchdog checked space
only after property changes and could run after the worker had started. The
controller now checks the same floors synchronously before recording a start,
changing properties or launching a worker, and reuses that check in the
watchdog. Failed resource commands and empty/non-numeric space values refuse
execution. Existing identity, preparation and checkpoint guards remain required.
`RECOVERY.json` retains the original publication hashes and records this revision
separately.

These are command-fixture tests, not execution against a pool. The development
host is macOS; no disposable FreeBSD runtime was available for this change.
FreeBSD shell behavior, real ZFS property/mount restoration, real worker child
shutdown and live resource transitions remain unqualified. Run native work only
in a disposable FreeBSD VM with file-backed pools and an admitted candidate;
never adapt these fixtures or receipts to qualify production. No production
pool, running job, checkpoint or preparation receipt is used by this suite.

## Reusable reference primitives

[Prefetch planning and cohort helpers](prefetch-reference/README.md) and
[sharing proof/throttling helpers](sharing-reference/README.md) retain their
qualified source bytes. They are reference components with generated-fixture
checks, not portable deployments of the host-specific accelerator, six-hour
saver, restore orchestration or status command. Private launch configurations,
job generations, backup/restore receipts and catalogs remain outside Git.

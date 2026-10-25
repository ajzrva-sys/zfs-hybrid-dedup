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

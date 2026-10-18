# Validation status — 17 October 2026

Native compilation and module loading passed in the guarded offline FreeBSD
VM. The latest full native attempt passed **21/21 original cases and 11/12
hybrid cases**, then failed its last hybrid replication harness case.

Passing hybrid coverage includes forced/default methods, exact block sharing,
independent writes and metadata, unique/shifted selection, sparse holes and
tails, incompatible block sizes, stale/unequal/busy requests, peer and snapshot
preservation across BRT-to-CDC conversion, explicit restoration, watching,
interrupted conversion and quota exhaustion.

The final case had passed snapshot and clone independence checks and completed
its full send/receive commands. Changing the received dataset's mountpoint
mounted it; the next explicit `zfs mount` returned `filesystem already mounted`.
Received-content verification and hybrid incremental assertions were not
reached. The full native evaluation remains **TEST_FAILURE**.

Three bounded repairs were used: a const pointer declaration, a safe BRT
existence query after an inspection panic, and settling sparse allocation
accounting before its test baseline. The final harness error remains unfixed;
all content/allocation assertions and immutable expectations remain present.

The existing local profile passed against 35 Microsoft fixtures and all 141
immutable baseline files. Its scope is source/models/conformance, not native
recovery. The source tree digest and passing/failing case inventories are
preserved in [VALIDATION.json](VALIDATION.json). This public summary omits raw
host inventories and private run output; original failed evidence is retained
in the development workspace.

The current revision has no matched pure-read/RAM or physical-savings result,
and planned sharing power-loss validation has not run. No 1.25x performance
pass is claimed. Historical CDC timings do not establish hybrid performance.

Guest restoration, VM configuration restoration and host state comparison
passed after the final run. The lab was stopped. No experimental host module
was installed.

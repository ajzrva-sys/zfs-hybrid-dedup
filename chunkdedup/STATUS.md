# Validation status — 19 October 2026

The latest development source snapshot is
`833fb482f3675b5ad87e3bbdcb18cca66760de1625ecd219be2c22359724a1ed`.
Its required local profile **passed**, checking 4,138 source files, 141 immutable
baseline files and 35 Microsoft conformance fixtures. Strict and ASan/UBSan
tests cover concurrent execution, bounded admission, ordered catalog merging,
dirty retries, hardlinks, stale/skipped files and cancellation cleanup.

The native CLI build and installation readback passed. A 40-worker deployment
showed 41 process threads and more than four cores of CPU use, confirming
multicore activity. These observations are not a matched throughput benchmark.
Parallel scanning changes only the CLI source, its new executor header and its
build file relative to the kernel-tested predecessor. Selection, sharing and
CDC installation remain sequential; kernel and on-disk formats are unchanged
by this userspace update. The full native suite was not rerun for that update.

The preceding durability-batching source snapshot,
`98882f3c8618f31f28dcb7208c0909f78eca5290de029a2763749b7a5bc5ebcc`,
passed **21 original and 12 hybrid cases** in the isolated guest through the
`native-smoke` profile. It reused the exact module/tools from an earlier
successful compilation of the same source, with fresh hash/provenance checks.
Guest restoration, shutdown and exact host comparison passed. This is fresh
native test execution with a reused build, not a fresh full-native compilation
or a relabeling of earlier failed evaluations.

The native cases include snapshots, clones, logical replication, independent
mutations and reference preservation. New batching checks cover dirty-file
admission, request bounds, preserved bytes/metadata and repeated clean
inspections without transaction-group advances. Fixture setup now settles
allocation and BRT visibility explicitly before measuring their existing
assertions. All 21 original and 12 hybrid cases and immutable expectations
remain present. Earlier source, fixture and infrastructure failures remain
retained in development evidence; the prior public results remain history in
[VALIDATION.json](VALIDATION.json).

The block-only command is also included with its focused scanner tests. It is
currently serial; parallel `--threads` support belongs to `zfs chunkdedup`.
Block-only operation needs the matching patched driver and native
`block_cloning` feature, without enabling the CDC feature.

Current-revision matched allocation, pure-read and RAM comparisons and forced
sharing power-loss qualification remain **NOT_RUN**. The **1.25x** foreground-read
target is unqualified. Functional tests and live CPU observations do not prove
a performance target or production readiness.

This public summary excludes host inventories, private paths, device identities
and raw operational logs. Use the guarded disposable guest for reproduction.

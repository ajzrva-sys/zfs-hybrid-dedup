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

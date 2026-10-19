# Background block deduplication

`zfs blockdedup` finds equal native ZFS blocks after ordinary writes and replaces
duplicate storage with BRT shared references. Files retain ordinary block
pointers, normal reads and independent copy-on-write mutations. The optimizer
requires this fork's patched FreeBSD module and matching administration tools;
stock FreeBSD does not expose its administrative interface.

```sh
zfs blockdedup optimize [-r] [-n|--dry-run] [--min-age DAYS] [--tmpdir DIRECTORY] DATASET_OR_PATH...
zfs blockdedup watch [-r] [-n|--dry-run] [--min-age DAYS] [--tmpdir DIRECTORY] [--interval SECONDS] [--once] DATASET_OR_PATH...
```

The minimum file age defaults to three days, and watching repeats hourly.
Traversal stays within one filesystem and skips snapshot directories and
symlinks. Select each child dataset explicitly. Temporary disk catalogs are
rebuilt each pass; keep their directory outside the datasets being scanned.
Root permission and the existing `block_cloning` pool feature are required.
Block-only optimization does not require enabling the CDC feature or property.

The operation adapts upstream OpenZFS commit
[`afb4010`](https://github.com/openzfs/zfs/commit/afb4010). It drains prior writes
before installation locks, revalidates identities and geometry, excludes
competing opens and mappings, and compares current bytes before sharing.
Hashes are discovery hints. Sharing stays within one dataset. Encrypted or
exported datasets, native inline deduplication and active CDC files are rejected.
Holes and embedded blocks remain ordinary storage.

`--dry-run` reports candidates without sharing. Summary values are block/file
counts, not saved-byte estimates. Snapshots, existing sharing, compression,
allocation geometry and metadata affect realized savings. A crash may discard
sharing progress without changing logical file contents.

This separate block-only scanner is currently sequential. The parallel hybrid
scanner is `zfs chunkdedup optimize --method=block --threads=N`; see
[HYBRID.md](HYBRID.md) for its property and admission requirements.

## Focused scanner test

The included C test compiles the actual scanner against a fake administrative
API. It covers duplicate discovery, unique data, geometry, tails, sparse blocks,
hardlinks, already-shared candidates, stale/busy files, mismatches, dry runs and
argument handling. It does not load a module or prove native locking or savings.

From the repository root, replacing the absolute source path below:

```sh
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations \
  -DBLOCKDEDUP_SOURCE='"/absolute/path/to/repo/cmd/zfs/zfs_blockdedup.c"' \
  -Ichunkdedup/lab-setup/tests \
  chunkdedup/lab-setup/tests/blockdedup_scanner_test.c \
  -o ../blockdedup-scanner-test
../blockdedup-scanner-test
```

The command uses CommonCrypto on macOS. Other systems use OpenSSL and require
`-lcrypto`. Native testing belongs in the [guarded offline lab](lab-setup/README.md).
Current validation scope and remaining checks are recorded in [STATUS.md](STATUS.md).

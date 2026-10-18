# Actual-core in-memory mock harness

This package compiles the **actual** `module/zfs/zfs_chunkdedup.c` and public `include/sys/zfs_chunkdedup.h` from a supplied patched OpenZFS tree. The implementation is not copied into the package. The surrounding DMU, ZAP, SA, transaction, mutex, vnode and feature APIs are replaced by the small in-memory model in `core_test.c` and `include/sys/`.

Run:

```sh
./run.sh /path/to/patched-openzfs
```

Requirements: Clang with AddressSanitizer and UndefinedBehaviorSanitizer, a POSIX shell, and standard C headers. macOS uses CommonCrypto from the system SDK. Other hosts need OpenSSL headers and `libcrypto`; that adapter is supplied but the recorded run below was on macOS. Set `CC` to a different Clang executable if necessary. The launcher builds in a temporary directory and removes the binary and debug symbols on exit. It does not load a kernel module, access a pool, require root, or mutate the supplied source tree.

Compilation uses GNU C99, `-Wall -Wextra -Werror`, AddressSanitizer and UndefinedBehaviorSanitizer. Only digest-library deprecation warnings are suppressed. AddressSanitizer leak detection is disabled because the fixture object sets intentionally survive until process exit. Undefined behavior and other sanitizer failures stop the run.

## Assertions covered

- Cross-record chunk slices, two-file sharing, logical reads and reference counts.
- Idempotent optimization, touched-record hydration, and reoptimization of mixed ordinary/optimized batches.
- Simulated transaction-assignment ENOSPC preserves ordinary bytes and does not install recipes.
- A deep copy of the model retains original data after live hydration and reclamation; this models the intended snapshot/clone isolation.
- Missing recipes, per-file metadata, or master metadata fail with EIO when the persistent inode marker remains set. Wrong reference counts are rejected before reclamation.
- Non-power-of-two, 512-aligned single native block files share, hydrate, and restore.
- Debug controls default to disabled. Recipe deletion and refcount increment faults are detected and restored using saved tokens.
- The actual sharing comparator rejects fabricated equal identifiers with different bytes and accepts equal bytes. This is a comparator fixture, not a cryptographic SHA-512 collision.
- Genuine model native holes retain their extent behavior; mapped records are DATA for seek operations. Restored data and complete native-record storage release, including EOF padding, are checked.
- An actual model maps ZAP containing more than 65,536 entries is rejected before scanning. Forced near-limit ZAP counts exercise conservative install preflight for maps/index/refs and check that no chunk objects or file mappings are installed.
- A 1 TiB logical sparse model file with two mapped records verifies and restores through bounded metadata enumeration, without scanning the large logical gap.

## Limits of this evidence

This is **not real DMU or crash-recovery proof**. Transaction holds and SA persistence are stubs; assignment failure is injected before mutation. Commit has no native TXG, disk I/O, ZIL, rollback or durability semantics. The snapshot/clone model is a deep object-set copy, not real DMU COW. Mutex stubs assert single-thread ownership, so they cannot establish concurrency, mmap, vnode/pager or lock-order correctness. Allocation flags approximate native extents and cannot measure physical savings, quotas, ARC use, foreground performance or actual free-range behavior. Pool/stream feature negotiation and unsupported-platform admission are outside this harness. Microsoft chunking conformance is covered by the separate chunker fixtures, not these hand-built plans.

Real FreeBSD compilation and disposable guest pool tests must be reported separately. `RESULTS.md` records only this packaged mock run.

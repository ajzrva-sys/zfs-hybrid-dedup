# Packaged mock run

Run UTC: 2026-10-09T20:41:49+00:00

Host: macOS arm64. Compiler: Apple Clang 21.0.0 (`clang-2100.1.1.101`).

Command, from this package directory:

```sh
./run.sh ../../openzfs
```

Result: **PASS**, exit status 0. Fresh strict compilation and execution with AddressSanitizer and UndefinedBehaviorSanitizer completed without reported sanitizer errors. Shell syntax validation also passed. No binaries or debug symbol bundles are included in the package.

Source fingerprints of the supplied patched OpenZFS tree:

- `module/zfs/zfs_chunkdedup.c`: `0a2516b9a016eb33772af62c03f7af90d310335213c7eb0d49682f8aa3aa2cc2`
- `include/sys/zfs_chunkdedup.h`: `ef6479201d4202e1eff5e8035cc21841b2be20e19a3a230f057574040e78ffad`

OpenZFS base revision: `71a9f9578616a90c3c14bb59629fb4d31bfd68d1` (the module and header above are prototype additions to that tree).

The passing assertions cover sharing/slices, idempotence, simulated ENOSPC admission, touched-record hydration, mixed reoptimization, deep-copy snapshot model, missing recipe/file/master metadata EIO, refcount corruption, odd native short blocks, debug fault gates and restore, fabricated collision comparator inputs, sparse extent/seek preservation, metadata caps and install preflight, 1 TiB logical sparse gap restoration, and reclamation.

This records the **in-memory DMU model only**. It does not establish real OpenZFS TXG atomicity, ZIL replay, crashes, concurrency, physical allocation, Microsoft chunk boundary conformance, or guest-pool validation. See `README.md` for exact scope and limitations.

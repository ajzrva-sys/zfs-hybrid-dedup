# Prefetch reference primitives

These are bounded catalog planning, advisory lookup-copy and read-ahead cohort
primitives. They retain the original source bytes, including the exact FreeBSD
catalog ABI admission. They are not a production accelerator deployment: the
host-specific memory admission, generation guard, restoration owners, launch
configuration and performance receipts are not included.

`native-window.py` includes an explicitly supplied private-guest client entry
point. Its generated-fixture tests use injected commands; running those tests
requires no kernel companion, pool or network access. Importing the planning
modules performs no prefetch requests. Do not use these references as admission
for an existing worker or as a checkpoint/recovery interface.

Requires Python 3.11 or later (`hashlib.file_digest`). From this directory, run:

```sh
python3.11 -B -m unittest test_advisory_table test_native_window test_warm_cohort
```

The copied tests qualify these primitives on generated data and injected device
responses. They do not establish production throughput or a sustained speedup.

## Source provenance

Retained from the October 26, 2026 observation-expiry repair's qualified source
snapshot. No private configuration, runtime manifest, seed, receipt, catalog or
operational log is included. Original source SHA-256 values:

| File | SHA-256 |
| --- | --- |
| `advisory_table.py` | `f3fb1451c8ecc3fc5d8d750c3229eccf65039eea8570482de55026f196305ff9` |
| `native-window.py` | `4068b4d10ca9c9dfd8b27c7b1ed7df8528940a16dd1cd1b0f3ff9da008c706b9` |
| `warm_cohort.py` | `79b4d3fc17fd5ee18ac5a6aa656d47dd7767ed7ccb0dca08d6a65ae06467311e` |
| `planner/planner.py` | `20bdd20a62d992543fb466fe7c751c3e0eb4aa31ca3444e1499d1a32d58840e9` |
| `planner/catalog-layout.json` | `f96e4f81db0ce7e69bf0da77570fe598f25406bcc7638c415bcddfef0dddb4bc` |
| `test_advisory_table.py` | `e9cd9cc0cd90599a3671c80016c73157a38b7255a2fd155da09bf7c41ae3ac16` |
| `test_native_window.py` | `4871264c09a4aec8d225cb6182ed9c1269b4a94a7b218805e2e929e95eea2a36` |
| `test_warm_cohort.py` | `0b5b5f9fc515df5c953c708d12f926cf77a3499c0dcd8bedba6c49f4fe3724ca` |

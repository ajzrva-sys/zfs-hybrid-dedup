# Sharing reference primitives

`engine.py` maintains packed incremental conflict proof for an immutable BLOCK
catalog. `budget.py` meters only its caller's I/O, and `impact.py` compares
observed group throughput over complete windows. These modules contain no host
controls, worker descriptors, process signals, cache controls or storage
commands. They are reference components, not a six-hour saver deployment or an
independently qualified recovery publisher.

From this directory, run the self-contained impact fixtures:

```sh
python3 -B -m unittest test_impact
```

The original engine qualification compares against a separately retained
historical sparse-proof oracle. Those tests and their private dependency chain
are not bundled here; no standalone engine-test pass is claimed for this
reference directory. The I/O budget is likewise preserved source, not a new
performance qualification.

## Source provenance

Retained unchanged from the October 26, 2026 shutdown-priority capture source.
Host orchestration, private admissions, publication receipts and deployment
configuration are excluded. Original source SHA-256 values:

| File | SHA-256 |
| --- | --- |
| `engine.py` | `3660d8365baf621c2a68f6c60c24b5d69fad9e3f8165b383c6a2ef13885c49b3` |
| `budget.py` | `bc5f31d7eaaef544fb245fd1c529ba7cba3264b879f0c12d178f3c9cb5b7084b` |
| `impact.py` | `ff18e698cdc4e230d92f540ccdbcc9ad9f766eb45325c73cfd66d4330f4d2d5d` |
| `test_impact.py` | `b8130a5cea879a95dbb9991e18a6c68a72d967d922d21591e3bd69381c9e2055` |

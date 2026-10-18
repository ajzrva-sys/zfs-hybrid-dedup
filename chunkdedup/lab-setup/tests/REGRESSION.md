# Native guest regression suite

Run `python3 /root/lab/tests/regression.py` only after installing the debug
prototype in the disposable guest. The suite requires hostname
`zfs-chunkdedup-lab`, FreeBSD, root, one CPU, only `lo0`, no imported pools,
and exact 8 GiB `/dev/vtbd1` and `/dev/vtbd2` test disks. It creates only
`cdlab` and `cdlab-recv`, with 512 MiB parent quotas. File I/O is bounded;
quota exhaustion uses a 16 MiB child dataset. Results and streams stay on
the guest UFS boot filesystem in `/root/lab/tests/work`.
Both lab pools use `atime=off`, so reading a received snapshot's live successor
for comparison does not modify it before the next incremental receive.
Incremental tests preserve the normal destination-modified rejection instead
of forcing rollback. A separate dataset explicitly tests `atime=on` metadata.

The suite covers cold logical reads, mmap, sendfile, direct requests using
buffered fallback, partial writes across native record boundaries, sparse
extents, tiny and partial EOF records, metadata, background watch, mmap races,
interrupted workers, independent copies, snapshots, clones, reclamation,
replication, admission exclusions and exhausted quotas. It reports physical
dataset usage and snapshot-retained space separately. Read throughput,
4 KiB write/fsync latency, sampled worker RSS and ARC size are measurements
for this one-CPU debug guest, not production performance claims.

The debug fault test requires a `ZFS_DEBUG` build. Its root-only ioctl operations
are unavailable in release builds, and the guest tunable
`vfs.zfs.chunkdedup_test_mode` defaults to zero. The helper enables it only
around reversible missing-recipe/refcount tests and restores it afterward.
Restoration tokens are bounded XDR nvlists under the guest UFS work directory.
The collision test exercises the actual optimizer's candidate comparator
with the same identifier and different bytes. It does not claim to have found
a cryptographic SHA-512 collision.

The helper builds against the installed prototype headers with SPL first:
`-I/opt/chunkdedup/include/libspl -I/opt/chunkdedup/include/libzfs`.
It links with `-L/opt/chunkdedup/lib -Wl,-rpath,/opt/chunkdedup/lib` and
`-lzfs_core -lnvpair`. The suite compiles with
`-O2 -std=c11 -Wall -Wextra -Werror`; the installed SPL headers supply
`uint_t` and `boolean_t` before the prototype library interfaces are read.

Forced crash tests require the root orchestrator to operate the disposable VM.
These commands contain no host actions:

    python3 /root/lab/tests/regression.py --recovery-prepare fsync
    python3 /root/lab/tests/regression.py --recovery-check fsync
    python3 /root/lab/tests/regression.py --recovery-prepare always
    python3 /root/lab/tests/regression.py --recovery-check always
    python3 /root/lab/tests/regression.py --recovery-prepare optimize
    python3 /root/lab/tests/regression.py --recovery-check optimize

Run each prepare command, wait for `RECOVERY-READY PHASE`, forcibly stop only
this VM, restart it, load the prototype module, and run the corresponding
check command. Prepare saves expectations on UFS before pool writes.
The `fsync` and `always` phases flush both an 18-byte cross-record write and
a 64 KiB within-record write, exercising immediate and indirect ZIL paths.
They temporarily increase only the guest TXG timeout to leave a window for
the power cut. No pool sync follows the ready marker. Check restores the
original timeout and validates changed and independent files after replay.

The `optimize` phase leaves a worker running after the first conversion batch.
Its marker records a partial map count. After power loss, check requires that
conversion remained incomplete, verifies every file against its durable UFS
manifest, and checks map/refcount consistency. Recovery imports scan only the
two guarded guest devices and reject any unexpected imported pool or vdev.
The suite leaves its own lab pools available for these recovery phases.

Local Python syntax validation and native integration style checks are
separate evidence from actual FreeBSD compilation and VM test results.
`python3 lab-setup/tests/test_worker_batches.py --openzfs /path/to/patched-openzfs --chunker /path/to/conformance/chunker` locally compiles the actual
worker batch functions with the real streaming chunker and a mocked ioctl
transport under ASan/UBSan. It covers random/zero input, 1 MiB and 16 MiB
boundaries with short tails, retained overlapping chunks, complete candidate
coverage, cancellation and failed submissions. It does not exercise kernel
storage. The guest suite separately checks all tail records become mapped.
For a separately patched OpenZFS checkout, run the portable test with
`--openzfs /path/to/openzfs --chunker /path/to/chunker`; adjacent source
trees remain an optional default. Compiler selection uses the `CC` variable.
Snapshot checks exercise both dataset names and file paths for status/verify,
including a previously untouched snapshot that must first be mounted, and
require conversion/restoration requests against snapshots to fail.

After exporting only the two lab pools and booting the guest's original
`zfs.ko` (FreeBSD OpenZFS 2.4.2), run:

    python3 /root/lab/tests/legacy_gate.py

This asserts the stock module version and absence of `openzfs.ko`, exact VM
identity/resources/devices, and no imported pool. It tries read-only imports
of each dedicated test disk; both must reject `org.ajzrva:chunkdedup`.
For the legacy stream check it creates only a new 128 MiB sparse file-backed
pool on the guest UFS work directory, receives the existing UFS full stream,
requires rejection before child creation, and destroys only that new pool.
It rechecks the exact file vdev before cleanup and never recreates or writes
the retained `vtbd1`/`vtbd2` pools. Results are saved durably as
`work/legacy-gate-results.json`. Root performs all VM boot/module switching.

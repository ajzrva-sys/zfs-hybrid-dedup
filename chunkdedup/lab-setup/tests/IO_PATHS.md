# Guest-only native I/O helper

Compile inside the lab VM:

    cc -O2 -std=c11 -Wall -Wextra -Werror -o io_paths io_paths.c

The helper accepts existing regular files only below `/mnt/cdlab/`, after
resolving symlinks. Files are bounded to 64 MiB. Verification requires a
separate expected-file inode of the same length.

Commands:

    io_paths verify FILE EXPECTED
    io_paths mmap-verify FILE EXPECTED
    io_paths sendfile FILE EXPECTED
    io_paths direct FILE EXPECTED
    io_paths pwrite FILE OFFSET HEX
    io_paths mmap-write FILE OFFSET HEX
    io_paths direct-write FILE OFFSET HEX

Mutations contain 1–4096 hexadecimal bytes and must remain within existing
EOF. Both mutation modes flush the file; mmap writes also use `MS_SYNC`.
`direct-write` requires exactly 4096 bytes of hexadecimal data and a
4096-byte-aligned offset. It uses `O_DIRECT`, a 4096-byte-aligned allocated
buffer, checks for an exact full write and flushes the file. Missing support
or a rejected direct write is a failure, including on a platform without
`O_DIRECT`. A command constructed by Python is:

```python
subprocess.run(["./io_paths", "direct-write", "/mnt/cdlab/test/file",
                "4096", "5a" * 4096], check=True)
```

Sendfile uses a private Unix socketpair and fork, compares the received stream
against the expected fixture, and uses five-second socket timeouts. No
network interface or remote endpoint is used. Direct I/O exits 77 only when
the platform does not define `O_DIRECT`; on FreeBSD an unsupported operation
or data mismatch fails the test.

Close all worker descriptors and unmount/remount the disposable dataset
before read-path tests, or use a newly mounted snapshot. Source pages cached
while the worker chunks the file can otherwise mask failures in native
recipe reads and the pager. The expected fixture should remain unoptimized.

Local Apple Clang C11 syntax validation with `-Wall -Wextra -Werror` passed.
FreeBSD compilation and guest execution must be reported separately.

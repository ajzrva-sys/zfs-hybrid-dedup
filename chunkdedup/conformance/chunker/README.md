# Windows-derived chunker for the OpenZFS prototype

This is a portable C adaptation of Microsoft's MIT-licensed BuildXL commit
`852a00e5b45ef8a8d06a2eb9040495c36243078f`, with the original Rabin tables,
16-byte window, four regression strengths, zero-run behavior, 32/64/128 KiB
profile and deterministic 1 MiB buffering. It is a userland library, not a
filesystem feature by itself.

The adaptation follows ManagedChunker's actual contract: the regression
engine receives one contiguous EOF buffer. Streaming callers use the
DeterministicChunker algorithm, discard the last provisional chunk from each
1 MiB pass, and rechunk that tail with subsequent data. The original C#
nonfinal-buffer/history paths are unreachable through ManagedChunker and are
not exposed as a second C interface. C indexes replace C# unsafe pointers.
No chunking approximation is used in the oracle or tests.

`chunkdedup_chunker.h` defines the API. Create a session, push any sequence of
byte buffers, finish once (repeated finish succeeds), then destroy. Callbacks
receive absolute 64-bit offsets, lengths, the first 32 bytes of SHA-512,
Microsoft cut-type numbers and the chunk's bytes. Bytes remain valid only
until the callback returns. Any nonzero callback return aborts the session
permanently. Hashes are **truncated SHA-512**, not SHA-512/256. Hash equality
alone does not establish byte equality; the filesystem conversion operation
must compare bytes before sharing.

Build with `./build.sh`. Apple uses CommonCrypto, and FreeBSD/Linux use
OpenSSL's libcrypto. Alternatively use `make` on Apple or
`make LDLIBS=-lcrypto` on FreeBSD/Linux. Compile `chunkdedup_chunker.c` and
`rabin64_table.h` into the userspace worker and link libcrypto; the public
header introduces no crypto-library dependency.

`chunkdedup-cli FILE|- [read-size]` emits tab-separated
`offset`, `length`, `sha512_first32_hex` records. Input `-` reads standard input.
It retains at most 1 MiB of chunking input independently of file size.

## Verification

`python3 check_fixtures.py` needs only the C executable. It compares recorded
outputs from the original Microsoft C# implementation and independently
rehashes every chunk.

`./build.sh check` runs the public API checks and all 35 recorded fixtures
without .NET. `./build.sh check-oracle` additionally executes the original
source oracle after `../reference/oracle/build.sh` builds it. Set
`DOTNET_COMMAND` to the absolute path of a .NET 8 SDK executable, or use
`dotnet` on PATH. Microsoft source files are compiled without changes; `Shims.cs` supplies only allocation, assertion, formatting and enum
interfaces. `Program.cs` drives the original ManagedChunker or regression
engine and uses the original Microsoft test-content generator.

The full conformance run passed 65 cases in 34.282 seconds locally:

- All 20 published 64 KiB basic node-hash vectors, including the original
  serialized node digest when a fixture has multiple chunks.
- Seed 69519 / length 1,254,972 and an 8 MiB seeded stream.
- Empty and tiny buffers, zero and FF runs, increasing byte patterns, and
  transitions around the 16-byte window and the minimum/maximum limits.
- Identical C and C# outputs for ten I/O segment sizes, from one byte through
  16 MiB, across three different fixtures.
- All four regression strengths, confirmed independently by the original
  C# engine's cut-type outputs.
- 81,921 identical offset/length/hash records for a 5,368,709,243-byte stream
  of repeated Microsoft-generated seeded content, streamed without storing a
  5 GiB fixture.

`test_chunker.c` also checks callback cancellation, persistent failure,
invalid input, output-capacity bounds, empty streams and finish lifecycle.
AddressSanitizer and UndefinedBehaviorSanitizer passed these checks and all
four regression fixtures; see `test-results/sanitizers.txt`.

Results are in `test-results/conformance.json`; source hashes and exact URLs
are in `../reference/provenance.json`. The original local SDK archive was checked against
Microsoft's SHA-512 release metadata before use; only its provenance is
included, with no SDK, compiled binaries or caches.

This establishes conformance to the pinned Windows-derived BuildXL code.
It does not claim a direct comparison against a particular Windows Server
release's installed deduplication service. OpenZFS integration and guest
validation must be reported separately.

## License

The Microsoft MIT license is retained in `LICENSE`, with copyright notices in
all adapted source and table files. Original sources remain under
`../reference/`. The oracle SDK and its build caches are intermediate tools,
not part of the portable library.

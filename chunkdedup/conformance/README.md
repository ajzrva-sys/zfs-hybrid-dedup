# Microsoft-derived chunker conformance bundle

This bundle contains the portable C chunker, 35 small fixture inputs with the
original Microsoft C# chunk boundaries and hashes, the unmodified pinned C#
sources and an independent oracle driver. It tests BuildXL commit
`852a00e5b45ef8a8d06a2eb9040495c36243078f`. It does not establish compatibility
with a particular Windows Server release or validate the OpenZFS kernel feature.

## Check the recorded fixtures without .NET

A C11 compiler, make, Python 3 and a SHA-512 implementation are required. Apple
builds use the system CommonCrypto. FreeBSD and Linux builds require OpenSSL's
libcrypto development headers and library.

From this directory, run exactly:

```sh
python3 verify_bundle.py
cd chunker
./build.sh check
```

The check builds the C CLI and public API tests, checks all 35 original-C#
fixtures byte for byte, and independently recomputes each chunk hash with
Python's SHA-512. Python does not choose chunk boundaries. The output ends with
`35 original-C# conformance fixtures and independent SHA-512 checks PASS`.

The CLI accepts `chunkdedup-cli FILE|- [read-size]` and emits three tab-separated
columns: zero-based logical offset, chunk length and lowercase hexadecimal first
32 bytes of SHA-512. This hash is truncated SHA-512, **not SHA-512/256**.

The four `regression-*.cuts.tsv` files also retain the original C# cut-type
values. The library exposes a callback API in `chunkdedup_chunker.h`. It retains
at most 1 MiB of chunking input, accepts arbitrary input segmentation and uses
64-bit file offsets. The library alone is not a deduplicating filesystem.

## Optional rebuild of the original C# oracle

Use a .NET 8 SDK already available to you. No SDK or compiled oracle is included.
`reference/dotnet-sdk-provenance.json` identifies the verified SDK used for the
recorded local run; another compatible .NET 8 SDK may be selected explicitly.

From this directory, with `DOTNET_COMMAND` set to an absolute SDK executable:

```sh
export DOTNET_COMMAND=/absolute/path/to/dotnet
./reference/oracle/build.sh
cd chunker
python3 check_oracle_fixtures.py
./build.sh check-oracle
```

Omit the export when `dotnet` is already on PATH. The build and both test scripts
set `DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1`,
`DOTNET_GENERATE_ASPNET_CERTIFICATE=false`, disable telemetry and use a local
`reference/.dotnet-cli-home`. Keep those settings for the first SDK invocation.
The oracle's `Program.cs` supplies input and output handling; `Shims.cs` supplies
allocation, assertion, enum and formatting dependencies. Neither replaces
Microsoft's chunking algorithm. All pinned Microsoft sources remain unmodified.

`check_oracle_fixtures.py` rechecks all 35 included expected outputs directly
against Microsoft's ManagedChunker. `check-oracle` reproduces the broader
65-case run, including 20 published node-hash vectors, 30 segmentation checks,
all four regression strengths and a 5,368,709,243-byte streamed input. The large
input is not written to disk; this optional test writes approximately 13 MiB of
C and C# result tables to `chunker/test-results/`. It regenerates the 34 basic,
adversarial and regression fixtures; the separately checked shifted fixture
remains included. Running the tests may update the recorded result JSON.

## Recorded evidence and licensing

`chunker/test-results/conformance.json` records the original 65-case PASS,
including 81,921 identical chunks beyond 4 GiB. `sanitizers.txt` records the API
and regression checks under AddressSanitizer and UndefinedBehaviorSanitizer.
The extra shifted 8 MiB fixture was checked separately against the same oracle;
all 35 fixture inputs are listed in `chunker/fixture-manifest.json`.

The MIT license and Microsoft copyright notice are retained in `chunker/LICENSE`
and the adapted sources. `reference/provenance.json` supplies the pinned source
URLs and SHA-256 digests. `verify_bundle.py` checks every delivered file against
`SHA256SUMS`, then checks the original Microsoft source digests independently.
This source bundle excludes SDK downloads, binaries, object files, debug bundles,
build caches and large generated result tables.

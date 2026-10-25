# Hybrid prototype verification kit

The native OpenZFS source is the repository root. Run `../scripts/verify.sh`
from this directory, or `./scripts/verify.sh` from the repository root.

- [Status and remaining work](STATUS.md)
- [Commands, selection policy and format boundaries](HYBRID.md)
- [Catalog estimates and saved reports](CATALOG_REPORT.md)
- [Recovery and operating guidance](RECOVERY.md)
- [Recovered operational references](operations/README.md)
- [Current dirty-data gauge and isolated validation](DIRTY_DATA.md)
- [Guarded FreeBSD lab recipe](lab-setup/README.md)
- [Microsoft conformance fixtures and oracle source](conformance/README.md)
- [Native core model](native-core-mock/README.md)
- [Tested implementation identity](SOURCE.json)

`workflow/policy.json` retains all 141 immutable baseline file hashes and the
21-case original native inventory. The separate hybrid suite has 12 cases.
Historical model reports bundled with those immutable fixtures are baseline
artifacts; current revision status is in STATUS.md and VALIDATION.json.

Local verification covers the actual chunker and core model, worker batching,
parallel scan/share execution, bounded catalog selection and reporting,
parallel catalog sorting, checkpoint integrity/recovery, error propagation and
packaging. [Checkpoint and resume](CATALOG_REPORT.md#checkpoint-and-resume)
describes stable paths, metadata validation and partial-prefix reuse.
An isolated native checkpoint test completed sharing with source analysis and
sorting blocked on resume. Production checkpoint recovery remains unverified.
The native dirty-data gauge has a
separate portable direct-function test and isolated VM evidence. These focused
checks do not establish a full native-suite pass for the combined revision.

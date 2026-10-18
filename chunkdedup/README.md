# Hybrid prototype verification kit

The native OpenZFS source is the repository root. Run `../scripts/verify.sh`
from this directory, or `./scripts/verify.sh` from the repository root.

- [Status and remaining work](STATUS.md)
- [Commands, selection policy and format boundaries](HYBRID.md)
- [Guarded FreeBSD lab recipe](lab-setup/README.md)
- [Microsoft conformance fixtures and oracle source](conformance/README.md)
- [Native core model](native-core-mock/README.md)
- [Tested implementation identity](SOURCE.json)

`workflow/policy.json` retains all 141 immutable baseline file hashes and the
21-case original native inventory. The separate hybrid suite has 12 cases.
Historical model reports bundled with those immutable fixtures are baseline
artifacts; current revision status is in STATUS.md and VALIDATION.json.

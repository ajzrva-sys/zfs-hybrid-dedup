#!/usr/bin/env python3
"""Check packaged file hashes and pinned upstream source provenance."""
import hashlib,json,pathlib
root=pathlib.Path(__file__).resolve().parent
count=0
for line in (root/'SHA256SUMS').read_text().splitlines():
 digest,name=line.split('  ',1)
 path=root/name
 assert path.is_file(),f'missing packaged file: {name}'
 assert hashlib.sha256(path.read_bytes()).hexdigest()==digest,f'changed packaged file: {name}'
 count+=1
provenance=json.loads((root/'reference/provenance.json').read_text())
assert provenance['commit']=='852a00e5b45ef8a8d06a2eb9040495c36243078f'
for source in provenance['sources']:
 path=root/'reference'/source['file']
 assert hashlib.sha256(path.read_bytes()).hexdigest()==source['sha256'],source['file']
print(f'{count} packaged file digests and {len(provenance["sources"])} pinned upstream digests PASS')

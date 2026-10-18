#!/usr/bin/env python3
"""Validate checked-in original-C# outputs without requiring a .NET runtime."""
import hashlib,json,pathlib,subprocess
root=pathlib.Path(__file__).resolve().parent
manifest=json.loads((root/'fixture-manifest.json').read_text())
assert manifest['count']==35
assert len(list((root/'fixtures').glob('*.bin')))==manifest['count']
for fixture in manifest['fixtures']:
 assert hashlib.sha256((root/'fixtures'/fixture['input']).read_bytes()).hexdigest()==fixture['sha256']
 assert hashlib.sha256((root/'fixtures'/fixture['expected']).read_bytes()).hexdigest()==fixture['expected_sha256']
count=0
for path in sorted((root/'fixtures').glob('*.bin')):
 expected=path.with_suffix('.tsv')
 assert expected.exists(),path.name
 output=subprocess.check_output([str(root/'chunkdedup-cli'),str(path)])
 assert output==expected.read_bytes(),path.name
 data=path.read_bytes();end=0
 for line in output.decode().splitlines():
  offset,length,digest=line.split('\t');offset=int(offset);length=int(length)
  assert offset==end and 0<length<=131072
  assert hashlib.sha512(data[offset:offset+length]).digest()[:32].hex()==digest
  end+=length
 assert end==len(data)
 count+=1
assert count==manifest['count']
print(f'{count} original-C# conformance fixtures and independent SHA-512 checks PASS')

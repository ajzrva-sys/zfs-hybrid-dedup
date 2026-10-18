#!/usr/bin/env python3
"""Runs C and the unmodified Microsoft C# oracle; never chunks in Python."""
import argparse, hashlib, json, os, pathlib, re, struct, subprocess, time
ROOT=pathlib.Path(__file__).resolve().parent
REF=ROOT.parent/'reference'
CLI=ROOT/'chunkdedup-cli'
ORACLE=[os.environ.get('DOTNET_COMMAND','dotnet'),
 str(REF/'oracle/bin/Oracle.dll')]
ENV=os.environ.copy()
ENV.update(DOTNET_CLI_TELEMETRY_OPTOUT='1',DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1',
 DOTNET_GENERATE_ASPNET_CERTIFICATE='false',
 DOTNET_CLI_HOME=str(REF/'.dotnet-cli-home'),
 DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true')

def rows(output):
 return [(int(a),int(b),h) for a,b,h in (x.split('\t') for x in output.decode().splitlines())]
def check_coverage(output,size):
 end=0
 for offset,length,digest in rows(output):
  assert offset==end and 0<length<=131072
  assert len(digest)==64
  end+=length
 assert end==size

def basic_node_hash(output):
 # Exact <=512-leaf serializer from pinned DedupNode.cs, not a chunker.
 chunks=rows(output)
 if not chunks:return hashlib.sha512(b'').digest()[:32].hex().upper()
 if len(chunks)==1:return chunks[0][2].upper()
 assert len(chunks)<=512
 data=struct.pack('<HH',0,len(chunks)-1)
 for offset,length,digest in chunks:
  data+=b'\0'+length.to_bytes(3,'little')+bytes.fromhex(digest)
 return hashlib.sha512(data).digest()[:32].hex().upper()

def run():
 start=time.monotonic();results=[]
 fixture_dir=ROOT/'fixtures';fixture_dir.mkdir(exist_ok=True)
 published=(REF/'DedupNodeContentHasherTests.cs').read_text(encoding='utf-8-sig')
 basic=published.split('class DedupContentHasherTestsBasic64KChunkerSizes')[1].split('public void Basic64KChunkerSizes')[0]
 published_cases=[]
 for expr,digest in re.findall(r'\[InlineData\(([^,]+), "([A-F0-9]+)"\)\]',basic):
  assert re.fullmatch(r'[0-9 *+\-]+',expr)
  published_cases.append((eval(expr,{'__builtins__':{}},{}),digest))
 # Include empty/tiny cases, all published buffer limits and zero data.
 cases=[(0,n) for n,h in published_cases]+[(69519,1254972),(0,8388608)]
 for seed,size in cases:
  path=fixture_dir/f'microsoft-seed{seed}-{size}.bin'
  subprocess.run(ORACLE+['fixture',str(seed),str(size),str(path)],env=ENV,check=True)
  expected=subprocess.check_output(ORACLE+[str(path),'262144'],env=ENV)
  actual=subprocess.check_output([str(CLI),str(path),'262144'])
  assert actual==expected,(seed,size)
  check_coverage(actual,size)
  matches=[h for n,h in published_cases if n==size]
  if seed==0 and matches:assert basic_node_hash(actual)==matches[0],(size,basic_node_hash(actual),matches[0])
  (fixture_dir/(path.stem+'.tsv')).write_bytes(expected)
  results.append({'test':'microsoft_published' if matches else 'seeded','seed':seed,'size':size,'chunks':len(rows(actual))})
 # Adversarial runs at every minimum/window/maximum/1MiB boundary.
 for name,data in [('empty',b''),('tiny-zeros',b'\0'*15),('tiny-nonzero',b'hello'),
  ('zeros',b'\0'*(2097152+123)),('ff',b'\xff'*(2097152+17)),
  ('ascending',bytes(range(256))*8192+b'finish'),
  ('zero-transition',b'\0'*32768+b'x'+b'\0'*131072+b'y'+b'\0'*1048576),
  ('window-transition',b'\0'*32767+b'x'+b'\0'*16+b'y'+b'\0'*131071)]:
  path=fixture_dir/(name+'.bin');path.write_bytes(data)
  expected=subprocess.check_output(ORACLE+[str(path),'262144'],env=ENV)
  actual=subprocess.check_output([str(CLI),str(path),'262144'])
  assert actual==expected,name;check_coverage(actual,len(data))
  (fixture_dir/(name+'.tsv')).write_bytes(expected)
  results.append({'test':name,'size':len(data),'chunks':len(rows(actual))})
 # Same exact chunks for independent I/O segmentation in BOTH implementations.
 for name in ['microsoft-seed0-2097153','zero-transition','ff']:
  path=fixture_dir/(name+'.bin');expected=(fixture_dir/(name+'.tsv')).read_bytes()
  for block in [1,15,16,17,4093,32768,65536,1048575,1048576,16777216]:
   a=subprocess.check_output([str(CLI),str(path),str(block)])
   b=subprocess.check_output(ORACLE+[str(path),str(block)],env=ENV)
   assert a==b==expected,(name,block)
   results.append({'test':'stream_segmentation','fixture':name,'read_size':block})
 # Independent C# confirmation of every partial-match regression strength.
 subprocess.run([str(ROOT/'test-chunker'),str(fixture_dir)],check=True)
 for strength in range(1,5):
  path=fixture_dir/f'regression-{strength}.bin'
  expected=subprocess.check_output(ORACLE+[str(path)],env=ENV)
  actual=subprocess.check_output([str(CLI),str(path)])
  assert actual==expected
  raw=subprocess.check_output(ORACLE+['raw',str(path)],env=ENV).decode().splitlines()
  (fixture_dir/(path.stem+'.cuts.tsv')).write_text('\n'.join(raw)+'\n')
  assert int(raw[0].split('\t')[3])==6+strength
  assert int(raw[0].split('\t')[1])==32768
  (fixture_dir/(path.stem+'.tsv')).write_bytes(expected)
  results.append({'test':'partial_regression','strength':strength,'cut_type':6+strength,'size':131072})
 # Repeat Microsoft seeded 64KiB pattern beyond uint32 without a large file.
 total=5*1024**3+123
 pattern=(fixture_dir/'microsoft-seed0-65536.bin').read_bytes()*16
 for label,command in [('c',[str(CLI),'-','262144']),('csharp',ORACLE+['-','65536'])]:
  out=ROOT/'test-results'/f'large-{label}.tsv'
  with out.open('wb') as output:
   p=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=output,env=ENV)
   remaining=total
   while remaining:
    n=min(remaining,len(pattern));p.stdin.write(pattern[:n]);remaining-=n
   p.stdin.close();assert p.wait()==0
  check_coverage(out.read_bytes(),total)
 assert (ROOT/'test-results/large-c.tsv').read_bytes()==(ROOT/'test-results/large-csharp.tsv').read_bytes()
 results.append({'test':'large_stream','size':total,'chunks':len(rows((ROOT/'test-results/large-c.tsv').read_bytes()))})
 report={'status':'PASS','tests':len(results),'elapsed_seconds':round(time.monotonic()-start,3),
  'oracle':'original pinned Microsoft C# ManagedChunker + DeterministicChunker + RegressionChunking',
  'details':results}
 (ROOT/'test-results/conformance.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps({k:v for k,v in report.items() if k!='details'},indent=2))
if __name__=='__main__':run()

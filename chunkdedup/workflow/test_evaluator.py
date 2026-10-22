#!/usr/bin/env python3
"""Held-out gate tests using copies of a real passing local workflow attempt.

These deliberately mutate evidence, including recomputing superficial hashes.
They test the evaluator's refusal paths; they do not rerun native operations or
turn copied evidence into a new prototype validation claim. Original evidence,
source checkout and delivered kit remain unchanged. Generated copies remain in
--output for inspection; expect approximately 150 MiB of scratch storage.
"""
import argparse,json,shutil,hashlib,importlib.util,sys
from pathlib import Path
parser=argparse.ArgumentParser(description='Run adversarial evaluator checks on copies of actual passing local-run artifacts. No build or VM actions.')
parser.add_argument('--baseline',type=Path,required=True,help='Passing local attempt directory containing run.json and evaluation.json')
parser.add_argument('--output',type=Path,required=True,help='New scratch directory outside the baseline and kit')
parser.add_argument('--kit',type=Path,default=Path(__file__).resolve().parent.parent)
args=parser.parse_args()
base=args.baseline.resolve();top=args.output.resolve();source_kit=args.kit.resolve()
if top.exists() or top.is_relative_to(base) or top.is_relative_to(source_kit):
 parser.error('output must be new and outside the baseline and kit')
receipt=json.loads((base/'evaluation.json').read_text());manifest=json.loads((base/'run.json').read_text())
if receipt.get('status')!='PASS' or manifest.get('profile')!='local':
 parser.error('baseline must be an actual passing local attempt; this harness tests local evidence gates')
if hashlib.sha256((base/'control/evaluate.py').read_bytes()).digest()!=hashlib.sha256((Path(__file__).parent/'evaluate.py').read_bytes()).digest():
 parser.error('baseline evaluator differs; generate a fresh passing run with the current controls')
top.mkdir(parents=True);run=top/'run';kit=top/'kit'
shutil.copytree(base,run,symlinks=True);shutil.copytree(source_kit,kit,symlinks=True)
for p in (run/'control').iterdir():shutil.copyfile(p,kit/'workflow'/p.name)
m=json.loads((run/'run.json').read_text());m['source']['path']=str(run/'source');m['kit']['path']=str(kit)
(run/'run.json').write_text(json.dumps(m,indent=2))
original={};results=[]
def remember(path):
 if path not in original:original[path]=path.read_bytes()
 return path
def put(path,data):remember(path).write_text(json.dumps(data,indent=2)+'\n')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():return json.loads((run/'run.json').read_text())
def helper():return json.loads((run/'local/results.json').read_text())
def row(manifest,name):return next(x for x in manifest['stages'] if x['name']==name)
def hrow(manifest,name):return next(x for x in manifest['results'] if x['name']==name)
def both(name,mutate):
 a,b=main(),helper();mutate(row(a,name));hrow(b,name).update({k:row(a,name)[k] for k in ('status','returncode','seconds','command','log_sha256','evidence')});put(run/'run.json',a);put(run/'local/results.json',b)
def artifact_change(name,p,value):
 put(p,value);new=sha(p)
 def change(r):
  for d in r['evidence']['artifacts']:
   if d['path']==str(p.relative_to(run/'local')):d['sha256']=new
 both(name,change)
def test(name,status,mutation=None,rules=None):
 original.clear()
 if mutation:mutation()
 spec=importlib.util.spec_from_file_location('heldout_gate',run/'control/evaluate.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
 module.Gate(run,run/'control/policy.json').run()
 report=json.loads((run/'evaluation.json').read_text())
 gotrules={x['rule'] for x in report['feedback']}
 passed=report['status']==status and (not rules or set(rules)<=gotrules)
 results.append(dict(name=name,expected=status,actual=report['status'],passed=passed,rules=sorted(gotrules)))
 for path,content in original.items():path.write_bytes(content)
 print(name,report['status'],'OK' if passed else 'BAD')
def changebyte(p):remember(p).write_bytes(p.read_bytes()+b'\nchanged\n')
def fixture_rows():return json.loads((run/'local/artifacts/fixture-results.json').read_text())
test('unchanged actual local artifacts','PASS')
test('source changed after freeze','INFRA_FAILURE',lambda:changebyte(run/'source/cmd/zfs/chunkdedup_chunker.c'),['source_integrity'])
test('process log changed','INFRA_FAILURE',lambda:changebyte(run/'local/logs/chunker_api.log'),['log_integrity'])
test('assertion artifact changed','INFRA_FAILURE',lambda:changebyte(run/'local/artifacts/chunker_api/assertions.json'),['artifact_integrity'])
def missingfixture():
 rows=fixture_rows();rows.pop();artifact_change('microsoft_conformance',run/'local/artifacts/fixture-results.json',rows)
test('missing fixture with refreshed artifact hashes','TEST_FAILURE',missingfixture,['fixture_coverage'])
def incorrectfixture():
 rows=fixture_rows();record=next(x for x in rows if x['actual_chunks']);p=run/'local'/record['output']['path'];remember(p).write_text('0\t1\t'+'00'*32+'\n');record['output']['sha256']=record['actual_sha256']=sha(p);record['actual_chunks']=1;record['boundaries_match']=record['hashes_match']=True;artifact_change('microsoft_conformance',run/'local/artifacts/fixture-results.json',rows)
test('wrong actual chunk output with refreshed hashes and claimed PASS','TEST_FAILURE',incorrectfixture,['fixture_boundary_hash'])
test('PASS hides nonzero subprocess status','TEST_FAILURE',lambda:both('chunker_api',lambda r:r.update(returncode=1)),['pass_exit_code'])
def nativeblocked():
 both('chunker_api',lambda r:r.update(status='TEST_FAILURE',returncode=1));a=main();a['profile']='native';put(run/'run.json',a)
test('source test failure blocks native without misclassification','TEST_FAILURE',nativeblocked,['executed_test_failed'])
def lowassertions():
 p=run/'local/artifacts/chunker_api/assertions.json';artifact_change('chunker_api',p,{'assertions':2,'failed':0});both('chunker_api',lambda r:r['evidence']['assertions'].update(observed=2))
test('insufficient runtime assertions with refreshed hashes','TEST_FAILURE',lowassertions,['assertion_coverage'])
def hybridpolicyboundary():
 p=run/'local/artifacts/worker_batches/hybrid-policy.json';value=json.loads(p.read_text());value['cases']=12;artifact_change('worker_batches',p,value)
test('hybrid boundary test omitted with refreshed artifact hashes','TEST_FAILURE',hybridpolicyboundary,['hybrid_policy_boundary'])
def hybridpolicyheader():
 p=run/'local/artifacts/worker_batches/hybrid-policy.json';value=json.loads(p.read_text());value['header_sha256']='0'*64;artifact_change('worker_batches',p,value)
test('hybrid policy proof from another header with refreshed hashes','TEST_FAILURE',hybridpolicyheader,['hybrid_policy_boundary'])
def checkpointproof(change):
 p=run/'local/artifacts/worker_batches/checkpoint.json';value=json.loads(p.read_text());change(value);artifact_change('worker_batches',p,value)
def missingcheckpoint():
 both('worker_batches',lambda r:r['evidence'].update(artifacts=[d for d in r['evidence']['artifacts'] if not d['path'].endswith('/checkpoint.json')]))
test('checkpoint runtime proof omitted','TEST_FAILURE',missingcheckpoint,['checkpoint_resume_proof'])
test('checkpoint resume case omitted with refreshed hashes','TEST_FAILURE',lambda:checkpointproof(lambda d:d['runs'][0]['cases'].remove('complete_driver_no_rescan')),['checkpoint_resume_proof'])
test('checkpoint cached sort case omitted with refreshed hashes','TEST_FAILURE',lambda:checkpointproof(lambda d:d['runs'][1]['cases'].remove('complete_driver_cached_sort')),['checkpoint_resume_proof'])
test('checkpoint proof from another driver with refreshed hashes','TEST_FAILURE',lambda:checkpointproof(lambda d:d['source_sha256'].update({'cmd/zfs/zfs_chunkdedup_user.c':'0'*64})),['checkpoint_resume_proof'])
test('checkpoint proof from another fixture with refreshed hashes','TEST_FAILURE',lambda:checkpointproof(lambda d:d['test_sha256'].update({'checkpoint_test.c':'0'*64})),['checkpoint_resume_proof'])
def preflightproof(change):
 p=run/'local/artifacts/worker_batches/recovery-preflight.json';value=json.loads(p.read_text());change(value);artifact_change('worker_batches',p,value)
def missingpreflight():
 both('worker_batches',lambda r:r['evidence'].update(artifacts=[d for d in r['evidence']['artifacts'] if not d['path'].endswith('/recovery-preflight.json')]))
test('recovery preflight proof omitted','TEST_FAILURE',missingpreflight,['recovery_preflight_proof'])
test('preflight restart refusal case omitted with refreshed hashes','TEST_FAILURE',lambda:preflightproof(lambda d:d['cases'].remove('preserve_performance_restart')),['recovery_preflight_proof'])
test('preflight proof from another helper with refreshed hashes','TEST_FAILURE',lambda:preflightproof(lambda d:d.update(preflight_sha256='0'*64)),['recovery_preflight_proof'])

def policydowngrade():
 p=run/'control/policy.json';d=json.loads(p.read_text());d['performance']['minimum_fixture_savings']=0;put(p,d);remember(kit/'workflow/policy.json').write_bytes(p.read_bytes());a=main();a['policy_sha256']=a['helpers']['policy.json']=sha(p);put(run/'run.json',a)
test('weakened policy even with refreshed helper hashes','INFRA_FAILURE',policydowngrade,['evidence_unusable'])
def rewrittenharness():
 p=kit/'conformance/chunker/test_chunker.c';changebyte(p);m=kit/'SHA256SUMS';remember(m).write_text(m.read_text().replace(next(line for line in m.read_text().splitlines() if line.endswith('  conformance/chunker/test_chunker.c')),sha(p)+'  conformance/chunker/test_chunker.c'))
test('test harness weakened and kit checksum refreshed','INFRA_FAILURE',rewrittenharness,['immutable_baseline'])
def relabel():
 b=helper();hrow(b,'core_model')['status']='TEST_FAILURE';hrow(b,'core_model')['returncode']=1;put(run/'local/results.json',b)
test('orchestrator hides helper test failure','INFRA_FAILURE',relabel,['helper_result_consistency'])
def replay():
 b=helper();b['run_id']='archived-run';put(run/'local/results.json',b)
test('historical helper nonce replay','INFRA_FAILURE',replay,['helper_run_binding'])
def omission():
 a=main();a['stages']=[x for x in a['stages'] if x['name']!='core_model'];put(run/'run.json',a)
test('omitted required stage','INFRA_FAILURE',omission,['local_stage_coverage'])
def missingtestedinput(name):
 p=run/'local/tested-sources.json';d=json.loads(p.read_text());d['files']=[x for x in d['files'] if x['path']!=name];tested=hashlib.sha256(json.dumps(d['files'],sort_keys=True,separators=(',',':')).encode()).hexdigest();d['tested_sources_digest']=tested;put(p,d)
 a,b=main(),helper();b['tested_sources_digest']=tested;b['tested_sources_manifest']['sha256']=sha(p)
 for rows in (a['stages'],b['results']):
  for entry in rows:
   if 'tested_sources_digest' in entry.get('evidence',{}):entry['evidence']['tested_sources_digest']=tested
 put(run/'run.json',a);put(run/'local/results.json',b)
test('parallel implementation header omitted with refreshed receipt hashes','INFRA_FAILURE',lambda:missingtestedinput('cmd/zfs/chunkdedup_parallel.h'),['tested_source_coverage'])
test('checkpoint implementation header omitted with refreshed receipt hashes','INFRA_FAILURE',lambda:missingtestedinput('cmd/zfs/chunkdedup_checkpoint.h'),['tested_source_coverage'])
test('actual sorter implementation omitted with refreshed receipt hashes','INFRA_FAILURE',lambda:missingtestedinput('cmd/zfs/chunkdedup_hybrid.c'),['tested_source_coverage'])
test('dirty-data gauge header omitted with refreshed receipt hashes','INFRA_FAILURE',lambda:missingtestedinput('include/sys/dsl_pool.h'),['tested_source_coverage'])
test('dirty-data gauge implementation omitted with refreshed receipt hashes','INFRA_FAILURE',lambda:missingtestedinput('module/zfs/dsl_pool.c'),['tested_source_coverage'])
(top/'results.json').write_text(json.dumps(dict(schema=1,scope='tamper tests on copies of actual local-run evidence, no native execution claim',results=results,passed=all(x['passed'] for x in results)),indent=2)+'\n')
sys.exit(0 if all(x['passed'] for x in results) else 1)

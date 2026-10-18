#!/usr/bin/env python3
"""Rebuild and test only the retained, isolated FreeBSD chunkdedup VM.

Build mode requires the frozen source manifest supplied by the workflow. Smoke
requires a previous build from this runner with exactly the same source digest.
Each run uses new guest UFS file-vdev pools. Retained vtbd1/vtbd2 pools are never
destroyed or recreated. Crash and legacy-module testing remain separate phases.
"""
import argparse
import ast
import base64
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

LAB = 'zfs-chunkdedup-lab'
HOST_LAB = '/vm/' + LAB
HOST_DATASET = 'zroot/vm/' + LAB
STAGES = ('host_preflight', 'media_freeze', 'guest_boot', 'guest_build',
          'guest_module_load', 'guest_provenance', 'guest_regression',
          'evidence_capture', 'guest_restore', 'host_restore', 'host_state_compare')
MAX_USED = 48 << 30
MIN_FREE = 128 << 30
HYBRID_TEST_FILES = ('hybrid.py', 'hybrid_ctl.c')
GUEST_PYTHON = '/usr/local/bin/python3.12'
GUEST_ENV = 'export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin\n'
TEST_PINS = {
    'regression.py': '83093c772fd82f5bf95e11582b77c883d5fb893ac841e7bba23b2cc70707732a',
    'io_paths.c': '1fa72a31d8564ebcae72d07cb6700f39131ec228af9140125c2ccadcca15b63b',
    'debug_ctl.c': '1b75d6067f05bec43c8a343fd311bf24abc3e54ca108f187d462375bbfa44d1d',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write((json.dumps(value, indent=2, sort_keys=True) + '\n').encode())
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def validate_manifest(repo, manifest, wanted):
    repo = Path(repo).resolve()
    assert manifest['schema'] == 1
    entries = manifest['files']
    assert entries and entries == sorted(entries, key=lambda e: e['path'])
    computed = digest(json.dumps(entries, sort_keys=True, separators=(',', ':')).encode())
    assert computed == wanted == manifest['source_digest'], 'source digest mismatch'
    seen = set()
    for entry in entries:
        name = PurePosixPath(entry['path'])
        assert not name.is_absolute() and '..' not in name.parts and str(name) == entry['path']
        assert entry['path'] not in seen and re.fullmatch('[0-9a-f]{64}', entry['sha256'])
        seen.add(entry['path'])
        path = repo / entry['path']
        assert path.parent.resolve().is_relative_to(repo), 'source path leaves frozen tree'
        if entry['type'] == 'symlink':
            assert path.is_symlink() and path.resolve().is_relative_to(repo)
            actual = digest(os.readlink(path).encode())
        else:
            assert entry['type'] == 'file' and path.is_file() and not path.is_symlink()
            actual = file_digest(path)
        assert actual == entry['sha256'], 'frozen source changed: ' + entry['path']
        mode = 755 if path.lstat().st_mode & 0o111 else 644
        assert str(mode) == str(entry['mode']), 'source mode mismatch: ' + entry['path']
    return entries


def suite_cases(text):
    tree = ast.parse(text)
    main = next(node for node in tree.body if isinstance(node, ast.If) and
                '__name__' in ast.unparse(node.test))
    cases = []
    for node in main.body:
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call) and \
                isinstance(node.value.func, ast.Name) and node.value.func.id == 'check':
            cases.append((ast.literal_eval(node.value.args[0]), node.value.args[1].id))
    assert len(cases) == 21 and len({name for name, _ in cases}) == 21
    return cases


def adapt_tests(tests, run_id, guest_run, prefix):
    """Adapt destinations and explicitly retain legacy CDC mode; assertions stay intact."""
    pool = 'cdwf-' + run_id
    substitutions = {
        'regression.py': [('cdlab', pool), ('/opt/chunkdedup', prefix),
                          ("'chunkdedup', 'optimize'", "'chunkdedup', 'optimize', '--method=chunk'"),
                          ("'chunkdedup', 'watch'", "'chunkdedup', 'watch', '--method=chunk'")],
        'io_paths.c': [('/mnt/cdlab/', '/mnt/' + pool + '/')],
        'debug_ctl.c': [('strncmp(dataset, "cdlab/", 6)',
                        'strncmp(dataset, "' + pool + '/", ' + str(len(pool) + 1) + ')'),
                       ('strncmp(resolved, "/mnt/cdlab/", 11)',
                        'strncmp(resolved, "/mnt/' + pool + '/", ' + str(len('/mnt/' + pool + '/')) + ')'),
                        ('/root/lab/tests/work', guest_run + '/tests/work')],
    }
    expected_counts = {'regression.py': [64, 6, 7, 2], 'io_paths.c': [3],
                       'debug_ctl.c': [1, 1, 2]}
    adapted, record = {}, {}
    for name, changes in substitutions.items():
        assert file_digest(tests / name) == TEST_PINS[name], 'immutable native test changed: ' + name
        original = (tests / name).read_text()
        text = original
        counts = []
        for index, (old, new) in enumerate(changes):
            count = text.count(old)
            expected = expected_counts[name][index]
            assert count == expected and count > 0, 'unexpected adapter literal: ' + old
            # A fixed immutable original digest pins the admitted substitution counts.
            text = text.replace(old, new)
            counts.append({'from': old, 'to': new, 'count': count})
        if name == 'regression.py':
            assert suite_cases(original) == suite_cases(text)
        adapted[name] = text.encode()
        record[name] = {'original_sha256': digest(original.encode()),
                        'adapted_sha256': digest(text.encode()), 'substitutions': counts}
    return adapted, record, suite_cases((tests / 'regression.py').read_text())


HOST_SNAPSHOT = r'''
import hashlib,json,os,pathlib,re,subprocess,sys
if sys.flags.optimize:raise RuntimeError('Python assertions must be enabled for native host guards')
os.environ['PATH']='/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin'
assert os.geteuid()==0
def run(*args,allow=False):
 p=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
 if not allow: assert p.returncode==0,(args,p.stderr.decode())
 return p.stdout.decode()
lab='zfs-chunkdedup-lab';base=pathlib.Path('/vm')/lab
def prop(name,key): return run('zfs','get','-Hp','-o','value',key,name).strip()
config=(base/(lab+'.conf')).read_text()
values={m[1]:m[2].strip('"\'') for line in config.splitlines() if
 (m:=re.fullmatch(r'([A-Za-z0-9_]+)=(.*)',line))}
row=next(line.split() for line in run('vm','list').splitlines() if line.split() and line.split()[0]==lab)
assert row[3:5]==['1','4G'] and row[6]=='No',row
assert values['loader']=='bhyveload' and values['cpu']=='1' and values['memory']=='4G'
assert not any(re.search('network|passthru|virtfs|virtiofs|share',key,re.I) for key in values)
for i,(disk,size) in enumerate([('boot',40<<30),('test-a',8<<30),('test-b',8<<30)]):
 ds='zroot/vm/'+lab+'/'+disk
 assert values['disk'+str(i)+'_name']=='/dev/zvol/'+ds and values['disk'+str(i)+'_type']=='virtio-blk'
 assert int(prop(ds,'volsize'))==size and prop(ds,'volmode')=='dev'
 assert prop(ds,'dedup')=='off' and prop(ds,'reservation')=='0' and prop(ds,'refreservation')=='0'
assert values['disk3_type']=='ahci-cd' and pathlib.Path(values['disk3_name']).parent==base/'inputs'
assert not any(re.fullmatch(r'disk[4-9]_.*',key) for key in values)
assert prop('zroot/vm/'+lab,'mountpoint')==str(base)
assert int(prop('zroot/vm/'+lab,'quota'))==64<<30
assert prop('zroot/vm/'+lab,'dedup')=='off'
assert prop('zroot/vm/'+lab,'reservation')=='0' and prop('zroot/vm/'+lab,'refreservation')=='0'
used=int(prop('zroot/vm/'+lab,'used'));free=int(prop('zroot/vm','available'))
assert used<48<<30 and free>=128<<30,(used,free)
auto=run('sysrc','-n','vm_list',allow=True).strip().strip('"\'').split()
assert 'ALL' not in auto and lab not in auto,auto
files=[pathlib.Path('/etc/rc.conf'),pathlib.Path('/etc/crontab'),pathlib.Path('/vm/.config/system.conf')]
if pathlib.Path('/etc/rc.conf.local').exists():files.append(pathlib.Path('/etc/rc.conf.local'))
files += sorted(p for p in pathlib.Path('/vm').glob('*/*.conf') if p.parent!=base)
hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
modules=sorted(re.findall(r'\s[0-9a-f]+\s+(\S+\.ko)\b',run('kldstat')))
vms=[' '.join(line.split()) for line in run('vm','list').splitlines() if line.split() and line.split()[0] not in ['NAME',lab]]
processes=sorted(line.strip() for line in run('ps','-axo','pid,lstart,comm').splitlines() if 'transmission-' in line)
enabled=sorted(run('service','-e').splitlines());statuses={}
for path in enabled:
 name=pathlib.Path(path).name
 try:
  p=subprocess.run(['service',name,'onestatus'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=5)
  text=' '.join(p.stdout.decode(errors='replace').split())
  state='not-running' if 'not running' in text else ('running' if 'is running' in text else 'unsupported')
  statuses[name]={'returncode':p.returncode,'state':state,'pids':re.findall(r'\bpid[s]?[: ]+([0-9 ]+)',text) if state=='running' else []}
 except subprocess.TimeoutExpired:statuses[name]={'returncode':124,'state':'timed-out','pids':[]}
static={'uname':run('uname','-sr'),'version':run('zfs','version'),'configs':hashes,'modules':modules,'vms':vms,'switches':run('vm','switch','list'),'processes':processes,
 'services':{'enabled':enabled,'statuses':statuses},
 'crontab_sha256':hashlib.sha256(run('crontab','-l',allow=True).encode()).hexdigest()}
poolkeys={'guid','version','bootfs','delegation','autoreplace','cachefile','failmode','listsnapshots','autoexpand','readonly','ashift','comment','multihost','autotrim','compatibility','health'}
poolprops={}
for line in run('zpool','get','-Hp','all').splitlines():
 parts=line.split('\t')
 if len(parts)==4 and (parts[1] in poolkeys or parts[1].startswith('feature@')):
  if parts[1].startswith('feature@') and parts[2] in ['enabled','active']:parts[2]='enabled-or-active'
  poolprops[parts[0]+':'+parts[1]]=parts[2:]
static['pool_properties']=poolprops
parentkeys={'mountpoint','quota','reservation','refquota','refreservation','dedup','compression','atime','readonly','volmode','encryption','sharenfs','sharesmb','filesystem_limit','snapshot_limit'}
static['parent_properties']={parts[1]:parts[2:] for line in run('zfs','get','-Hp','all','zroot/vm').splitlines() if len(parts:=line.split('\t'))==4 and parts[1] in parentkeys}
print(json.dumps({'static':static,'lab_config':config,'lab_values':values,'lab_state':row[7:], 'used':used,'free':free}))
'''


GUEST_DRIVER = r'''
import base64,difflib,hashlib,importlib.util,json,os,pathlib,platform,re,shutil,subprocess,sys,tarfile,time
if sys.flags.optimize:raise RuntimeError('Python assertions must be enabled for native safety and tests')
MEDIA=pathlib.Path('/media/cdwf');C=json.loads((MEDIA/'config.json').read_text())
R=pathlib.Path(C['guest_run']);P=C['prefix'];POOL=C['pool'];Z=P+'/sbin/zfs';ZP=P+'/sbin/zpool'
def sha(path):
 h=hashlib.sha256()
 with open(path,'rb') as f:
  for b in iter(lambda:f.read(1<<20),b''):h.update(b)
 return h.hexdigest()
def command(*args,allow=False,timeout=180,log=None,env=None,cwd=None):
 if log:
  with open(R/log,'ab') as f:p=subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,timeout=timeout,env=env,cwd=cwd)
 else:p=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout,env=env,cwd=cwd)
 if not allow and p.returncode:
  error=RuntimeError(str(args)+' failed '+str(p.returncode)+(p.stderr.decode(errors='replace') if not log else ''))
  error.returncode=p.returncode;raise error
 return p
def save(name,value):
 path=R/name;temp=path.with_suffix('.tmp')
 with open(temp,'w') as f:json.dump(value,f,sort_keys=True);f.flush();os.fsync(f.fileno())
 os.replace(temp,path)
 fd=os.open(R,os.O_RDONLY);os.fsync(fd);os.close(fd)
def guard():
 assert platform.system()=='FreeBSD' and platform.node()=='zfs-chunkdedup-lab' and os.geteuid()==0
 assert platform.release().startswith('15.1-'),'only the retained FreeBSD15.1 lab is supported'
 assert command('/sbin/sysctl','-n','hw.ncpu').stdout.strip()==b'1'
 assert command('/sbin/ifconfig','-l').stdout.strip()==b'lo0'
 for disk,size in [('vtbd0',40<<30),('vtbd1',8<<30),('vtbd2',8<<30)]:
  assert int(command('/usr/sbin/diskinfo','/dev/'+disk).stdout.split()[2])==size
 assert command('/bin/df','-T','/root/lab').stdout.decode().splitlines()[-1].split()[1]=='ufs'
def prerequisites():
 assert sys.version_info[:2]==(3,12),'the provisioned guest Python3.12 is required'
 tools=['cc']
 if C['mode']=='build':tools+=['bash','autoconf','automake','aclocal','libtoolize','gmake','pkg-config']
 missing=[name for name in tools if shutil.which(name) is None]
 assert not missing,('guest dependencies must be provisioned before this runner',missing)
 if C['mode']=='build':assert pathlib.Path('/usr/src/sys/conf/kmod.mk').is_file(),'matching FreeBSD kernel source is required'
def imported():
 p=command('/sbin/zpool','list','-H','-o','name',allow=True)
 return [n for n in p.stdout.decode().splitlines() if n and n!='no pools available']
def export_order(names):
 # The receiver has datasets mounted beneath the source pool's mountpoint.
 # Export that foreign child filesystem before its parent mountpoint.
 return sorted(names,key=lambda name:(name not in ['cdlab-recv',POOL+'-recv'],name))
def source_check():
 entries=json.loads((MEDIA/'source-files.json').read_text())['files'];observed=[];changes=[];errors=[]
 root=(R/'source').resolve()
 for e in entries:
  path=R/'source'/e['path'];value={'path':e['path'],'type':'missing','sha256':None,'mode':None}
  try:
   assert path.parent.resolve().is_relative_to(root),'source parent escaped root'
   if path.is_symlink():
    value['type']='symlink';value['sha256']=hashlib.sha256(os.readlink(path).encode()).hexdigest()
    assert path.resolve().is_relative_to(root),'source symlink escaped root'
   elif path.is_file():value['type']='file';value['sha256']=sha(path)
   elif path.exists():value['type']='other'
   if path.exists() or path.is_symlink():value['mode']=type(e['mode'])(755 if path.lstat().st_mode&0o111 else 644)
  except (OSError,AssertionError) as error:errors.append({'path':e['path'],'error':str(error)})
  observed.append(value)
  if value!=e:changes.append({'path':e['path'],'expected':e,'observed':value})
 # Keep the complete observation before rejecting it. Failure evidence must not
 # require starting the guest again, or restoring changed source after a build.
 actual_digest=hashlib.sha256(json.dumps(observed,sort_keys=True,separators=(',',':')).encode()).hexdigest()
 receipt={'run_id':C['run_id'],'source_digest':actual_digest,'expected_source_digest':C['source_digest'],
  'files':observed,'changes':changes,'errors':errors,'observed_at':time.time()}
 save('source-observed.json',receipt)
 if changes:
  with tarfile.open(MEDIA/'source.tar.gz') as archive:
   for change in changes[:32]:
    e=change['expected'];path=R/'source'/e['path']
    if e['type']!='file' or change['observed']['type']!='file' or not path.parent.resolve().is_relative_to(root):continue
    member=archive.getmember('source/'+e['path'])
    if not member.isfile() or member.size>32768 or path.stat().st_size>32768:continue
    before=archive.extractfile(member).read();after=path.read_bytes()
    if b'\0' in before or b'\0' in after:continue
    try:
     change['text_diff']='\n'.join(list(difflib.unified_diff(before.decode().splitlines(),after.decode().splitlines(),
      fromfile='frozen/'+e['path'],tofile='observed/'+e['path']))[:200])
    except UnicodeDecodeError:pass
  save('source-observed.json',receipt)
 assert not errors,('source inspection failed',errors)
 assert observed==entries,'guest source differs from immutable manifest'
 assert actual_digest==C['source_digest']
 return {'source_digest':C['source_digest'],'files':observed}
def exact_pools(retained=None):
 retained=retained or {};pools=imported();assert set(pools)<=set([POOL,POOL+'-recv'])|set(retained),pools
 proof={}
 for name in pools:
  text=command(ZP,'status','-P',name).stdout.decode()
  if name in retained:
   assert re.findall(r'^\s+(/\S+)\s+ONLINE',text,re.M)==[retained[name]['device']],text
   assert command(ZP,'get','-H','-o','value','guid',name).stdout.decode().strip()==retained[name]['guid']
   continue
  expected=str(R/('disk-b.img' if name==POOL+'-recv' else 'disk-a.img'))
  actual=re.findall(r'^\s+(/\S+)\s+ONLINE',text,re.M)
  assert actual==[expected],(actual,expected)
  proof[name]={'path':expected,'guid':command(ZP,'get','-H','-o','value','guid',name).stdout.decode().strip()}
 return proof
def guest_meta():
 return {'hostname':platform.node(),'release':platform.release(),'cpu':1,'interfaces':['lo0'],
 'source_digest':C['source_digest'],'module_sha256':sha(R/'source/module/openzfs.ko') if C['mode']=='build' else C['build']['module_sha256'],
 'binary_sha256':sha(Z),'pools':exact_pools(),'prefix':P,'run_id':C['run_id'],'payload_nonce':C['nonce'],
 'python_assertions_enabled':not sys.flags.optimize,'python_executable':sys.executable,'python_version':platform.python_version()}
guard();phase=sys.argv[1]
if phase!='prepare':
 C=json.loads((R/'config.json').read_text());P=C['prefix'];Z=P+'/sbin/zfs';ZP=P+'/sbin/zpool'
if phase=='prepare':
 prerequisites()
 assert not R.exists();R.mkdir(parents=True)
 for name,wanted in json.loads((MEDIA/'SHA256.json').read_text()).items():assert sha(MEDIA/name)==wanted,(name,'input digest')
 save('config.json',C)
 retained={}
 for name in imported():
  assert name in ['cdlab','cdlab-recv'],'unexpected guest pool'
  text=command('/sbin/zpool','status','-P',name).stdout.decode();disk='/dev/vtbd2' if name.endswith('-recv') else '/dev/vtbd1'
  assert re.findall(r'^\s+(/\S+)\s+ONLINE',text,re.M)==[disk],text
  retained[name]={'device':disk,'guid':command('/sbin/zpool','get','-H','-o','value','guid',name).stdout.decode().strip()}
 loaded=command('/sbin/kldstat').stdout.decode();old='openzfs' if 'openzfs.ko' in loaded else 'zfs'
 oldpath='/boot/modules/openzfs.ko' if old=='openzfs' else '/boot/kernel/zfs.ko'
 mounts=[line.split()[:3] for line in command('/sbin/mount','-p').stdout.decode().splitlines()
  if line.split() and line.split()[0].split('/')[0] in retained]
 save('baseline.json',{'module':old,'path':oldpath,'sha256':sha(oldpath),'retained':retained,
  'module_version':command('/sbin/sysctl','-n','vfs.zfs.version.module').stdout.decode().strip(),
  'retained_mounts':mounts,'retained_export_order':export_order(retained),
  'loader_sha256':sha('/boot/loader.conf'),'rc_sha256':sha('/etc/rc.conf'),
  'original_binary_sha256':sha('/opt/chunkdedup/sbin/zfs')})
 shutil.copy2(oldpath,R/'baseline-module.ko')
 for name in export_order(retained):command('/sbin/zpool','export',name)
 assert not imported()
 with tarfile.open(MEDIA/'source.tar.gz') as t:t.extractall(R)
 with tarfile.open(MEDIA/'tests.tar.gz') as t:t.extractall(R)
 with tarfile.open(MEDIA/'hybrid.tar.gz') as t:
  assert {m.name for m in t.getmembers()}=={'hybrid/'+name for name in C['hybrid_tests']}
  for member in t.getmembers():
   assert member.isfile() and member.size<=1<<20
   data=t.extractfile(member).read();assert hashlib.sha256(data).hexdigest()==C['hybrid_tests'][pathlib.Path(member.name).name]
  t.extractall(R)
 save('source-before.json',source_check())
 if C['mode']=='smoke':
  B=json.loads(pathlib.Path('/root/lab/workflow/latest-build.json').read_text())
  assert B['source_digest']==C['source_digest'],'smoke cannot validate changed source'
  assert sha(B['module'])==B['module_sha256'] and sha(B['binary'])==B['binary_sha256']
  oldprefix=P;C['build']=B;C['prefix']=P=B['prefix'];Z=P+'/sbin/zfs';ZP=P+'/sbin/zpool'
  path=R/'tests/regression.py';text=path.read_text();assert text.count(oldprefix)>0;text=text.replace(oldprefix,P);path.write_text(text)
  C['adapter']['regression.py']['adapted_sha256']=hashlib.sha256(text.encode()).hexdigest()
  C['adapter']['regression.py']['substitutions'][1]['to']=P;save('config.json',C)
elif phase=='build':
 assert C['mode']=='build'
 # autoreconf honors AUTOPOINT. The bundled gettext helpers are frozen source;
 # retain them rather than copying the guest gettext package's newer versions.
 env=os.environ.copy();env.update({'LIBS':'-L/usr/local/lib -lintl','CPPFLAGS':'-I/usr/local/include','LDFLAGS':'-L/usr/local/lib','MAKE':'gmake','AUTOPOINT':'/usr/bin/true'})
 src=R/'source';started=time.time()
 def build_part(name,operation,infrastructure=True):
  receipt={'started':started,'source_digest':C['source_digest'],'phase':name,'status':'RUNNING'}
  save('build-status.json',receipt)
  try:return operation()
  except Exception as error:
   infra=infrastructure or isinstance(error,subprocess.TimeoutExpired)
   receipt.update(finished=time.time(),status='INFRA_FAILURE' if infra else 'TEST_FAILURE',
    error=str(error),operation_returncode=124 if isinstance(error,subprocess.TimeoutExpired) else getattr(error,'returncode',1))
   save('build-status.json',receipt)
   print('CDWF-BUILD-FAILURE:'+json.dumps(receipt),file=sys.stderr,flush=True)
   raise SystemExit(2 if infra else 1)
 build_part('autogen',lambda:command('./autogen.sh',cwd=src,env=env,log='build.log',timeout=300))
 build_part('bootstrap_integrity',lambda:save('source-bootstrap.json',source_check()))
 build_part('configure',lambda:command('./configure','--with-config=all','--prefix='+P,'--enable-debug','--enable-debuginfo','--disable-pyzfs','--disable-systemd',cwd=src,env=env,log='build.log',timeout=300))
 build_part('compile',lambda:command('/usr/bin/nice','-n','20','gmake','-j1',cwd=src,env=env,log='build.log',timeout=C['build_timeout']),False)
 stage=R/'stage'
 def stage_hierarchy():
  # module/Makefile.bsd beforeinstall runs BSD.debug.dist under usr/lib;
  # bsd.kmod.mk installs the module under the staged boot/modules directory.
  for path in ['usr/lib','boot/modules']:(stage/path).mkdir(parents=True,exist_ok=True)
 build_part('stage_hierarchy',stage_hierarchy)
 build_part('install',lambda:command('gmake','-j1','install','DESTDIR='+str(stage),cwd=src,env=env,log='build.log',timeout=300))
 def copy_prefix():
  assert not pathlib.Path(P).exists();shutil.copytree(stage/P.lstrip('/'),P,symlinks=True)
 build_part('copy_prefix',copy_prefix)
 build_part('source_integrity',lambda:save('source-after.json',source_check()))
 B=build_part('build_receipt',lambda:{'source_digest':C['source_digest'],'module':str(src/'module/openzfs.ko'),'module_sha256':sha(src/'module/openzfs.ko'),
    'binary':Z,'binary_sha256':sha(Z),'prefix':P,'build_run_id':C['run_id'],'started':started,'finished':time.time(),'phase':'complete','status':'PASS'})
 save('build-status.json',B);C['build']=B;save('config.json',C)
elif phase=='load':
 B=C['build'];assert B['source_digest']==C['source_digest']
 baseline=json.loads((R/'baseline.json').read_text());assert not imported()
 command('/sbin/kldunload',baseline['module']);command('/sbin/kldload',B['module'])
 assert command('/sbin/sysctl','-n','vfs.zfs.version.module').stdout.startswith(b'2.4.4-')
 assert command('/sbin/sysctl','-n','vfs.zfs.chunkdedup_test_mode').stdout.strip()==b'0'
 assert sha(B['module'])==B['module_sha256'] and sha(Z)==B['binary_sha256']
 save('loaded.json',{'run_id':C['run_id'],'payload_nonce':C['nonce'],**B,'module_version':command('/sbin/sysctl','-n','vfs.zfs.version.module').stdout.decode().strip()})
elif phase=='provenance':
 save('source-after.json',source_check());save('guest-provenance.json',guest_meta())
elif phase=='test':
 started=time.time();spec=importlib.util.spec_from_file_location('regression',R/'tests/regression.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
 m.ZFS=Z;m.ZPOOL=ZP
 def test_guard():
  m.guest_guard();assert not imported();assert not m.ROOT.exists();m.WORK.mkdir(exist_ok=False)
  m.command('/usr/bin/cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-o',m.HERE/'io_paths',m.HERE/'io_paths.c')
  return {'fresh_file_vdevs':True,'retained_devices_never_created':True}
 def test_setup():
  for suffix,file,enabled,mount in [('', 'disk-a.img',True,str(m.ROOT)),('-recv','disk-b.img',False,'none')]:
   path=R/file;fd=os.open(path,os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600);os.ftruncate(fd,1<<30);os.fsync(fd);os.close(fd)
   args=['create','-f','-d','-o','cachefile=none','-o','feature@extensible_dataset=enabled','-O','mountpoint='+mount,'-O','compression=off','-O','dedup=off','-O','atime=off','-O','quota=512M']
   if enabled:args+=['-o','feature@chunkdedup=enabled']
   m.zpool(*args,POOL+suffix,path)
  m.zfs('create','-o','mountpoint='+str(m.EXPECTED),POOL+'/expected');m.zfs('create','-o','mountpoint='+str(m.DATA),'-o','recordsize=128K','-o','chunkdedup=on','-o','quota=128M',m.SOURCE)
  return {'source':m.SOURCE,'file_vdevs':exact_pools(),'receive_feature_initially_disabled':True}
 m.guard=test_guard;m.setup=test_setup
 performance={}
 try:
  for name,func in C['cases']:
   if func=='foreground_metrics':
    fixture=next(row['detail'] for row in m.RESULTS if row['test']=='deterministic mixed fixtures')
    assert m.digest(m.BASE)==m.digest(m.DATA/'b.bin')==fixture['base_sha256']
    performance={'run_id':C['run_id'],'cache_mode':'remount_without_arc_flush','method':'sha256_logical_read',
     'ordinary_sha256':fixture['base_sha256'],'optimized_sha256':fixture['base_sha256']}
   m.check(name,getattr(m,func))
 except BaseException as e:
  save('native-result.json',{'schema':1,'run_id':C['run_id'],'payload_nonce':C['nonce'],'source_digest':C['source_digest'],
   'started':started,'finished':time.time(),'checks':m.RESULTS,'error':str(e),'original_suite_sha256':C['suite_sha256'],'adapter':C['adapter'],'guest':guest_meta(),'recovery':'not_run','legacy':'not_run'})
  raise
 save('native-result.json',{'schema':1,'run_id':C['run_id'],'payload_nonce':C['nonce'],'source_digest':C['source_digest'],'started':started,'finished':time.time(),
  'checks':m.RESULTS,'original_suite_sha256':C['suite_sha256'],'adapter':C['adapter'],'guest':guest_meta(),'recovery':'not_run','legacy':'not_run',
  'performance_context':performance})
 # Keep original21 results separate from the new hybrid suite and bind both.
 spec=importlib.util.spec_from_file_location('hybrid',R/'hybrid/hybrid.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
 try:
  hybrid=h.run(m,C,R)
 finally:
  report=R/'hybrid-result.json'
  if report.is_file():
   receipt=json.loads((R/'native-result.json').read_text());receipt['hybrid_result']={'path':report.name,'sha256':sha(report),'test_files':C['hybrid_tests']};save('native-result.json',receipt)
 if C['mode']=='build':
  latest=pathlib.Path('/root/lab/workflow/latest-build.json');temp=latest.with_suffix('.tmp');temp.write_text(json.dumps(C['build']));os.replace(temp,latest)
elif phase=='capture':
 data={}
 for name in ['source-before.json','source-bootstrap.json','source-after.json','source-observed.json','build-status.json','loaded.json','guest-provenance.json','native-result.json','hybrid-result.json','baseline.json','restore.json']:
  if (R/name).is_file():data[name]=json.loads((R/name).read_text())
 if (R/'hybrid.log').is_file():data['hybrid.log']=(R/'hybrid.log').read_text(errors='replace')[-100000:]
 if (R/'tests/work/results.json').is_file():data['regression.json']=json.loads((R/'tests/work/results.json').read_text())
 for name in ['build.log','build-console.log','regression.log']:
  if (R/name).is_file():data[name]=(R/name).read_text(errors='replace')[-100000:]
 print('CDWF-DATA:'+base64.b64encode(json.dumps(data).encode()).decode(),flush=True)
elif phase=='restore':
 baseline=json.loads((R/'baseline.json').read_text())
 # Preparation may have stopped halfway through exporting retained pools.
 # Validate any remaining original pools before exporting, then restore all
 # originally imported GUIDs after reloading the original guest module.
 ctl=ZP if pathlib.Path(ZP).is_file() else '/sbin/zpool'
 oldctl=ZP;ZP=ctl
 for name in export_order(exact_pools(baseline['retained'])):command(ZP,'export',name)
 for name in export_order(imported()):command(ZP,'export',name)
 assert not imported();ZP=oldctl
 if sha(baseline['path'])!=baseline['sha256']:shutil.copy2(R/'baseline-module.ko',baseline['path'])
 assert sha(baseline['path'])==baseline['sha256'] and sha('/boot/loader.conf')==baseline['loader_sha256'] and sha('/etc/rc.conf')==baseline['rc_sha256']
 assert sha('/opt/chunkdedup/sbin/zfs')==baseline['original_binary_sha256']
 loaded=command('/sbin/kldstat').stdout.decode()
 if 'openzfs.ko' in loaded:command('/sbin/kldunload','openzfs')
 elif 'zfs.ko' in loaded:command('/sbin/kldunload','zfs')
 command('/sbin/kldload',baseline['path'])
 for name in reversed(export_order(baseline['retained'])):
  proof=baseline['retained'][name]
  command('/sbin/zpool','import','-d',proof['device'],proof['guid'])
  assert command('/sbin/zpool','get','-H','-o','value','guid',name).stdout.decode().strip()==proof['guid']
 restored={'run_id':C['run_id'],'source_digest':C['source_digest'],'payload_nonce':C['nonce'],
 'baseline_module_sha256':baseline['sha256'],'loader_unchanged':True,'rc_unchanged':True,
 'original_binary_unchanged':True,'pools_exported':True,'retained':baseline['retained'],
 'retained_import_order':list(reversed(export_order(baseline['retained']))),'export_method':'receiver_before_source'}
 save('restore.json',restored)
 print('CDWF-RESTORE:'+base64.b64encode(json.dumps(restored).encode()).decode(),flush=True)
else:raise AssertionError('unsupported driver phase')
'''


class NativeFailure(Exception):
    def __init__(self, message, returncode=1, test=False, urgent=False):
        super().__init__(message)
        self.returncode = returncode
        self.test = test
        self.urgent = urgent


class Console:
    def __init__(self, host, path, monitor):
        self.process = subprocess.Popen(['ssh', '-tt', '-o', 'BatchMode=yes',
                                         '-o', 'StrictHostKeyChecking=yes', '-o', 'UpdateHostKeys=no',
                                         '-o', 'ConnectTimeout=15', 'root@' + host,
                                         'vm console ' + LAB],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        self.log = Path(path).open('wb')
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.pending = b''
        self.monitor = monitor
        self.last_monitor = self.last_heartbeat = time.monotonic()

    def send(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def until(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        expression = re.compile(pattern)
        while time.monotonic() < deadline:
            match = expression.search(self.pending)
            if match:
                result = self.pending[:match.end()]
                self.pending = self.pending[match.end():]
                return result, match
            now = time.monotonic()
            if now - self.last_monitor >= 30:
                self.monitor()
                self.last_monitor = now
            if now - self.last_heartbeat >= 30:
                print('Native guest stage still running; guarded host storage checked.', flush=True)
                self.last_heartbeat = now
            for key, _ in self.selector.select(1):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    raise NativeFailure('serial console closed unexpectedly', urgent=True)
                self.log.write(data)
                self.log.flush()
                self.pending += data.replace(b'\r', b'')
                if len(self.pending) > 24 << 20:
                    raise NativeFailure('serial response exceeded 24 MiB', urgent=True)
        panic = b'panic:' in self.pending or b'Fatal trap' in self.pending
        raise NativeFailure('guest kernel panic' if panic else 'serial stage timed out', test=panic, urgent=True)

    def command(self, script, nonce, timeout=180, test=False):
        marker = 'CDWF-' + nonce
        wrapper = "printf '\\n%s-BEGIN\\n'; (\n%s\n); rc=$?; printf '\\n%s-END:%%s\\n' \"$rc\"" % (marker, script, marker)
        encoded = base64.b64encode(wrapper.encode()).decode()
        # The guest tty input line limit is small; never paste a long payload.
        self.send(('CDWF_INPUT=/tmp/cdwf-' + nonce + '.b64; : > "$CDWF_INPUT"\n').encode())
        self.until(rb'LAB# ', 30)
        for offset in range(0, len(encoded), 80):
            line = "printf '%s' " + shlex.quote(encoded[offset:offset + 80]) + ' >> "$CDWF_INPUT"\n'
            assert len(line) <= 180
            self.send(line.encode())
            self.until(rb'LAB# ', 30)
        self.send(b'/usr/bin/base64 -d "$CDWF_INPUT" | /bin/sh; rm "$CDWF_INPUT"\n')
        self.until(re.escape((marker + '-BEGIN\n').encode()), timeout)
        body, match = self.until(re.escape((marker + '-END:').encode()) + rb'(\d+)\n', timeout)
        rc = int(match.group(1))
        self.until(rb'LAB# ', 30)
        if rc:
            raise NativeFailure('guest stage exited ' + str(rc), rc, test and rc != 2)
        return body[:match.start()].decode(errors='replace')

    def close(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=5)
        self.selector.close()
        self.log.close()


class Runner:
    def __init__(self, args):
        self.args = args
        self.directory = args.run_dir.resolve()
        self.directory.mkdir(parents=True, exist_ok=False)
        self.console = None
        self.before = None
        self.changed_config = False
        self.started = False
        self.locked = False
        self.rows = []
        self.outcome = 'PASS'
        self.nonce = os.urandom(16).hex()
        self.cancelled = False
        self.host_cleanup = False
        self.remote = HOST_LAB + '/workflow-' + args.run_id
        self.guest_run = '/root/lab/workflow/' + args.run_id
        self.data = {'schema': 1, 'schema_version': 1, 'run_id': args.run_id,
                     'source_digest': args.source_digest, 'mode': args.mode,
                     'profile': 'native' if args.mode == 'build' else 'native-smoke',
                     'payload_nonce': self.nonce, 'started': time.time(),
                     'results': self.rows, 'recovery': 'not_run', 'legacy': 'not_run'}

    def ssh(self, script, timeout=60, data=None):
        remote = 'env PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin /bin/sh -c ' + shlex.quote(script)
        p = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15',
                            '-o', 'StrictHostKeyChecking=yes', '-o', 'UpdateHostKeys=no',
                            'root@' + self.args.host, remote], input=data, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
        if p.returncode:
            raise NativeFailure(p.stderr.decode(errors='replace') or p.stdout.decode(errors='replace'), p.returncode)
        return p.stdout

    def snapshot(self):
        return json.loads(self.ssh('python3 -c ' + shlex.quote(HOST_SNAPSHOT)))

    def monitor(self):
        text = self.ssh("zfs get -Hp -o value used " + HOST_DATASET +
                        '; zfs get -Hp -o value available zroot/vm').splitlines()
        if len(text) != 2 or int(text[0]) >= MAX_USED or int(text[1]) < MIN_FREE:
            raise NativeFailure('host storage guard reached', urgent=True)
        # Existing Transmission process identities are a conservative workload guard.
        processes = self.ssh("ps -axo pid,lstart,comm | awk '/transmission-/ {print}'").decode().splitlines()
        if self.before and sorted(line.strip() for line in processes) != self.before['static']['processes']:
            raise NativeFailure('existing Transmission service state changed; stop testing', urgent=True)

    def artifact(self, path):
        path = Path(path)
        return {'path': str(path.relative_to(self.directory)), 'sha256': file_digest(path)}

    def save(self):
        self.data.update(outcome=self.outcome, status=self.outcome, finished=time.time())
        atomic_json(self.directory / 'results.json', self.data)
        atomic_json(self.directory / 'native.json', self.data)

    def stage(self, name, command, action, test=False):
        started = time.time()
        log = self.directory / (name + '.log')
        row = {'name': name, 'status': 'PASS', 'returncode': 0,
               'seconds': 0, 'command': command, 'log': log.name, 'evidence': {}}
        try:
            result = action()
            if isinstance(result, dict):
                row['evidence'] = result
            log.write_text(json.dumps(result, indent=2, sort_keys=True) if result is not None else 'completed\n')
        except BaseException as error:
            row['status'] = ('TEST_FAILURE' if isinstance(error, NativeFailure) and
                             error.test else 'INFRA_FAILURE')
            row['returncode'] = getattr(error, 'returncode', 1)
            row['evidence']['error'] = str(error)
            log.write_text(str(error) + '\n')
            if self.outcome != 'INFRA_FAILURE':
                self.outcome = row['status']
            raise
        finally:
            row['seconds'] = round(time.time() - started, 3)
            row['log_sha256'] = file_digest(log)
            row['evidence'].setdefault('artifacts', []).append(self.artifact(log))
            self.rows.append(row)
            self.save()

    def skip(self, name, reason):
        log = self.directory / (name + '.log')
        log.write_text(reason + '\n')
        blocked = next((row['name'] for row in reversed(self.rows)
                        if row['status'] in ['TEST_FAILURE', 'INFRA_FAILURE']), None)
        self.rows.append({'name': name, 'status': 'SKIPPED', 'returncode': 0,
                          'seconds': 0, 'command': [], 'log': log.name,
                          'log_sha256': file_digest(log), 'evidence': {'reason': reason,
                          'blocked_by': blocked, 'artifacts': [self.artifact(log)]}})

    def preflight(self):
        assert self.args.host == 'bsd', 'only the explicitly named retained bsd lab is admitted'
        for name, wanted in TEST_PINS.items():
            assert file_digest(self.args.kit / 'lab-setup/tests' / name) == wanted, 'native test pin changed'
        validate_manifest(self.args.repo.resolve(), json.loads(self.args.source_manifest.read_text()), self.args.source_digest)
        self.before = self.snapshot()
        assert self.before['lab_state'] == ['Stopped'], 'lab must be stopped before this runner'
        path = self.directory / 'host-before.json'
        atomic_json(path, self.before)
        lock = HOST_LAB + '/.native-workflow-lock'
        self.ssh('set -eu; mkdir ' + shlex.quote(lock) + '; printf %s ' +
                 shlex.quote(self.nonce) + ' > ' + shlex.quote(lock + '/owner'))
        self.locked = True
        self.ssh('mkdir ' + shlex.quote(self.remote))
        return {'artifacts': [self.artifact(path)], 'lab_initially_stopped': True,
                'storage_guard': {'max_used': MAX_USED, 'minimum_free': MIN_FREE}}

    def media(self):
        repo = self.args.repo.resolve()
        manifest = json.loads(self.args.source_manifest.read_text())
        entries = validate_manifest(repo, manifest, self.args.source_digest)
        tests = self.args.kit.resolve() / 'lab-setup/tests'
        prefix = '/opt/chunkdedup-workflow/' + self.args.run_id
        adapted, adaptation, cases = adapt_tests(tests, self.args.run_id, self.guest_run, prefix)
        media = self.directory / 'media'
        media.mkdir()
        config = {'schema': 1, 'run_id': self.args.run_id, 'nonce': self.nonce,
                  'source_digest': self.args.source_digest, 'guest_run': self.guest_run,
                  'prefix': prefix, 'mode': self.args.mode, 'pool': 'cdwf-' + self.args.run_id,
                  'build_timeout': self.args.build_timeout, 'cases': cases, 'adapter': adaptation,
                  'suite_sha256': adaptation['regression.py']['original_sha256'],
                  'hybrid_tests': {name: file_digest(tests / name) for name in HYBRID_TEST_FILES}}
        atomic_json(media / 'config.json', config)
        atomic_json(media / 'source-files.json', manifest)
        (media / 'native_driver.py').write_text(GUEST_DRIVER)
        with tarfile.open(media / 'source.tar.gz', 'w:gz') as tar:
            for e in entries:
                path = repo / e['path']
                info = tarfile.TarInfo('source/' + e['path'])
                info.mode = int(str(e['mode']), 8)
                info.mtime = 0
                if e['type'] == 'symlink':
                    info.type = tarfile.SYMTYPE
                    info.linkname = os.readlink(path)
                    tar.addfile(info)
                else:
                    data = path.read_bytes()
                    assert digest(data) == e['sha256']
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
        with tarfile.open(media / 'tests.tar.gz', 'w:gz') as tar:
            for name, data in adapted.items():
                info = tarfile.TarInfo('tests/' + name)
                info.mode = 0o644
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        with tarfile.open(media / 'hybrid.tar.gz', 'w:gz') as tar:
            for name in HYBRID_TEST_FILES:
                data = (tests / name).read_bytes()
                assert digest(data) == config['hybrid_tests'][name], 'hybrid test changed during media freeze'
                info = tarfile.TarInfo('hybrid/' + name)
                info.mode, info.size, info.mtime = 0o644, len(data), 0
                tar.addfile(info, io.BytesIO(data))
        atomic_json(media / 'SHA256.json', {p.name: file_digest(p) for p in media.iterdir() if p.is_file()})
        validate_manifest(repo, manifest, self.args.source_digest)
        iso = self.directory / 'source.iso'
        if sys.platform == 'darwin':
            args = ['hdiutil', 'makehybrid', '-quiet', '-iso', '-joliet', '-o', str(iso), str(media)]
        elif sys.platform.startswith('freebsd'):
            args = ['makefs', '-t', 'cd9660', '-o', 'rockridge', str(iso), str(media)]
        else:
            args = ['xorriso', '-as', 'mkisofs', '-R', '-J', '-o', str(iso), str(media)]
        subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
        assert iso.stat().st_size <= 256 << 20, 'native source ISO exceeds bounded upload'
        self.iso_sha = file_digest(iso)
        remote_iso = HOST_LAB + '/inputs/workflow-' + self.args.run_id + '.iso'
        self.monitor()
        self.ssh('set -eu; umask 077; set -C; cat > ' + shlex.quote(remote_iso),
                 timeout=300, data=iso.read_bytes())
        actual = self.ssh('sha256 -q ' + shlex.quote(remote_iso)).decode().strip()
        assert actual == self.iso_sha, 'uploaded source ISO changed'
        configpath = HOST_LAB + '/' + LAB + '.conf'
        backup = self.remote + '/original.conf'
        new = re.sub(r'^disk3_name=.*$', 'disk3_name="' + remote_iso + '"',
                     self.before['lab_config'], flags=re.M)
        assert new != self.before['lab_config'] and new.count('disk3_name=') == 1
        self.ssh('cp -p ' + shlex.quote(configpath) + ' ' + shlex.quote(backup))
        self.changed_config = True
        self.ssh('cat > ' + shlex.quote(configpath), data=new.encode())
        return {'iso_sha256': self.iso_sha, 'source_digest': self.args.source_digest,
                'original_suite_sha256': config['suite_sha256'], 'adapter': adaptation,
                'source_file_count': len(entries), 'artifacts': [self.artifact(media / 'config.json'),
                self.artifact(media / 'source-files.json'), self.artifact(media / 'SHA256.json'),
                self.artifact(media / 'tests.tar.gz'), self.artifact(media / 'hybrid.tar.gz'),
                self.artifact(media / 'native_driver.py')]}

    def boot(self):
        self.monitor()
        self.started = True
        self.ssh('vm start ' + LAB)
        self.console = Console(self.args.host, self.directory / 'console.log', self.monitor)
        self.console.until(rb'Connected\n', 60)
        self.console.send(b'\r')
        self.console.until(rb'LAB# ', 300)
        self.console.send(b'stty -echo\n')
        self.console.until(rb'LAB# ', 30)
        script = "set -eu\n" + GUEST_ENV + "mkdir -p /media/cdwf\nmount -t cd9660 -o ro /dev/cd0 /media/cdwf\n" + GUEST_PYTHON + " /media/cdwf/native_driver.py prepare"
        self.console.command(script, self.nonce + 'boot', 180)
        return {'serial_autologin': True, 'media_read_only': True, 'retained_pools_preserved': True,
                'python_executable': GUEST_PYTHON, 'initial_carriage_return_after_connected': True}

    def guest(self, phase, timeout=180, test=False):
        script = GUEST_ENV + GUEST_PYTHON + ' /media/cdwf/native_driver.py ' + phase
        if phase in ['build', 'test']:
            script += ' > ' + shlex.quote(self.guest_run + '/' +
                                         ('regression.log' if phase == 'test' else 'build-console.log')) + ' 2>&1'
        body = self.console.command(script, self.nonce + phase, timeout, test)
        if phase == 'restore':
            match = re.search(r'CDWF-RESTORE:([A-Za-z0-9+/=]+)', body)
            assert match, 'restoration receipt missing'
            value = json.loads(base64.b64decode(match.group(1), validate=True))
            assert value['run_id'] == self.args.run_id and value['payload_nonce'] == self.nonce
            path = self.directory / 'restore.json'
            atomic_json(path, value)
            return {'phase': phase, 'artifacts': [self.artifact(path)]}
        return {'console_response': body[-10000:], 'phase': phase}

    def capture(self):
        body = self.console.command(GUEST_ENV + GUEST_PYTHON + ' /media/cdwf/native_driver.py capture', self.nonce + 'capture', 300)
        match = re.search(r'CDWF-DATA:([A-Za-z0-9+/=]+)', body)
        assert match, 'fresh guest evidence missing'
        evidence = json.loads(base64.b64decode(match.group(1), validate=True))
        paths = []
        for name, value in evidence.items():
            assert re.fullmatch(r'[a-z-]+\.(json|log)', name), name
            path = self.directory / name
            if name.endswith('.json'):
                atomic_json(path, value)
            else:
                path.write_text(value)
            paths.append(self.artifact(path))
        self.data['guest_evidence'] = paths
        self.data['source_iso_sha256'] = self.iso_sha
        if 'native-result.json' in evidence:
            result = evidence['native-result.json']
            assert result['run_id'] == self.args.run_id and result['payload_nonce'] == self.nonce
            assert result['source_digest'] == self.args.source_digest
        return {'artifacts': paths, 'fresh_nonce': self.nonce}

    def lab_state(self, timeout=10):
        text = self.ssh('vm list', timeout=timeout).decode()
        row = next(line.split() for line in text.splitlines()
                   if line.split() and line.split()[0] == LAB)
        return row[7:]

    def restore_host(self, emergency=False):
        if self.console:
            self.console.close()
            self.console = None
        if self.started:
            if self.lab_state() != ['Stopped']:
                action = 'vm poweroff -f ' if emergency else 'vm stop '
                self.ssh(action + LAB, timeout=10)
            deadline = time.monotonic() + (10 if emergency else 20)
            while time.monotonic() < deadline:
                if self.lab_state(timeout=5) == ['Stopped']:
                    break
                time.sleep(2)
            else:
                self.ssh('vm poweroff -f ' + LAB, timeout=10)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and self.lab_state(timeout=5) != ['Stopped']:
                    time.sleep(1)
            if self.lab_state(timeout=5) != ['Stopped']:
                raise NativeFailure('lab did not stop; end-state safety failed', urgent=True)
            self.started = False
        if self.changed_config:
            configpath = HOST_LAB + '/' + LAB + '.conf'
            self.ssh('cp -p ' + shlex.quote(self.remote + '/original.conf') + ' ' + shlex.quote(configpath), timeout=10)
            self.changed_config = False
        if self.locked:
            lock = HOST_LAB + '/.native-workflow-lock'
            self.ssh('set -eu; [ "$(cat ' + shlex.quote(lock + '/owner') + ')" = ' +
                     shlex.quote(self.nonce) + ' ]; rm ' + shlex.quote(lock + '/owner') + '; rmdir ' + shlex.quote(lock), timeout=10)
            self.locked = False
        return {'guest_stopped': True, 'original_vm_config_restored': True, 'retained_run_files': True,
                'emergency_stop': emergency, 'guest_module_restoration_skipped': emergency}

    def compare(self):
        after = self.snapshot()
        atomic_json(self.directory / 'host-after.json', after)
        report = {'passed': self.before['static'] == after['static'] and
                  self.before['lab_config'] == after['lab_config'] and after['lab_state'] == ['Stopped'],
                  'lab_stopped': after['lab_state'] == ['Stopped'],
                  'vm_config_unchanged': self.before['lab_config'] == after['lab_config'],
                  'static_before': self.before['static'], 'static_after': after['static']}
        atomic_json(self.directory / 'host-comparison.json', report)
        assert report['passed'], 'existing host configuration/service state changed'
        return {'artifacts': [self.artifact(self.directory / 'host-after.json'),
                             self.artifact(self.directory / 'host-comparison.json')], 'passed': True}

    def run(self):
        failure = None
        try:
            self.stage('host_preflight', ['ssh', self.args.host, 'read-only lab/config/service checks'], self.preflight)
            self.stage('media_freeze', ['freeze verified source; create and attach read-only ISO'], self.media)
            self.stage('guest_boot', ['vm start', LAB, 'serial prepare'], self.boot)
            if self.args.mode == 'build':
                self.stage('guest_build', ['autogen; configure; nice gmake -j1; staged install'],
                           lambda: self.guest('build', self.args.build_timeout + 900, True), True)
            else:
                self.skip('guest_build', 'explicit smoke: identical-source previous build required')
            self.stage('guest_module_load', ['unload guest module; load verified test module'], lambda: self.guest('load'))
            self.stage('guest_provenance', ['verify all frozen files and loaded binary/module digests'], lambda: self.guest('provenance'))
            self.stage('guest_regression', ['21 original native assertions and separate hybrid suite on new UFS file-vdev pools'],
                       lambda: self.guest('test', self.args.test_timeout, True), True)
        except BaseException as error:
            failure = error
        finally:
            urgent = self.cancelled or getattr(failure, 'urgent', False)
            if failure and self.console and not urgent:
                try:
                    self.console.send(b'\x03')
                    self.console.until(rb'LAB# ', 15)
                except BaseException:
                    urgent = True
            if self.console and not urgent:
                try:
                    self.stage('evidence_capture', ['serial current-run JSON and bounded logs'], self.capture)
                except BaseException as error:
                    failure = failure or error
                    urgent = self.cancelled or getattr(error, 'urgent', False)
                if not urgent:
                    try:
                        self.stage('guest_restore', ['export only new file-vdev pools; restore original guest module'], lambda: self.guest('restore'))
                    except BaseException as error:
                        failure = failure or error
                        urgent = self.cancelled or getattr(error, 'urgent', False)
            if urgent:
                self.outcome = 'INFRA_FAILURE'
                self.data['emergency_stop_reason'] = str(failure or 'serial cleanup unavailable')
                done = {row['name'] for row in self.rows}
                for name in ('evidence_capture', 'guest_restore'):
                    if name not in done:
                        self.skip(name, 'emergency stop: serial cleanup skipped; original module restoration not verified')
            try:
                self.host_cleanup = True
                self.stage('host_restore', ['stop only lab; restore lab VM config; release own lock'],
                           lambda: self.restore_host(emergency=urgent or self.cancelled))
                if self.before:
                    self.stage('host_state_compare', ['compare existing host config, modules, VMs and service identities'], self.compare)
            except BaseException as error:
                failure = failure or error
            if self.cancelled:
                failure = failure or NativeFailure('native runner cancelled; lab cleanup completed', 130, urgent=True)
                self.outcome = 'INFRA_FAILURE'
            completed = {row['name'] for row in self.rows}
            for name in STAGES:
                if name not in completed:
                    self.skip(name, 'previous stage failed')
            self.save()
        if failure:
            print(str(failure), file=sys.stderr)
            return 1
        return 0


def main(argv=None):
    if sys.flags.optimize:
        raise SystemExit('Python assertions must be enabled for native safety and tests')
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--kit', type=Path, required=True)
    p.add_argument('--run-dir', type=Path, required=True)
    p.add_argument('--host', required=True)
    p.add_argument('--run-id', required=True)
    p.add_argument('--source-manifest', type=Path, required=True)
    p.add_argument('--source-digest', required=True)
    p.add_argument('--mode', choices=['build', 'smoke'], default='build')
    p.add_argument('--build-timeout', type=int, default=3600)
    p.add_argument('--test-timeout', type=int, default=1200)
    args = p.parse_args(argv)
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,39}', args.run_id):
        p.error('run-id must be 1..40 lowercase letters/digits/hyphens')
    if not re.fullmatch('[0-9a-f]{64}', args.source_digest):
        p.error('source-digest must be a SHA256 digest')
    if args.host != 'bsd':
        p.error('only the explicit retained bsd lab is supported')
    if not 60 <= args.build_timeout <= 7200 or not 60 <= args.test_timeout <= 3600:
        p.error('timeouts must be bounded: build60..7200, test60..3600 seconds')
    runner = Runner(args)
    def cancel(signum, frame):
        runner.cancelled = True
        if not runner.host_cleanup:
            raise NativeFailure('native runner cancelled by signal ' + str(signum), 130, urgent=True)
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGINT, cancel)
    return runner.run()


if __name__ == '__main__':
    sys.exit(main())

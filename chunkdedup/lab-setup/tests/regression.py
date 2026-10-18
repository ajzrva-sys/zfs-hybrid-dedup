#!/usr/bin/env python3
"""Bounded guest-only end-to-end correctness checks for native chunkdedup.
Never execute against the BSD host: every destructive target is a fresh pool
on the two exact 8 GiB guest disks, and every file stays in the guest lab.
"""
import ctypes
import errno
import hashlib
import json
import mmap
import os
from pathlib import Path
import platform
import random
import re
import signal
import shutil
import stat
import statistics
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
WORK = HERE / 'work'
ZFS = '/opt/chunkdedup/sbin/zfs'
ZPOOL = '/opt/chunkdedup/sbin/zpool'
ROOT = Path('/mnt/cdlab')
SOURCE = 'cdlab/data'
RESULTS = []
START = time.time()


def command(*args, ok=True, timeout=180, input_data=None):
    p = subprocess.run([str(x) for x in args], input=input_data,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    if ok and p.returncode:
        raise AssertionError(f'{args}: {p.returncode}: {p.stderr.decode(errors="replace")}')
    if not ok and not p.returncode:
        raise AssertionError(f'{args}: unexpectedly succeeded')
    return p


def zfs(*args, **kw):
    return command(ZFS, *args, **kw)


def zpool(*args, **kw):
    return command(ZPOOL, *args, **kw)


def check(name, action):
    t = time.time()
    try:
        detail = action()
        RESULTS.append({'test': name, 'passed': True,
                        'seconds': round(time.time() - t, 3), 'detail': detail})
        print('PASS', name, flush=True)
    except Exception as e:
        RESULTS.append({'test': name, 'passed': False,
                        'seconds': round(time.time() - t, 3), 'error': str(e)})
        save()
        print('FAIL', name, str(e), flush=True)
        raise
    save()


def save():
    if WORK.exists():
        durable_json(WORK / 'results.json', {
            'hostname': platform.node(), 'kernel': platform.release(),
            'elapsed_seconds': round(time.time() - START, 3), 'results': RESULTS,
            'limitations': ['Crash and forced power loss are orchestrated separately.',
                            'O_DIRECT requests on chunked records use buffered fallback.',
                            'Corruption and adversarial hash collision injection are separate tests.']
        })


def bytes_file(path, data):
    with open(path, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def xattr(path, value=None):
    # FreeBSD uses extattr(2), which Python's os.xattr interface may omit.
    libc = ctypes.CDLL(None, use_errno=True)
    fn = libc.extattr_get_file if value is None else libc.extattr_set_file
    fn.restype = ctypes.c_ssize_t
    fn.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
                   ctypes.c_void_p, ctypes.c_size_t]
    buffer = ctypes.create_string_buffer(128 if value is None else value)
    length = 128 if value is None else len(value)
    n = fn(os.fsencode(path), 1, b'chunkdedup-test', buffer, length)
    if n < 0:
        raise OSError(ctypes.get_errno(), 'extattr(2) failed')
    return buffer.raw[:n]


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(256 << 10), b''):
            h.update(block)
    return h.hexdigest()


def compare(path, expected):
    assert path.stat().st_size == expected.stat().st_size
    assert digest(path) == digest(expected), str(path)


def stats(dataset=SOURCE):
    text = zfs('chunkdedup', 'status', dataset).stdout.decode()
    fields = dict((k, int(v)) for k, v in re.findall(r'([a-z_]+)=(\d+)', text))
    assert set(fields) == {'files', 'records', 'chunks', 'chunk_bytes', 'logical_bytes', 'references'}, text
    return fields


def optimize(path):
    return zfs('chunkdedup', 'optimize', '--min-age', '0', path).stdout.decode()


def cold(dataset=SOURCE):
    zfs('unmount', dataset)
    zfs('mount', dataset)


def helper(op, path, *args):
    command(HERE / 'io_paths', op, path, *args)


def guest_guard():
    assert platform.system() == 'FreeBSD'
    assert platform.node() == 'zfs-chunkdedup-lab'
    assert os.geteuid() == 0
    assert command('/sbin/sysctl', '-n', 'hw.ncpu').stdout.strip() == b'1'
    assert command('/sbin/ifconfig', '-l').stdout.strip() == b'lo0'
    assert command('/sbin/sysctl', '-n', 'vfs.zfs.version.module').stdout.strip().startswith(b'2.4.4-')
    assert command('/sbin/sysctl', '-n', 'vfs.zfs.chunkdedup_test_mode').stdout.strip() == b'0'
    for disk in ['vtbd1', 'vtbd2']:
        information = command('/usr/sbin/diskinfo', f'/dev/{disk}').stdout.split()
        assert int(information[2]) == 8 * 1024 ** 3, information
    assert Path(ZFS).is_file() and Path(ZPOOL).is_file()


def pool_guard(allow_import=False):
    guest_guard()
    for pool, disk in [('cdlab', 'vtbd1'), ('cdlab-recv', 'vtbd2')]:
        p = subprocess.run([ZPOOL, 'list', '-H', '-o', 'name', pool],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        if p.returncode and allow_import:
            zpool('import', '-d', '/dev/' + disk, pool)
        text = zpool('status', '-P', pool).stdout.decode()
        devices = re.findall(r'/dev/(\S+)', text)
        assert devices == [disk], text
        assert 'state: ONLINE' in text, text
    assert set(zpool('list', '-H', '-o', 'name').stdout.decode().splitlines()) == {'cdlab', 'cdlab-recv'}
    # Crash expectations and fault restoration tokens must live on UFS.
    assert WORK.is_dir()
    assert command('/bin/df', '-T', WORK).stdout.decode().splitlines()[-1].split()[1] == 'ufs'


def guard():
    guest_guard()
    inventory = subprocess.run([ZPOOL, 'list', '-H', '-o', 'name'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=30)
    pools = inventory.stdout.decode().splitlines()
    pools = [p for p in pools if p.strip() and p != 'no pools available']
    assert inventory.returncode in [0, 1] and not pools, (
        f'Guest must have no imported pools, found {pools}: {inventory.stderr!r}')
    assert not ROOT.exists() or not list(ROOT.iterdir())
    WORK.mkdir(exist_ok=False)
    command('/usr/bin/cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-o', HERE / 'io_paths', HERE / 'io_paths.c')
    return {'devices': ['/dev/vtbd1', '/dev/vtbd2'], 'bytes_each': 8 * 1024 ** 3}


def setup():
    zpool('create', '-f', '-d', '-o', 'feature@extensible_dataset=enabled',
          '-o', 'feature@chunkdedup=enabled', '-O', 'mountpoint=/mnt/cdlab',
          '-O', 'compression=off', '-O', 'dedup=off', '-O', 'atime=off',
          '-O', 'quota=512M', 'cdlab', '/dev/vtbd1')
    zpool('create', '-f', '-d', '-o', 'feature@extensible_dataset=enabled',
          '-O', 'mountpoint=none', '-O', 'atime=off', '-O', 'quota=512M',
          'cdlab-recv', '/dev/vtbd2')
    zfs('create', '-o', 'mountpoint=/mnt/cdlab/expected', 'cdlab/expected')
    zfs('create', '-o', 'mountpoint=/mnt/cdlab/data', '-o', 'recordsize=128K',
        '-o', 'chunkdedup=on', '-o', 'quota=128M', SOURCE)
    return {'source': SOURCE, 'receive_feature_initially_disabled': True}


EXPECTED = ROOT / 'expected'
DATA = ROOT / 'data'
BASE = EXPECTED / 'base.bin'


def fixture_setup():
    r = random.Random(173)
    data = r.randbytes((4 << 20) + 123)
    bytes_file(BASE, data)
    for name in ['a.bin', 'b.bin', 'c.bin', 'd.bin']:
        bytes_file(DATA / name, data)
    bytes_file(EXPECTED / 'shifted.bin', b'S' * 7919 + data)
    bytes_file(DATA / 'shifted.bin', b'S' * 7919 + data)
    bytes_file(EXPECTED / 'zero.bin', bytes(2 << 20))
    bytes_file(DATA / 'zero.bin', bytes(2 << 20))
    bytes_file(DATA / 'small.bin', b'undersized' * 12)
    os.chmod(DATA / 'a.bin', 0o640)
    assert xattr(DATA / 'a.bin', b'preserved') == b'preserved'
    zpool('sync', 'cdlab')
    return {'total_live_fixture_bytes': sum(p.stat().st_size for p in DATA.iterdir()),
            'seed': 173, 'base_sha256': digest(BASE)}


BASE_METADATA = {}
SAVED_SPACE = {}


def sharing():
    before = DATA.joinpath('a.bin').stat()
    BASE_METADATA.update({'mode': before.st_mode, 'mtime_ns': before.st_mtime_ns,
                          'uid': before.st_uid, 'gid': before.st_gid})
    SAVED_SPACE['before'] = int(zfs('get', '-Hp', '-o', 'value', 'used', SOURCE).stdout)
    for p in DATA.iterdir():
        optimize(p)
    zpool('sync', 'cdlab')
    SAVED_SPACE['after'] = int(zfs('get', '-Hp', '-o', 'value', 'used', SOURCE).stdout)
    s = stats()
    assert s['files'] == 7 and s['records'] > 100, s
    assert s['references'] > s['chunks'] * 3, s
    assert SAVED_SPACE['after'] < SAVED_SPACE['before'], SAVED_SPACE
    after = DATA.joinpath('a.bin').stat()
    assert after.st_mode == BASE_METADATA['mode'] and after.st_mtime_ns == BASE_METADATA['mtime_ns']
    assert after.st_uid == BASE_METADATA['uid'] and after.st_gid == BASE_METADATA['gid']
    assert xattr(DATA / 'a.bin') == b'preserved'
    assert zpool('get', '-H', '-o', 'value', 'feature@chunkdedup', 'cdlab').stdout.strip() == b'active'
    cold()
    for name in ['a.bin', 'b.bin', 'c.bin', 'd.bin']:
        compare(DATA / name, BASE)
    compare(DATA / 'shifted.bin', EXPECTED / 'shifted.bin')
    compare(DATA / 'zero.bin', EXPECTED / 'zero.bin')
    assert (DATA / 'small.bin').read_bytes() == b'undersized' * 12
    return {'stats': s, 'physical_dataset_used_bytes': SAVED_SPACE}


def ordinary_io():
    for op in ['verify', 'mmap-verify', 'sendfile', 'direct']:
        cold()
        helper(op, DATA / 'b.bin', BASE)
    return {'cold_reads': ['pread', 'mmap', 'sendfile', 'O_DIRECT buffered fallback']}


def mutation_io():
    expected = bytearray(BASE.read_bytes())
    for op, offset, payload in [('pwrite', (128 << 10) - 3, b'edge-change'),
                                ('mmap-write', (256 << 10) - 2, b'mmap-edge'),
                                ('direct-write', 768 << 10, b'Z' * 4096)]:
        helper(op, DATA / 'a.bin', offset, payload.hex())
        expected[offset:offset + len(payload)] = payload
    bytes_file(EXPECTED / 'changed.bin', expected)
    cold()
    compare(DATA / 'a.bin', EXPECTED / 'changed.bin')
    compare(DATA / 'b.bin', BASE)
    zfs('chunkdedup', 'verify', DATA / 'a.bin')
    # Idempotent conversion and repair of only changed native records.
    optimize(DATA / 'a.bin')
    optimize(DATA / 'a.bin')
    cold()
    compare(DATA / 'a.bin', EXPECTED / 'changed.bin')
    compare(DATA / 'b.bin', BASE)
    return {'independent_files_preserved': True, 'mixed_record_reoptimization': True,
            'aligned_O_DIRECT_write_buffered_fallback': True}


def namespace_io():
    original = DATA / 'c.bin'
    renamed = DATA / 'renamed.bin'
    alias = DATA / 'hardlink.bin'
    os.rename(original, renamed)
    os.link(renamed, alias)
    assert renamed.stat().st_ino == alias.stat().st_ino
    compare(alias, BASE)
    os.unlink(renamed)
    with open(alias, 'rb') as f:
        os.unlink(alias)
        assert hashlib.sha256(f.read()).hexdigest() == digest(BASE)
    zpool('sync', 'cdlab')
    zfs('chunkdedup', 'verify', '-r', SOURCE)
    return {'rename': True, 'hardlink': True, 'open_unlinked_read': True}


def truncation_io():
    target = DATA / 'd.bin'
    with open(target, 'r+b') as f:
        f.truncate((3 << 20) + 37)
        f.flush(); os.fsync(f.fileno())
    assert target.read_bytes() == BASE.read_bytes()[:(3 << 20) + 37]
    optimize(target)
    with open(target, 'r+b') as f:
        f.seek(0, os.SEEK_END)
        f.write(b'appended-after-partial-record')
        f.flush(); os.fsync(f.fileno())
    assert target.read_bytes() == BASE.read_bytes()[:(3 << 20) + 37] + b'appended-after-partial-record'
    with open(target, 'r+b') as f:
        f.truncate(5 << 20)
        f.flush(); os.fsync(f.fileno())
    content = target.read_bytes()
    assert content[:(3 << 20) + 37] == BASE.read_bytes()[:(3 << 20) + 37]
    assert content[-4096:] == bytes(4096)
    # Native sparse regions stay logical zeros after optimization.
    sparse = DATA / 'sparse.bin'
    with open(sparse, 'wb') as f:
        f.write(b'head' * 1024); f.seek((2 << 20) - 4096); f.write(b'tail' * 1024)
        f.flush(); os.fsync(f.fileno())
    want = digest(sparse)
    optimize(sparse)
    cold()
    assert digest(sparse) == want
    with open(sparse, 'rb') as f:
        hole = os.lseek(f.fileno(), 0, os.SEEK_HOLE)
        assert 0 <= hole <= (2 << 20)
    return {'truncate': True, 'partial_eof_append': True, 'extension_zeros': True,
            'sparse_content': True, 'seek_hole_conservative_allowed': True}


def admission():
    for prop in ['dedup=on', 'sharenfs=on', 'sharesmb=on', 'userquota@root=1M',
                 'groupquota@wheel=1M', 'projectquota@1=1M', 'defaultuserquota=1M']:
        zfs('set', prop, SOURCE, ok=False)
    zfs('create', '-o', 'mountpoint=none', 'cdlab/dedup-conflict')
    zfs('set', 'dedup=on', 'cdlab/dedup-conflict')
    zfs('set', 'chunkdedup=on', 'cdlab/dedup-conflict', ok=False)
    zfs('set', 'dedup=off', 'cdlab/dedup-conflict')
    zfs('set', 'chunkdedup=on', 'dedup=on', 'cdlab/dedup-conflict', ok=False)
    for prop in ['sharenfs=on', 'sharesmb=on', 'defaultuserquota=1M', 'userquota@root=1M']:
        zfs('set', 'chunkdedup=on', prop, 'cdlab/dedup-conflict', ok=False)
    # Inherited property changes must not re-enable native dedup or exports
    # in descendants that already contain recipes.
    for prop in ['dedup=on', 'sharenfs=on', 'sharesmb=on']:
        zfs('set', prop, 'cdlab', ok=False)
    for prop in ['dedup', 'sharenfs', 'sharesmb']:
        zfs('inherit', prop, SOURCE, ok=False)
    assert zfs('get', '-H', '-o', 'value', 'dedup', SOURCE).stdout.strip() == b'off'
    zpool('set', 'feature@encryption=enabled', 'cdlab')
    key = WORK / 'encryption.key'
    key.write_bytes(bytes(range(32)))
    os.chmod(key, 0o600)
    zfs('create', '-o', 'encryption=aes-256-gcm', '-o', 'keyformat=raw',
        '-o', 'keylocation=file://' + str(key), '-o', 'mountpoint=none',
        '-o', 'chunkdedup=off', 'cdlab/encrypted')
    zfs('set', 'chunkdedup=on', 'cdlab/encrypted', ok=False)
    zfs('create', '-V', '8M', '-s', 'cdlab/notfiles')
    zfs('set', 'chunkdedup=on', 'cdlab/notfiles', ok=False)
    fresh = DATA / 'fresh.bin'
    bytes_file(fresh, BASE.read_bytes())
    before = stats()
    text = zfs('chunkdedup', 'optimize', fresh).stdout.decode()
    assert 'processed=0' in text and stats() == before, text
    with open(fresh, 'r+b') as competing:
        text = optimize(fresh)
        assert 'processed=0' in text and stats() == before, text
    with open(fresh, 'rb') as fd:
        mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
        text = optimize(fresh)
        assert 'processed=0' in text and stats() == before, text
        mm.close()
    os.utime(fresh, (time.time() - 4 * 86400, time.time() - 4 * 86400))
    assert 'processed=1' in zfs('chunkdedup', 'optimize', fresh).stdout.decode()
    zfs('set', 'chunkdedup=off', SOURCE)
    zfs('chunkdedup', 'optimize', '--min-age', '0', fresh, ok=False)
    cold()
    compare(fresh, BASE)
    zfs('set', 'dedup=on', SOURCE, ok=False)
    zfs('set', 'chunkdedup=on', SOURCE)
    return {'new_write_or_mapping_excluded': True, 'default_age_three_days': True,
            'off_retains_reads': True, 'quota_and_export_guards': True}


def snapshots_clones_copy():
    zfs('snapshot', SOURCE + '@retained')
    # Dataset status must mount a previously untouched snapshot, and both
    # dataset and file-path verification must address its own objset.
    snapshot = SOURCE + '@retained'
    snapfile = DATA / '.zfs/snapshot/retained/b.bin'
    snapshot_status = zfs('chunkdedup', 'status', snapshot).stdout.decode()
    assert snapshot_status.startswith(snapshot + ' ')
    zfs('chunkdedup', 'verify', snapshot)
    path_status = zfs('chunkdedup', 'status', snapfile).stdout.decode()
    assert path_status.startswith(snapshot + ' ')
    zfs('chunkdedup', 'verify', snapfile)
    zfs('chunkdedup', 'optimize', '--min-age', '0', snapshot, ok=False)
    zfs('chunkdedup', 'unoptimize', snapfile, ok=False)
    before = digest(DATA / 'b.bin')
    zfs('clone', '-o', 'mountpoint=/mnt/cdlab/clone', SOURCE + '@retained', 'cdlab/clone')
    clone = ROOT / 'clone' / 'b.bin'
    helper('pwrite', clone, 17, b'clone-alone'.hex())
    assert digest(clone) != before and digest(DATA / 'b.bin') == before
    compare(DATA / '.zfs/snapshot/retained/b.bin', BASE)
    # cp uses copy_file_range when available; mapped source rejects the native
    # blockclone path and the VFS falls back to logical reads.
    command('/bin/cp', DATA / 'b.bin', DATA / 'copied.bin')
    compare(DATA / 'copied.bin', BASE)
    zfs('destroy', 'cdlab/clone')
    return {'snapshot_bytes': True, 'snapshot_dataset_and_path_status_verify': True,
            'snapshot_mutations_rejected': True, 'clone_independence': True,
            'copy_logical_fallback': True}


def send_receive():
    zfs('snapshot', SOURCE + '@fullsend')
    stream = WORK / 'fullsend.zfs'
    with open(stream, 'wb') as out:
        p = subprocess.run([ZFS, 'send', SOURCE + '@fullsend'], stdout=out,
                           stderr=subprocess.PIPE, timeout=180)
        assert p.returncode == 0, p.stderr
    assert 0 < stream.stat().st_size < 96 << 20
    raw = stream.read_bytes()
    zfs('receive', '-u', 'cdlab-recv/data', ok=False, input_data=raw)
    zpool('set', 'feature@chunkdedup=enabled', 'cdlab-recv')
    for label, prop in [('dedup', 'dedup=on'), ('nfs', 'sharenfs=on'),
                         ('smb', 'sharesmb=on')]:
        parent = 'cdlab-recv/unsafe-' + label
        zfs('create', '-o', 'mountpoint=none', '-o', 'canmount=off', '-o', prop, parent)
        zfs('receive', '-u', parent + '/data', ok=False, input_data=raw)
        zfs('list', '-H', '-o', 'name', parent + '/data', ok=False)
    control = 'cdlab-recv/existing-control'
    zfs('create', '-o', 'mountpoint=none', control)
    control_result = subprocess.run([ZFS, 'receive', '-F', '-u', control], input=raw,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=180)
    quota_control = {'unrestricted_existing_receive_supported': control_result.returncode == 0}
    if control_result.returncode:
        quota_control['limitation'] = ('Existing-target full receive is unsupported here; '
                                      'finite-quota receive admission is code-verified, '
                                      'with no successful runtime positive control.')
        quota_control['control_error'] = control_result.stderr.decode(errors='replace')
    else:
        zfs('set', 'mountpoint=/mnt/cdlab/existing-control', control)
        if zfs('get', '-H', '-o', 'value', 'mounted', control).stdout.strip() != b'yes':
            zfs('mount', control)
        compare(ROOT / 'existing-control' / 'b.bin', BASE)
        zfs('chunkdedup', 'verify', '-r', control)
    for label, prop in [('default', 'defaultuserquota=1M'),
                         ('identity', 'userquota@root=1M')]:
        target = 'cdlab-recv/quota-' + label
        zfs('create', '-o', 'mountpoint=none', target)
        zfs('set', prop, target)
        zfs('receive', '-F', '-u', target, ok=False, input_data=raw)
        assert int(zfs('get', '-Hp', '-o', 'value', prop.split('=')[0], target).stdout) == 1 << 20
    zfs('receive', '-o', 'mountpoint=/mnt/cdlab/received', 'cdlab-recv/data', input_data=raw)
    received = ROOT / 'received'
    compare(received / 'b.bin', BASE)
    zfs('chunkdedup', 'verify', '-r', 'cdlab-recv/data')
    helper('pwrite', DATA / 'b.bin', 43, b'incremental-change'.hex())
    zfs('snapshot', SOURCE + '@incremental')
    p = command(ZFS, 'send', '-i', SOURCE + '@fullsend', SOURCE + '@incremental')
    assert len(p.stdout) < 32 << 20
    zfs('receive', 'cdlab-recv/data', input_data=p.stdout)
    compare(received / 'b.bin', DATA / 'b.bin')
    zfs('chunkdedup', 'verify', '-r', 'cdlab-recv/data')
    return {'full': True, 'incremental': True,
            'receiver_without_feature_rejected_before_installation': True,
            'unsafe_inherited_dedup_and_exports_rejected': True,
            'existing_user_and_default_quotas_rejected': True,
            'quota_receiver_positive_control': quota_control,
            'full_stream_bytes': stream.stat().st_size}


def unoptimization_and_gc():
    zfs('chunkdedup', 'unoptimize', DATA / 'a.bin')
    cold()
    compare(DATA / 'a.bin', EXPECTED / 'changed.bin')
    # Drop all live files, retaining snapshots to distinguish logical GC from
    # physical space that snapshots still own.
    for p in DATA.iterdir():
        if p.is_file():
            p.unlink()
    zpool('sync', 'cdlab')
    s = stats()
    assert s['files'] == 0 and s['records'] == 0 and s['chunks'] == 0, s
    assert s['references'] == 0 and s['chunk_bytes'] == 0, s
    compare(DATA / '.zfs/snapshot/retained/b.bin', BASE)
    used_by_snapshots = int(zfs('get', '-Hp', '-o', 'value', 'usedbysnapshots', SOURCE).stdout)
    assert used_by_snapshots > 0
    return {'stats': s, 'snapshot_retained_physical_bytes': used_by_snapshots}


def no_space():
    zfs('create', '-o', 'mountpoint=/mnt/cdlab/full', '-o', 'chunkdedup=on',
        '-o', 'recordsize=128K', '-o', 'quota=16M', 'cdlab/full')
    path = ROOT / 'full' / 'a.bin'
    bytes_file(path, BASE.read_bytes())
    optimize(path)
    before = digest(path)
    zpool('sync', 'cdlab')
    used = int(zfs('get', '-Hp', '-o', 'value', 'used', 'cdlab/full').stdout)
    zfs('set', f'quota={used}', 'cdlab/full')
    try:
        with open(path, 'r+b') as f:
            f.seek(16); f.write(b'no-space-change'); f.flush(); os.fsync(f.fileno())
    except OSError as error:
        assert error.errno in [28, 69, 122], error
    else:
        raise AssertionError('quota exhaustion unexpectedly accepted a record rewrite')
    assert digest(path) == before
    zfs('chunkdedup', 'verify', path)
    zfs('set', 'quota=16M', 'cdlab/full')
    return {'failed_write_preserved_bytes': True, 'reads_work_when_full': True}


def small_eof():
    dataset = 'cdlab/small-eof'
    directory = ROOT / 'small-eof'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'quota=8M', dataset)
    results = []
    for size in [1, 120, 4097, (40 << 10) + 13, (64 << 10) - 7,
                 (100 << 10) + 37]:
        data = random.Random(size).randbytes(size)
        source, alias = directory / f'{size}.bin', directory / f'{size}-copy.bin'
        bytes_file(source, data); bytes_file(alias, data)
        optimize(source); optimize(alias)
        cold(dataset)
        assert source.read_bytes() == data and alias.read_bytes() == data
        with open(source, 'r+b') as f:
            f.seek(size - 1); f.write(b'Z'); f.flush(); os.fsync(f.fileno())
        assert source.read_bytes() == data[:-1] + b'Z' and alias.read_bytes() == data
        optimize(source)
        with open(source, 'ab') as f:
            f.write(b'EOF'); f.flush(); os.fsync(f.fileno())
        assert source.read_bytes() == data[:-1] + b'ZEOF' and alias.read_bytes() == data
        zfs('chunkdedup', 'verify', source)
        results.append({'bytes': size, 'mapped_read_partial_write_append': True})
    return results


def atime_metadata():
    dataset = 'cdlab/metadata'
    directory = ROOT / 'metadata'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'atime=on', '-o', 'quota=8M', dataset)
    path = directory / 'aged.bin'
    data = random.Random(62).randbytes((1 << 20) + 83)
    bytes_file(path, data)
    os.chmod(path, 0o640)
    xattr(path, b'metadata-stays')
    setfacl, getfacl = shutil.which('setfacl'), shutil.which('getfacl')
    assert setfacl and getfacl
    command(setfacl, '-m', 'u:nobody:r::allow', path)
    acl = command(getfacl, '-q', path).stdout
    os.chflags(path, stat.UF_NODUMP)
    age = time.time_ns() - 4 * 86400 * 1000000000
    os.utime(path, ns=(age - 9000000000, age))
    fields = ['st_atime_ns', 'st_mtime_ns', 'st_ctime_ns', 'st_mode',
              'st_uid', 'st_gid', 'st_size', 'st_flags']
    before = path.stat()
    optimize(path)
    after = path.stat()
    assert {key: getattr(before, key) for key in fields} == {
        key: getattr(after, key) for key in fields}, (before, after)
    assert xattr(path) == b'metadata-stays'
    assert command(getfacl, '-q', path).stdout == acl
    zfs('chunkdedup', 'unoptimize', path)
    restored = path.stat()
    assert {key: getattr(before, key) for key in fields} == {
        key: getattr(restored, key) for key in fields}, (before, restored)
    assert path.read_bytes() == data
    return {'atime_enabled': True, 'optimizer_preserved': fields,
            'unoptimizer_preserved': fields, 'xattr_preserved': True,
            'custom_nfs4_acl_preserved': True, 'bsd_nodump_flag_preserved': True}


def sparse_extents():
    dataset = 'cdlab/extents'
    directory = ROOT / 'extents'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'recordsize=128K', '-o', 'quota=8M', dataset)
    path = directory / 'sparse.bin'
    with open(path, 'wb') as f:
        f.write(b'head' * 1024)
        f.seek((4 << 20) - 4096)
        f.write(b'tail' * 1024)
        f.flush(); os.fsync(f.fileno())
    zpool('sync', 'cdlab')

    def extents():
        pairs = []
        with open(path, 'rb') as f:
            offset = 0
            while offset < path.stat().st_size:
                try:
                    begin = os.lseek(f.fileno(), offset, os.SEEK_DATA)
                except OSError as error:
                    assert error.errno == errno.ENXIO, error
                    break
                end = os.lseek(f.fileno(), begin, os.SEEK_HOLE)
                assert offset <= begin < end <= path.stat().st_size
                pairs.append([begin, end])
                offset = end
        return pairs

    before_extents = extents()
    assert before_extents == [[0, 128 << 10], [(4 << 20) - (128 << 10), 4 << 20]], before_extents
    want = digest(path)
    used_before = int(zfs('get', '-Hp', '-o', 'value', 'used', dataset).stdout)
    optimize(path)
    zpool('sync', 'cdlab')
    cold(dataset)
    assert digest(path) == want
    after_extents = extents()
    assert after_extents == before_extents, (before_extents, after_extents)
    s = stats(dataset)
    assert s['records'] == 2 and s['logical_bytes'] == 256 << 10, s
    used_after = int(zfs('get', '-Hp', '-o', 'value', 'used', dataset).stdout)
    # Recipe/index metadata and boundary overlap may add bounded storage;
    # the 3.75 MiB genuine hole must never become full zero-chunk records.
    assert used_after <= used_before + (512 << 10), (used_before, used_after)
    return {'data_extents_before': before_extents, 'data_extents_after': after_extents,
            'native_records_with_recipes': s['records'],
            'physical_dataset_used_before': used_before,
            'physical_dataset_used_after': used_after}


def background_and_mmap_race():
    dataset = 'cdlab/watch'
    directory = ROOT / 'watch'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'quota=16M', dataset)
    path = directory / 'racing.bin'
    expected = bytearray(random.Random(918).randbytes((2 << 20) + 31))
    bytes_file(path, expected)
    max_rss = 0
    log = WORK / 'watch.log'
    with open(path, 'r+b') as f, mmap.mmap(f.fileno(), 0) as mm, open(log, 'wb') as out:
        watcher = subprocess.Popen([ZFS, 'chunkdedup', 'watch', '--interval', '1',
                                    '--min-age', '0', dataset], stdout=out, stderr=out)
        try:
            for i in range(12):
                offset = (128 << 10) - 2 + i * 4096
                payload = f'race-{i:03}'.encode()
                mm[offset:offset + len(payload)] = payload
                expected[offset:offset + len(payload)] = payload
                mm.flush(); os.fsync(f.fileno())
                p = command('/bin/ps', '-o', 'rss=', '-p', watcher.pid)
                max_rss = max(max_rss, int(p.stdout.strip()))
                assert watcher.poll() is None
                assert stats(dataset)['files'] == 0
                time.sleep(0.15)
        finally:
            watcher.send_signal(signal.SIGTERM)
            assert watcher.wait(timeout=20) == 0
    assert path.read_bytes() == expected
    # The next watcher pass converts the now closed file.
    with open(log, 'ab') as out:
        watcher = subprocess.Popen([ZFS, 'chunkdedup', 'watch', '--interval', '1',
                                    '--min-age', '0', dataset], stdout=out, stderr=out)
        try:
            deadline = time.monotonic() + 90
            while stats(dataset)['files'] == 0:
                assert watcher.poll() is None and time.monotonic() < deadline
                p = command('/bin/ps', '-o', 'rss=', '-p', watcher.pid)
                max_rss = max(max_rss, int(p.stdout.strip()))
                time.sleep(0.2)
        finally:
            watcher.send_signal(signal.SIGTERM)
            assert watcher.wait(timeout=20) == 0
    cold(dataset)
    assert path.read_bytes() == expected
    zfs('chunkdedup', 'verify', path)
    return {'racing_writable_mapping_deferred': True, 'closed_file_background_conversion': True,
            'watcher_peak_sampled_rss_kib': max_rss, 'log': str(log)}


def interrupted_optimization():
    dataset = 'cdlab/interrupted'
    directory = ROOT / 'interrupted'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'recordsize=128K', '-o', 'quota=32M', dataset)
    path = directory / 'candidate.bin'
    data = random.Random(451).randbytes((16 << 20) + 123)
    bytes_file(path, data)
    expected = hashlib.sha256(data).hexdigest()
    log = WORK / 'interrupted.log'
    with open(log, 'wb') as out:
        worker = subprocess.Popen([ZFS, 'chunkdedup', 'optimize', '--min-age', '0', path],
                                   stdout=out, stderr=out)
        try:
            deadline = time.monotonic() + 90
            while True:
                s = stats(dataset)
                if s['records']:
                    assert worker.poll() is None, 'conversion completed before interruption'
                    worker.kill()
                    break
                assert worker.poll() is None and time.monotonic() < deadline
                time.sleep(0.1)
        finally:
            if worker.poll() is None:
                worker.kill()
            worker.wait(timeout=30)
    mapped = stats(dataset)['records']
    assert 0 < mapped < 129, mapped
    cold(dataset)
    assert digest(path) == expected
    zfs('chunkdedup', 'verify', path)
    optimize(path)
    cold(dataset)
    assert digest(path) == expected and stats(dataset)['records'] == 129
    return {'records_at_interruption': mapped, 'resume_preserved_content': True}


def batch_tail_boundaries():
    dataset = 'cdlab/batch-tails'
    directory = ROOT / 'batch-tails'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'recordsize=128K', '-o', 'quota=48M', dataset)
    expected = {}
    for size in [(1 << 20) + 123, (16 << 20) + 123]:
        path = directory / str(size)
        data = random.Random(size).randbytes(size)
        bytes_file(path, data)
        expected[path] = hashlib.sha256(data).hexdigest()
        optimize(path)
        s = stats(dataset)
        wanted_records = sum((p.stat().st_size + (128 << 10) - 1) //
                             (128 << 10) for p in expected)
        assert s['records'] == wanted_records, (size, s, wanted_records)
        assert s['logical_bytes'] == sum(p.stat().st_size for p in expected), s
    cold(dataset)
    for path, wanted in expected.items():
        assert digest(path) == wanted
        zfs('chunkdedup', 'verify', path)
        optimize(path)
    assert stats(dataset)['records'] == 138
    return {'sizes': [p.stat().st_size for p in expected],
            'all_tail_records_mapped': True, 'idempotent_reoptimization': True}


def foreground_metrics():
    records = {}
    want = digest(BASE)
    for label, path, dataset in [('ordinary', BASE, 'cdlab/expected'),
                                 ('optimized', DATA / 'b.bin', SOURCE)]:
        times = []
        for _ in range(3):
            cold(dataset)
            start = time.monotonic()
            assert digest(path) == want
            times.append(time.monotonic() - start)
        seconds = statistics.median(times)
        records[label] = {'cold_logical_read_seconds': times,
                          'median_mib_per_second': path.stat().st_size / (1 << 20) / seconds}
    dataset = 'cdlab/metrics'
    directory = ROOT / 'metrics'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'recordsize=128K', '-o', 'quota=16M', dataset)
    original = random.Random(817).randbytes(2 << 20)
    for name in ['ordinary.bin', 'optimized.bin', 'duplicate.bin']:
        bytes_file(directory / name, original)
    optimize(directory / 'optimized.bin'); optimize(directory / 'duplicate.bin')
    for label in ['ordinary', 'optimized']:
        path = directory / (label + '.bin')
        latencies = []
        offset = (512 << 10) + 4096
        with open(path, 'r+b') as f:
            for i in range(8):
                payload = bytes([65 + i]) * 4096
                start = time.monotonic()
                assert os.pwrite(f.fileno(), payload, offset) == len(payload)
                os.fsync(f.fileno())
                latencies.append(time.monotonic() - start)
                assert os.pread(f.fileno(), len(payload), offset) == payload
            assert os.pwrite(f.fileno(), original[offset:offset + 4096], offset) == 4096
            os.fsync(f.fileno())
        assert path.read_bytes() == original
        assert (directory / 'duplicate.bin').read_bytes() == original
        records[label]['same_region_4k_pwrite_fsync_seconds'] = latencies
        records[label]['median_pwrite_fsync_seconds'] = statistics.median(latencies)
        records[label]['first_pwrite_fsync_seconds'] = latencies[0]
    records['kernel_memory'] = {}
    for key in ['kstat.zfs.misc.arcstats.size', 'vm.stats.vm.v_free_count']:
        p = subprocess.run(['/sbin/sysctl', '-n', key], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=15)
        if p.returncode == 0:
            records['kernel_memory'][key] = int(p.stdout.strip())
    return records


def debug_corruption():
    dataset = 'cdlab/faults'
    directory = ROOT / 'faults'
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'quota=8M', dataset)
    path = directory / 'target.bin'
    bytes_file(path, BASE.read_bytes()[:1 << 20])
    optimize(path)
    before = digest(path)
    ctl = HERE / 'debug_ctl'
    command('/usr/bin/cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I/opt/chunkdedup/include/libspl',
            '-I/opt/chunkdedup/include/libzfs', '-L/opt/chunkdedup/lib',
            '-Wl,-rpath,/opt/chunkdedup/lib', '-o', ctl, HERE / 'debug_ctl.c',
            '-lzfs_core', '-lnvpair')
    key = 'vfs.zfs.chunkdedup_test_mode'
    assert command('/sbin/sysctl', '-n', key).stdout.strip() == b'0'
    command(ctl, 'collision', dataset, ok=False)
    command('/sbin/sysctl', key + '=1')
    try:
        command(ctl, 'collision', dataset)
        for kind in ['recipe', 'refcount']:
            token = WORK / ('fault-' + kind + '.xdr')
            cold(dataset)
            command(ctl, 'inject', dataset, path, kind, token)
            try:
                try:
                    digest(path)
                except OSError as error:
                    assert error.errno == errno.EIO, error
                else:
                    raise AssertionError('corrupt mapping returned logical bytes')
                zfs('chunkdedup', 'verify', path, ok=False)
                zfs('chunkdedup', 'unoptimize', path, ok=False)
                # Content and refcounts must not be freed by a failed mutation.
                try:
                    with open(path, 'r+b') as f:
                        f.seek(17); f.write(b'guard'); f.flush()
                except OSError as error:
                    assert error.errno == errno.EIO, error
                else:
                    raise AssertionError('corrupt mapping accepted mutation')
            finally:
                command(ctl, 'restore', dataset, path, kind, token)
            cold(dataset)
            assert digest(path) == before
            zfs('chunkdedup', 'verify', path)
    finally:
        command('/sbin/sysctl', key + '=0')
    return {'missing_recipe_fails_closed': True, 'refcount_mismatch_fails_closed': True,
            'restoration_preserved_content': True,
            'actual_candidate_comparator_collision_checks': True,
            'cryptographic_hash_collision_not_claimed': True}


def durable_json(path, value):
    data = json.dumps(value, indent=2).encode()
    # A crash must leave either the previous complete report or the new one.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent,
                                         prefix=path.name + '.',
                                         delete=False) as f:
            temporary = Path(f.name)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def recovery_prepare(phase):
    pool_guard()
    assert phase in ['fsync', 'always', 'optimize']
    manifest = WORK / ('recovery-' + phase + '.json')
    assert not manifest.exists(), 'recovery phase already prepared'
    dataset = 'cdlab/recovery-' + phase
    directory = ROOT / ('recovery-' + phase)
    zfs('create', '-o', 'mountpoint=' + str(directory), '-o', 'chunkdedup=on',
        '-o', 'recordsize=128K', '-o', 'quota=' + ('64M' if phase == 'optimize' else '16M'), dataset)
    if phase == 'optimize':
        original = random.Random(445).randbytes((16 << 20) + 83)
        names = ['first.bin', 'second.bin']
        for name in names:
            bytes_file(directory / name, original)
        zpool('sync', 'cdlab')
        total_records = 2 * ((len(original) + (128 << 10) - 1) // (128 << 10))
        expected = {'phase': phase, 'dataset': dataset, 'manifest_filesystem': 'ufs',
                    'files': {name: hashlib.sha256(original).hexdigest() for name in names},
                    'bytes_each': len(original), 'prepared_at': time.time(),
                    'total_native_records': total_records}
        durable_json(manifest, expected)
        # Leave the worker alive after returning so the root orchestrator can
        # cut VM power during conversions. Only this guest starts a process.
        out = open(WORK / 'recovery-optimize-worker.log', 'wb')
        worker = subprocess.Popen([ZFS, 'chunkdedup', 'optimize', '-r', '--min-age', '0', dataset],
                                   stdout=out, stderr=out, start_new_session=True)
        deadline = time.monotonic() + 120
        while True:
            mapped = stats(dataset)['records']
            if mapped:
                assert mapped < total_records and worker.poll() is None
                break
            assert worker.poll() is None and time.monotonic() < deadline
            time.sleep(0.1)
        expected.update({'writes_acknowledged': True, 'worker_pid': worker.pid,
                         'records_at_marker': mapped, 'conversion_still_running': True})
        durable_json(manifest, expected)
        out.close()
        print('RECOVERY-READY', phase, str(manifest), flush=True)
        return
    original = random.Random(332).randbytes((1 << 20) + 83)
    for name in ['changed.bin', 'untouched.bin']:
        bytes_file(directory / name, original)
        optimize(directory / name)
    zfs('set', 'sync=' + ('always' if phase == 'always' else 'standard'), dataset)
    timeout_key = 'vfs.zfs.txg.timeout'
    old_timeout = int(command('/sbin/sysctl', '-n', timeout_key).stdout)
    command('/sbin/sysctl', timeout_key + '=3600')
    # A requested sync wakes a thread that may still be sleeping on its old
    # timeout, and its next timeout is computed from the new value.
    zpool('sync', 'cdlab')
    # For this tiny bounded delta there is no memory-pressure TXG trigger.
    # Keep the timeout high so the root orchestrator can cut power while only
    # ZIL durability protects the modifications. This changes only the guest.
    edits = [((128 << 10) - 3, b'log-immediate-edge'),
             ((384 << 10) + 17, random.Random(19).randbytes(64 << 10))]
    changed = bytearray(original)
    for offset, payload in edits:
        changed[offset:offset + len(payload)] = payload
    expected = {'phase': phase, 'dataset': dataset, 'old_txg_timeout': old_timeout,
                'timeout_key': timeout_key, 'manifest_filesystem': 'ufs',
                'files': {'changed.bin': hashlib.sha256(changed).hexdigest(),
                          'untouched.bin': hashlib.sha256(original).hexdigest()},
                'bytes_each': len(original), 'write_sizes': [len(x[1]) for x in edits],
                'prepared_at': time.time()}
    # Persist expectations on UFS before the protected writes.
    durable_json(manifest, expected)
    fd = os.open(directory / 'changed.bin', os.O_RDWR)
    try:
        for offset, payload in edits:
            assert os.pwrite(fd, payload, offset) == len(payload)
            os.fsync(fd)
    finally:
        os.close(fd)
    expected['writes_acknowledged'] = True
    durable_json(manifest, expected)
    print('RECOVERY-READY', phase, str(manifest), flush=True)
    # No pool sync, snapshot, unmount or verify follows this marker.


def recovery_check(phase):
    pool_guard(allow_import=True)
    assert phase in ['fsync', 'always', 'optimize']
    manifest = WORK / ('recovery-' + phase + '.json')
    expected = json.loads(manifest.read_text())
    assert expected['phase'] == phase and expected['writes_acknowledged']
    dataset = expected['dataset']
    assert dataset == 'cdlab/recovery-' + phase
    directory = ROOT / ('recovery-' + phase)
    for name, want in expected['files'].items():
        path = directory / name
        assert path.stat().st_size == expected['bytes_each'] and digest(path) == want, path
        zfs('chunkdedup', 'verify', path)
    if phase != 'optimize':
        command('/sbin/sysctl', expected['timeout_key'] + '=' + str(expected['old_txg_timeout']))
    else:
        expected['records_after_crash'] = stats(dataset)['records']
        assert 0 <= expected['records_after_crash'] < expected['total_native_records'], (
            'power cut happened after conversion finished', expected)
    expected['recovery_checked_at'] = time.time()
    expected['recovery_passed'] = True
    durable_json(manifest, expected)
    print('RECOVERY-CHECK-PASS', phase, flush=True)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] in ['--recovery-prepare', '--recovery-check']:
        (recovery_prepare if sys.argv[1] == '--recovery-prepare' else recovery_check)(sys.argv[2])
        sys.exit(0)
    assert len(sys.argv) == 1, 'unsupported test mode'
    check('guest guard and helper build', guard)
    check('isolated pools and bounded datasets', setup)
    check('deterministic mixed fixtures', fixture_setup)
    check('sharing, saved physical space and metadata', sharing)
    check('cold native read paths', ordinary_io)
    check('foreground logical read and memory metrics', foreground_metrics)
    check('partial writes, mmap writes and idempotent reoptimization', mutation_io)
    check('rename, hardlink and open-unlinked lifecycle', namespace_io)
    check('truncate, partial EOF append, extension and sparse data', truncation_io)
    check('age, busy file, property, quota and export admission', admission)
    check('tiny files and 40-100 KiB partial EOF records', small_eof)
    check('atime-on conversion preserves file timestamps and metadata', atime_metadata)
    check('sparse extents and bounded physical hole allocation', sparse_extents)
    check('background watching and writable mmap races', background_and_mmap_race)
    check('short tails crossing 1 MiB conversion batches', batch_tail_boundaries)
    check('interrupted conversion and resumable mixed mappings', interrupted_optimization)
    check('debug-only collision comparator and corrupt mapping rejection', debug_corruption)
    check('snapshot, clone independence and logical copy', snapshots_clones_copy)
    check('full/incremental replication and receiver feature rejection', send_receive)
    check('unoptimization, live reference GC and snapshot retention', unoptimization_and_gc)
    check('quota exhaustion preserves logical reads', no_space)
    print('REGRESSION-COMPLETE', flush=True)

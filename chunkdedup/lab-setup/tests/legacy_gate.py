#!/usr/bin/env python3
"""Guest-only rejection tests using the original FreeBSD 2.4.2 ZFS tools.
Root orchestrates the VM module switch; this script performs no host actions.
"""
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import time

HERE = Path(__file__).resolve().parent
WORK = HERE / 'work'
ZFS = '/sbin/zfs'
ZPOOL = '/sbin/zpool'
POOL = 'cdlab-legacy-gate'
DISK = WORK / 'legacy-gate-disk.bin'
RESULTS = []


def run(*args, timeout=90, stdin=None):
    return subprocess.run([str(x) for x in args], stdin=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)


def must(*args, **kwargs):
    p = run(*args, **kwargs)
    assert p.returncode == 0, (args, p.returncode, p.stdout, p.stderr)
    return p.stdout


def pools():
    p = run(ZPOOL, 'list', '-H', '-o', 'name')
    assert p.returncode in [0, 1], p.stderr
    return [x for x in p.stdout.decode().splitlines() if x and x != 'no pools available']


def guard():
    assert platform.system() == 'FreeBSD' and platform.node() == 'zfs-chunkdedup-lab'
    assert os.geteuid() == 0
    assert must('/sbin/sysctl', '-n', 'hw.ncpu').strip() == b'1'
    assert must('/sbin/ifconfig', '-l').strip() == b'lo0'
    module = must('/sbin/sysctl', '-n', 'vfs.zfs.version.module').decode().strip()
    assert module.startswith('2.4.2') and 'FreeBSD' in module, module
    modules = must('/sbin/kldstat').decode()
    assert 'openzfs.ko' not in modules and 'zfs.ko' in modules, modules
    for disk in ['vtbd1', 'vtbd2']:
        information = must('/usr/sbin/diskinfo', '/dev/' + disk).split()
        assert int(information[2]) == 8 * 1024 ** 3, information
    assert pools() == []
    assert WORK.is_dir()
    assert must('/bin/df', '-T', WORK).decode().splitlines()[-1].split()[1] == 'ufs'
    assert not DISK.exists()
    assert HERE == Path('/root/lab/tests')
    return {'module': module, 'kernel': platform.release(), 'hostname': platform.node()}


def pool_rejection(name, disk):
    p = run(ZPOOL, 'import', '-N', '-o', 'readonly=on', '-d', '/dev/' + disk, name)
    message = (p.stdout + p.stderr).decode(errors='replace')
    assert p.returncode != 0, 'old software unexpectedly imported ' + name
    assert 'org.ajzrva:chunkdedup' in message and 'support' in message.lower(), message
    assert pools() == []
    return {'test': 'stock read-only import rejection', 'pool': name,
            'disk': '/dev/' + disk, 'exit_code': p.returncode, 'output': message}


def receive_rejection():
    stream = WORK / 'fullsend.zfs'
    assert stream.is_file() and 0 < stream.stat().st_size < 96 << 20
    assert pools() == []
    fd = os.open(DISK, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        os.ftruncate(fd, 128 << 20)
        os.fsync(fd)
        inode = os.fstat(fd).st_ino
    finally:
        os.close(fd)
    created = False
    try:
        must(ZPOOL, 'create', '-f', '-o', 'cachefile=none', '-O', 'mountpoint=none',
             '-O', 'quota=64M', POOL, DISK)
        created = True
        assert pools() == [POOL]
        text = must(ZPOOL, 'status', '-P', POOL).decode()
        devices = re.findall(r'(/\S+)', text)
        assert devices == [str(DISK)], text
        with open(stream, 'rb') as source:
            p = run(ZFS, 'receive', '-u', POOL + '/received', stdin=source, timeout=120)
        message = (p.stdout + p.stderr).decode(errors='replace')
        assert p.returncode != 0, 'old software accepted the required stream feature'
        assert 'support' in message.lower() and ('stream' in message.lower() or
                                                'feature' in message.lower()), message
        assert run(ZFS, 'list', '-H', '-o', 'name', POOL + '/received').returncode != 0
        return {'test': 'stock required stream feature rejection', 'exit_code': p.returncode,
                'output': message, 'sparse_ufs_disk_bytes': 128 << 20,
                'child_dataset_installed': False}
    finally:
        if created:
            assert pools() == [POOL]
            text = must(ZPOOL, 'status', '-P', POOL).decode()
            assert re.findall(r'(/\S+)', text) == [str(DISK)], text
            must(ZPOOL, 'destroy', POOL)
        assert pools() == []
        assert DISK.stat().st_ino == inode and DISK.stat().st_uid == 0
        DISK.unlink()


def save(metadata, success, error=None):
    path = WORK / 'legacy-gate-results.json'
    payload = {'metadata': metadata, 'passed': success, 'results': RESULTS,
               'checked_at': time.time(), 'error': error}
    with open(path, 'w') as out:
        json.dump(payload, out, indent=2)
        out.flush(); os.fsync(out.fileno())
    fd = os.open(WORK, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


if __name__ == '__main__':
    metadata = guard()
    try:
        for name, disk in [('cdlab', 'vtbd1'), ('cdlab-recv', 'vtbd2')]:
            RESULTS.append(pool_rejection(name, disk))
            save(metadata, False)
            print('PASS legacy read-only import rejects', name, flush=True)
        RESULTS.append(receive_rejection())
        save(metadata, True)
        print('LEGACY-GATE-COMPLETE', flush=True)
    except Exception as error:
        save(metadata, False, str(error))
        raise

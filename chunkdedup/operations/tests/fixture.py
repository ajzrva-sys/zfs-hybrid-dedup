"""Strict command double: no ZFS, mount, sysctl or identity command is forwarded."""
import json
import os
from pathlib import Path
import signal
import sys
import time

root = Path(os.environ['CONTROLLER_FIXTURE'])
cfg = json.loads((root / 'config.json').read_text())
name, args = Path(sys.argv[0]).name, sys.argv[1:]
with (root / 'commands.jsonl').open('a') as log:
    log.write(json.dumps([name, *args]) + '\n')


def emit(value):
    print(value)
    sys.exit(0)


def refuse():
    (root / 'fixture-error').write_text(repr([name, *args]))
    print('Unexpected fixture command: ' + repr([name, *args]), file=sys.stderr)
    sys.exit(99)


def load():
    return json.loads((root / 'model.json').read_text())


def save(model):
    path = root / 'model.new'
    path.write_text(json.dumps(model))
    path.replace(root / 'model.json')


def unhealthy(key, default):
    # A worker-created marker makes watchdog transitions deterministic.
    if (root / 'worker-ready').exists():
        return cfg.get('guard_' + key, cfg.get(key, default))
    return cfg.get(key, default)


if name == 'hostname' and args == ['-s']:
    emit(cfg.get('host', 'fixture-host'))
if name == 'kenv' and args == ['-q', 'smbios.system.uuid']:
    emit(cfg.get('uuid', 'fixture-uuid'))
if name == 'sysctl' and args[0] == '-n':
    emit(cfg.get(args[1], {'vfs.zfs.version.module': '2.4.4-1',
                         'vfs.zfs.chunkdedup_test_mode': '0'}[args[1]]))
if name == 'sha256' and args[0] == '-q':
    emit('bad' if cfg.get('bad_hash') == Path(args[1]).name else 'admitted')
if name == 'stat' and args[0] == '-f':
    fmt, path = args[1], Path(args[2])
    if cfg.get('bad_stat') == path.name:
        emit('999')
    if fmt == '%z':
        emit(path.stat().st_size)
    if fmt == '%u':
        emit(0)
    if fmt == '%Lp':
        emit('700')
    if fmt == '%u:%Lp':
        emit('0:700')
    if fmt == '%u:%Lp:%l':
        emit('0:600:1')
if name == 'df' and args == ['-k', '/tmp']:
    if cfg.get('df_error'):
        sys.exit(1)
    emit('Filesystem 1024-blocks Used Available Capacity Mounted\nfixture 999999999 0 '
         + str(unhealthy('tmp', 268435456)) + ' 0% /tmp')
if name == 'sleep' and args == ['15']:
    time.sleep(0.03)
    sys.exit(0)
if name == 'pgrep':
    sys.exit(1)  # Fixture worker has no sort children.
if name == 'nice' and args[:2] == ['-n', '20']:
    os.environ.pop('LD_PRELOAD', None)
    os.execv(args[2], args[2:])
if name == 'mount':
    if args == ['-p']:
        emit('zroot/ROOT/zfs-hybrid-candidate / zfs rw 0 0')
    model = load()
    if args[:2] == ['-t', 'zfs']:
        model[args[-2]]['mounted'] = 'yes'
    elif args[:2] == ['-u', '-o']:
        if cfg.get('remount_error') and args[2] == 'ro':
            sys.exit(1)
        for row in model.values():
            if row['point'] == args[-1]:
                row['mode'] = args[2]
    else:
        refuse()
    save(model)
    sys.exit(0)
if name == 'zpool':
    if args[0] in ('sync', 'status'):
        emit('fixture ONLINE')
    if args[0] == 'get':
        prop, pool = args[-2:]
        if args[args.index('-o') + 1] != 'value':
            emit('fixture\tfree\t274877906944')
        if prop == 'guid':
            emit(cfg.get(pool + '_guid', {'targetpool': '201', 'scratchpool': '202'}[pool]))
        if prop == 'health':
            emit(unhealthy(pool + '_health', 'ONLINE'))
        if prop == 'feature@chunkdedup':
            emit(cfg.get('feature', 'enabled'))
        if prop == 'free':
            emit(unhealthy('free', 274877906944))
if name == 'zfs':
    model = load()
    if args[:2] == ['chunkdedup', 'optimize']:
        (root / 'worker-ready').write_text(str(os.getpid()))
        if cfg.get('block_worker'):
            signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
            while True:
                time.sleep(0.02)
        sys.exit(cfg.get('worker_rc', 0))
    if args[:2] == ['chunkdedup', 'status']:
        emit('fixture accounting unchanged')
    if args[0] == 'unmount':
        if cfg.get('unmount_error'):
            sys.exit(1)
        model[args[1]]['mounted'] = 'no'
        save(model)
        sys.exit(0)
    if args[0] in ('set', 'inherit'):
        ds = args[-1]
        if args[0] == 'set':
            prop, value = args[-2].split('=', 1)
            source = 'local'
        else:
            prop = args[-2]
            original = model[ds]['original'][prop]
            value, source = original
            expected = ['inherit', '-S', prop, ds] if source == 'received' else ['inherit', prop, ds]
            if args != expected:
                refuse()
        if cfg.get('restore_error') and prop == 'readonly' and value == 'on' and (root / 'worker-ready').exists():
            sys.exit(1)
        model[ds][prop] = [value, source]
        save(model)
        sys.exit(0)
    if args[0] == 'list':
        emit('\n'.join(ds + '\t' + row['mounted'] for ds, row in model.items()))
    if args[0] == 'get':
        prop, ds = args[-2:]
        if args[args.index('-o') + 1] == 'value':
            if ds == 'scratchpool/dedupe-catalogs':
                emit({'guid': cfg.get('scratch_dataset_guid', '203'), 'mounted': 'yes',
                      'mountpoint': str(root / 'mnt/scratchpool/dedupe-catalogs')}[prop])
            if prop == 'guid':
                emit(cfg.get('dataset_guid', model[ds]['guid']))
            if prop == 'mountpoint':
                emit(model[ds]['point'])
            if prop == 'mounted':
                emit(model[ds]['mounted'])
            emit(model[ds][prop][0])
        if prop == 'readonly,chunkdedup,canmount,mountpoint':
            emit('\n'.join('\t'.join([ds, p, *row[p], '-'])
                           for ds, row in model.items()
                           for p in ('readonly', 'chunkdedup', 'canmount', 'mountpoint')))
        if prop == 'used,referenced,logicalreferenced,usedbysnapshots':
            emit(ds + '\tused\t1')
refuse()

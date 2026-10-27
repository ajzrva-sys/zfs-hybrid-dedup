#!/usr/bin/env python3
"""Execute an adapted temporary controller with strict command doubles; no pool I/O."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
CONTROLLER = HERE.parent / 'run.sh.example'


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='controller-fixture-', dir='/tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.run = self.root / 'run'
        self.run.mkdir()
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for name in ('awk', 'cat', 'cmp', 'date', 'env', 'ln', 'mkdir', 'mv',
                     'sort', 'touch', 'tr', 'wc'):
            target = shutil.which(name)
            self.assertIsNotNone(target, name)
            (self.bin / name).symlink_to(target)
        fixture = '#!' + sys.executable + '\n' + (HERE / 'fixture.py').read_text()
        for name in ('hostname', 'kenv', 'sysctl', 'sha256', 'stat', 'df', 'sleep',
                     'pgrep', 'nice', 'mount', 'zpool', 'zfs'):
            path = self.bin / name
            path.write_text(fixture)
            path.chmod(0o755)
        text = CONTROLLER.read_text()
        # Only configuration/path bindings change. Guards and control flow are executed intact.
        bindings = {
            'export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin':
                'export PATH=' + shlex.quote(str(self.bin)),
            '/var/tmp/dedupe-controller': str(self.run),
            '/var/tmp/dedupe-checkpoints': str(self.root / 'checkpoints'),
            '/mnt': str(self.root / 'mnt'),
            '/targetpool': str(self.root / 'targetpool'),
            '/usr/bin/nice': str(self.bin / 'nice'),
            '/sbin/zfs': str(self.bin / 'zfs'),
            '/sbin/zpool': str(self.bin / 'zpool'),
            'REPLACE_WITH_REVIEWED_HOST_UUID': 'fixture-uuid',
            'REPLACE_WITH_REVIEWED_HOST': 'fixture-host',
            'REPLACE_WITH_RECOVERY_DATASET_GUID': '101',
            'REPLACE_WITH_TARGET_POOL_GUID': '201',
            'REPLACE_WITH_SCRATCH_POOL_GUID': '202',
            'REPLACE_WITH_SCRATCH_DATASET_GUID': '203',
        }
        for old, new in bindings.items():
            self.assertIn(old, text)
            text = text.replace(old, new)
        self.script = self.root / 'controller.sh'
        self.script.write_text(text)
        self.cfg = {}
        self.model = {}
        self.dataset(1, 'targetpool/archive/recovery-target', '101', 'yes', 'local')
        self.dataset(2, 'targetpool/child', '102', 'no', 'inherited from targetpool')
        self.dataset(3, 'targetpool/received', '103', 'yes', 'received')
        self.model['targetpool/child']['chunkdedup'] = ['off', 'default']
        self.model['targetpool/child']['original']['chunkdedup'] = ['off', 'default']
        (self.root / 'checkpoints').mkdir()
        self.checkpoint = self.root / 'mnt/scratchpool/dedupe-catalogs/checkpoint'
        self.checkpoint.mkdir(parents=True)
        for name in ('manifest', 'lock', 'files.raw', 'native.raw', 'chunks.raw',
                     'native.sorted', 'chunks.sorted', 'report.receipt', 'chunk-estimates.jsonl'):
            (self.checkpoint / name).write_bytes(b'x' * {'manifest': 4384, 'report.receipt': 64}.get(name, 1))
        for name in ('PREPARED', 'progress_probe.so'):
            (self.run / name).write_text('fixture only\n')
        for name in ('module', 'zfs', 'zpool'):
            (self.run / ('admitted-' + name + '.sha256')).write_text('admitted\n')

    def reset_fixture(self):
        self.doCleanups()
        self.setUp()

    def dataset(self, ident, ds, guid, mounted, source):
        point = str(self.root / ('targetpool/' + ds.split('/', 1)[1]))
        props = {'readonly': ['on', source], 'chunkdedup': ['off', source],
                 'canmount': ['on', 'default'], 'mountpoint': [point, 'local']}
        self.model[ds] = dict(props, original=dict(props), guid=guid, mounted=mounted,
                              point=point, mode='ro', ident=ident)

    def prepare(self):
        (self.root / 'config.json').write_text(json.dumps(self.cfg))
        (self.root / 'model.json').write_text(json.dumps(self.model))
        self.original = json.loads(json.dumps(self.model))
        props, datasets, queue = [], [], []
        for ds, row in self.model.items():
            props.extend('\t'.join([ds, p, *row[p], '-']) for p in
                         ('readonly', 'chunkdedup', 'canmount', 'mountpoint'))
            datasets.append('\t'.join([ds, row['guid'], row['mounted'], row['point'],
                                       '-', '-', 'off', 'off', 'off', 'off', '1']))
            point = row['point'] if row['mounted'] == 'yes' else str(self.run / 'mounts' / str(row['ident']))
            queue.append('\t'.join([str(row['ident']), ds, row['guid'], row['mounted'], point, '1']))
        for name, rows in [('properties-before.tsv', props), ('datasets-before.tsv', datasets),
                           ('prepared-queue.tsv', queue)]:
            (self.run / name).write_text('\n'.join(rows) + '\n')

    def carried(self, ident=2):
        ds, row = next((ds, r) for ds, r in self.model.items() if r['ident'] == ident)
        folder = self.run / 'datasets' / str(ident)
        folder.mkdir(parents=True)
        for name, value in {'carried-from': 'prior-generation', 'dataset': ds,
                            'dataset-guid': row['guid'], 'status': 'PASS',
                            'exit-status': '0', 'completed_epoch': '123'}.items():
            (folder / name).write_text(value + '\n')
        return folder

    def start(self):
        self.prepare()
        self.output = (self.root / 'output').open('w')
        self.addCleanup(self.output.close)
        process = subprocess.Popen(['/bin/sh', str(self.script)], stdout=self.output,
                                   stderr=subprocess.STDOUT, start_new_session=True,
                                   env=dict(os.environ, CONTROLLER_FIXTURE=str(self.root)))
        def cleanup():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        self.addCleanup(cleanup)
        return process

    def wait(self, process):
        try:
            rc = process.wait(timeout=25)
        except subprocess.TimeoutExpired:
            self.fail('controller timed out: ' + (self.root / 'output').read_text())
        self.assertFalse((self.root / 'fixture-error').exists(), (self.root / 'output').read_text())
        return rc

    def commands(self):
        path = self.root / 'commands.jsonl'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def state(self):
        return dict(line.split('\t', 1) for line in (self.run / 'state.tsv').read_text().splitlines())

    def restored(self):
        model = json.loads((self.root / 'model.json').read_text())
        for ds, row in self.original.items():
            for key in ('readonly', 'chunkdedup', 'canmount', 'mountpoint', 'mounted', 'mode'):
                self.assertEqual(model[ds][key], row[key], (ds, key))

    def rejected(self):
        self.assertNotEqual(self.wait(self.start()), 0)
        self.assertFalse((self.run / 'started_epoch').exists())
        self.assertFalse(any(c[0] == 'mount' and c[1:] != ['-p'] or
                             c[0] == 'zfs' and c[1] in ('set', 'inherit', 'unmount', 'chunkdedup')
                             for c in self.commands()))
        self.restored()

    def test_identity_refusals(self):
        for key, value in [('host', 'wrong'), ('uuid', 'wrong'),
                           ('targetpool_guid', '0'), ('scratchpool_guid', '0'),
                           ('scratch_dataset_guid', '0'), ('vfs.zfs.version.module', 'wrong'),
                           ('vfs.zfs.chunkdedup_test_mode', '1'), ('feature', 'active'),
                           ('bad_hash', 'zfs-hybrid-candidate.ko'), ('bad_hash', 'zfs'),
                           ('bad_hash', 'zpool')]:
            with self.subTest(key=key, value=value):
                self.reset_fixture()
                self.cfg = {key: value}
                self.rejected()

    def test_unadapted_example_is_inert(self):
        text = CONTROLLER.read_text()
        text = text.replace('export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin',
                            'export PATH=' + shlex.quote(str(self.bin)))
        self.script.write_text(text)
        self.rejected()
        self.assertEqual(self.commands(), [['hostname', '-s']])

    def test_preparation_required(self):
        (self.run / 'PREPARED').unlink()
        self.rejected()

    def test_missing_checkpoint_receipts(self):
        for name in [p.name for p in self.checkpoint.iterdir()]:
            with self.subTest(name=name):
                self.reset_fixture()
                (self.checkpoint / name).unlink()
                self.rejected()

    def test_invalid_checkpoint_metadata(self):
        self.cfg['bad_stat'] = 'manifest'
        self.rejected()

    def test_resource_preflight(self):
        for key, value in [('tmp', 0), ('tmp', 'invalid'), ('free', 0),
                           ('free', ''), ('free', 'invalid'), ('targetpool_health', 'DEGRADED'),
                           ('scratchpool_health', 'FAULTED'), ('df_error', True)]:
            with self.subTest(key=key):
                self.reset_fixture()
                self.cfg = {key: value}
                self.rejected()

    def test_stop_preflight(self):
        (self.run / 'STOP').touch()
        self.rejected()

    def test_success_and_carried_receipt(self):
        folder = self.carried()
        before = {p.name: p.read_bytes() for p in folder.iterdir()}
        self.assertEqual(self.wait(self.start()), 0, (self.root / 'output').read_text())
        self.assertEqual(self.state()['state'], 'PASS')
        self.assertEqual(self.state()['datasets_completed'], '3')
        self.assertEqual(before, {p.name: p.read_bytes() for p in folder.iterdir()})
        self.assertEqual(sum(c[:3] == ['zfs', 'chunkdedup', 'optimize'] for c in self.commands()), 2)
        self.restored()

    def test_invalid_carried_receipt(self):
        folder = self.carried()
        for name, value in [('status', 'FAILED'), ('exit-status', '7'),
                            ('dataset-guid', '999'), ('completed_epoch', '0')]:
            with self.subTest(name=name):
                self.reset_fixture()
                folder = self.carried()
                (folder / name).write_text(value + '\n')
                self.rejected()

    def test_worker_failure_restored(self):
        self.cfg['worker_rc'] = 7
        self.assertNotEqual(self.wait(self.start()), 0)
        self.assertEqual(self.state()['state'], 'FAILED')
        self.assertEqual(self.state()['datasets_completed'], '0')
        for ident in (1, 2, 3):
            folder = self.run / 'datasets' / str(ident)
            self.assertEqual((folder / 'status').read_text(), 'FAILED\n')
            self.assertEqual((folder / 'exit-status').read_text(), '7\n')
        self.restored()

    def await_worker(self, process):
        deadline = time.monotonic() + 15
        while not (self.root / 'worker-ready').exists():
            if process.poll() is not None or time.monotonic() > deadline:
                self.fail('worker never started: ' + (self.root / 'output').read_text())
            time.sleep(0.01)

    def test_interruption_restored(self):
        self.cfg['block_worker'] = True
        process = self.start()
        self.await_worker(process)
        process.send_signal(signal.SIGTERM)
        self.assertNotEqual(self.wait(process), 0)
        self.assertEqual(self.state()['state'], 'FAILED')
        self.assertEqual(self.state()['datasets_completed'], '0')
        self.assertFalse((self.run / 'datasets/1/status').exists())
        self.restored()

    def test_guard_termination_restored(self):
        for key, value, reason in [('guard_tmp', 0, 'Temporary catalog'),
                                   ('guard_free', 0, 'Backup pool free'),
                                   ('guard_targetpool_health', 'DEGRADED', 'health changed'),
                                   ('guard_tmp', 'invalid', 'Resource guard failed')]:
            with self.subTest(key=key):
                self.reset_fixture()
                self.cfg.update(block_worker=True, **{key: value})
                process = self.start()
                self.await_worker(process)
                self.assertNotEqual(self.wait(process), 0)
                self.assertEqual(self.state()['state'], 'FAILED')
                self.assertEqual(self.state()['datasets_completed'], '0')
                self.assertIn(reason, (self.run / 'stop-reason.txt').read_text())
                self.restored()

    def test_private_mount_interruption_and_cleanup_errors(self):
        for error in (None, 'unmount_error', 'remount_error'):
            with self.subTest(error=error):
                self.reset_fixture()
                self.carried(1)
                folder = self.carried(3)
                before = {p.name: p.read_bytes() for p in folder.iterdir()}
                self.cfg['block_worker'] = True
                if error:
                    self.cfg[error] = True
                process = self.start()
                self.await_worker(process)
                process.send_signal(signal.SIGTERM)
                self.assertNotEqual(self.wait(process), 0)
                self.assertEqual(self.state()['state'], 'RESTORATION_FAILED' if error else 'FAILED')
                self.assertEqual(self.state()['datasets_completed'], '2')
                self.assertFalse((self.run / 'datasets/2/status').exists())
                self.assertEqual(before, {p.name: p.read_bytes() for p in folder.iterdir()})
                if not error:
                    self.restored()

    def test_cleanup_failure_distinct_from_worker_failure(self):
        self.cfg.update(worker_rc=7, restore_error=True)
        self.assertNotEqual(self.wait(self.start()), 0)
        self.assertEqual(self.state()['state'], 'RESTORATION_FAILED')
        self.assertEqual((self.run / 'datasets/1/exit-status').read_text(), '7\n')
        self.assertNotEqual(json.loads((self.root / 'model.json').read_text()), self.original)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controller', type=Path, default=CONTROLLER)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--case', help='run one named test method')
    args = parser.parse_args()
    CONTROLLER = args.controller.resolve()
    source_hash = hashlib.sha256(CONTROLLER.read_bytes()).hexdigest()
    suite = (unittest.TestSuite([ControllerTests(args.case)]) if args.case else
             unittest.defaultTestLoader.loadTestsFromTestCase(ControllerTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    stable = hashlib.sha256(CONTROLLER.read_bytes()).hexdigest() == source_hash
    if args.report:
        args.report.write_text(json.dumps(dict(controller_sha256=source_hash,
            source_stable=stable, tests=result.testsRun, failures=len(result.failures),
            errors=len(result.errors), scope='command fixtures; no kernel or pool execution'), indent=2) + '\n')
    sys.exit(0 if result.wasSuccessful() and stable else 1)

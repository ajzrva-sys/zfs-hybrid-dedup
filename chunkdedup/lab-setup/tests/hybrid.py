#!/usr/bin/env python3
"""Current-source hybrid checks, separate from the immutable 21-case CDC suite.

This module is imported only by the guarded guest runner. All datasets are fresh
children of that runner's file-vdev pool; failed case data is retained for diagnosis.
"""
import errno
import hashlib
import json
import mmap
import os
from pathlib import Path
import random
import re
import subprocess
import time

CASES = (
    'hybrid CLI contracts and forced methods',
    'native exact sharing preserves metadata and COW',
    'automatic unique data stays native',
    'automatic shifted cohort selects chunking',
    'native sparse tails and incompatible geometry',
    'verified sharing rejects stale unequal and busy ranges',
    'native peers and snapshots survive CDC conversion',
    'existing CDC resume and explicit restoration',
    'automatic background watching shares native blocks',
    'interrupted CDC conversion resumes through automatic mode',
    'exhausted quota preserves content during conversion',
    'hybrid snapshots clones and logical replication',
)


class Suite:
    def __init__(self, original, config, root):
        self.m, self.config, self.root = original, config, Path(root)
        self.pool, self.prefix = config['pool'], config['prefix']
        self.base = self.pool + '/hybrid'
        self.mount = original.ROOT / 'hybrid'
        self.rows, self.datasets = [], []
        self.report = self.root / 'hybrid-result.json'
        self.ctl = self.root / 'hybrid/hybrid_ctl'
        self.started = time.time()

    def save(self, status):
        value = {
            'schema': 1, 'run_id': self.config['run_id'],
            'payload_nonce': self.config['nonce'], 'source_digest': self.config['source_digest'],
            'test_files': self.config['hybrid_tests'], 'status': status,
            'started': self.started, 'finished': time.time(), 'checks': self.rows,
            'scope': 'current FreeBSD binary and module in guarded guest; crash and performance are separate',
        }
        temporary = self.report.with_suffix('.tmp')
        with temporary.open('wb') as stream:
            stream.write((json.dumps(value, indent=2, sort_keys=True) + '\n').encode())
            stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, self.report)
        fd = os.open(self.report.parent, os.O_RDONLY)
        try: os.fsync(fd)
        finally: os.close(fd)

    def check(self, name, function):
        started = time.time()
        try:
            detail = function()
            self.rows.append({'test': name, 'passed': True, 'detail': detail,
                              'seconds': round(time.time() - started, 3)})
            self.save('RUNNING')
            print('HYBRID PASS', name, flush=True)
        except BaseException as error:
            self.rows.append({'test': name, 'passed': False, 'error': str(error),
                              'seconds': round(time.time() - started, 3)})
            self.save('TEST_FAILURE')
            print('HYBRID FAIL', name, str(error), flush=True)
            raise
        # Cases own only these newly created children. Preserve failed evidence.
        for dataset in reversed(self.datasets):
            assert dataset.startswith(self.base + '/')
            self.m.zfs('destroy', '-r', dataset)
        self.datasets.clear()

    def new(self, name, quota='48M'):
        dataset, path = self.base + '/' + name, self.mount / name
        self.m.zfs('create', '-o', 'mountpoint=' + str(path), '-o', 'recordsize=128K',
                   '-o', 'chunkdedup=on', '-o', 'compression=off', '-o', 'atime=off',
                   '-o', 'quota=' + quota, dataset)
        self.datasets.append(dataset)
        return dataset, path

    def write(self, path, data):
        self.m.bytes_file(path, data)
        return hashlib.sha256(data).hexdigest()

    def data(self, size=2 << 20, seed=981):
        return random.Random(seed).randbytes(size)

    def optimize(self, path, method=None, ok=True, threads=None):
        args = ['chunkdedup', 'optimize', '-r', '--min-age', '0']
        if method is not None:
            args += ['--method=' + method]
        if threads is not None:
            args += ['--threads=' + str(threads)]
        return self.m.zfs(*args, path, ok=ok).stdout.decode()

    def inspect(self, dataset, path, offset=0):
        return json.loads(self.m.command(self.ctl, 'inspect', dataset, path, offset).stdout)

    def share(self, dataset, source, dest, source_offset=0, offset=0, length=128 << 10, variant=None):
        args = [self.ctl, 'share', dataset, source, dest, source_offset, offset, length]
        if variant:
            args.append(variant)
        return json.loads(self.m.command(*args).stdout)

    def methods(self, path):
        return self.m.zfs('chunkdedup', 'status', '--methods', path).stdout.decode()

    def representation(self, path, expected):
        text = self.methods(path)
        assert re.search(r'representation=' + expected + r'\b', text), text
        return text

    def selections(self, text):
        rows = re.findall(r'^(.+): method=(block|chunk) native_estimate=(\d+) chunk_estimate=(\d+) reason=([a-z-]+)', text, re.M)
        assert rows, text
        return [{'path': p, 'method': method, 'native': int(native), 'chunk': int(chunk), 'reason': reason}
                for p, method, native, chunk, reason in rows]

    def metadata(self, path):
        st = path.stat()
        return (st.st_mode, st.st_uid, st.st_gid, st.st_mtime_ns, st.st_ctime_ns,
                self.m.xattr(path))

    def cli(self):
        ds, directory = self.new('cli')
        ordinary, chunked = directory / 'ordinary', directory / 'chunked'
        expected = self.write(ordinary, self.data())
        self.write(chunked, self.data(seed=982))
        legacy = self.m.zfs('chunkdedup', 'status', ds).stdout.decode()
        fields = re.findall(r'([a-z_]+)=(\d+)', legacy)
        assert {k for k, _ in fields} == {'files', 'records', 'chunks', 'chunk_bytes', 'logical_bytes', 'references'}, legacy
        default = self.selections(self.optimize(ordinary, threads=1))
        forced = self.selections(self.optimize(chunked, 'chunk', threads=4))
        assert default[0]['method'] == 'block' and forced[0]['method'] == 'chunk'
        assert forced[0]['reason'] == 'forced', forced
        self.representation(ordinary, 'ordinary'); self.representation(chunked, 'chunk')
        status = self.methods(ds)
        assert 'pool_block_sharing:' in status and 'pool=' + self.pool in status, status
        assert self.m.digest(ordinary) == expected
        self.m.zfs('chunkdedup', 'optimize', '--method=invalid', ordinary, ok=False)
        self.m.zfs('chunkdedup', 'verify', '--method=block', ordinary, ok=False)
        for value in ['0', '65', '-1', 'invalid', '4trailing']:
            self.m.zfs('chunkdedup', 'optimize', '--threads=' + value, ordinary, ok=False)
        self.m.zfs('chunkdedup', 'verify', '--threads=4', ordinary, ok=False)
        return {'six_field_status_preserved': True, 'default': default, 'forced': forced,
                'pool_statistics_labeled': True, 'explicit_scan_threads': [1, 4],
                'invalid_scan_threads_rejected': True}

    def exact(self):
        ds, directory = self.new('exact')
        source, dest = directory / 'a', directory / 'b'
        expected = self.write(source, self.data((2 << 20) + 123))
        self.write(dest, source.read_bytes())
        for path in [source, dest]:
            os.chmod(path, 0o640); self.m.xattr(path, b'hybrid-metadata')
            os.utime(path, ns=(1_600_000_000_123456789, 1_600_000_000_123456789))
        before = [self.metadata(p) for p in [source, dest]]
        text = self.optimize(directory, 'block', threads=4)
        assert all(row['method'] == 'block' for row in self.selections(text))
        self.m.zpool('sync', self.pool)
        assert before == [self.metadata(p) for p in [source, dest]]
        info = self.inspect(ds, dest)
        assert info['error'] == 0 and info['block_shared'], info
        repeated = self.share(ds, source, dest)
        assert repeated['error'] == 0 and repeated['already_shared_bytes'] == 128 << 10, repeated
        assert repeated['shared_bytes'] == 0, repeated
        assert self.m.digest(source) == self.m.digest(dest) == expected
        self.m.helper('pwrite', dest, 19, b'independent-native-write'.hex())
        assert self.m.digest(source) == expected and self.m.digest(dest) != expected
        self.representation(source, 'ordinary'); self.representation(dest, 'ordinary')
        return {'verified_shared_block': info, 'already_shared_noop': repeated,
                'timestamps_permissions_attributes_preserved': True, 'independent_COW': True}

    def unique(self):
        ds, directory = self.new('unique')
        expected = {}
        for i in range(3):
            path = directory / ('unique-' + str(i))
            expected[path] = self.write(path, self.data(seed=990 + i))
        rows = self.selections(self.optimize(directory, threads=4))
        assert len(rows) == 3 and all(row['method'] == 'block' for row in rows), rows
        for path, checksum in expected.items():
            self.representation(path, 'ordinary'); assert self.m.digest(path) == checksum
        assert self.m.stats(ds)['files'] == 0
        return {'selected': rows, 'unique_files_remain_native': True}

    def shifted(self):
        ds, directory = self.new('shifted')
        base, expected = self.data(4 << 20, 991), {}
        for i in range(4):
            path = directory / ('shift-' + str(i))
            expected[path] = self.write(path, bytes([80 + i]) * (7919 * (i + 1)) + base)
        rows = self.selections(self.optimize(directory, threads=4))
        chunked = [row for row in rows if row['method'] == 'chunk']
        assert len(chunked) >= 2, rows
        assert all(row['chunk'] * 5 <= row['native'] * 4 for row in chunked), rows
        for path, checksum in expected.items():
            assert self.m.digest(path) == checksum
        self.m.zfs('chunkdedup', 'verify', '-r', ds)
        assert self.m.stats(ds)['files'] >= 2
        return {'selected': rows, 'shifted_cohort_chunking': True}

    def geometry(self):
        ds, directory = self.new('geometry')
        source, dest = directory / 'tail-a', directory / 'tail-b'
        expected = self.write(source, self.data((1 << 20) + 123, 992))
        self.write(dest, source.read_bytes())
        block = self.inspect(ds, source)['block_size']
        result = self.share(ds, source, dest, 1 << 20, 1 << 20, 123)
        assert result['error'] == 0 and result['shared_bytes'] == 123, result
        bad = self.share(ds, source, dest, 1, 0, block)
        assert bad['error'] in [errno.EINVAL, errno.ENOTSUP], bad
        bad = self.share(ds, source, dest, 0, 0, block - 1)
        assert bad['error'] in [errno.EINVAL, errno.ENOTSUP], bad
        sparse = directory / 'sparse'
        with open(sparse, 'wb') as stream:
            stream.write(b'head' * 1024); stream.seek((2 << 20) - 4096); stream.write(b'tail' * 1024)
            stream.flush(); os.fsync(stream.fileno())
        checksum = self.m.digest(sparse)
        # Settle every data record before sampling allocation; a clean hole
        # inspection does not drain the file's dirty head and tail records.
        settled = json.loads(self.m.command(self.ctl, 'sync-batch', ds, sparse, 1).stdout)
        assert settled['error'] == 0, settled
        hole = self.inspect(ds, sparse, block)
        assert hole['error'] == 0 and hole['block_hole'], hole
        allocated = sparse.stat().st_blocks
        self.optimize(directory, 'block')
        assert self.m.digest(source) == self.m.digest(dest) == expected
        assert self.m.digest(sparse) == checksum and sparse.stat().st_blocks <= allocated + 32, {
            'expected_sha256': checksum, 'actual_sha256': self.m.digest(sparse),
            'blocks_before': allocated, 'blocks_after': sparse.stat().st_blocks,
            'maximum_blocks_after': allocated + 32}
        assert self.inspect(ds, sparse, block)['block_hole']
        self.m.zfs('set', 'recordsize=16K', ds)
        small = directory / 'small-record'; self.write(small, source.read_bytes())
        small_info = self.inspect(ds, small)
        assert small_info['block_size'] != block, (small_info, block)
        bad = self.share(ds, source, small, length=block)
        assert bad['error'] in [errno.EINVAL, errno.ENOTSUP], bad
        return {'short_EOF_shared': result, 'holes_preserved': True,
                'sparse_blocks_before': allocated, 'sparse_blocks_after': sparse.stat().st_blocks,
                'sparse_baseline_sync': settled, 'sparse_baseline_synced_txg': hole['synced_txg'],
                'incompatible_geometry_rejected': bad}

    def admission(self):
        ds, directory = self.new('admission')
        source, dest, unequal = [directory / name for name in ['source', 'dest', 'unequal']]
        expected = self.write(source, self.data(seed=993)); self.write(dest, source.read_bytes())
        self.write(unequal, self.data(seed=994))
        # A range inspection may report dirty bytes; it must not force a TXG.
        # Write identical bytes without fsync, then use one explicit barrier for
        # both objects. A natural background sync may win the immediate probe.
        for path in [source, dest]:
            self.m.xattr(path, b'hybrid-metadata')
            first = path.read_bytes()[:1]
            fd = os.open(path, os.O_WRONLY)
            try:
                assert os.pwrite(fd, first, 0) == 1
            finally:
                os.close(fd)
        before_batch = [self.metadata(path) for path in [source, dest]]
        dirty = json.loads(self.m.command(self.ctl, 'inspect-raw', ds, source, 0).stdout)
        assert dirty['error'] in [0, errno.EAGAIN], dirty
        initial = json.loads(self.m.command(self.ctl, 'inspect-initial', ds, source).stdout)
        assert initial['error'] == 0, initial
        invalid_batches = {}
        for count, variant in [(0, None), (257, None), (1, 'zero-object')]:
            args = [self.ctl, 'sync-batch', ds, source, count]
            if variant:
                args.append(variant)
            result = json.loads(self.m.command(*args).stdout)
            assert result['error'] == errno.EINVAL, result
            invalid_batches[variant or str(count)] = result['error']
        result = json.loads(self.m.command(self.ctl, 'sync-batch', ds, source, 2, dest).stdout)
        assert result['error'] == 0, result
        for path in [source, dest]:
            result = json.loads(self.m.command(self.ctl, 'inspect-raw', ds, path, 0).stdout)
            assert result['error'] == 0, result
        assert before_batch == [self.metadata(path) for path in [source, dest]]
        assert self.m.digest(source) == self.m.digest(dest) == expected
        result = json.loads(self.m.command(self.ctl, 'sync-batch', ds, source, 1).stdout)
        assert result['error'] == 0, result
        result = json.loads(self.m.command(self.ctl, 'sync-batch', ds, source, 256, dest).stdout)
        assert result['error'] == 0, result
        stable = json.loads(self.m.command(self.ctl, 'inspect-loop', ds, source, 32).stdout)
        assert stable['error'] == 0 and stable['count'] == 32, stable
        # The former unconditional barrier advanced the TXG on every query.
        # At least one stable adjacent pair tolerates incidental pool syncing.
        assert stable['txg_changes'] < 31, stable
        result = json.loads(self.m.command(self.ctl, 'inspect-stale', ds, source, 0).stdout)
        assert result['error'] == errno.ESTALE, result
        rejected = {}
        for variant in ['stale-generation', 'stale-mtime', 'stale-source']:
            result = self.share(ds, source, dest, variant=variant)
            assert result['error'] in [errno.ESTALE, errno.EAGAIN], result
            rejected[variant] = result['error']
        result = self.share(ds, source, unequal)
        assert result['error'] == errno.EILSEQ, result
        with open(dest, 'rb'):
            result = self.share(ds, source, dest)
            assert result['error'] == errno.EBUSY, result
        for access in [mmap.ACCESS_READ, mmap.ACCESS_WRITE]:
            with open(dest, 'r+b') as stream:
                mapping = mmap.mmap(stream.fileno(), 0, access=access)
                try:
                    result = self.share(ds, source, dest)
                    assert result['error'] == errno.EBUSY, result
                finally:
                    mapping.close()
        assert self.m.digest(source) == self.m.digest(dest) == expected
        return {'stale_identity_errors': rejected, 'unequal_bytes_rejected': True,
                'competing_open_and_read_write_mappings_rejected': True,
                'sync_batch': {'dirty_range_error': dirty['error'],
                               'invalid_batches': invalid_batches,
                               'one_two_and_256_object_batches': True,
                               'bytes_and_metadata_preserved': True,
                               'clean_initial_inspections': stable}}

    def peer_conversion(self):
        ds, directory = self.new('peer-conversion')
        source, dest = directory / 'source', directory / 'dest'
        expected = self.write(source, self.data(seed=995)); self.write(dest, source.read_bytes())
        text = self.optimize(directory, 'block')
        self.m.zpool('sync', self.pool)
        assert self.inspect(ds, dest)['block_shared'], text
        self.m.zfs('snapshot', ds + '@native')
        rows = self.selections(self.optimize(dest, 'chunk'))
        self.representation(dest, 'chunk'); self.representation(source, 'ordinary')
        assert self.m.digest(source) == self.m.digest(dest) == expected
        snapshot = directory / '.zfs/snapshot/native'
        assert self.m.digest(snapshot / 'source') == self.m.digest(snapshot / 'dest') == expected
        self.m.helper('pwrite', dest, (128 << 10) - 3, b'changed-CDC-record'.hex())
        assert self.m.digest(source) == expected and self.m.digest(snapshot / 'dest') == expected
        assert self.m.digest(dest) != expected
        self.m.zfs('chunkdedup', 'verify', ds)
        self.m.zpool('sync', self.pool)
        retained = int(self.m.zfs('get', '-Hp', '-o', 'value', 'usedbysnapshots', ds).stdout)
        assert retained > 0
        return {'selected': rows, 'ordinary_peer_snapshot_preserved': True,
                'snapshot_retained_bytes': retained}

    def restore(self):
        ds, directory = self.new('restore')
        path = directory / 'target'; expected = self.write(path, self.data(seed=996))
        self.optimize(path, 'chunk'); self.representation(path, 'chunk')
        rows = self.selections(self.optimize(path))
        assert rows[0]['method'] == 'chunk' and rows[0]['reason'] == 'existing-chunked', rows
        self.m.zfs('chunkdedup', 'unoptimize', path)
        self.representation(path, 'ordinary')
        rows = self.selections(self.optimize(path, 'block'))
        assert rows[0]['method'] == 'block'
        self.representation(path, 'ordinary'); assert self.m.digest(path) == expected
        return {'existing_CDC_preserved': True, 'explicit_restore_then_native': rows}

    def watch(self):
        ds, directory = self.new('watch')
        source, dest = directory / 'source', directory / 'dest'
        expected = self.write(source, self.data(seed=997)); self.write(dest, source.read_bytes())
        logfile = self.root / 'hybrid-watch.log'
        with logfile.open('wb') as log:
            worker = subprocess.Popen([self.m.ZFS, 'chunkdedup', 'watch', '--threads=4', '--interval', '1', '--min-age', '0', ds], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 45
                while not self.inspect(ds, dest).get('block_shared', False):
                    assert worker.poll() is None and time.monotonic() < deadline, logfile.read_text()
                    time.sleep(.25)
            finally:
                worker.terminate(); worker.wait(timeout=30)
        assert self.m.digest(source) == self.m.digest(dest) == expected
        self.representation(dest, 'ordinary')
        return {'default_watch_uses_native_sharing': True, 'log': logfile.name}

    def interruption(self):
        ds, directory = self.new('interruption', '40M')
        path = directory / 'target'; expected = self.write(path, self.data((16 << 20) + 123, 998))
        total = (path.stat().st_size + (128 << 10) - 1) // (128 << 10)
        logfile = self.root / 'hybrid-interruption.log'
        with logfile.open('wb') as log:
            worker = subprocess.Popen([self.m.ZFS, 'chunkdedup', 'optimize', '--method=chunk', '--min-age', '0', path], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 90
                while True:
                    records = self.m.stats(ds)['records']
                    if records:
                        assert worker.poll() is None, 'conversion finished before bounded interruption'
                        worker.kill(); break
                    assert worker.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            finally:
                if worker.poll() is None: worker.kill()
                worker.wait(timeout=30)
        mapped = self.m.stats(ds)['records']
        assert 0 < mapped < total, (mapped, total)
        assert self.m.digest(path) == expected
        rows = self.selections(self.optimize(path))
        assert rows[0]['method'] == 'chunk' and rows[0]['reason'] == 'existing-chunked', rows
        assert self.m.stats(ds)['records'] == total and self.m.digest(path) == expected
        self.m.zfs('chunkdedup', 'verify', ds)
        return {'records_at_interruption': mapped, 'total_records': total, 'automatic_resume': rows}

    def exhaustion(self):
        ds, directory = self.new('exhaustion', '16M')
        path = directory / 'target'; expected = self.write(path, self.data(4 << 20, 999))
        self.m.zpool('sync', self.pool)
        used = int(self.m.zfs('get', '-Hp', '-o', 'value', 'used', ds).stdout)
        self.m.zfs('set', 'quota=' + str(used), ds)
        try:
            self.optimize(path, 'chunk', ok=False)
            assert self.m.digest(path) == expected
            self.m.zfs('chunkdedup', 'verify', ds)
        finally:
            self.m.zfs('set', 'quota=16M', ds)
        self.optimize(path, 'chunk'); assert self.m.digest(path) == expected
        return {'exhausted_conversion_preserves_bytes': True, 'retry_after_space_restored': True}

    def replication(self):
        ds, directory = self.new('replication')
        a, b, c = [directory / name for name in ['a', 'b', 'c']]
        expected = self.write(a, self.data(seed=1000)); self.write(b, a.read_bytes())
        self.write(c, self.data(seed=1001)); self.optimize(directory, 'block'); self.optimize(c, 'chunk')
        original = {p.name: self.m.digest(p) for p in [a, b, c]}
        self.m.zfs('snapshot', ds + '@full')
        clone_ds, clone_dir = self.pool + '/hybrid-clone', self.m.ROOT / 'hybrid-clone'
        self.m.zfs('clone', '-o', 'mountpoint=' + str(clone_dir), ds + '@full', clone_ds)
        try:
            self.m.helper('pwrite', clone_dir / 'b', 17, b'clone-only'.hex())
            assert self.m.digest(b) == expected and self.m.digest(clone_dir / 'b') != expected
            for p in [a, b, c]: assert self.m.digest(directory / '.zfs/snapshot/full' / p.name) == original[p.name]
        finally:
            self.m.zfs('destroy', clone_ds)
        receive = self.pool + '-recv/hybrid'
        stream = self.root / 'hybrid-full.zfs'
        with stream.open('wb') as out:
            process = subprocess.run([self.m.ZFS, 'send', ds + '@full'], stdout=out, stderr=subprocess.PIPE, timeout=180)
            assert process.returncode == 0, process.stderr
        assert 0 < stream.stat().st_size < 24 << 20
        self.m.zfs('receive', '-u', receive, input_data=stream.read_bytes())
        recv_dir = self.m.ROOT / 'hybrid-recv'
        def mount_received():
            if self.m.zfs('get', '-H', '-o', 'value', 'mounted', receive).stdout.strip() == b'no':
                self.m.zfs('mount', receive)
            assert self.m.zfs('get', '-H', '-o', 'value', 'mounted', receive).stdout.strip() == b'yes'
        try:
            self.m.zfs('set', 'mountpoint=' + str(recv_dir), receive); mount_received()
            for name, checksum in original.items(): assert self.m.digest(recv_dir / name) == checksum
            self.m.zfs('chunkdedup', 'verify', receive)
            self.m.helper('pwrite', b, 23, b'incremental-change'.hex())
            changed = self.m.digest(b)
            self.m.zfs('snapshot', ds + '@incremental')
            with stream.open('wb') as out:
                process = subprocess.run([self.m.ZFS, 'send', '-i', ds + '@full', ds + '@incremental'], stdout=out, stderr=subprocess.PIPE, timeout=180)
                assert process.returncode == 0, process.stderr
            self.m.zfs('receive', '-u', receive, input_data=stream.read_bytes())
            mount_received()
            assert self.m.digest(recv_dir / 'b') == changed
            assert self.m.digest(recv_dir / 'a') == original['a'] and self.m.digest(recv_dir / 'c') == original['c']
            self.m.zfs('chunkdedup', 'verify', receive)
        finally:
            self.m.zfs('destroy', '-r', receive)
        return {'snapshot_clone_independence': True, 'full_incremental_logical_replication': True,
                'stream_bytes': stream.stat().st_size}

    def run(self):
        self.m.guest_guard()
        self.m.command('/usr/bin/cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I' + self.prefix + '/include/libspl', '-I' + self.prefix + '/include/libzfs',
                       '-L' + self.prefix + '/lib', '-Wl,-rpath,' + self.prefix + '/lib',
                       '-o', self.ctl, self.root / 'hybrid/hybrid_ctl.c', '-lzfs_core', '-lnvpair')
        self.m.zpool('set', 'feature@block_cloning=enabled', self.pool)
        self.m.zfs('create', '-o', 'mountpoint=' + str(self.mount), '-o', 'quota=96M', self.base)
        functions = [self.cli, self.exact, self.unique, self.shifted, self.geometry, self.admission,
                     self.peer_conversion, self.restore, self.watch, self.interruption, self.exhaustion,
                     self.replication]
        for name, function in zip(CASES, functions): self.check(name, function)
        assert len(self.rows) == len(CASES) and all(row['passed'] for row in self.rows)
        self.save('PASS')
        return self.rows


def run(original, config, root):
    return Suite(original, config, root).run()

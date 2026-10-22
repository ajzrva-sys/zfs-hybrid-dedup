#!/usr/bin/env python3
"""Exercise actual checkpoint persistence and scan-resume hooks in fresh processes."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time


def function(source, name):
    lines = source.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if line.startswith(name + '('):
            for end in range(index + 1, len(lines)):
                if lines[end] == '}\n':
                    return ''.join(lines[index - 1:end + 1])
    raise AssertionError('missing production function: ' + name)


def contents(directory):
    return {path.name: path.read_bytes() for path in directory.iterdir() if path.is_file()}


def emit_driver(repo, folder):
    source = (repo / 'cmd/zfs/zfs_chunkdedup_user.c').read_text()
    types = source[source.index('struct cd_catalog_file {'):
                   source.index('static boolean_t cd_deferred(int);')]
    for name in ('cd_catalog_seen', 'cd_scan_job', 'cd_scan'):
        start = source.index('struct ' + name + ' {')
        end = source.index('\n};', start) + 3
        types += source[start:end] + '\n'
    (folder / 'checkpoint_types.inc').write_text(types)
    catalog = ('cd_catalog_seen_slot', 'cd_catalog_seen', 'cd_catalog_record',
               'cd_catalog_next', 'cd_catalog_append', 'cd_catalog_init',
               'cd_catalog_fini', 'cd_catalog_report_init', 'cd_catalog_report_finish',
               'cd_deferred')
    (folder / 'checkpoint_catalog.inc').write_text('\n'.join(function(source, name) for name in catalog))
    begin, end = '/* CHECKPOINT_DRIVER_BEGIN */', '/* CHECKPOINT_DRIVER_END */'
    assert source.count(begin) == source.count(end) == 1
    (folder / 'checkpoint_hooks.inc').write_text(source.split(begin)[1].split(end)[0])
    scan = ('cd_scan_work', 'cd_scan_copy', 'cd_scan_take', 'cd_scan_drain',
            'cd_scan_init', 'cd_scan_fini', 'cd_scan_same', 'cd_scan_submit')
    (folder / 'checkpoint_scan.inc').write_text('\n'.join(function(source, name) for name in scan))
    (folder / 'checkpoint_pass.inc').write_text(function(source, 'cd_pass'))
    (folder / 'checkpoint_cli.inc').write_text(function(source, 'cd_signal') + '\n' +
                                              function(source, 'zfs_do_chunkdedup'))
    (folder / 'checkpoint_hybrid.inc').write_text('#include "' +
        str(repo / 'cmd/zfs/chunkdedup_hybrid.c') + '"\n')


def exercise(binary, folder, include_driver=True):
    passed = []
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(*arguments):
        subprocess.run([str(binary), *map(str, arguments)], check=True,
                       timeout=45, env=environment)

    partial, complete = folder / 'partial', folder / 'complete'
    run('create', partial)
    run('verify', partial)
    passed.append('checkpoint_prefix_reload')
    run('cancel-tail', partial)
    run('verify', partial)
    passed.append('cancellation_tail_recovery')
    run('create', complete, 'complete')
    run('verify', complete, 'complete')
    run('sorted', complete, 1)
    run('verify', complete, 'complete', 1)
    run('sorted', complete, 3)
    run('verify', complete, 'complete', 3)
    passed.append('sorted_cache_persistence')

    report = folder / 'published-report.jsonl'
    run('report-save', complete, report)
    run('report-verify', complete, report)
    receipt = complete / 'report.receipt'
    saved_receipt, saved_report = receipt.read_bytes(), report.read_bytes()
    other_name = folder / 'second-report-name'
    os.link(report, other_name)
    run('report-verify', complete, report)
    other_name.write_bytes(saved_report + b'changed through hardlink')
    run('report-reject', complete, report)
    report.write_bytes(saved_report)
    other_name.unlink()
    run('report-verify', complete, report)

    # Keep only the merge output reader alive after killing the owning process.
    # The production exec'd sort must hold the checkpoint lock independently.
    read_fd, write_fd = os.pipe()
    owner = subprocess.Popen([str(binary), 'orphan-sort', str(complete), str(write_fd)],
                             pass_fds=(write_fd,), stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=environment)
    os.close(write_fd)
    child = None
    child_released = False
    try:
        notice = owner.stdout.readline().split()
        assert len(notice) == 2 and notice[0] == 'MERGE', notice
        child = int(notice[1])
        deadline = time.monotonic() + 10
        while True:
            observed = subprocess.run(['ps', '-p', str(child), '-o', 'comm='],
                                      capture_output=True, text=True)
            if Path(observed.stdout.strip()).name == 'sort':
                break
            assert time.monotonic() < deadline, 'merge child did not exec actual sort'
            time.sleep(0.01)
        owner.kill()
        owner.wait(timeout=10)
        run('locked', complete)
        os.close(read_fd)
        read_fd = -1
        # The reader closes; sort exits on EPIPE/SIGPIPE and releases its lock.
        with (complete / 'lock').open('rb') as lock:
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    fcntl.flock(lock, fcntl.LOCK_UN)
                    child_released = True
                    break
                except BlockingIOError:
                    assert time.monotonic() < deadline, 'orphan sort did not release lock'
                    time.sleep(0.01)
        run('verify', complete, 'complete', 3)
    finally:
        if read_fd >= 0:
            os.close(read_fd)
        if owner.poll() is None:
            owner.kill()
            owner.wait(timeout=10)
        if child is not None and not child_released:
            # Failure cleanup is bounded and restricted to the recorded child.
            try:
                os.kill(child, 15)
            except ProcessLookupError:
                pass
    print('PASS actual execd sort retains checkpoint lock after parent SIGKILL; reopen waits for exit and restores committed prefix')
    passed.append('orphan_sort_lock')
    report.write_bytes(saved_report + b'altered report')
    run('report-reject', complete, report)
    report.write_bytes(saved_report)
    receipt.unlink()
    run('report-reject', complete, report)
    receipt.write_bytes(saved_receipt)
    receipt.chmod(0o600)
    receipt.write_bytes(b'corrupt receipt')
    run('report-reject', complete, report)
    receipt.write_bytes(saved_receipt)
    run('report-verify', complete, report)
    passed.append('report_receipt_integrity')

    holder = subprocess.Popen([str(binary), 'hold', str(complete)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True, env=environment)
    try:
        assert holder.stdout.readline().strip() == 'LOCKED'
        run('reject', complete)
    finally:
        holder.communicate('x', timeout=45)
        assert holder.returncode == 0
    run('verify', complete, 'complete', 3)
    passed.append('exclusive_lock')

    before = contents(complete)
    run('reject', complete, 'wrong-dataset-binding')
    assert contents(complete) == before
    passed.append('wrong_binding')
    missing = folder / 'missing-manifest'
    missing.mkdir()
    run('reject', missing)
    assert set(contents(missing)) <= {'lock'}

    for damage in ('missing', 'truncated', 'digest'):
        path = folder / ('manifest-' + damage)
        run('create', path)
        manifest = path / 'manifest'
        if damage == 'missing':
            manifest.unlink()
        elif damage == 'truncated':
            manifest.write_bytes(manifest.read_bytes()[:-1])
        else:
            data = bytearray(manifest.read_bytes())
            data[0] ^= 1
            manifest.write_bytes(data)
        before = contents(path)
        run('reject', path)
        assert contents(path) == before
    passed.append('manifest_corruption')

    raw_names = ['files.raw', 'native.raw', 'chunks.raw', 'native.sorted', 'chunks.sorted']
    for index, name in enumerate(raw_names):
        for damage in ('missing', 'truncated', 'digest'):
            path = folder / (str(index) + '-' + damage)
            run('create', path, 'complete')
            run('sorted', path, 3)
            damaged = path / name
            if damage == 'missing':
                damaged.unlink()
            elif damage == 'truncated':
                damaged.write_bytes(damaged.read_bytes()[:-1])
            else:
                data = bytearray(damaged.read_bytes())
                data[0] ^= 1
                damaged.write_bytes(data)
            before = contents(path)
            run('reject', path)
            assert contents(path) == before, 'invalid checkpoint was modified'
    passed.append('payload_corruption')

    path = folder / 'validate-all-before-truncate'
    run('create', path)
    with (path / 'files.raw').open('ab') as stream:
        stream.write(b'uncommitted tail must survive failed validation')
    data = bytearray((path / 'chunks.raw').read_bytes())
    data[0] ^= 1
    (path / 'chunks.raw').write_bytes(data)
    before = contents(path)
    run('reject', path)
    assert contents(path) == before
    passed.append('validate_before_truncate')
    print('PASS actual checkpoint prefixes, fresh-process reload, sorted caches, locking, cancellation tails and fail-closed corruption')

    if include_driver:
        run('driver-write', folder / 'driver')
        before = contents(folder / 'driver' / 'state')
        report = (folder / 'driver' / 'report.jsonl').read_bytes()
        run('driver-resume', folder / 'driver')
        assert contents(folder / 'driver' / 'state') == before
        assert (folder / 'driver' / 'report.jsonl').read_bytes() == report
        passed.extend(('complete_driver_no_rescan', 'complete_driver_cached_sort',
                       'published_report_preserved'))
        for mismatch in ('wrong-dataset', 'wrong-guid', 'wrong-pool'):
            run('driver-reject', folder / 'driver', mismatch)
            assert contents(folder / 'driver' / 'state') == before
        passed.append('driver_identity_rejection')
        published = folder / 'driver' / 'report.jsonl'
        published.write_bytes(report + b'corrupt published report')
        run('driver-reject', folder / 'driver')
        published.write_bytes(report)
        assert contents(folder / 'driver' / 'state') == before
        for invalid in ('status', 'verify', 'unoptimize', 'watch', 'multiple', 'empty'):
            run('driver-invalid', folder / 'driver', invalid)
            assert contents(folder / 'driver' / 'state') == before
        passed.append('cli_restrictions')
        run('driver-partial-write', folder / 'partial-driver')
        run('driver-partial-resume', folder / 'partial-driver')
        run('driver-resume', folder / 'partial-driver')
        passed.append('partial_driver_only_unfinished')
        print('PASS completed driver checkpoint resumes in a fresh process with zero scan/inspect/read/hash calls')
        print('PASS partial driver checkpoint rebuilds seen identities and analyzes only the unfinished file; public checkpoint flag restrictions')
    return sorted(passed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--report', type=Path,
                        default=os.environ.get('WORKFLOW_CHECKPOINT_REPORT'))
    args = parser.parse_args()
    repo = args.repo.resolve()
    if args.report:
        assert not args.report.exists(), 'refusing to reuse checkpoint test receipt'
    results = []
    assert (repo / 'cmd/zfs/chunkdedup_checkpoint.h').is_file(), 'production checkpoint header missing'
    with tempfile.TemporaryDirectory(prefix='checkpoint-test-') as temporary:
        folder = Path(temporary)
        emit_driver(repo, folder)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread', '-g',
            '-I' + str(repo / 'cmd/zfs'), '-I' + str(folder),
            str(Path(__file__).with_name('checkpoint_test.c'))]
        libraries = [] if sys.platform == 'darwin' else ['-lcrypto']
        for sanitized in (False, True):
            binary = folder / ('sanitized' if sanitized else 'strict')
            extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []
            subprocess.run(command + extra + ['-o', str(binary)] + libraries, check=True)
            case = folder / (binary.name + '-cases')
            case.mkdir()
            results.append({'mode': binary.name, 'cases': exercise(binary, case)})
    if args.report:
        def sha(path):
            return hashlib.sha256(path.read_bytes()).hexdigest()
        sources = ('chunkdedup_checkpoint.h', 'zfs_chunkdedup_user.c',
                   'chunkdedup_hybrid.c', 'chunkdedup_hybrid.h', 'chunkdedup_parallel.h')
        args.report.write_text(json.dumps({
            'schema': 1, 'status': 'PASS',
            'scope': 'Actual persistence/sort/driver hooks; doubled ZFS analysis transport; no native or scale claim',
            'source_sha256': {'cmd/zfs/' + name: sha(repo / 'cmd/zfs' / name) for name in sources},
            'test_sha256': {name: sha(Path(__file__).with_name(name))
                            for name in ('test_checkpoint.py', 'checkpoint_test.c')},
            'runs': results,
        }, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()

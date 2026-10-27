#!/usr/bin/env python3
"""Deterministic SIGKILL recovery using actual checkpoint/sort code, no ZFS I/O."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from test_checkpoint import emit_driver

PUBLICATION = ('payload-synced', 'manifest-synced', 'manifest-renamed', 'directory-synced')
SORT = ('sort-writing', 'sort-merge')
SHARE = ('share-before', 'share-after')
SHARING_PUBLICATION = ('share-state-synced', 'share-state-renamed', 'share-directory-synced')
SELECTED_PUBLICATION = ('selected-plan-synced', 'share-state-synced', 'manifest-synced')


def interrupt(command, point, *, hit=1, timeout=30):
    """Only kill a newly created session; never accepts an existing PID."""
    env = dict(os.environ, CD_CRASH_POINT=point, CD_CRASH_HIT=str(hit),
               ASAN_OPTIONS='detect_leaks=0:abort_on_error=1')
    with tempfile.TemporaryFile() as log:
        child = subprocess.Popen(list(map(str, command)), env=env, stdout=log,
                                 stderr=log, start_new_session=True)
        stopped = False
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                pid, status = os.waitpid(child.pid, os.WUNTRACED | os.WNOHANG)
                if pid:
                    if os.WIFSTOPPED(status):
                        stopped = True
                        break
                    child.returncode = os.waitstatus_to_exitcode(status)
                    break
                time.sleep(0.01)
            log.seek(0)
            output = log.read().decode(errors='replace')
            assert stopped and f'CDCRASH {point} {hit}\n' in output, output
        finally:
            if child.returncode is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=10)
        assert child.returncode == -signal.SIGKILL
        return {'point': point, 'hit': hit, 'termination': 'SIGKILL', 'marker_observed': True}


def wait_unlock(state, timeout=10):
    # Orphan sorts retain this lock independently of their killed parent.
    import fcntl
    deadline = time.monotonic() + timeout
    with (state / 'lock').open('rb') as lock:
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                fcntl.flock(lock, fcntl.LOCK_UN)
                return
            except BlockingIOError:
                assert time.monotonic() < deadline, 'orphan sort lock not released'
                time.sleep(0.01)


def exercise(binary, root):
    rows = []
    def run(*args):
        subprocess.run([str(binary), *map(str, args)], check=True, timeout=45)
    for point in PUBLICATION:
        state = root / point
        run('create', state)
        before = (state / 'manifest').read_bytes()
        row = interrupt([binary, 'crash-update', state], point)
        if point in PUBLICATION[:2]:
            assert (state / 'manifest').read_bytes() == before
            run('verify', state)  # old committed prefix survives; tail is truncated
            run('crash-update', state)
        else:
            assert (state / 'manifest').read_bytes() != before
        run('crash-verify', state)  # committed new prefix survives; no duplicate append
        rows.append(dict(row, committed_prefix=True, safe_replay=True))
    for point in SORT:
        state = root / point
        run('create', state, 'complete')
        before = (state / 'manifest').read_bytes()
        row = interrupt([binary, 'crash-sort', state], point)
        assert (state / 'manifest').read_bytes() == before
        wait_unlock(state)
        run('verify', state, 'complete')
        run('crash-sort', state)
        cached = {p.name: p.read_bytes() for p in state.iterdir() if p.is_file()}
        run('crash-sort', state)
        assert all((state / name).read_bytes() == data for name, data in cached.items())
        rows.append(dict(row, committed_scan=True, cached_sort_preserved=True))
    for point in SHARING_PUBLICATION:
        state = root / point
        run('create', state, 'complete')
        run('sorted', state, 3)
        run('sharing-seal', state)
        before = (state / 'sharing.state').read_bytes()
        row = interrupt([binary, 'sharing-update', state], point)
        wait_unlock(state)
        committed = point != 'share-state-synced'
        assert ((state / 'sharing.state').read_bytes() != before) == committed
        run('sharing-verify', state, 2 if committed else 1)
        if not committed:
            run('sharing-update', state)
        run('sharing-verify', state, 2)
        rows.append(dict(row, selected_plan_preserved=True,
                         sharing_frontier_old_or_new_committed=True,
                         canonical_counters_deferred_preserved=True))
    for point in SELECTED_PUBLICATION:
        state = root / ('selected-' + point)
        run('create', state, 'complete')
        run('sorted', state, 3)
        committed = {name: (state / name).read_bytes()
                     for name in ('files.raw', 'native.raw', 'chunks.raw', 'native.sorted', 'chunks.sorted')}
        row = interrupt([binary, 'sharing-seal', state], point)
        wait_unlock(state)
        run('sharing-recover', state, 1)
        run('sharing-verify', state, 1)
        assert all((state / name).read_bytes() == data for name, data in committed.items())
        rows.append(dict(row, initial_selected_publication_old_or_new_valid=True,
                         scan_and_both_sorts_preserved=True, sharing_not_started=True))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    assert not args.report.exists()
    repo = args.repo.resolve(); here = Path(__file__).resolve().parent
    import shlex
    results = []
    with tempfile.TemporaryDirectory(prefix='crash-recovery-') as temporary:
        root = Path(temporary); emit_driver(repo, root)
        for mode in ('strict', 'sanitized'):
            binary = root / mode
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE=1', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread', '-g',
                '-include', str(here / 'recovery_fault.h'), '-I' + str(repo / 'cmd/zfs'),
                '-I' + str(root), str(here / 'checkpoint_test.c'), '-o', str(binary)]
            if mode == 'sanitized': command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if sys.platform != 'darwin': command += ['-lcrypto']
            subprocess.run(command, check=True)
            cases = root / (mode + '-cases'); cases.mkdir()
            results.append({'mode': mode, 'cases': exercise(binary, cases)})
    paths = [repo / 'cmd/zfs' / n for n in ('chunkdedup_checkpoint.h', 'chunkdedup_hybrid.c',
             'chunkdedup_hybrid.h', 'zfs_chunkdedup_user.c')]
    tests = [here / n for n in ('recovery_fault.h', 'checkpoint_test.c', 'test_crash_recovery.py')]
    args.report.write_text(json.dumps({'schema': 1, 'status': 'PASS', 'runs': results,
        'source_sha256': {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
        'test_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in tests},
        'scope': 'Process SIGKILL and real sort children; doubled ZFS transport',
        'native_sharing': 'NOT_RUN', 'forced_vm_power_loss': 'NOT_RUN'}, indent=2) + '\n')


if __name__ == '__main__':
    main()

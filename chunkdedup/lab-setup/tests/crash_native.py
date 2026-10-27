#!/usr/bin/env python3
"""Imported only inside the guarded offline guest, using its owned file-vdev pool."""
import json
import os
from pathlib import Path
import subprocess
from test_crash_recovery import interrupt, wait_unlock, PUBLICATION, SORT, SHARE


def run(suite):
    s = suite
    s.report = s.root / 'crash-progress.json'
    rows = []
    for number, point in enumerate(PUBLICATION + SORT + SHARE):
        def case():
            dataset, directory = s.new('crash-' + str(number))
            source, dest = directory / 'a', directory / 'b'
            expected = s.write(source, s.data(2 << 20))
            s.write(dest, source.read_bytes())
            for path in (source, dest):
                os.chmod(path, 0o640); s.m.xattr(path, b'crash-recovery')
                os.utime(path, ns=(1600000000123456789, 1600000000123456789))
            before = [s.metadata(path) for path in (source, dest)]
            state = s.root / ('crash-state-' + str(number))
            report = s.root / ('crash-report-' + str(number) + '.jsonl')
            command = [s.m.ZFS, 'chunkdedup', 'optimize', '--method=block', '--threads=2',
                       '--min-age=0', '--checkpoint', str(state), '--report', str(report), '-r', str(directory)]
            # Creation commits an empty initial manifest. Interrupt the next
            # commit so a previously durable manifest always exists.
            evidence = interrupt(command, point, hit=2 if point in PUBLICATION else 1)
            wait_unlock(state)
            assert expected == s.m.digest(source) == s.m.digest(dest)
            assert before == [s.metadata(path) for path in (source, dest)]
            committed = {p.name: p.read_bytes() for p in state.iterdir()
                         if p.name in ('manifest', 'files.raw', 'native.raw', 'chunks.raw',
                                       'native.sorted', 'chunks.sorted')}
            published = report.read_bytes() if report.exists() else None
            if point == 'share-after':
                assert any(s.inspect(dataset, p)['block_shared'] for p in (source, dest))
            completed = subprocess.run(command, capture_output=True, timeout=120)
            assert completed.returncode == 0, completed.stderr.decode(errors='replace')
            assert expected == s.m.digest(source) == s.m.digest(dest)
            assert before == [s.metadata(path) for path in (source, dest)]
            if point in SHARE:
                assert all((state / name).read_bytes() == data for name, data in committed.items())
            if published is not None: assert report.read_bytes() == published
            saved = {p.name: p.read_bytes() for p in state.iterdir() if p.is_file()}
            report_bytes = report.read_bytes()
            replay = subprocess.run(command, capture_output=True, timeout=120)
            assert replay.returncode == 0, replay.stderr.decode(errors='replace')
            assert all((state / name).read_bytes() == data for name, data in saved.items())
            assert report.read_bytes() == report_bytes
            # Wrong configuration and source metadata must refuse without
            # rewriting the committed checkpoint or replaying native shares.
            incompatible = [arg.replace('--method=block', '--method=chunk') for arg in command]
            assert subprocess.run(incompatible, capture_output=True, timeout=30).returncode != 0
            assert all((state / name).read_bytes() == data for name, data in saved.items())
            manifest = state / 'manifest'; original = manifest.read_bytes()
            manifest.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            damaged = manifest.read_bytes()
            assert subprocess.run(command, capture_output=True, timeout=30).returncode != 0
            assert manifest.read_bytes() == damaged
            manifest.write_bytes(original)
            # Successful sharing must actually be native and independently COW.
            assert any(s.inspect(dataset, p)['block_shared'] for p in (source, dest))
            s.m.helper('pwrite', dest, 19, b'crash-replay-independent'.hex())
            assert s.m.digest(source) == expected and s.m.digest(dest) != expected
            assert subprocess.run(command, capture_output=True, timeout=30).returncode != 0
            assert all((state / name).read_bytes() == data for name, data in saved.items())
            evidence.update(contents=True, metadata=True, checkpoint_integrity=True,
                            incompatible_refused=True, stale_source_refused=True,
                            safe_replay=True, native_COW=True, report_preserved=True)
            rows.append(evidence)
            return evidence
        s.check('crash recovery ' + point, case)
    receipt = {'schema': 1, 'status': 'PASS', 'source_digest': s.config['source_digest'],
               'run_id': s.config['run_id'], 'payload_nonce': s.config['nonce'],
               'test_files': s.config['hybrid_tests'],
               'interruption': 'SIGKILL of owned process session in offline guest',
               'forced_vm_power_loss': 'NOT_RUN', 'cases': rows}
    path = s.root / 'crash-result.json'
    path.write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt

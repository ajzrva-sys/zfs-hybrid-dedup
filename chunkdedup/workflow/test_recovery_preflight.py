#!/usr/bin/env python3
"""Bounded, host-independent tests for the read-only recovery decision check."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from recovery_preflight import evaluate

HERE = Path(__file__).resolve().parent


def run():
    facts = {'launch_argv': ['/sbin/zfs', 'chunkdedup', 'optimize', '--threads=40',
                             '--checkpoint', '/durable/123'],
             'checkpoint_path': '/durable/123', 'job_root': '/jobs/current',
             'requested_workers': 40, 'file_catalog_bytes': 4131172296,
             'snapshot_limit_bytes': 8589934592, 'group_index_bytes': 82212384,
             'group_index_limit_bytes': 268435456, 'sort_native_bytes': 95711856054,
             'sort_chunk_bytes': 173598633288, 'scratch_reserve_bytes': 1073741824,
             'temp_free_bytes': 500000000000, 'restart_requested': False,
             'restart_reason': 'none', 'retained_scan_bytes': 0,
             'active_checkpoint': False, 'safety_stop_requested': False}
    cases = []

    def check(name, changes=None, remove=(), status='HOLD'):
        value = copy.deepcopy(facts)
        value.update(changes or {})
        for key in remove:
            value.pop(key, None)
        result = evaluate(value)
        assert result['status'] == status, (name, result)
        assert result['capacity_is_lower_bound'] is True
        assert (not result['issues']) == (status == 'PASS'), (name, result)
        cases.append(name)
        return result

    baseline = check('actual_scale_pass', status='PASS')
    assert baseline['capacity']['required_free_bytes'] == 448114036750
    check('checkpoint_missing', {'launch_argv': facts['launch_argv'][:-2]})
    check('checkpoint_mismatch', {'checkpoint_path': '/durable/456'})
    check('checkpoint_in_job', {'checkpoint_path': '/jobs/current/checkpoint',
          'launch_argv': facts['launch_argv'][:-1] + ['/jobs/current/checkpoint']})
    check('checkpoint_parent_escape', {'checkpoint_path': '/jobs/current/../checkpoint'})
    check('checkpoint_prefix_sibling', {'job_root': '/durable/12'}, status='PASS')
    check('checkpoint_duplicate', {'launch_argv': facts['launch_argv'] + ['--checkpoint=/durable/123']})
    for key in facts:
        check('missing_' + key, remove=(key,))
    check('unknown_catalog_bytes', {'file_catalog_bytes': None})
    check('boolean_is_not_bytes', {'temp_free_bytes': True})
    check('negative_bytes', {'sort_native_bytes': -1})
    check('snapshot_over_cap', {'snapshot_limit_bytes': facts['file_catalog_bytes'] - 1})
    check('snapshot_at_cap', {'snapshot_limit_bytes': facts['file_catalog_bytes']}, status='PASS')
    check('group_index_over_cap', {'group_index_limit_bytes': facts['group_index_bytes'] - 1})
    check('group_index_at_cap', {'group_index_limit_bytes': facts['group_index_bytes']}, status='PASS')
    check('thread_mismatch', {'requested_workers': 1})
    check('space_below', {'temp_free_bytes': baseline['capacity']['required_free_bytes'] - 1})
    check('space_exact', {'temp_free_bytes': baseline['capacity']['required_free_bytes']}, status='PASS')
    check('scratch_zero', {'scratch_reserve_bytes': 0})
    check('preserve_performance_restart', {'restart_requested': True, 'restart_reason': 'performance',
          'retained_scan_bytes': 70_000_000_000_000})
    check('checkpointed_restart', {'restart_requested': True, 'restart_reason': 'performance',
          'retained_scan_bytes': 70_000_000_000_000, 'active_checkpoint': True}, status='PASS')
    check('recovery_restart', {'restart_requested': True, 'restart_reason': 'recovery',
          'retained_scan_bytes': 70_000_000_000_000, 'active_checkpoint': True}, status='PASS')
    stop = evaluate({'safety_stop_requested': True, 'restart_requested': False})
    assert stop['status'] == 'PASS' and stop['decision'] == 'SAFETY_STOP_ONLY' and 'capacity' not in stop
    cases.append('safety_stop_separate')
    check('safety_stop_cannot_admit_restart', {'safety_stop_requested': True, 'restart_requested': True})
    assert evaluate([])['status'] == 'HOLD'
    cases.append('nonobject_hold')
    with tempfile.TemporaryDirectory() as directory:
        source, output = [Path(directory) / name for name in ('facts.json', 'decision.json')]
        for name, content, expected in [('cli_pass', json.dumps(facts), 0),
                                       ('cli_missing', '{}', 2), ('cli_invalid', '{', 2)]:
            source.write_text(content)
            result = subprocess.run([sys.executable, str(HERE / 'recovery_preflight.py'),
                                     '--input', str(source), '--output', str(output)], check=False,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            assert result.returncode == expected, (name, result.stderr)
            assert json.loads(output.read_text())['status'] == ('PASS' if expected == 0 else 'HOLD')
            assert source.read_text() == content
            cases.append(name)
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    cases = run()
    result = {'schema': 1, 'status': 'PASS', 'cases': cases,
              'preflight_sha256': hashlib.sha256((HERE / 'recovery_preflight.py').read_bytes()).hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    if args.report:
        args.report.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()

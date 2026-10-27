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
    external = copy.deepcopy(facts)
    generation = {'pid': 123, 'start_epoch': 1792641600, 'executable_sha256': '1'*64}
    binding = {'dataset': 'fixture/fs', 'pool_guid': 11, 'dataset_guid': 22,
               'device': 7, 'object': 34, 'target': '/source', 'age': 3,
               'method': 'block', 'recursive': True,
               'report_path': '/durable/123/chunk-estimates.jsonl'}
    external.update(restart_requested=True, restart_reason='performance',
                    retained_scan_bytes=70_000_000_000_000, active_checkpoint=False,
                    active_generation=copy.deepcopy(generation), source_binding=copy.deepcopy(binding),
                    launch_executable_sha256='2'*64)
    external['launch_argv'] += ['--method=block', '--min-age=3', '--report', binding['report_path'], '-r', '/source']
    payloads = {name: {'bytes': size, 'sha256': str(i+1)*64} for i, (name, size) in enumerate([
        ('files.raw', facts['file_catalog_bytes']), ('native.raw', facts['sort_native_bytes']),
        ('chunks.raw', facts['sort_chunk_bytes']), ('native.sorted', facts['sort_native_bytes']),
        ('chunks.sorted', facts['sort_chunk_bytes'])])}
    external['external_checkpoint'] = {
        'schema': 1, 'kind': 'verified_legacy_catalog_import', 'checkpoint_path': '/durable/123',
        'manifest_sha256': 'c'*64, 'exported_generation': copy.deepcopy(generation),
        'retained_scan_bytes': external['retained_scan_bytes'], 'source_binding': copy.deepcopy(binding),
        'record_size': 1608, 'complete': True, 'sorted_mask': 3, 'payloads': payloads,
        'original_file_table_sha256': 'd'*64,
        'implementation': {'importer_sha256': 'a'*64, 'adopter_sha256': 'b'*64},
        'verification': {'status': 'PASS', 'receipt_sha256': 'e'*64, 'manifest_sha256': 'c'*64,
            'payload_sha256': {name: value['sha256'] for name, value in payloads.items()},
            'source_binding': copy.deepcopy(binding), 'metadata_table_sha256': 'd'*64,
            'metadata_rows': facts['file_catalog_bytes']//1608, 'metadata_failures': 0, 'source_data_reads': 0},
        'native_proof': {'status': 'PASS', 'receipt_sha256': 'f'*64,
            'producer_executable_sha256': '1'*64, 'consumer_executable_sha256': '2'*64,
            'importer_sha256': 'a'*64, 'adopter_sha256': 'b'*64,
            'inspect_attempts': 0, 'read_attempts': 0, 'chunk_lookup_attempts': 0, 'sort_attempts': 0,
            'complete_scan_reused': True, 'both_sorts_reused': True, 'actual_native_shares_verified': True,
            'completed_shares_reused': True, 'contents_metadata_verified': True, 'payloads_unchanged': True}}

    def external_check(name, mutate=lambda value: None, status='HOLD'):
        value = copy.deepcopy(external)
        mutate(value)
        result = evaluate(value)
        assert result['status'] == status, (name, result)
        assert (not result['issues']) == (status == 'PASS'), (name, result)
        assert value['active_checkpoint'] is False or name == 'external_wrong_scope'
        cases.append(name)
        return result

    imported = external_check('external_verified_pass', status='PASS')
    assert imported['external_checkpoint']['active_checkpoint'] is False
    assert imported['external_checkpoint']['receipt_bytes_authenticated'] is False
    external_check('external_unknown', lambda v: v.update(external_checkpoint=None))
    external_check('external_wrong_scope', lambda v: v.update(active_checkpoint=True))
    external_check('external_generation_mismatch', lambda v: v['active_generation'].update(pid=999))
    external_check('external_retained_mismatch', lambda v: v['external_checkpoint'].update(retained_scan_bytes=1))
    external_check('external_path_mismatch', lambda v: v['external_checkpoint'].update(checkpoint_path='/elsewhere'))
    def import_inside_target(value):
        old_report = value['source_binding']['report_path']
        new_checkpoint, new_report = '/source/checkpoint', '/source/checkpoint/chunk-estimates.jsonl'
        value['launch_argv'] = [new_checkpoint if a == value['checkpoint_path'] else
                                new_report if a == old_report else a for a in value['launch_argv']]
        value['checkpoint_path'] = new_checkpoint
        value['external_checkpoint']['checkpoint_path'] = new_checkpoint
        for binding in (value['source_binding'], value['external_checkpoint']['source_binding'],
                        value['external_checkpoint']['verification']['source_binding']):
            binding['report_path'] = new_report
    external_check('external_checkpoint_in_source', import_inside_target)
    external_check('external_source_mismatch', lambda v: v['source_binding'].update(dataset_guid=999))
    external_check('external_launch_mismatch', lambda v: v['launch_argv'].append('/different-target'))
    external_check('external_scan_incomplete', lambda v: v['external_checkpoint'].update(complete=False))
    external_check('external_sort_incomplete', lambda v: v['external_checkpoint'].update(sorted_mask=1))
    external_check('external_payload_missing', lambda v: v['external_checkpoint']['payloads'].pop('chunks.sorted'))
    external_check('external_payload_size_mismatch', lambda v: v['external_checkpoint']['payloads']['native.raw'].update(bytes=1))
    external_check('external_payload_hash_unknown', lambda v: v['external_checkpoint']['payloads']['native.raw'].update(sha256=None))
    external_check('external_manifest_mismatch', lambda v: v['external_checkpoint']['verification'].update(manifest_sha256='0'*64))
    external_check('external_metadata_table_mismatch', lambda v: v['external_checkpoint']['verification'].update(metadata_table_sha256='0'*64))
    external_check('external_metadata_rows_mismatch', lambda v: v['external_checkpoint']['verification'].update(metadata_rows=1))
    external_check('external_metadata_failure', lambda v: v['external_checkpoint']['verification'].update(metadata_failures=1))
    external_check('external_metadata_data_read', lambda v: v['external_checkpoint']['verification'].update(source_data_reads=1))
    external_check('external_native_proof_missing', lambda v: v['external_checkpoint'].pop('native_proof'))
    external_check('external_native_producer_mismatch', lambda v: v['external_checkpoint']['native_proof'].update(producer_executable_sha256='0'*64))
    external_check('external_native_consumer_mismatch', lambda v: v.update(launch_executable_sha256='0'*64))
    external_check('external_native_implementation_mismatch', lambda v: v['external_checkpoint']['native_proof'].update(adopter_sha256='0'*64))
    external_check('external_native_analysis_attempt', lambda v: v['external_checkpoint']['native_proof'].update(read_attempts=1))
    external_check('external_native_sort_attempt', lambda v: v['external_checkpoint']['native_proof'].update(sort_attempts=1))
    external_check('external_native_bool_attempt', lambda v: v['external_checkpoint']['native_proof'].update(sort_attempts=False))
    external_check('external_native_completed_shares_unproven', lambda v: v['external_checkpoint']['native_proof'].update(completed_shares_reused=False))
    external_check('external_native_contents_unproven', lambda v: v['external_checkpoint']['native_proof'].update(contents_metadata_verified=False))
    external_check('external_capacity_hold', lambda v: v.update(temp_free_bytes=1))
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

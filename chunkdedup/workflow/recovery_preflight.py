#!/usr/bin/env python3
"""Read JSON recovery/capacity facts; emit PASS or HOLD without running a job.

Exit 0 means the stated decision passed; exit 2 means HOLD. Capacity is a
lower-bound check with an explicit caller scratch reserve, not full admission.
Paths are checked lexically; callers remain responsible for fresh host facts.
"""
import argparse
import json
from pathlib import Path, PurePosixPath


def evaluate(facts):
    result = {'schema': 1, 'status': 'HOLD', 'decision': 'PRESERVE_ACTIVE_RUN',
              'issues': [], 'capacity_is_lower_bound': True,
              'scope': 'Read-only decision from caller-supplied facts; no runtime admission or mutation'}
    issues = result['issues']
    if not isinstance(facts, dict):
        issues.append('input must be a JSON object')
        return result

    def boolean(name):
        value = facts.get(name)
        if type(value) is not bool:
            issues.append(name + ' must be a known boolean')
            return None
        return value

    def number(name, positive=False):
        value = facts.get(name)
        if type(value) is not int or value < (1 if positive else 0):
            issues.append(name + ' must be a known ' + ('positive' if positive else 'nonnegative') + ' integer')
            return None
        return value

    safety_stop = boolean('safety_stop_requested')
    restart = boolean('restart_requested')
    if safety_stop is True:
        if restart is True:
            issues.append('safety stop is separate from launch/restart admission')
        if not issues:
            result.update(status='PASS', decision='SAFETY_STOP_ONLY')
        return result

    retained = number('retained_scan_bytes')
    active_checkpoint = boolean('active_checkpoint')
    reason = facts.get('restart_reason')
    if reason not in ('none', 'performance', 'recovery') or (restart is False and reason != 'none') or (restart is True and reason == 'none'):
        issues.append('restart_reason must match restart_requested (none, performance or recovery)')
    legacy_restart = restart is True and reason == 'performance' and retained is not None and retained > 0 and active_checkpoint is False

    argv = facts.get('launch_argv')
    if not isinstance(argv, list) or not argv or any(not isinstance(a, str) or not a or '\0' in a for a in argv):
        issues.append('launch_argv must be a known nonempty string array')
        argv = []

    def option(name):
        values = []
        for i, arg in enumerate(argv):
            if arg == name:
                values.append(argv[i + 1] if i + 1 < len(argv) else None)
            elif arg.startswith(name + '='):
                values.append(arg[len(name) + 1:])
        if len(values) != 1 or values[0] is None or values[0].startswith('--'):
            issues.append('launch_argv must contain exactly one ' + name + ' value')
            return None
        return values[0]

    def absolute(name):
        value = facts.get(name)
        if not isinstance(value, str) or not value or '\0' in value:
            issues.append(name + ' must be a known absolute normalized path')
            return None
        path = PurePosixPath(value)
        if not path.is_absolute() or str(path) != value or '..' in path.parts or value.startswith('//'):
            issues.append(name + ' must be a known absolute normalized path')
            return None
        return path

    checkpoint, job = absolute('checkpoint_path'), absolute('job_root')
    checkpoint_arg = option('--checkpoint')
    if checkpoint is not None and checkpoint_arg != str(checkpoint):
        issues.append('launch checkpoint does not match stable checkpoint_path')
    if checkpoint is not None and job is not None and (checkpoint == job or job in checkpoint.parents):
        issues.append('checkpoint_path must be outside disposable job_root')
    workers = number('requested_workers', positive=True)
    threads = option('--threads')
    if workers is not None and threads != str(workers):
        issues.append('launch threads do not match requested_workers')

    file_bytes = number('file_catalog_bytes')
    snapshot_limit = number('snapshot_limit_bytes', positive=True)
    group_bytes = number('group_index_bytes')
    group_limit = number('group_index_limit_bytes', positive=True)
    if workers is not None and workers > 1:
        if file_bytes is not None and snapshot_limit is not None and file_bytes > snapshot_limit:
            issues.append('file catalog exceeds parallel-share snapshot limit')
        if group_bytes is not None and group_limit is not None and group_bytes > group_limit:
            issues.append('group index exceeds memory limit; disk fallback prevents requested readiness')
    native, chunks = number('sort_native_bytes'), number('sort_chunk_bytes')
    scratch, free = number('scratch_reserve_bytes', positive=True), number('temp_free_bytes')
    if None not in (file_bytes, native, chunks, scratch, free):
        lower = native + 2 * chunks + file_bytes
        required = lower + scratch
        result['capacity'] = {'sort_additional_bytes': native + 2 * chunks,
                              'immutable_table_working_copy_bytes': file_bytes,
                              'additional_lower_bound_bytes': lower,
                              'caller_scratch_reserve_bytes': scratch,
                              'required_free_bytes': required, 'available_free_bytes': free}
        if free < required:
            issues.append('temporary free space is below lower bound plus supplied scratch reserve')

    external = facts.get('external_checkpoint')
    external_valid = False
    if 'external_checkpoint' in facts:
        before_external = len(issues)

        def need(ok, message):
            if not ok:
                issues.append('external_checkpoint: ' + message)

        def sha(value):
            return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)

        def positive(value):
            return type(value) is int and value > 0

        def zero(value):
            return type(value) is int and value == 0

        def path_ok(value):
            if not isinstance(value, str) or not value or '\0' in value:
                return False
            path = PurePosixPath(value)
            return path.is_absolute() and str(path) == value and '..' not in path.parts and not value.startswith('//')

        def generation(value):
            return (isinstance(value, dict) and positive(value.get('pid'))
                    and positive(value.get('start_epoch')) and sha(value.get('executable_sha256')))

        need(legacy_restart, 'only a retained legacy performance restart may use this exception')
        if not isinstance(external, dict):
            need(False, 'must be a known complete verification object')
        else:
            need(type(external.get('schema')) is int and external['schema'] == 1
                 and external.get('kind') == 'verified_legacy_catalog_import', 'unsupported import schema/kind')
            need(checkpoint is not None and external.get('checkpoint_path') == str(checkpoint), 'stable path differs from launch checkpoint')
            need(sha(external.get('manifest_sha256')), 'expected manifest SHA256 missing')
            active = facts.get('active_generation')
            need(generation(active) and generation(external.get('exported_generation'))
                 and external['exported_generation'] == active, 'exported/current active generation mismatch')
            need(type(external.get('retained_scan_bytes')) is int and external['retained_scan_bytes'] == retained,
                 'retained scan byte binding mismatch')
            binding = facts.get('source_binding')
            valid_binding = isinstance(binding, dict)
            if valid_binding:
                dataset = binding.get('dataset')
                valid_binding = (isinstance(dataset, str) and bool(dataset) and '\0' not in dataset
                    and all(positive(binding.get(k)) for k in ('pool_guid', 'dataset_guid', 'device', 'object'))
                    and type(binding.get('age')) is int and binding['age'] >= 0
                    and binding.get('method') == 'block' and type(binding.get('recursive')) is bool
                    and path_ok(binding.get('target')) and path_ok(binding.get('report_path')))
            need(valid_binding and external.get('source_binding') == binding, 'fresh source/options binding missing or changed')
            if valid_binding:
                target = PurePosixPath(binding['target'])
                need(checkpoint is not None and checkpoint != target and target not in checkpoint.parents,
                     'checkpoint is inside the scanned source target')
                need(option('--method') == 'block' and option('--min-age') == str(binding['age'])
                     and option('--report') == binding['report_path']
                     and argv[-1:] == [binding['target']] and argv.count(binding['target']) == 1,
                     'source/options differ from actual launch arguments')
                recursive_count = argv.count('-r') + argv.count('--recursive')
                need(recursive_count == int(binding['recursive']), 'recursive launch binding mismatch')
                report = PurePosixPath(binding['report_path'])
                need(checkpoint is not None and checkpoint in report.parents, 'report is outside stable checkpoint')
            need(type(external.get('record_size')) is int and external['record_size'] == 1608
                 and file_bytes is not None and file_bytes > 0 and file_bytes % 1608 == 0,
                 'unsupported native file-table ABI/size')
            need(external.get('complete') is True and type(external.get('sorted_mask')) is int
                 and external['sorted_mask'] == 3, 'complete scan and both cached sorts required')
            payloads = external.get('payloads')
            expected_sizes = {'files.raw': file_bytes, 'native.raw': native, 'chunks.raw': chunks,
                              'native.sorted': native, 'chunks.sorted': chunks}
            valid_payloads = isinstance(payloads, dict) and set(payloads) == set(expected_sizes)
            if valid_payloads:
                valid_payloads = all(isinstance(payloads[name], dict)
                    and type(payloads[name].get('bytes')) is int
                    and payloads[name]['bytes'] == size and sha(payloads[name].get('sha256'))
                    for name, size in expected_sizes.items())
            need(valid_payloads, 'five exact catalog sizes and SHA256 bindings required')
            original = external.get('original_file_table_sha256')
            need(sha(original), 'original file-table SHA256 missing')
            implementation = external.get('implementation')
            valid_implementation = (isinstance(implementation, dict)
                and all(sha(implementation.get(k)) for k in ('importer_sha256', 'adopter_sha256')))
            need(valid_implementation, 'import/adoption implementation SHA256 bindings missing')
            verification = external.get('verification')
            valid_verification = isinstance(verification, dict)
            if valid_verification:
                valid_verification = (verification.get('status') == 'PASS' and sha(verification.get('receipt_sha256'))
                    and verification.get('manifest_sha256') == external.get('manifest_sha256')
                    and verification.get('source_binding') == binding
                    and verification.get('metadata_table_sha256') == original
                    and type(verification.get('metadata_rows')) is int and file_bytes is not None
                    and verification['metadata_rows'] == file_bytes // 1608
                    and zero(verification.get('metadata_failures')) and zero(verification.get('source_data_reads'))
                    and valid_payloads and verification.get('payload_sha256') ==
                        {name: payloads[name]['sha256'] for name in expected_sizes})
            need(valid_verification, 'fresh manifest/payload/source-metadata verification receipt mismatch')
            consumer = facts.get('launch_executable_sha256')
            need(sha(consumer), 'launch executable SHA256 missing')
            proof = external.get('native_proof')
            valid_proof = isinstance(proof, dict)
            if valid_proof:
                valid_proof = (proof.get('status') == 'PASS' and sha(proof.get('receipt_sha256'))
                    and generation(active) and proof.get('producer_executable_sha256') == active['executable_sha256']
                    and proof.get('consumer_executable_sha256') == consumer and valid_implementation
                    and all(proof.get(k) == implementation[k] for k in ('importer_sha256', 'adopter_sha256'))
                    and all(zero(proof.get(k)) for k in ('inspect_attempts', 'read_attempts', 'chunk_lookup_attempts', 'sort_attempts'))
                    and all(proof.get(k) is True for k in ('complete_scan_reused', 'both_sorts_reused',
                        'actual_native_shares_verified', 'completed_shares_reused',
                        'contents_metadata_verified', 'payloads_unchanged')))
            need(valid_proof, 'hash-bound actual native legacy resume proof missing or incompatible')
            external_valid = len(issues) == before_external
            if external_valid:
                result['external_checkpoint'] = {'status': 'CHECKED_CALLER_SUPPLIED_FACTS',
                    'manifest_sha256': external['manifest_sha256'],
                    'verification_receipt_sha256': verification['receipt_sha256'],
                    'native_proof_receipt_sha256': proof['receipt_sha256'],
                    'active_checkpoint': active_checkpoint, 'receipt_bytes_authenticated': False}
    if legacy_restart and not external_valid:
        issues.append('performance restart would discard retained scan work; preserve the active run')
    if not issues:
        result.update(status='PASS', decision='RESTART_CHECK_PASSED' if restart else 'LAUNCH_CHECK_PASSED')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error('input and output must differ')
    try:
        facts = json.loads(args.input.read_text())
        result = evaluate(facts)
    except (OSError, ValueError) as error:
        result = {'schema': 1, 'status': 'HOLD', 'decision': 'PRESERVE_ACTIVE_RUN',
                  'issues': ['input unavailable or invalid: ' + str(error)],
                  'capacity_is_lower_bound': True}
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return 0 if result['status'] == 'PASS' else 2


if __name__ == '__main__':
    raise SystemExit(main())

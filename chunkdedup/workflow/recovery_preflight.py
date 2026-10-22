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
    if restart is True and reason == 'performance' and retained is not None and retained > 0 and active_checkpoint is False:
        issues.append('performance restart would discard retained scan work; preserve the active run')

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

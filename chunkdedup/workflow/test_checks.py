#!/usr/bin/env python3
"""Exercise helper classification using isolated source copies; retain evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REQUIRED = (
    'cmd/zfs/chunkdedup_chunker.c', 'cmd/zfs/chunkdedup_chunker.h',
    'cmd/zfs/rabin64_table.h', 'cmd/zfs/zfs_chunkdedup_user.c',
    'include/sys/zfs_chunkdedup.h', 'module/zfs/zfs_chunkdedup.c',
    'cmd/zfs/chunkdedup_hybrid.c', 'cmd/zfs/chunkdedup_hybrid.h',
    'cmd/zfs/chunkdedup_parallel.h', 'cmd/zfs/chunkdedup_checkpoint.h',
    'include/sys/dsl_pool.h', 'module/zfs/dsl_pool.c',
    'chunkdedup/operations/run.sh.example',
    'chunkdedup/operations/tests/test_controller.py',
    'chunkdedup/operations/tests/fixture.py',
)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--run-dir', type=Path, required=True)
parser.add_argument('--kit', type=Path, default=HERE.parent)
options = parser.parse_args()
repo, root, kit = [p.resolve() for p in (options.repo, options.run_dir, options.kit)]
assert not root.exists() and not root.is_relative_to(repo) and not root.is_relative_to(kit)
root.mkdir(parents=True)
source = root / 'source'
for name in REQUIRED + ('chunkdedup/lab-setup/tests/test_restore_validate.py',):
    destination = source / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(repo / name, destination)
originals = {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in REQUIRED}
records = []


def run(name, extra, expected_code, expected_status):
    command = [sys.executable, str(HERE / 'checks.py'), '--repo', str(source),
               '--kit', str(kit), '--run-dir', str(root / name), '--run-id', name] + extra
    with (root / (name + '.log')).open('wb') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    report = json.loads((root / name / 'results.json').read_text())
    assert result.returncode == expected_code, (name, result.returncode, report)
    assert report['status'] == expected_status, report
    assert report['run_id'] == name
    for stage in report.get('results', []):
        assert hashlib.sha256((root / name / stage['log']).read_bytes()).hexdigest() == stage['log_sha256']
    records.append({'case': name, 'expected': expected_status, 'actual': report['status'],
                    'returncode': result.returncode,
                    'stages': {s['name']: s['status'] for s in report.get('results', [])}})
    return report


baseline = run('baseline', [], 0, 'PASS')
assert len(baseline['results']) == 7 and all(s['status'] == 'PASS' for s in baseline['results'])
tested = json.loads((root / 'baseline/tested-sources.json').read_text())
assert {entry['path'] for entry in tested['files']} == set(REQUIRED)
infra = run('missing-compiler', ['--cc', str(root / 'absent-compiler')], 2, 'INFRA_FAILURE')
assert next(s for s in infra['results'] if s['name'] == 'toolchain')['status'] == 'INFRA_FAILURE'
run('assertions-disabled', ['--cc', 'clang -DNDEBUG'], 2, 'INFRA_FAILURE')
run('wrong-source-digest', ['--source-digest', '0' * 64], 2, 'INFRA_FAILURE')
with (source / REQUIRED[0]).open('a') as stream:
    stream.write('\nTHIS_IS_AN_INTENTIONAL_SYNTAX_DEFECT_FOR_CLASSIFICATION;\n')
defect = run('source-compile-defect', [], 1, 'TEST_FAILURE')
assert next(s for s in defect['results'] if s['name'] == 'chunker_api')['status'] == 'TEST_FAILURE'
assert next(s for s in defect['results'] if s['name'] == 'core_model')['status'] == 'PASS'
assert all(hashlib.sha256((repo / name).read_bytes()).hexdigest() == expected
           for name, expected in originals.items())
(root / 'selftest-results.json').write_text(json.dumps({'schema': 1, 'status': 'PASS',
    'scope': 'Local helper classification; no native guest actions', 'cases': records}, indent=2) + '\n')
print(f'{len(records)} helper classification cases PASS: {root / "selftest-results.json"}')

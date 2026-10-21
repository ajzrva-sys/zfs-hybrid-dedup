#!/usr/bin/env python3
"""Differential checks of the actual disk and memory catalog selection paths."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def function(source, name):
    lines = source.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.startswith(name + '('):
            for end in range(i + 1, len(lines)):
                if lines[end] == '}\n':
                    return ''.join(lines[i - 1:end + 1])
    raise AssertionError(name)


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--repo', type=Path, required=True)
    repo = p.parse_args().repo.resolve()
    source = (repo / 'cmd/zfs/zfs_chunkdedup_user.c').read_text()
    types = source[source.index('struct cd_catalog_file {'):source.index('static boolean_t cd_deferred(int);')]
    names = ('cd_catalog_root', 'cd_catalog_join', 'cd_catalog_next', 'cd_catalog_same',
             'cd_catalog_append', 'cd_catalog_connect', 'cd_catalog_charge', 'cd_catalog_cost',
             'cd_catalog_report_init', 'cd_catalog_report_string', 'cd_catalog_report_file',
             'cd_catalog_report_finish', 'cd_catalog_select')
    with tempfile.TemporaryDirectory(prefix='catalog-memory-') as folder:
        folder = Path(folder)
        (folder / 'types.inc').write_text(types)
        (folder / 'record.inc').write_text(function(source, 'cd_catalog_record').replace('cd_catalog_record(', 'cd_catalog_record_impl(', 1))
        (folder / 'core.inc').write_text('\n'.join(function(source, x) for x in names))
        command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
            '-g', '-I'+str(repo/'cmd/zfs'), '-I'+str(folder),
            str(Path(__file__).with_name('catalog_memory_test.c'))]
        for sanitized in (False, True):
            binary = folder / ('sanitized' if sanitized else 'strict')
            extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []
            subprocess.run(command + extra + ['-o', str(binary)], check=True)
            subprocess.run([str(binary), str(folder)], check=True, timeout=45,
                stdout=subprocess.DEVNULL, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1'))
            reports = []
            for mode in ('memory', 'disk'):
                rows = [json.loads(x) for x in (folder/(mode+'.jsonl')).read_text().splitlines()]
                assert rows[0]['estimates_only'] and rows[0]['scope']=='sharing-group'
                assert len([x for x in rows if x['type']=='file'])==8
                assert any('"' in x['path'] and '\n' in x['path'] and 'α' in x['path'] for x in rows if x['type']=='file')
                assert any(os.fsencode(x['path']).endswith(b'\xce\xb1\xff') for x in rows if x['type']=='file')
                reports.append(rows[1:])
            assert reports[0]==reports[1], 'report groups/estimates/members differ'
        print('catalog memory/disk equivalence, bounded fallback, report escaping/atomicity, cancellation and I/O reduction: PASS')


if __name__ == '__main__':
    main()

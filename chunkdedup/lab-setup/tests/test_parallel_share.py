#!/usr/bin/env python3
"""Compile the actual share driver with bounded deterministic transport fixtures."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def function(source, name):
    lines = source.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.startswith(name + '('):
            assert i > 0 and lines[i - 1].startswith('static '), name
            for end in range(i + 1, len(lines)):
                if lines[end] == '}\n':
                    return ''.join(lines[i - 1:end + 1])
    raise AssertionError('missing production function: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    source = (repo / 'cmd/zfs/zfs_chunkdedup_user.c').read_text()
    here = Path(__file__).resolve().parent
    types = source[source.index('struct cd_catalog_file {'):
                   source.index('static boolean_t cd_deferred(int);')]
    begin, end = '/* SHARE_DRIVER_BEGIN */', '/* SHARE_DRIVER_END */'
    assert source.count(begin) == source.count(end) == 1, 'share extraction markers'
    core = source.split(begin, 1)[1].split(end, 1)[0]
    with tempfile.TemporaryDirectory(prefix='parallel-share-') as folder:
        folder = Path(folder)
        (folder / 'types.inc').write_text(types)
        (folder / 'catalog.inc').write_text('\n'.join(function(source, name) for name in
            ('cd_catalog_record', 'cd_catalog_next', 'cd_catalog_same')))
        (folder / 'transport.inc').write_text('\n'.join(function(source, name) for name in
            ('cd_identity', 'cd_catalog_open_file', 'cd_deferred')))
        (folder / 'share.inc').write_text(core)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-g',
            '-I' + str(repo / 'cmd/zfs'), '-I' + str(folder),
            str(here / 'parallel_share_test.c')]
        for sanitized in (False, True):
            binary = folder / ('share-sanitized' if sanitized else 'share')
            extra = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                     if sanitized else [])
            subprocess.run(command + extra + ['-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=45,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                         UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Compile the actual share driver with bounded deterministic transport fixtures."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
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


def emit(repo, folder):
    source = (repo / 'cmd/zfs/zfs_chunkdedup_user.c').read_text()
    types = source[source.index('struct cd_catalog_file {'):
                   source.index('static boolean_t cd_deferred(int);')]
    begin, end = '/* SHARE_DRIVER_BEGIN */', '/* SHARE_DRIVER_END */'
    assert source.count(begin) == source.count(end) == 1, 'share extraction markers'
    core = source.split(begin, 1)[1].split(end, 1)[0]
    (folder / 'types.inc').write_text(types)
    (folder / 'catalog.inc').write_text('\n'.join(function(source, name) for name in
        ('cd_catalog_record', 'cd_catalog_next', 'cd_catalog_same')))
    (folder / 'transport.inc').write_text('\n'.join(function(source, name) for name in
        ('cd_identity', 'cd_catalog_open_file', 'cd_deferred')))
    (folder / 'share.inc').write_text(core)
    (folder / 'apply.inc').write_text(function(source, 'cd_catalog_apply'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--emit-only', type=Path,
                       help='write extracted includes into a new directory; no compilation or execution')
    modes.add_argument('--native-large-snapshot', action='store_true',
                       help='FreeBSD only: actual 3.847 GiB snapshot capacity/cleanup check, unsanitized')
    args = parser.parse_args()
    repo = args.repo.resolve()
    here = Path(__file__).resolve().parent
    if args.emit_only:
        args.emit_only.mkdir(parents=True, exist_ok=False)
        emit(repo, args.emit_only)
        print('SHARE_FIXTURE_EMITTED', args.emit_only.resolve())
        return
    if args.native_large_snapshot:
        if not sys.platform.startswith('freebsd'):
            parser.error('--native-large-snapshot requires FreeBSD')
        pages = int(subprocess.check_output(['/sbin/sysctl', '-n', 'vm.stats.vm.v_free_count']))
        page_size = int(subprocess.check_output(['/sbin/sysctl', '-n', 'hw.pagesize']))
        if pages * page_size < 16 << 30:
            parser.error('--native-large-snapshot requires at least 16 GiB freshly free RAM')
    with tempfile.TemporaryDirectory(prefix='parallel-share-') as folder:
        folder = Path(folder)
        emit(repo, folder)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-g',
            '-I' + str(repo / 'cmd/zfs'), '-I' + str(folder),
            str(here / 'parallel_share_test.c')]
        for sanitized in ((False,) if args.native_large_snapshot else (False, True)):
            extra = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                     if sanitized else [])
            variants = ('native',) if args.native_large_snapshot else ('default', 'small')
            for variant in variants:
                binary = folder / f'share-{variant}-{sanitized}'
                definitions = (['-DCD_SHARE_SNAPSHOT_LIMIT=(UINT64_C(4)*sizeof(struct cd_catalog_file))']
                               if variant == 'small' else [])
                libraries = [] if sys.platform == 'darwin' else ['-lcrypto']
                subprocess.run(command + extra + definitions + ['-o', str(binary)] + libraries, check=True)
                arguments = (['--native-large-snapshot'] if variant == 'native' else
                             ['--small-cap'] if variant == 'small' else [])
                subprocess.run([str(binary)] + arguments, check=True,
                    timeout=660 if variant == 'native' else 45,
                    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                             UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Compile and exercise the actual FreeBSD dirty-data kstat functions.

The portable harness substitutes userspace mutex/kstat implementations. It does
not load a kernel module, access a pool, or establish native kernel behavior.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    source = (repo / 'module/zfs/dsl_pool.c').read_text()
    guard = '#if defined(_KERNEL) && defined(__FreeBSD__)\n'
    if source.count(guard) != 1:
        raise SystemExit('FAIL: expected one FreeBSD dirty-data implementation')
    implementation = source.split(guard, 1)[1].split('\n#else\n', 1)[0]
    for name in ('dsl_pool_dirty_kstat_update', 'dsl_pool_dirty_kstat_init',
                 'dsl_pool_dirty_kstat_fini'):
        if implementation.count('\n' + name + '(') != 1:
            raise SystemExit('FAIL: missing actual dirty-data function: ' + name)
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix='dirty-data-') as directory:
        directory = Path(directory)
        (directory / 'dirty_data_impl.inc').write_text(implementation)
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wno-missing-field-initializers', '-pthread', '-g',
            '-I' + str(directory), str(here / 'dirty_data_test.c')]
        for sanitized in (False, True):
            binary = directory / ('dirty-data-sanitized' if sanitized else 'dirty-data')
            extra = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                     if sanitized else [])
            subprocess.run(command + extra + ['-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                         UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))


if __name__ == '__main__':
    main()

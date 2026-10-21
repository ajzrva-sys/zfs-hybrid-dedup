#!/usr/bin/env python3
"""Compare the actual parallel helper and real sort processes with serial output."""
import argparse
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile


def execute(command, timeout=50):
    process = subprocess.Popen(command, start_new_session=True,
        env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                 UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
    try:
        code = process.wait(timeout=timeout)
        if code:
            raise subprocess.CalledProcessError(code, command)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    repo = parser.parse_args().repo.resolve()
    source = repo / 'cmd/zfs/chunkdedup_hybrid.c'
    fixture = Path(__file__).with_name('parallel_sort_test.c')
    with tempfile.TemporaryDirectory(prefix='parallel-sort-') as folder:
        folder = Path(folder)
        work = folder / 'runs'
        work.mkdir()
        base = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall',
            '-Wextra', '-Werror', '-Wwrite-strings', '-pthread', '-g', '-I' + str(source.parent),
            '-DHYBRID_SORT_SOURCE="' + str(source) + '"', str(fixture)]
        for sanitized in (False, True):
            binary = folder / ('sanitized' if sanitized else 'strict')
            extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []
            subprocess.run(base + extra + ['-o', str(binary)], check=True)
            execute([str(binary), str(work)])


if __name__ == '__main__':
    main()

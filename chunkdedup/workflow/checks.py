#!/usr/bin/env python3
"""Verify current prototype sources locally against immutable delivered tests.

No module loading, pool access, network, package installation or source writes.
All generated code, binaries, logs and reports remain in a new run directory.
Exit 0: PASS, 1: TEST_FAILURE, 2: INFRA_FAILURE without a test failure.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid

sys.dont_write_bytecode = True
REQUIRED = (
    'cmd/zfs/chunkdedup_chunker.c', 'cmd/zfs/chunkdedup_chunker.h',
    'cmd/zfs/rabin64_table.h', 'cmd/zfs/zfs_chunkdedup_user.c',
    'include/sys/zfs_chunkdedup.h', 'module/zfs/zfs_chunkdedup.c',
    'cmd/zfs/chunkdedup_hybrid.h',
)
MINIMUM_ASSERTIONS = {'chunker_api': 26, 'core_model': 104,
                      'worker_batches': 26, 'packaging': 8}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def tree(root):
    files = []
    for directory, dirs, names in os.walk(root, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d != '.git')
        for name in list(dirs):
            if (Path(directory) / name).is_symlink():
                names.append(name)
                dirs.remove(name)
        for name in sorted(names):
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            if path.is_symlink():
                files.append({'path': relative, 'type': 'symlink',
                              'sha256': hashlib.sha256(os.readlink(path).encode()).hexdigest(),
                              'mode': 755 if path.lstat().st_mode & 0o111 else 644})
            elif path.is_file():
                files.append({'path': relative, 'type': 'file', 'sha256': digest(path),
                              'mode': 755 if path.stat().st_mode & 0o111 else 644})
            else:
                raise ValueError(f'unsupported source file type: {relative}')
    return sorted(files, key=lambda item: item['path'])


def safe_relative(name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or str(p) != name:
        raise ValueError(f'unsafe manifest path: {name}')
    return p


class CheckError(Exception):
    def __init__(self, status, message, returncode=1):
        super().__init__(message)
        self.status, self.returncode = status, returncode


TRACE_HEADER = r'''
#ifndef WORKFLOW_ASSERTIONS_H
#define WORKFLOW_ASSERTIONS_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static uint64_t workflow_assertions, workflow_failed;
static void workflow_report(void) {
    const char *path = getenv("WORKFLOW_ASSERT_REPORT");
    FILE *file = path ? fopen(path, "w") : NULL;
    if (!file) { fputs("cannot write workflow assertion evidence\n", stderr); exit(97); }
    fprintf(file, "{\"assertions\":%llu,\"failed\":%llu}\n",
        (unsigned long long)workflow_assertions, (unsigned long long)workflow_failed);
    if (fclose(file)) exit(97);
}
static void workflow_check(int ok, const char *expression, const char *file, int line) {
    workflow_assertions++;
    if (!ok) {
        workflow_failed++; workflow_report();
        fprintf(stderr, "ASSERTION FAILED %s:%d: %s\n", file, line, expression);
        abort();
    }
}
#undef assert
#define assert(expression) workflow_check(!!(expression), #expression, __FILE__, __LINE__)
#endif
'''

PY_TRACE = r'''
import ast,json,os,sys
source=sys.argv[1]
sys.argv=sys.argv[1:]
counter={'assertions':0,'failed':0}
def checked(value):
    counter['assertions']+=1
    if not value: counter['failed']+=1
    return value
class Trace(ast.NodeTransformer):
    def visit_Assert(self,node):
        node.test=ast.Call(func=ast.Name(id='_workflow_checked',ctx=ast.Load()),args=[node.test],keywords=[])
        return node
unit=ast.fix_missing_locations(Trace().visit(ast.parse(open(source).read(),filename=source)))
scope={'__file__':source,'__name__':'__main__','_workflow_checked':checked}
try:
    exec(compile(unit,source,'exec'),scope)
finally:
    with open(os.environ['WORKFLOW_ASSERT_REPORT'],'w') as output:
        json.dump(counter,output)
'''


class Runner:
    def __init__(self, args):
        self.args, self.root = args, args.run_dir
        self.results, self.current = [], None
        self.cancelled = False
        self.environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                                TMPDIR=str(self.root / 'tmp'),
                                ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                                UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        self.cc = shlex.split(args.cc or os.environ.get('CC', 'clang'))
        self.crypto = [] if sys.platform == 'darwin' else ['-lcrypto']

    def artifact(self, path):
        path = Path(path).resolve()
        if not path.is_relative_to(self.root):
            raise ValueError('evidence artifact outside run directory')
        return {'path': path.relative_to(self.root).as_posix(), 'sha256': digest(path)}

    def save_artifact(self, filename, value):
        path = self.root / 'artifacts' / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        write_json(path, value)
        return self.artifact(path)

    def execute(self, command, *, cwd=None, env=None, stdout=None, infra=False,
                allow_failure=False):
        command = [str(part) for part in command]
        self.current['evidence'].setdefault('commands', []).append(command)
        if not self.current['command']:
            self.current['command'] = command
        log_path = self.root / self.current['log']
        with log_path.open('ab') as log:
            log.write(('\n$ ' + shlex.join(command) + '\n').encode())
            log.flush()
            output = open(stdout, 'wb') if stdout else log
            try:
                process = subprocess.Popen(command, cwd=cwd or self.root,
                                           env=env or self.environment,
                                           stdout=output, stderr=log,
                                           start_new_session=True)
                try:
                    result = process.wait(timeout=self.args.timeout)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        pass
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                    raise CheckError('INFRA_FAILURE' if infra else 'TEST_FAILURE',
                                     f'command timed out after {self.args.timeout}s', 124)
                except KeyboardInterrupt:
                    self.cancelled = True
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        pass
                    # The leader may have exited before a descendant did.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                    log.write(b'\nINFRA_FAILURE: verification cancelled; subprocess group stopped\n')
                    raise CheckError('INFRA_FAILURE', 'verification cancelled', 130)
            except FileNotFoundError as error:
                raise CheckError('INFRA_FAILURE', f'command unavailable: {error}', 127)
            finally:
                if stdout:
                    output.close()
            log.write(f'\nexit={result}\n'.encode())
        if result and not allow_failure:
            raise CheckError('INFRA_FAILURE' if infra else 'TEST_FAILURE',
                             f'command returned {result}', result)
        return result

    def check(self, name, function):
        self.current = {'name': name, 'status': 'PASS', 'returncode': 0,
                        'command': [], 'log': f'logs/{name}.log',
                        'evidence': {'artifacts': [], 'tested_sources_digest': self.tested_digest}}
        log = self.root / self.current['log']
        log.write_text('')
        started = time.monotonic()
        try:
            if self.cancelled:
                raise CheckError('INFRA_FAILURE', 'skipped after verification cancellation', 130)
            function()
        except KeyboardInterrupt:
            self.cancelled = True
            self.current.update(status='INFRA_FAILURE', returncode=130)
            self.current['evidence']['error'] = 'verification cancelled'
        except CheckError as error:
            self.current.update(status=error.status, returncode=error.returncode)
            self.current['evidence']['error'] = str(error)
        except (OSError, ValueError, KeyError, AssertionError, subprocess.SubprocessError) as error:
            self.current.update(status='INFRA_FAILURE', returncode=2)
            self.current['evidence']['error'] = f'{type(error).__name__}: {error}'
        self.current['seconds'] = round(time.monotonic() - started, 6)
        with log.open('a') as stream:
            stream.write(json.dumps({k: v for k, v in self.current.items() if k != 'log_sha256'},
                                    sort_keys=True) + '\n')
        self.current['log_sha256'] = digest(log)
        self.results.append(self.current)
        self.persist()

    def require_sources(self, *names):
        missing = [name for name in names if not (self.snapshot / name).is_file()]
        if missing:
            raise CheckError('TEST_FAILURE', f'missing current source inputs: {missing}')

    def assertions(self, name, path):
        if not path.is_file():
            raise CheckError('TEST_FAILURE', 'runtime did not produce assertion evidence')
        report = json.loads(path.read_text())
        self.current['evidence']['artifacts'].append(self.artifact(path))
        observed = report['assertions']
        self.current['evidence']['assertions'] = {
            'expected_minimum': MINIMUM_ASSERTIONS[name], 'observed': observed,
            'failed': report['failed']}
        if observed < MINIMUM_ASSERTIONS[name] or report['failed']:
            raise CheckError('TEST_FAILURE', f'insufficient or failed assertions: {report}')

    def baseline(self):
        manifest = self.args.kit / 'SHA256SUMS'
        wanted = lambda name: name.startswith(('conformance/', 'native-core-mock/')) or name in (
            'lab-setup/build-media.py', 'lab-setup/test_media.py',
            'lab-setup/verify-media.sh', 'lab-setup/tests/test_worker_batches.py')
        verified = []
        for line in manifest.read_text().splitlines():
            expected, name = line.split('  ', 1)
            safe_relative(name)
            if not wanted(name):
                continue
            source = self.args.kit / name
            if source.is_symlink() or not source.is_file() or digest(source) != expected:
                raise CheckError('INFRA_FAILURE', f'immutable kit mismatch: {name}')
            destination = self.baseline_root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            if digest(destination) != expected:
                raise CheckError('INFRA_FAILURE', f'immutable kit changed while copying: {name}')
            destination.chmod(0o444)
            verified.append({'path': name, 'sha256': expected})
        expected_inputs = ('conformance/chunker/test_chunker.c',
                           'conformance/chunker/chunkdedup_cli.c',
                           'conformance/chunker/fixture-manifest.json',
                           'native-core-mock/core_test.c', 'native-core-mock/include/sys/zfs_context.h',
                           'lab-setup/build-media.py', 'lab-setup/test_media.py',
                           'lab-setup/verify-media.sh', 'lab-setup/tests/test_worker_batches.py')
        present = {entry['path'] for entry in verified}
        if not set(expected_inputs).issubset(present):
            raise CheckError('INFRA_FAILURE', 'kit manifest omits required immutable test inputs')
        fixture_manifest = self.baseline_root / 'conformance/chunker/fixture-manifest.json'
        fixtures = json.loads(fixture_manifest.read_text())
        if fixtures['count'] != 35 or len(fixtures['fixtures']) != 35:
            raise CheckError('INFRA_FAILURE', 'expected exactly 35 original-C# fixture pairs')
        for fixture in fixtures['fixtures']:
            for name, expected in ((fixture['input'], fixture['sha256']),
                                   (fixture['expected'], fixture['expected_sha256'])):
                safe_relative(name)
                if digest(fixture_manifest.parent / 'fixtures' / name) != expected:
                    raise CheckError('INFRA_FAILURE', f'original-C# fixture changed: {name}')
        self.fixture_manifest = fixtures
        self.baseline_files = verified
        self.baseline_verified = True
        self.current['evidence'].update(
            counts={'verified_files': len(verified), 'fixture_pairs': 35, 'mismatches': 0},
            fixture_manifest_sha256=digest(fixture_manifest),
            immutable_snapshot_digest=hashlib.sha256(canonical(verified)).hexdigest())
        self.current['evidence']['artifacts'].append(self.save_artifact('baseline-files.json', verified))

    def toolchain(self):
        if not self.cc:
            raise CheckError('INFRA_FAILURE', 'empty compiler command')
        if any(arg.startswith('-DNDEBUG') or (arg == '-D' and index + 1 < len(self.cc)
               and self.cc[index + 1].startswith('NDEBUG')) for index, arg in enumerate(self.cc)):
            raise CheckError('INFRA_FAILURE', 'compiler command disables assertions with NDEBUG')
        folder = self.root / 'artifacts/toolchain'
        folder.mkdir(parents=True)
        source = folder / 'probe.c'
        source.write_text('#include <stdlib.h>\n#include <assert.h>\n'
                          + ('#include <CommonCrypto/CommonDigest.h>\n' if not self.crypto else '#include <openssl/sha.h>\n')
                          + 'int main(void) { unsigned char digest[64];\n'
                          + ('CC_SHA512("x",1,digest);\n' if not self.crypto else 'SHA512((const unsigned char *)"x",1,digest);\n')
                          + 'assert(digest[0]==0xa4 && digest[1]==0xab); return 0; }\n')
        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                 '-fsanitize=address,undefined', '-g']
        self.execute(self.cc + ['--version'], infra=True)
        self.execute(self.cc + flags + [str(source), '-o', str(folder / 'probe')] + self.crypto, infra=True)
        self.execute([folder / 'probe'], infra=True)
        bad = folder / 'asan-negative.c'
        bad.write_text('#include <stdlib.h>\nint main(void) { volatile int n=1; '
                       'char *p=malloc(n); p[n]=42; return p[n]; }\n')
        self.execute(self.cc + flags + [str(bad), '-o', str(folder / 'asan-negative')] + self.crypto, infra=True)
        result = self.execute([folder / 'asan-negative'], infra=True, allow_failure=True)
        log = (self.root / self.current['log']).read_text(errors='replace')
        if not result or 'AddressSanitizer: heap-buffer-overflow' not in log:
            raise CheckError('INFRA_FAILURE', 'sanitizer did not diagnose the negative runtime probe')
        self.toolchain_ok = True
        self.current['evidence'].update(counts={'probes_expected': 2, 'probes_completed': 2},
                                       negative_probe_returncode=result)
        self.current['evidence']['artifacts'] += [self.artifact(source), self.artifact(bad),
                                                self.artifact(folder / 'probe'), self.artifact(folder / 'asan-negative')]

    def ready(self):
        if not self.baseline_verified or not self.toolchain_ok:
            raise CheckError('INFRA_FAILURE', 'immutable baseline or toolchain preflight failed')

    def wrapped_test(self, name, original, anchor, include_flags, extra_sources, argc=False):
        self.ready()
        folder = self.root / 'artifacts' / name
        folder.mkdir(parents=True)
        trace = folder / 'workflow_assertions.h'
        trace.write_text(TRACE_HEADER)
        text = original.read_text()
        if anchor not in text:
            raise CheckError('INFRA_FAILURE', f'immutable harness anchor missing: {anchor}')
        text = text.replace(anchor, f'#include "{trace}"\n#define main workflow_test_main\n' + anchor, 1)
        text += '\n#undef main\n'
        text += ('int main(int argc,char **argv) { int rc=workflow_test_main(argc,argv); workflow_report(); return rc; }\n'
                 if argc else 'int main(void) { int rc=workflow_test_main(); workflow_report(); return rc; }\n')
        wrapped = folder / 'test.c'
        wrapped.write_text(text)
        binary, report = folder / 'test', folder / 'assertions.json'
        self.execute(self.cc + ['-std=gnu99', '-Wall', '-Wextra', '-Werror',
                               '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-g']
                     + include_flags + [str(wrapped)] + [str(p) for p in extra_sources]
                     + ['-o', str(binary)] + self.crypto)
        environment = dict(self.environment, WORKFLOW_ASSERT_REPORT=str(report))
        runtime_error = None
        try:
            self.execute([binary], env=environment)
        except CheckError as error:
            runtime_error = error
        self.current['evidence']['artifacts'] += [self.artifact(wrapped), self.artifact(binary)]
        self.assertions(name, report)
        if runtime_error:
            raise runtime_error

    def api(self):
        self.require_sources(*REQUIRED[:3])
        self.wrapped_test('chunker_api', self.baseline_root / 'conformance/chunker/test_chunker.c',
                          'struct capture {', ['-I' + str(self.snapshot / 'cmd/zfs')],
                          [self.snapshot / REQUIRED[0]], argc=True)

    def model(self):
        self.require_sources(*REQUIRED[4:])
        self.wrapped_test('core_model', self.baseline_root / 'native-core-mock/core_test.c',
                          'static pair_t *find',
                          ['-I' + str(self.baseline_root / 'native-core-mock/include'),
                           '-I' + str(self.snapshot / 'include'),
                           '-I' + str(self.snapshot / 'module/zfs')], [])
        self.current['evidence']['scope'] = 'Actual current core C against in-memory DMU/ZAP model; no native durability or concurrency proof'

    def conformance(self):
        self.ready()
        self.require_sources(*REQUIRED[:3])
        folder = self.root / 'artifacts/microsoft_conformance'
        folder.mkdir(parents=True)
        binary = folder / 'chunkdedup-cli'
        self.execute(self.cc + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                               '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-g',
                               '-I' + str(self.snapshot / 'cmd/zfs'),
                               str(self.baseline_root / 'conformance/chunker/chunkdedup_cli.c'),
                               str(self.snapshot / REQUIRED[0]), '-o', str(binary)] + self.crypto)
        rows, mismatches, total_expected, total_verified = [], 0, 0, 0
        for fixture in self.fixture_manifest['fixtures']:
            source = self.baseline_root / 'conformance/chunker/fixtures' / fixture['input']
            expected = source.with_name(fixture['expected'])
            output = folder / fixture['expected']
            self.execute([binary, source], stdout=output)
            reference = [line.split('\t') for line in expected.read_text().splitlines()]
            actual = [line.split('\t') for line in output.read_text().splitlines()]
            data = source.read_bytes()
            boundaries = len(reference) == len(actual)
            hashes, position = True, 0
            verified = 0
            try:
                for index, entry in enumerate(actual):
                    offset, length, chunk_hash = entry
                    offset, length = int(offset), int(length)
                    valid = offset == position and 0 < length <= 131072 and offset + length <= len(data)
                    if index >= len(reference) or entry[:2] != reference[index][:2]:
                        boundaries = False
                    if not valid or hashlib.sha512(data[offset:offset + length]).digest()[:32].hex() != chunk_hash:
                        hashes = False
                    else:
                        verified += 1
                    position = offset + length
                if position != len(data):
                    boundaries = False
            except (ValueError, IndexError):
                boundaries = hashes = False
            matches = boundaries and hashes and output.read_bytes() == expected.read_bytes()
            mismatches += not matches
            total_expected += len(reference)
            total_verified += verified
            rows.append({'name': fixture['input'], 'input_sha256': digest(source),
                         'expected_sha256': digest(expected), 'actual_sha256': digest(output),
                         'expected_chunks': len(reference), 'actual_chunks': len(actual),
                         'boundaries_match': boundaries, 'hashes_match': hashes,
                         'output': self.artifact(output)})
        self.current['evidence']['counts'] = {'fixtures_expected': 35, 'fixtures_completed': len(rows),
                                             'chunks_expected': total_expected, 'chunks_verified': total_verified,
                                             'mismatches': mismatches}
        self.current['evidence']['artifacts'] += [self.artifact(binary), self.save_artifact('fixture-results.json', rows)]
        if mismatches:
            raise CheckError('TEST_FAILURE', f'{mismatches} original-C# fixture mismatches')

    def worker(self):
        self.ready()
        self.require_sources(*REQUIRED[:4])
        folder = self.root / 'artifacts/worker_batches'
        folder.mkdir(parents=True)
        trace, report = folder / 'workflow_assertions.h', folder / 'assertions.json'
        trace.write_text(TRACE_HEADER)
        adapter = folder / 'compiler-adapter'
        config = {'cc': self.cc, 'folder': str(folder), 'trace': str(trace)}
        adapter.write_text('#!' + sys.executable + '\n' + 'import json,os,pathlib,shutil,subprocess,sys\n'
                           + 'config=json.loads(' + repr(json.dumps(config)) + ')\n'
                           + r'''args=sys.argv[1:]
original=next(pathlib.Path(a) for a in args if a.endswith('/test.c'))
text=original.read_text()
anchor='typedef unsigned int uint_t;'
assert anchor in text
text=text.replace(anchor,'#include "'+config['trace']+'"\n#define main workflow_test_main\n'+anchor,1)
text+='\n#undef main\nint main(void) { int rc=workflow_test_main(); workflow_report(); return rc; }\n'
folder=pathlib.Path(config['folder'])
source=folder/'test.c';source.write_text(text)
requested=pathlib.Path(args[args.index('-o')+1])
binary=folder/'test'
args=[str(source) if a==str(original) else a for a in args]
args[args.index('-o')+1]=str(binary)
command=config['cc']+args
(folder/'compile-command.json').write_text(json.dumps(command))
status=subprocess.run(command).returncode
if not status: shutil.copyfile(binary,requested);requested.chmod(0o755)
sys.exit(status)
''')
        adapter.chmod(0o755)
        environment = dict(self.environment, CC=str(adapter), WORKFLOW_ASSERT_REPORT=str(report))
        runtime_error = None
        try:
            self.execute([sys.executable, self.baseline_root / 'lab-setup/tests/test_worker_batches.py',
                          '--openzfs', self.snapshot, '--chunker', self.snapshot / 'cmd/zfs'], env=environment)
        except CheckError as error:
            runtime_error = error
        for name in ('test.c', 'test', 'compile-command.json'):
            if (folder / name).is_file():
                self.current['evidence']['artifacts'].append(self.artifact(folder / name))
        self.assertions('worker_batches', report)
        self.current['evidence']['scope'] = 'Actual current CLI batch functions and current chunker; ioctl/nvlist test doubles'
        policy = folder / 'hybrid-policy.json'
        self.execute([sys.executable, self.args.kit / 'lab-setup/tests/test_hybrid_policy.py',
                      '--repo', self.snapshot, '--report', policy],
                     env=dict(self.environment, CC=shlex.join(self.cc)))
        self.current['evidence']['artifacts'].append(self.artifact(policy))
        self.current['evidence']['hybrid_policy'] = json.loads(policy.read_text())

        if runtime_error:
            raise runtime_error

    def packaging(self):
        self.ready()
        folder = self.root / 'artifacts/packaging'
        folder.mkdir(parents=True)
        tracer, report = folder / 'trace.py', folder / 'assertions.json'
        tracer.write_text(PY_TRACE)
        environment = dict(self.environment, WORKFLOW_ASSERT_REPORT=str(report))
        runtime_error = None
        try:
            self.execute([sys.executable, tracer, self.baseline_root / 'lab-setup/test_media.py'], env=environment)
        except CheckError as error:
            runtime_error = error
        self.current['evidence']['artifacts'].append(self.artifact(tracer))
        self.assertions('packaging', report)
        self.current['evidence']['scope'] = 'Immutable media-builder unit fixtures; synthetic package files, no real package installation or VM actions'
        if runtime_error:
            raise runtime_error

    def persist(self):
        statuses = [row['status'] for row in self.results]
        status = ('TEST_FAILURE' if 'TEST_FAILURE' in statuses else
                  'INFRA_FAILURE' if 'INFRA_FAILURE' in statuses else 'PASS')
        write_json(self.root / 'results.json', {'schema': 1, 'run_id': self.args.run_id,
                   'source_digest': self.source_digest, 'tested_sources_digest': self.tested_digest,
                   'status': status, 'scope': 'local-only', 'source_stable': self.source_stable,
                   'source_manifest': self.artifact(self.root / 'source-files.json'),
                   'tested_sources_manifest': self.artifact(self.root / 'tested-sources.json'),
                   'results': self.results})
        return status

    def run(self):
        self.baseline_verified = self.toolchain_ok = False
        self.source_stable = True
        self.baseline_root = self.root / 'baseline'
        self.snapshot = self.root / 'source'
        self.snapshot.mkdir()
        initial = tree(self.args.repo)
        computed = hashlib.sha256(canonical(initial)).hexdigest()
        self.source_digest = self.args.source_digest or computed
        if self.args.source_digest and self.args.source_digest != computed:
            raise CheckError('INFRA_FAILURE', 'passed source_digest does not match canonical supplied source tree')
        relevant = [entry for entry in initial if entry['path'] in REQUIRED]
        for entry in relevant:
            if entry['type'] != 'file':
                continue
            destination = self.snapshot / entry['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.args.repo / entry['path'], destination)
            destination.chmod(0o755 if entry['mode'] == 755 else 0o644)
            if digest(destination) != entry['sha256']:
                raise CheckError('INFRA_FAILURE', f'source changed while copying: {entry["path"]}')
        self.tested_digest = hashlib.sha256(canonical(relevant)).hexdigest()
        write_json(self.root / 'source-files.json', {'schema': 1, 'source_digest': computed, 'files': initial})
        write_json(self.root / 'tested-sources.json', {'schema': 1, 'tested_sources_digest': self.tested_digest, 'files': relevant})
        for name, function in [('baseline_integrity', self.baseline), ('toolchain', self.toolchain),
                               ('chunker_api', self.api), ('microsoft_conformance', self.conformance),
                               ('core_model', self.model), ('worker_batches', self.worker),
                               ('packaging', self.packaging)]:
            self.check(name, function)
        if self.cancelled:
            self.source_stable = False
            self.persist()
            return 2
        self.source_stable = tree(self.args.repo) == initial
        staged_stable = all((self.snapshot / entry['path']).is_file()
                            and digest(self.snapshot / entry['path']) == entry['sha256']
                            for entry in relevant if entry['type'] == 'file')
        baseline_stable = all((self.baseline_root / entry['path']).is_file()
                              and digest(self.baseline_root / entry['path']) == entry['sha256']
                              for entry in getattr(self, 'baseline_files', []))
        self.source_stable = self.source_stable and staged_stable and baseline_stable
        if not self.source_stable:
            self.results[0]['status'] = 'INFRA_FAILURE'
            self.results[0]['returncode'] = 2
            self.results[0]['evidence']['error'] = 'source tree, staged sources or immutable baseline changed during verification'
            log = self.root / self.results[0]['log']
            with log.open('a') as stream:
                stream.write(self.results[0]['evidence']['error'] + '\n')
            self.results[0]['log_sha256'] = digest(log)
        status = self.persist()
        return {'PASS': 0, 'TEST_FAILURE': 1, 'INFRA_FAILURE': 2}[status]


def main():
    def terminate(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True, help='new directory, outside source and kit')
    parser.add_argument('--kit', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--cc', help='compiler command; defaults to shlex-parsed CC or clang')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--source-digest', help='optional orchestrator digest; must match supplied tree')
    parser.add_argument('--run-id', default=None)
    args = parser.parse_args()
    args.repo, args.kit, args.run_dir = args.repo.resolve(), args.kit.resolve(), args.run_dir.resolve()
    args.run_id = args.run_id or str(uuid.uuid4())
    if args.timeout <= 0 or not args.repo.is_dir() or not args.kit.is_dir():
        parser.error('positive timeout and existing source/kit directories required')
    if args.run_dir.exists() or any(args.run_dir.is_relative_to(path) for path in (args.repo, args.kit)):
        parser.error('run directory must be new and outside source and immutable kit')
    args.run_dir.mkdir(parents=True)
    for name in ('logs', 'artifacts', 'tmp'):
        (args.run_dir / name).mkdir()
    runner = Runner(args)
    try:
        status = runner.run()
    except (CheckError, OSError, ValueError, KeyboardInterrupt) as error:
        message = 'verification cancelled' if isinstance(error, KeyboardInterrupt) else str(error)
        log = args.run_dir / 'logs/driver-failure.log'
        log.write_text('INFRA_FAILURE: ' + message + '\n')
        write_json(args.run_dir / 'results.json', {'schema': 1, 'run_id': args.run_id,
                   'source_digest': args.source_digest, 'status': 'INFRA_FAILURE',
                   'scope': 'local-only', 'results': runner.results, 'error': message,
                   'failure_log': {'path': log.relative_to(args.run_dir).as_posix(), 'sha256': digest(log)}})
        status = 2
    print(args.run_dir / 'results.json')
    return status


if __name__ == '__main__':
    sys.exit(main())

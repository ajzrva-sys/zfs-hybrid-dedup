#!/usr/bin/env python3
"""Local tests for native runner guards/adapter/serial transport; no SSH or VM."""
import importlib.util
import ast
import copy
import difflib
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import tempfile
import subprocess
import sys
import tarfile
from types import SimpleNamespace
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('native_runner', HERE / 'native.py')
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


class FakeConsole:
    command = native.Console.command

    def __init__(self, code=0):
        self.lines = []
        self.code = code

    def send(self, data):
        self.lines.append(data)

    def until(self, pattern, timeout):
        if b'-END:' in pattern:
            data = b'output\nCDWF-unit-nonce-END:' + str(self.code).encode() + b'\n'
        elif b'-BEGIN' in pattern:
            data = b'CDWF-unit-nonce-BEGIN\n'
        else:
            data = b'LAB# '
        match = re.search(pattern, data)
        assert match
        return data, match


class NativeTests(unittest.TestCase):
    def runner(self, directory):
        return native.Runner(SimpleNamespace(run_dir=Path(directory) / 'run',
                    run_id='unit', source_digest='0' * 64, mode='build', host='bsd'))

    def test_embedded_scripts_compile(self):
        compile(native.HOST_SNAPSHOT, 'host', 'exec')
        compile(native.GUEST_DRIVER, 'guest', 'exec')

    def test_adapter_preserves_cases_and_exact_prefix_guards(self):
        tests = HERE.parent / 'lab-setup/tests'
        adapted, records, cases = native.adapt_tests(tests, 'unit-one',
                                                     '/root/lab/workflow/unit-one',
                                                     '/opt/chunkdedup-workflow/unit-one')
        self.assertEqual(len(cases), 21)
        self.assertEqual(native.suite_cases(adapted['regression.py'].decode()), cases)
        self.assertIn(b'strncmp(dataset, "cdwf-unit-one/", 14)', adapted['debug_ctl.c'])
        self.assertIn(b'strncmp(resolved, "/mnt/cdwf-unit-one/", 19)', adapted['debug_ctl.c'])
        self.assertNotIn(b'"cdlab/", 6', adapted['debug_ctl.c'])
        self.assertNotIn(b'"/mnt/cdlab/", 11', adapted['debug_ctl.c'])
        self.assertEqual([v['count'] for v in records['debug_ctl.c']['substitutions']], [1, 1, 2])
        self.assertEqual(records['io_paths.c']['substitutions'][0]['count'], 3)
        self.assertEqual([v['count'] for v in records['regression.py']['substitutions']], [64, 6, 7, 2])
        self.assertEqual(adapted['regression.py'].count(b"'chunkdedup', 'optimize', '--method=chunk'"), 7)
        self.assertEqual(adapted['regression.py'].count(b"'chunkdedup', 'watch', '--method=chunk'"), 2)
        self.assertIn('cachefile=none', native.GUEST_DRIVER)
        self.assertNotIn("'destroy'", native.GUEST_DRIVER)

    def test_hybrid_report_has_exact_capture_hash_and_separate_coverage(self):
        path = HERE.parent / 'lab-setup/tests/hybrid.py'
        spec = importlib.util.spec_from_file_location('hybrid_suite', path)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            config = {'pool': 'cdwf-unit', 'prefix': '/opt/unit', 'run_id': 'unit',
                      'nonce': 'nonce', 'source_digest': '0' * 64,
                      'hybrid_tests': {'hybrid.py': '1' * 64, 'hybrid_ctl.c': '2' * 64}}
            suite = module.Suite(SimpleNamespace(ROOT=Path('/mnt/cdwf-unit')), config, directory)
            suite.rows.append({'test': module.CASES[0], 'passed': True, 'detail': {}})
            suite.save('RUNNING')
            document = json.loads(suite.report.read_text())
            captured = Path(directory) / 'captured.json'
            native.atomic_json(captured, document)
            self.assertEqual(native.file_digest(suite.report), native.file_digest(captured))
            self.assertEqual(document['run_id'], 'unit')
            self.assertEqual(document['payload_nonce'], 'nonce')
            self.assertEqual(document['source_digest'], '0' * 64)
            self.assertEqual(document['test_files'], config['hybrid_tests'])
            self.assertEqual(len(module.CASES), 12)
            self.assertEqual(len(set(module.CASES)), 12)
            self.assertNotEqual(set(module.CASES), {name for name, _ in native.suite_cases(
                (HERE.parent / 'lab-setup/tests/regression.py').read_text())})

    def test_new_hybrid_media_and_report_are_bound_separately(self):
        self.assertEqual(native.HYBRID_TEST_FILES, ('hybrid.py', 'hybrid_ctl.c'))
        self.assertIn("t.getmembers()}=={'hybrid/'+name for name in C['hybrid_tests']}", native.GUEST_DRIVER)
        self.assertIn("sha(report),'test_files':C['hybrid_tests']", native.GUEST_DRIVER)
        self.assertIn("'hybrid-result.json'", native.GUEST_DRIVER)
        self.assertLess(native.GUEST_DRIVER.index("'checks':m.RESULTS,'original_suite_sha256'"),
                        native.GUEST_DRIVER.index('hybrid=h.run(m,C,R)'))

    def test_changed_test_pin_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            tests = Path(directory)
            for name in native.TEST_PINS:
                data = (HERE.parent / 'lab-setup/tests' / name).read_bytes()
                (tests / name).write_bytes(data + (b'\n# changed\n' if name == 'regression.py' else b''))
            with self.assertRaisesRegex(AssertionError, 'immutable native test changed'):
                native.adapt_tests(tests, 'unit', '/root/lab/workflow/unit', '/opt/unit')

    def test_manifest_exact_bytes_modes_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / 'a.c').write_text('int a;\n')
            (repo / 'a.c').chmod(0o644)
            (repo / 'link').symlink_to('a.c')
            entries = [{'path': 'a.c', 'type': 'file', 'sha256': native.file_digest(repo / 'a.c'), 'mode': 644},
                       {'path': 'link', 'type': 'symlink', 'sha256': native.digest(b'a.c'), 'mode': 755}]
            fingerprint = native.digest(json.dumps(entries, sort_keys=True, separators=(',', ':')).encode())
            manifest = {'schema': 1, 'files': entries, 'source_digest': fingerprint}
            self.assertEqual(native.validate_manifest(repo, manifest, fingerprint), entries)
            (repo / 'a.c').write_text('int changed;\n')
            with self.assertRaisesRegex(AssertionError, 'frozen source changed'):
                native.validate_manifest(repo, manifest, fingerprint)

    def test_serial_payload_uses_short_paced_lines(self):
        import base64
        console = FakeConsole()
        script = 'echo hello\n' * 200
        self.assertEqual(console.command(script, 'unit-nonce'), 'output\n')
        self.assertTrue(all(len(line) <= 180 for line in console.lines))
        chunks = []
        for line in console.lines:
            if line.startswith(b"printf '%s'"):
                chunks.append(shlex.split(line.decode())[2])
        wrapper = base64.b64decode(''.join(chunks), validate=True).decode()
        self.assertIn(script, wrapper)
        self.assertIn('CDWF-unit-nonce-END:%s', wrapper)
        self.assertGreater(len(chunks), 20)

    def test_serial_failure_is_not_pass(self):
        with self.assertRaises(native.NativeFailure) as caught:
            FakeConsole(7).command('false', 'unit-nonce', test=True)
        self.assertEqual(caught.exception.returncode, 7)
        self.assertTrue(caught.exception.test)

    def test_all_ssh_transports_use_authorized_root_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = self.runner(directory)
            with mock.patch.object(native.subprocess, 'run', return_value=SimpleNamespace(
                    returncode=0, stdout=b'', stderr=b'')) as call:
                runner.ssh('true')
                self.assertIn('root@bsd', call.call_args.args[0])
            with mock.patch.object(native.subprocess, 'Popen', side_effect=RuntimeError('fake')) as call:
                with self.assertRaisesRegex(RuntimeError, 'fake'):
                    native.Console('bsd', Path(directory) / 'console.log', lambda: None)
                self.assertIn('root@bsd', call.call_args.args[0])

    def test_emergency_cleanup_stops_only_lab_and_restores_config(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = self.runner(directory)
            runner.started = runner.changed_config = runner.locked = True
            events = []
            states = iter([['Running'], ['Stopped'], ['Stopped']])
            runner.lab_state = lambda timeout=10: next(states)
            runner.ssh = lambda command, **kwargs: events.append((command, kwargs)) or b''
            result = runner.restore_host(emergency=True)
            self.assertEqual(events[0][0], 'vm poweroff -f zfs-chunkdedup-lab')
            self.assertNotIn('vm stop ', '\n'.join(command for command, _ in events))
            self.assertTrue(any('/original.conf' in command for command, _ in events))
            self.assertTrue(all(kwargs.get('timeout') == 10 for _, kwargs in events))
            self.assertTrue(result['guest_stopped'])
            self.assertTrue(result['guest_module_restoration_skipped'])

    def test_storage_guard_during_build_is_infrastructure_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = self.runner(directory)
            def action():
                raise native.NativeFailure('storage guard', urgent=True)
            with self.assertRaises(native.NativeFailure):
                runner.stage('guest_build', ['fake build'], action, test=True)
            self.assertEqual(runner.rows[0]['status'], 'INFRA_FAILURE')

    def test_guest_uses_provisioned_absolute_python_and_package_path(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = self.runner(directory)
            calls = []
            runner.console = SimpleNamespace(command=lambda *args: calls.append(args) or '')
            runner.guest('provenance')
            self.assertTrue(calls[0][0].startswith(native.GUEST_ENV + '/usr/local/bin/python3.12 '))
            self.assertIn('/usr/local/bin', native.GUEST_ENV)
            self.assertNotIn("tools=['python3'", native.GUEST_DRIVER)

    def test_initial_carriage_return_waits_for_connected(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = self.runner(directory)
            events = []
            fake = SimpleNamespace(until=lambda pattern, timeout: events.append(('until', pattern)),
                                   send=lambda data: events.append(('send', data)),
                                   command=lambda *args: events.append(('command', args[0])))
            runner.monitor = lambda: None
            runner.ssh = lambda *args: b''
            with mock.patch.object(native, 'Console', return_value=fake):
                runner.boot()
            self.assertEqual(events[:3], [('until', rb'Connected\n'), ('send', b'\r'), ('until', rb'LAB# ')])
            self.assertIn('/usr/local/bin/python3.12 /media/cdwf/native_driver.py prepare', events[-1][1])

    def test_receiver_mounts_export_before_source_mountpoint(self):
        tree = ast.parse(native.GUEST_DRIVER)
        helper = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'export_order')
        namespace = {'POOL': 'cdwf-unit-recv'}
        exec(compile(ast.Module(body=[helper], type_ignores=[]), 'helper', 'exec'), namespace)
        order = namespace['export_order']
        self.assertEqual(order(['cdlab', 'cdlab-recv']), ['cdlab-recv', 'cdlab'])
        self.assertEqual(order(['cdwf-unit-recv', 'cdwf-unit-recv-recv']),
                         ['cdwf-unit-recv-recv', 'cdwf-unit-recv'])
        self.assertIn("for name in export_order(retained):command('/sbin/zpool','export',name)", native.GUEST_DRIVER)
        self.assertIn("for name in export_order(exact_pools(baseline['retained']))", native.GUEST_DRIVER)
        self.assertIn("for name in reversed(export_order(baseline['retained']))", native.GUEST_DRIVER)

    def build_helper(self):
        receipts = []
        helper = next(node for node in ast.walk(ast.parse(native.GUEST_DRIVER))
                      if isinstance(node, ast.FunctionDef) and node.name == 'build_part')
        namespace = {'started': 123.0, 'C': {'source_digest': '0' * 64},
                     'save': lambda name, value: receipts.append(dict(value)),
                     'time': SimpleNamespace(time=lambda: 124.0), 'subprocess': subprocess,
                     'sys': SimpleNamespace(stderr=io.StringIO()), 'json': json}
        exec(compile(ast.Module(body=[helper], type_ignores=[]), 'build_part', 'exec'), namespace)
        return namespace['build_part'], receipts

    def test_observed_mtree_install_failure_is_infrastructure(self):
        helper, receipts = self.build_helper()
        def observed_failure():
            error = RuntimeError('mtree: stage/usr/lib: No such file or directory; gmake install failed 2')
            error.returncode = 2
            raise error
        with self.assertRaises(SystemExit) as caught:
            helper('install', observed_failure)
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(receipts[-1]['status'], 'INFRA_FAILURE')
        self.assertEqual(receipts[-1]['operation_returncode'], 2)
        self.assertEqual(receipts[-1]['phase'], 'install')
        with self.assertRaises(native.NativeFailure) as serial:
            FakeConsole(2).command('install', 'unit-nonce', test=True)
        self.assertFalse(serial.exception.test)

    def test_compilation_failure_and_timeout_are_distinguished(self):
        helper, receipts = self.build_helper()
        def compiler_failure():
            error = RuntimeError('current-source gmake compilation failed 2')
            error.returncode = 2
            raise error
        with self.assertRaises(SystemExit) as caught:
            helper('compile', compiler_failure, False)
        self.assertEqual(caught.exception.code, 1)
        self.assertEqual(receipts[-1]['status'], 'TEST_FAILURE')
        self.assertEqual(receipts[-1]['operation_returncode'], 2)
        def timeout():
            raise subprocess.TimeoutExpired('gmake', 60)
        with self.assertRaises(SystemExit) as caught:
            helper('compile', timeout, False)
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(receipts[-1]['status'], 'INFRA_FAILURE')
        self.assertEqual(receipts[-1]['operation_returncode'], 124)

    def test_staged_module_debug_and_install_directories_exist(self):
        helper = next(node for node in ast.walk(ast.parse(native.GUEST_DRIVER))
                      if isinstance(node, ast.FunctionDef) and node.name == 'stage_hierarchy')
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory) / 'stage'
            namespace = {'stage': stage}
            exec(compile(ast.Module(body=[helper], type_ignores=[]), 'stage_hierarchy', 'exec'), namespace)
            namespace['stage_hierarchy']()
            self.assertTrue((stage / 'usr/lib').is_dir())
            self.assertTrue((stage / 'boot/modules').is_dir())
            self.assertIn("['build.log','build-console.log','regression.log']", native.GUEST_DRIVER)

    def source_fixture(self, directory):
        root = Path(directory)
        run = root / 'run'
        media = root / 'media'
        source = run / 'source'
        source.mkdir(parents=True)
        media.mkdir()
        path = source / 'gettext.m4'
        path.write_text('bundled gettext helper\n')
        path.chmod(0o644)
        entries = [{'path': path.name, 'type': 'file', 'mode': 644,
                    'sha256': native.file_digest(path)}]
        fingerprint = native.digest(json.dumps(entries, sort_keys=True, separators=(',', ':')).encode())
        (media / 'source-files.json').write_text(json.dumps({'files': entries}))
        with tarfile.open(media / 'source.tar.gz', 'w:gz') as archive:
            archive.add(path, arcname='source/gettext.m4')
        receipts = []
        namespace = {'R': run, 'MEDIA': media, 'pathlib': __import__('pathlib'),
                     'C': {'run_id': 'unit', 'source_digest': fingerprint},
                     'json': json, 'hashlib': hashlib, 'os': __import__('os'),
                     'tarfile': tarfile, 'difflib': difflib,
                     'time': SimpleNamespace(time=lambda: 123.0),
                     'sha': native.file_digest,
                     'save': lambda name, value: receipts.append((name, copy.deepcopy(value)))}
        helper = next(node for node in ast.parse(native.GUEST_DRIVER).body
                      if isinstance(node, ast.FunctionDef) and node.name == 'source_check')
        exec(compile(ast.Module(body=[helper], type_ignores=[]), 'source_check', 'exec'), namespace)
        return namespace['source_check'], path, entries, receipts

    def test_source_mutation_is_rejected_after_observation_and_diff_are_saved(self):
        with tempfile.TemporaryDirectory() as directory:
            check, path, entries, receipts = self.source_fixture(directory)
            self.assertEqual(check()['files'], entries)
            path.write_text('guest gettext helper replacement\n')
            with self.assertRaisesRegex(AssertionError, 'guest source differs'):
                check()
            name, receipt = receipts[-1]
            self.assertEqual(name, 'source-observed.json')
            self.assertEqual(receipt['changes'][0]['expected'], entries[0])
            self.assertEqual(receipt['changes'][0]['observed']['sha256'], native.file_digest(path))
            self.assertIn('-bundled gettext helper', receipt['changes'][0]['text_diff'])
            self.assertIn('+guest gettext helper replacement', receipt['changes'][0]['text_diff'])
            self.assertNotEqual(receipt['source_digest'], receipt['expected_source_digest'])
            self.assertIn('source-bootstrap.json', native.GUEST_DRIVER)
            self.assertIn('source-observed.json', native.GUEST_DRIVER.split("elif phase=='capture':", 1)[-1])

    def test_missing_source_is_rejected_with_complete_observation(self):
        with tempfile.TemporaryDirectory() as directory:
            check, path, entries, receipts = self.source_fixture(directory)
            path.unlink()
            with self.assertRaisesRegex(AssertionError, 'guest source differs'):
                check()
            receipt = receipts[-1][1]
            self.assertEqual(len(receipt['files']), len(entries))
            self.assertEqual(receipt['changes'][0]['observed']['type'], 'missing')
            self.assertIsNone(receipt['changes'][0]['observed']['sha256'])

    def test_bootstrap_tool_override_preserves_pinned_helpers_before_configure(self):
        tree = ast.parse(native.GUEST_DRIVER)
        update = next(node for node in ast.walk(tree)
                      if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                      and node.func.attr == 'update' and isinstance(node.args[0], ast.Dict)
                      and any(isinstance(key, ast.Constant) and key.value == 'AUTOPOINT'
                              for key in node.args[0].keys))
        values = ast.literal_eval(update.args[0])
        self.assertEqual(values['AUTOPOINT'], '/usr/bin/true')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            helper = source / 'gettext.m4'
            helper.write_text('pinned gettext helper\n')
            overwrite = source / 'autopoint'
            overwrite.write_text('#!/bin/sh\nprintf replacement > gettext.m4\n')
            overwrite.chmod(0o755)
            # Reproduce the observed replacement when no override is present.
            subprocess.run([str(overwrite), '--force'], cwd=source, check=True)
            self.assertEqual(helper.read_text(), 'replacement')
            helper.write_text('pinned gettext helper\n')
            subprocess.run(['/bin/sh', '-c', '"$AUTOPOINT" --force'],
                           cwd=source, env=values, check=True)
            self.assertEqual(helper.read_text(), 'pinned gettext helper\n')
        self.assertLess(native.GUEST_DRIVER.index("build_part('autogen'"),
                        native.GUEST_DRIVER.index("build_part('bootstrap_integrity'"))
        self.assertLess(native.GUEST_DRIVER.index("build_part('bootstrap_integrity'"),
                        native.GUEST_DRIVER.index("build_part('configure'"))


if __name__ == '__main__':
    unittest.main()

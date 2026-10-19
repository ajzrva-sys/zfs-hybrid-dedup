#!/usr/bin/env python3
"""Adversarial boundary checks for repair and isolated-source handling."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import subprocess
import sys

spec = importlib.util.spec_from_file_location('workflow_runner', Path(__file__).with_name('run.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RepairBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='chunkdedup-control-')
        self.root = Path(self.temp.name)
        (self.root / 'failure.log').write_text('Expected SHA-512 identifier differs from actual output.\n')
        self.manifest = {'stages': [{'name': 'microsoft_conformance', 'status': 'TEST_FAILURE',
                                    'log': 'failure.log'}]}
        self.proposal = {'diagnosis': 'Wrong digest prefix.', 'hypothesis': 'Restore SHA-512 bytes.',
                         'evidence': [{'stage': 'microsoft_conformance',
                                       'quote': 'Expected SHA-512 identifier differs'}],
                         'patch': '--- a/cmd/zfs/chunkdedup_chunker.c\n'
                                  '+++ b/cmd/zfs/chunkdedup_chunker.c\n'
                                  '@@ -1 +1 @@\n-wrong\n+right\n'}

    def tearDown(self):
        self.temp.cleanup()

    def test_valid_failure_bound_patch(self):
        self.assertEqual(runner.validate_proposal(self.proposal, self.root, self.manifest),
                         ['cmd/zfs/chunkdedup_chunker.c'])

    def test_cannot_repair_tests_or_escape(self):
        for path in ('tests/fixture.c', '../outside.c', 'cmd/../../outside.c', '/tmp/outside.c'):
            with self.subTest(path=path):
                value = dict(self.proposal, patch=self.proposal['patch'].replace('cmd/zfs/chunkdedup_chunker.c', path))
                with self.assertRaises(ValueError):
                    runner.validate_proposal(value, self.root, self.manifest)

    def test_cannot_claim_unobserved_evidence(self):
        for evidence in ([{'stage': 'microsoft_conformance', 'quote': 'Invented failure diagnosis'}],
                         [{'stage': 'toolchain', 'quote': 'Expected SHA-512 identifier differs'}], []):
            with self.assertRaises(ValueError):
                runner.validate_proposal(dict(self.proposal, evidence=evidence), self.root, self.manifest)

    def test_cannot_mistake_infrastructure_for_source_bug(self):
        self.manifest['stages'][0]['status'] = 'INFRA_FAILURE'
        with self.assertRaises(ValueError):
            runner.validate_proposal(self.proposal, self.root, self.manifest)

    def test_snapshot_preserves_input_and_rejects_external_link(self):
        source = self.root / 'original'
        source.mkdir()
        (source / 'unit.c').write_text('original\n')
        files = runner.inventory(source, ['unit.c'])
        candidate = self.root / 'candidate'
        runner.copy_source(source, candidate, files)
        (candidate / 'unit.c').write_text('changed\n')
        self.assertEqual((source / 'unit.c').read_text(), 'original\n')
        (source / 'external').symlink_to('/etc/passwd')
        with self.assertRaises(ValueError):
            runner.inventory(source, ['external'])

    def test_metadata_only_forbidden_delta_is_rejected(self):
        source = self.root / 'source'
        (source / 'cmd/zfs').mkdir(parents=True)
        (source / 'tests').mkdir()
        (source / 'cmd/zfs/unit.c').write_text('unit\n')
        (source / 'tests/known-good.c').write_text('test\n')
        files = runner.inventory(source, runner.all_source_names(source))
        (source / 'cmd/zfs/unit.c').write_text('changed\n')
        (source / 'tests/known-good.c').chmod(0o755)
        with self.assertRaises(ValueError):
            runner.validate_delta(files, source, ['cmd/zfs/unit.c'])

    def test_helper_abnormal_exit_cannot_use_stale_pass_receipt(self):
        folder = self.root / 'local'
        folder.mkdir()
        (folder / 'stage.log').write_text('previous completed stage\n')
        (self.root / 'local-driver.log').write_text('driver timed out\n')
        (folder / 'results.json').write_text(json.dumps({'results': [
            {'name': 'unit', 'status': 'PASS', 'log': 'stage.log'}]}))
        result = runner.import_stages(self.root, folder, 124)
        self.assertEqual(result[-1]['status'], 'INFRA_FAILURE')
        self.assertEqual(result[-1]['returncode'], 124)

    def test_real_git_difference_is_clean_but_whitespace_still_fails(self):
        before = self.root / 'before'
        after = self.root / 'after'
        before.mkdir()
        after.mkdir()
        (before / 'unit.c').write_text('original\n')
        for content, expected_code, expected_raw in [
                ('original\n', 0, 0), ('corrected\n', 0, 1),
                ('corrected \n', 3, 3), ('corrected\n\n', 3, 3)]:
            with self.subTest(content=content):
                (after / 'unit.c').write_text(content)
                command = ['git', 'diff', '--no-index', '--check', str(before), str(after)]
                code, _, raw = runner.whitespace_check(command, self.root / 'whitespace.log', no_index=True)
                self.assertEqual((code, raw), (expected_code, expected_raw))

    def test_installer_refuses_external_config_before_any_writes(self):
        source = self.root / 'install-repo'
        (source / 'cmd/zfs').mkdir(parents=True)
        (source / 'cmd/zfs/zfs_chunkdedup_user.c').write_text('/* fixture */\n')
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'preserved').write_text('keep\n')
        (source / '.codex').symlink_to(outside)
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('install.py')),
                                 '--repo', str(source)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((source / 'scripts').exists())
        self.assertEqual(sorted(p.name for p in outside.iterdir()), ['preserved'])

    def test_repair_limit_never_becomes_success(self):
        """Exercise the real controller with a deliberately always-failing gate.

        This checks loop bounds; actual C defect detection is a separate run.
        """
        source = self.root / 'repo'
        (source / 'cmd/zfs').mkdir(parents=True)
        unit = source / 'cmd/zfs/unit.c'
        unit.write_text('#define VALUE 0\n')
        calls = {'verify': 0, 'repair': 0}
        real_run = subprocess.run

        def fake_git_output(argv, **kwargs):
            return runner.BASE if 'rev-parse' in argv else b'cmd/zfs/unit.c\0'

        def allow_ancestry(argv, **kwargs):
            if 'merge-base' in argv:
                return subprocess.CompletedProcess(argv, 0)
            return real_run(argv, **kwargs)

        def always_fail(current, files, original, parent, number, args, controls, history):
            calls['verify'] += 1
            run = parent / ('attempt-%02d' % number)
            run.mkdir()
            runner.copy_source(current, run / 'source', files)
            (run / 'failure.log').write_text('Controlled gate remains failed for controller bound test.\n')
            manifest = {'stages': [{'name': 'microsoft_conformance', 'status': 'TEST_FAILURE',
                                   'log': 'failure.log'}]}
            return run, manifest, {'status': 'TEST_FAILURE'}, 1

        def next_proposal(run, manifest, parent, number, args):
            calls['repair'] += 1
            return {'diagnosis': 'Controlled gate remains failed.', 'hypothesis': 'Bound test %d' % number,
                    'evidence': [{'stage': 'microsoft_conformance',
                                  'quote': 'Controlled gate remains failed'}],
                    'patch': '--- a/cmd/zfs/unit.c\n+++ b/cmd/zfs/unit.c\n@@ -1 +1 @@\n'
                             '-#define VALUE %d\n+#define VALUE %d\n' % (number - 1, number)}

        output = self.root / 'limit-run'
        argv = ['run.py', '--repo', str(source), '--output', str(output), '--repair']
        with mock.patch.object(sys, 'argv', argv), \
                mock.patch.object(runner.subprocess, 'check_output', fake_git_output), \
                mock.patch.object(runner.subprocess, 'run', allow_ancestry), \
                mock.patch.object(runner, 'verify_attempt', always_fail), \
                mock.patch.object(runner, 'propose', next_proposal):
            self.assertEqual(runner.main(), 1)
        self.assertEqual(calls, {'verify': 4, 'repair': 3})
        self.assertEqual(json.loads((output / 'summary.json').read_text())['status'], 'TEST_FAILURE')
        self.assertEqual(unit.read_text(), '#define VALUE 0\n')


class NativeProfileSelectionTests(unittest.TestCase):
    def test_both_native_profiles_require_explicit_host(self):
        for profile in ('native', 'native-smoke'):
            with self.subTest(profile=profile):
                result = subprocess.run([sys.executable, str(Path(__file__).with_name('run.py')),
                                         '--repo', '/unused', '--profile', profile],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn('needs --host', result.stderr)

    def test_cached_receipt_option_is_smoke_only(self):
        for profile in ('local', 'native'):
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('run.py')),
                                     '--repo', '/unused', '--host', 'bsd', '--profile', profile,
                                     '--cached-build-receipt', '/unused.json'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('only for native-smoke', result.stderr)

    def test_only_single_smoke_build_skip_is_successful(self):
        build = dict(name='guest_build', status='SKIPPED', returncode=0, log='build.log')
        passed = dict(name='guest_regression', status='PASS', returncode=0, log='test.log')
        variants = [('native-smoke', [build, passed], 0, False),
                    ('native', [build, passed], 0, True),
                    ('native-smoke', [build, build, passed], 0, True),
                    ('native-smoke', [build, dict(passed, status='SKIPPED')], 0, True),
                    ('native-smoke', [build, dict(passed, returncode=1)], 0, True),
                    ('native-smoke', [dict(build, returncode=1), passed], 1, True),
                    ('native-smoke', [build, passed], 124, True),
                    ('native', [dict(build, status='PASS'), passed], 0, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); folder = root / 'native'; folder.mkdir()
            (root / 'native-driver.log').write_text('selection fixture only\n')
            for profile, rows, code, rejected in variants:
                with self.subTest(profile=profile, rows=rows, code=code):
                    runner.write(folder / 'results.json', dict(profile=profile, results=rows))
                    result = runner.import_stages(root, folder, code)
                    self.assertEqual(len(result) > len(rows), rejected)
                    if rejected: self.assertEqual(result[-1]['status'], 'INFRA_FAILURE')

    def test_selected_mode_keeps_same_local_prerequisite_flow(self):
        for profile, mode in [('local', None), ('native', 'build'), ('native-smoke', 'smoke')]:
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source, controls, parent = root / 'source', root / 'controls', root / 'run'
                (source / 'cmd/zfs').mkdir(parents=True)
                (source / 'cmd/zfs/unit.c').write_text('/* selection fixture */\n')
                controls.mkdir(); parent.mkdir()
                for name in ('run.py', 'checks.py', 'native.py', 'evaluate.py', 'policy.json', 'repair-schema.json'):
                    (controls / name).write_text('{}\n')
                files = runner.inventory(source, ['cmd/zfs/unit.c'])
                args = SimpleNamespace(profile=profile, run_id='selection', host='bsd',
                                       kit=root, check_timeout=60, native_timeout=60,
                                       cached_build_receipt=root / 'cached-build.json')
                native_calls, evaluation_profiles = [], []

                def execute(command, log, **kwargs):
                    log.write_text('Invocation-selection fixture only.\n')
                    if len(command) > 1 and Path(command[1]).name == 'evaluate.py':
                        run = parent / 'attempt-00'
                        evaluation_profiles.append(json.loads((run / 'run.json').read_text())['profile'])
                        runner.write(run / 'evaluation.json', {'status': 'PASS'})
                    if len(command) > 1 and Path(command[1]).name == 'native.py':
                        native_calls.append(command)
                    return 0, 0

                with mock.patch.object(runner, 'execute', execute), \
                        mock.patch.object(runner, 'import_stages', return_value=[]):
                    runner.verify_attempt(source, files, source, parent, 0, args, controls, [])
                self.assertEqual(evaluation_profiles, ['local'] if mode is None else ['local', profile])
                self.assertEqual(len(native_calls), 0 if mode is None else 1)
                if mode is not None:
                    command = native_calls[0]
                    self.assertEqual(command[command.index('--mode') + 1], mode)
                    self.assertEqual(command[command.index('--host') + 1], 'bsd')
                    if profile == 'native-smoke':
                        self.assertEqual(command[command.index('--cached-build-receipt') + 1], str(args.cached_build_receipt))
                    else:
                        self.assertNotIn('--cached-build-receipt', command)


if __name__ == '__main__':
    unittest.main(verbosity=2)

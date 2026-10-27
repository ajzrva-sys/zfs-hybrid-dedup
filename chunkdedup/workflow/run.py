#!/usr/bin/env python3
"""Freeze, execute, independently evaluate and optionally repair a prototype.

Only isolated candidate copies are repaired. Accepted changes are emitted as a
patch; this runner never changes the developer's checkout or installs host ZFS.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import shlex
import subprocess
import sys
import time
import uuid

BASE = '71a9f9578616a90c3c14bb59629fb4d31bfd68d1'
HERE = Path(__file__).resolve().parent
ALLOWED = ('cmd/', 'include/', 'lib/', 'module/')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def inventory(root, paths):
    result = []
    for name in sorted(paths):
        path = root / name
        if path.is_symlink():
            target = os.readlink(path)
            resolved = (path.parent / target).resolve()
            if os.path.isabs(target) or not resolved.is_relative_to(root.resolve()):
                raise ValueError('unsafe source symlink: ' + name)
            result.append(dict(path=name, type='symlink',
                               sha256=hashlib.sha256(target.encode()).hexdigest(), mode=755))
        elif path.is_file():
            result.append(dict(path=name, type='file', sha256=sha(path),
                               mode=755 if path.stat().st_mode & 0o111 else 644))
        else:
            raise ValueError('source is missing: ' + name)
    return result


def tree_digest(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True,
                                    separators=(',', ':')).encode()).hexdigest()


def copy_source(origin, target, files):
    target.mkdir()
    for entry in files:
        src, dst = origin / entry['path'], target / entry['path']
        dst.parent.mkdir(parents=True, exist_ok=True)
        if entry['type'] == 'symlink':
            dst.symlink_to(os.readlink(src))
        else:
            shutil.copyfile(src, dst)
            dst.chmod(int(str(entry['mode']), 8))
    if inventory(target, [e['path'] for e in files]) != files:
        raise ValueError('source changed while freezing')


def all_source_names(root):
    names = []
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in list(dirs):
            if (Path(directory) / name).is_symlink():
                files.append(name)
                dirs.remove(name)
        names.extend((Path(directory) / name).relative_to(root).as_posix() for name in files)
    return sorted(names)


def validate_delta(before, candidate, declared):
    """Git metadata can rename/chmod paths absent from ---/+++ headers."""
    after = inventory(candidate, all_source_names(candidate))
    previous = {x['path']: x for x in before}
    current = {x['path']: x for x in after}
    changed = {name for name in set(previous) | set(current) if previous.get(name) != current.get(name)}
    if not changed or not changed <= set(declared) or any(not name.startswith(ALLOWED) for name in changed):
        raise ValueError('repair actual delta contains undeclared/forbidden paths: ' + repr(sorted(changed)))
    return after


def execute(argv, log, cwd=None, timeout=600, data=None, grace=10):
    start = time.monotonic()
    with log.open('wb') as output:
        try:
            # A new process group makes timeout cancellation include descendants.
            process = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                process.communicate(data, timeout=timeout)
                code = process.returncode
            except subprocess.TimeoutExpired:
                import signal
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=grace)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                output.write(b'\nINFRA_FAILURE: command timeout\n')
                code = 124
            except KeyboardInterrupt:
                import signal
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=grace)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                raise
        except OSError as error:
            output.write(('INFRA_FAILURE: ' + str(error) + '\n').encode())
            code = 127
    return code, round(time.monotonic() - start, 3)


def stage(name, run, command, code, seconds, log, evidence, infra=False):
    return dict(name=name, status='PASS' if code == 0 else
                ('INFRA_FAILURE' if infra or code in (124, 127) else 'TEST_FAILURE'),
                returncode=code, seconds=seconds, command=command,
                log=log.relative_to(run).as_posix(), log_sha256=sha(log), evidence=evidence)


def import_stages(run, folder, code):
    path = folder / 'results.json'
    if not path.is_file():
        log = folder.parent / (folder.name + '-driver.log')
        return [stage(folder.name + '_driver', run, [], code or 2, 0, log,
                      {'error': 'helper produced no structured evidence'}, infra=True)]
    report = json.loads(path.read_text())
    results = report['results']
    for item in results:
        item['log'] = (folder / item['log']).relative_to(run).as_posix()
        # Evidence paths in helpers are relative to their own result directory.
        item['evidence_root'] = folder.relative_to(run).as_posix()
    smoke_skip = (report.get('profile') == 'native-smoke' and
                  sum(item['name'] == 'guest_build' for item in results) == 1 and
                  sum(item['name'] == 'guest_build' and item['status'] == 'SKIPPED'
                      for item in results) == 1)
    successful = results and all(item.get('returncode') == 0 and
                (item['status'] == 'PASS' or
                 (smoke_skip and item['name'] == 'guest_build' and item['status'] == 'SKIPPED'))
                for item in results)
    invalid_pass = any(item['status'] in ('PASS', 'SKIPPED') and item.get('returncode') != 0 for item in results)
    if invalid_pass or (code != 0 and successful) or (code == 0 and not successful):
        log = folder.parent / (folder.name + '-driver.log')
        results.append(stage(folder.name + '_driver', run, [], code or 2, 0, log,
                             {'error': 'helper exit contradicts structured outcomes'}, infra=True))
    return results


def whitespace_check(command, log, no_index=False):
    code, seconds = execute(command, log)
    raw_code = code
    # --no-index returns 1 for a clean difference, including with --check.
    # A diagnostic or any other status still fails the whitespace gate.
    if no_index and code == 1 and log.read_bytes() == b'':
        code = 0
    return code, seconds, raw_code


def verify_attempt(source, files, original, parent, number, args, controls, repairs):
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    nonce = args.run_id + '-a%d' % number
    run = parent / ('attempt-%02d' % number)
    run.mkdir()
    frozen = run / 'source'
    copy_source(source, frozen, files)
    digest = tree_digest(files)
    write(run / 'source-files.json', dict(schema=1, source_digest=digest, files=files))
    shutil.copytree(controls, run / 'control')
    log = run / 'source-freeze.log'
    log.write_text('Frozen %d source entries\nSHA256 %s\n' % (len(files), digest))
    stages = [stage('source_freeze', run, ['source snapshot'], 0, 0, log,
                    {'files_count': len(files), 'tree_digest': digest})]
    log = run / 'static-analysis.log'
    if number == 0:
        command = ['git', '-C', str(original), 'diff', '--check']
        code, seconds, raw_code = whitespace_check(command, log)
    else:
        command = ['git', 'diff', '--no-index', '--check', str(parent / 'attempt-00/source'), str(frozen)]
        code, seconds, raw_code = whitespace_check(command, log, no_index=True)
    stages.append(stage('static_analysis', run, command, code, seconds, log,
                        {'check': 'git whitespace/error check', 'git_returncode': raw_code}))
    command = [sys.executable, str(run / 'control/checks.py'), '--repo', str(frozen),
               '--kit', str(args.kit), '--run-dir', str(run / 'local'), '--source-digest', digest,
               '--run-id', nonce]
    code, _ = execute(command, run / 'local-driver.log', timeout=args.check_timeout)
    stages.extend(import_stages(run, run / 'local', code))
    manifest = dict(schema=1, run_id=nonce, profile=args.profile,
                    kit={'path': str(args.kit)},
                    started=started,
                    source=dict(path=str(frozen), original_path=str(original), base_revision=BASE,
                                tree_digest=digest, files=files),
                    policy_sha256=sha(controls / 'policy.json'),
                    helpers={p.name: sha(p) for p in controls.iterdir() if p.is_file()},
                    stages=stages, repair_history=repairs)
    if args.profile in ('native', 'native-smoke') and all(s['status'] == 'PASS' for s in stages):
        # Independent source/test/control acceptance must precede any VM writes.
        write(run / 'run.json', dict(manifest, profile='local'))
        evaluator = [sys.executable, str(run / 'control/evaluate.py'), '--run-dir', str(run),
                     '--policy', str(run / 'control/policy.json')]
        code, _ = execute(evaluator, run / 'prerequisite-evaluator.log')
        prerequisite = run / 'evaluation.json'
        passed = code == 0 and prerequisite.is_file() and json.loads(prerequisite.read_text())['status'] == 'PASS'
        if prerequisite.exists():
            prerequisite.replace(run / 'prerequisite-evaluation.json')
        if passed:
            command = [sys.executable, str(run / 'control/native.py'), '--repo', str(frozen),
                       '--kit', str(args.kit), '--run-dir', str(run / 'native'),
                       '--source-manifest', str(run / 'source-files.json'), '--source-digest', digest,
                       '--run-id', nonce, '--host', args.host,
                       '--mode', 'smoke' if args.profile == 'native-smoke' else 'build']
            if getattr(args, 'crash_recovery', False):
                command += ['--crash-recovery']
            if args.profile == 'native-smoke':
                command += ['--cached-build-receipt', str(args.cached_build_receipt)]
            code, _ = execute(command, run / 'native-driver.log', timeout=args.native_timeout, grace=180)
            stages.extend(import_stages(run, run / 'native', code))
    write(run / 'run.json', manifest)
    command = [sys.executable, str(run / 'control/evaluate.py'), '--run-dir', str(run),
               '--policy', str(run / 'control/policy.json')]
    code, _ = execute(command, run / 'evaluator.log')
    if not (run / 'evaluation.json').exists():
        write(run / 'evaluation.json', dict(status='INFRA_FAILURE',
              feedback=['Evaluator did not produce evidence; inspect evaluator.log']))
        code = 2
    evaluation = json.loads((run / 'evaluation.json').read_text())
    if code != 0 and evaluation['status'] == 'PASS':
        shutil.copyfile(run / 'evaluation.json', run / 'evaluator-rejected.json')
        evaluation['status'] = 'INFRA_FAILURE'
        evaluation.setdefault('feedback', []).append({'category': 'INFRA_FAILURE',
                  'stage': 'evaluator_driver', 'rule': 'contradictory_exit',
                  'observed': code, 'expected': 0, 'action': 'Inspect evaluator.log; PASS is rejected.'})
        write(run / 'evaluation.json', evaluation)
    print('%s: %s — %s' % (run.name, evaluation['status'], run), flush=True)
    return run, manifest, evaluation, code


def failure_records(run, manifest):
    records = {s['name']: s for s in manifest['stages'] if s['status'] == 'TEST_FAILURE'}
    evaluation = run / 'evaluation.json'
    if evaluation.is_file():
        stages = {s['name']: s for s in manifest['stages']}
        grouped = {}
        for feedback in json.loads(evaluation.read_text()).get('feedback', []):
            if isinstance(feedback, dict) and feedback.get('category') == 'TEST_FAILURE' and feedback.get('stage') in stages:
                grouped.setdefault(feedback['stage'], []).append(feedback)
        for name, feedback in grouped.items():
            if name not in records:
                # A performance/behavior gate can fail after execution itself passed.
                # Preserve the evaluator's observed values as a diagnostic receipt.
                log = run / ('evaluated-failure-' + name + '.log')
                log.write_text(json.dumps(feedback, indent=2) + '\n')
                records[name] = dict(stages[name], status='TEST_FAILURE', log=log.name,
                                     evidence={'evaluation_feedback': feedback})
    return records


def validate_proposal(proposal, run, manifest):
    for key in ('diagnosis', 'hypothesis', 'patch'):
        if not isinstance(proposal.get(key), str) or not proposal[key].strip():
            raise ValueError('repair proposal missing ' + key)
    if len(proposal['patch'].encode()) > 1048576:
        raise ValueError('repair patch exceeds 1 MiB')
    failed = failure_records(run, manifest)
    citations = proposal.get('evidence', [])
    if not citations:
        raise ValueError('repair needs observed failure evidence')
    for item in citations:
        item_stage = failed.get(item.get('stage'))
        quote = item.get('quote', '')
        if not item_stage or len(quote.strip()) < 10 or quote not in (run / item_stage['log']).read_text(errors='replace'):
            raise ValueError('repair cites no matching observed failure')
    touched = []
    for line in proposal['patch'].splitlines():
        if line.startswith(('rename from ', 'rename to ', 'copy from ', 'copy to ')):
            raise ValueError('repair needs textual file edits, not rename/copy metadata')
        if line.startswith(('old mode ', 'new mode ', 'new file mode ', 'deleted file mode ')) and line.split()[-1] not in ('100644', '100755'):
            raise ValueError('repair cannot introduce symlinks or submodules')
        if line.startswith('diff --git '):
            fields = shlex.split(line)
            if len(fields) != 4 or any(not name.startswith(('a/', 'b/')) or
                      not name[2:].startswith(ALLOWED) or '..' in Path(name[2:]).parts for name in fields[2:]):
                raise ValueError('Git patch metadata refers to a forbidden path')
        if line.startswith(('--- ', '+++ ')):
            name = line[4:].split('\t', 1)[0]
            if name == '/dev/null':
                continue
            if not name.startswith(('a/', 'b/')):
                raise ValueError('patch must use a/ and b/ paths')
            name = name[2:]
            if not name.startswith(ALLOWED) or '..' in Path(name).parts or '\\' in name:
                raise ValueError('repair path outside implementation: ' + name)
            touched.append(name)
    if not touched or 'GIT binary patch' in proposal['patch']:
        raise ValueError('repair needs a textual implementation patch')
    return sorted(set(touched))


def propose(run, manifest, parent, attempt, args):
    schema = parent / 'control/repair-schema.json'
    output = parent / ('repair-%d.json' % attempt)
    failed = [dict(name=s['name'], log=str(run / s['log']), evidence=s.get('evidence', {}))
              for s in failure_records(run, manifest).values()]
    context = dict(run_id=manifest['run_id'], source=str(run / 'source'), failed_stages=failed,
                   evaluation=str(run / 'evaluation.json'), repair_history=manifest.get('repair_history', []))
    prompt = ('Diagnose the actual failing verification in %s. Inspect evaluation.json, failing logs and '
              'the relevant implementation. Do not dump the full run.json source inventory. '
              'Return one JSON repair proposal with diagnosis, observed stage/quote evidence, '
              'testable hypothesis, and a unified textual patch using a/ and b/ paths. Only cmd/, '
              'include/, lib/, module/ implementation files may change. Preserve behavior and Microsoft '
              'chunker conformance. Do not edit tests, harnesses, evaluation or policy. You have read-only '
              'access: return the patch, do not apply it. A missing tool or environment is not a source '
              'bug. Avoid previous unsuccessful approaches in repair_history. Keep the change minimal.' % run)
    prompt += (' Each evidence.stage MUST be an EXACT name from failed_stages below, without filenames, '
               'explanations or suffixes. Each evidence.quote MUST be an exact contiguous substring '
               'copied from that stage log, preserving actual whitespace. Do not use ellipses, '
               'paraphrases, evaluation filenames, source snippets, or your own verification as '
               'stage-log evidence. One precise failure quote is sufficient.\n' + json.dumps(context))
    if args.repair_command:
        command = json.loads(args.repair_command)
        if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
            raise ValueError('--repair-command must be a JSON argv array')
        # The command reads a structured request on stdin and writes proposal JSON to stdout.
        request = dict(run_dir=str(run), source=str(run / 'source'), attempt=attempt, prompt=prompt)
        code, _ = execute(command, output, cwd=run / 'source', timeout=args.repair_timeout,
                          data=json.dumps(request).encode())
    else:
        command = ['codex', '--no-daemon', '--ask-for-approval', 'never',
                   'exec', '--ignore-user-config', '--ephemeral',
                   '--skip-git-repo-check', '--sandbox', 'read-only',
                   '--json', '--cd', str(run / 'source'), '--output-schema', str(schema),
                   '--output-last-message', str(output), '-']
        code, _ = execute(command, parent / ('repair-%d-events.jsonl' % attempt),
                          timeout=args.repair_timeout, data=prompt.encode())
    if code:
        raise RuntimeError('repair provider failed with exit %d; preserved logs' % code)
    return json.loads(output.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--kit', type=Path, default=HERE.parent)
    parser.add_argument('--profile', choices=('local', 'native', 'native-smoke'), default='local')
    parser.add_argument('--crash-recovery', action='store_true', help='test-only fault build and guest SIGKILL suite; native fresh build only')
    parser.add_argument('--host', help='explicit SSH host for the isolated bhyve lab')
    parser.add_argument('--cached-build-receipt', type=Path, help='prior PASS build receipt; native-smoke only')
    parser.add_argument('--output', type=Path, help='new run directory; defaults to REPO_PARENT/verification')
    parser.add_argument('--repair', action='store_true', help='request at most three evidence-based repair proposals')
    parser.add_argument('--repair-command', help='JSON argv for a custom proposal provider; implies --repair')
    parser.add_argument('--check-timeout', type=int, default=600)
    parser.add_argument('--native-timeout', type=int, default=5400)
    parser.add_argument('--repair-timeout', type=int, default=600)
    args = parser.parse_args()
    if args.crash_recovery and args.profile != 'native':
        parser.error('--crash-recovery requires --profile native and a fresh test-only build')
    args.repo, args.kit = args.repo.resolve(), args.kit.resolve()
    if args.profile in ('native', 'native-smoke') and not args.host:
        parser.error('native verification needs --host; no host is selected implicitly')
    if args.cached_build_receipt and args.profile != 'native-smoke':
        parser.error('--cached-build-receipt is accepted only for native-smoke')
    if args.profile == 'native-smoke' and not args.cached_build_receipt:
        parser.error('native-smoke needs --cached-build-receipt to freeze the actual cached prefix')
    if args.cached_build_receipt:
        args.cached_build_receipt = args.cached_build_receipt.resolve()
    run_id = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ').lower() + '-' + uuid.uuid4().hex[:8]
    args.run_id = run_id
    parent = (args.output or args.repo.parent / 'verification' / run_id).resolve()
    if parent.exists() or parent.is_relative_to(args.repo) or parent.is_relative_to(args.kit):
        parser.error('output must be new and outside source and kit')
    parent.mkdir(parents=True)
    try:
        head = subprocess.check_output(['git', '-C', str(args.repo), 'rev-parse', 'HEAD'], text=True).strip()
        subprocess.run(['git', '-C', str(args.repo), 'merge-base', '--is-ancestor', BASE, head], check=True)
        names = subprocess.check_output(['git', '-C', str(args.repo), 'ls-files', '-z']).decode().split('\0')
        names = [x for x in names if x and x != 'scripts/zfs-images']
        files = inventory(args.repo, names)
        original_files, original_names = files, sorted(names)
        def original_guard():
            current_names = subprocess.check_output(['git', '-C', str(args.repo), 'ls-files', '-z']).decode().split('\0')
            current_names = sorted(x for x in current_names if x and x != 'scripts/zfs-images')
            if current_names != original_names or inventory(args.repo, current_names) != original_files:
                raise ValueError('original checkout changed during verification; changes were preserved, not reverted')
        controls = parent / 'control'
        controls.mkdir()
        for name in ('run.py', 'checks.py', 'native.py', 'evaluate.py', 'policy.json', 'repair-schema.json'):
            shutil.copyfile(HERE / name, controls / name)
        source = args.repo
        repairs, seen = [], {tree_digest(files)}
        maximum = 3 if args.repair or args.repair_command else 0
        for attempt in range(maximum + 1):
            run, manifest, evaluation, code = verify_attempt(source, files, args.repo, parent,
                                                            attempt, args, controls, repairs)
            original_guard()
            if evaluation['status'] == 'PASS':
                if attempt:
                    patch = subprocess.run(['git', 'diff', '--no-index', '--binary',
                          str(parent / 'attempt-00/source'), str(run / 'source')], stdout=subprocess.PIPE).stdout
                    # Normalize temporary absolute paths into ordinary patch paths.
                    for old, new in (('a' + str(parent / 'attempt-00/source') + '/', 'a/'),
                                     ('b' + str(run / 'source') + '/', 'b/')):
                        patch = patch.replace(old.encode(), new.encode())
                    (parent / 'accepted.patch').write_bytes(patch)
                write(parent / 'summary.json', dict(status='PASS', profile=args.profile, attempts=attempt + 1,
                      accepted_candidate=str(run / 'source'), evaluation=str(run / 'evaluation.json'),
                      original_checkout_modified=False,
                      original_checkout_verified_digest=tree_digest(original_files), repair_history=repairs))
                return 0
            if evaluation['status'] != 'TEST_FAILURE' or attempt == maximum:
                write(parent / 'summary.json', dict(status=evaluation['status'], attempts=attempt + 1,
                      evaluation=str(run / 'evaluation.json'), repair_history=repairs))
                return 1 if evaluation['status'] == 'TEST_FAILURE' else 2
            proposal = propose(run, manifest, parent, attempt + 1, args)
            original_guard()
            paths = validate_proposal(proposal, run, manifest)
            candidate = parent / ('candidate-%d' % (attempt + 1))
            copy_source(run / 'source', candidate, files)
            patch = parent / ('repair-%d.patch' % (attempt + 1))
            patch.write_text(proposal['patch'])
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=candidate, check=True)
            subprocess.run(['git', 'apply', str(patch)], cwd=candidate, check=True)
            files = validate_delta(files, candidate, paths)
            names = [x['path'] for x in files]
            digest = tree_digest(files)
            if digest in seen:
                raise ValueError('repair repeats an already tested source tree')
            seen.add(digest)
            repairs.append(dict(attempt=attempt + 1, diagnosis=proposal['diagnosis'],
                         evidence=proposal['evidence'], hypothesis=proposal['hypothesis'],
                         paths=paths, patch_sha256=sha(patch), source_digest=digest))
            source = candidate
        return 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, RuntimeError, KeyboardInterrupt) as error:
        write(parent / 'summary.json', dict(status='INFRA_FAILURE', error=str(error)))
        print('INFRA_FAILURE: %s\nEvidence: %s' % (error, parent), file=sys.stderr)
        return 2


if __name__ == '__main__':
    def cancelled(signum, frame):
        raise KeyboardInterrupt('verification cancelled')
    signal.signal(signal.SIGTERM, cancelled)
    sys.exit(main())

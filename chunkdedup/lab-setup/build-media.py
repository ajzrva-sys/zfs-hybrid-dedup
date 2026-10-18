#!/usr/bin/env python3
"""Freeze source-only offline inputs and optionally build a read-only ISO.

Does not contact a remote host, install packages, mount images, or start a VM.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile

BASE_TAG = 'zfs-2.4.4'
BASE_COMMIT = '71a9f9578616a90c3c14bb59629fb4d31bfd68d1'
MICROSOFT_COMMIT = '852a00e5b45ef8a8d06a2eb9040495c36243078f'
NEW_SOURCE = {
    'cmd/zfs/CHUNKDEDUP-MIT-LICENSE', 'cmd/zfs/chunkdedup_chunker.c',
    'cmd/zfs/chunkdedup_chunker.h', 'cmd/zfs/rabin64_table.h',
    'cmd/zfs/zfs_chunkdedup_user.c', 'include/sys/zfs_chunkdedup.h',
    'module/zfs/zfs_chunkdedup.c',
}
TEST_NAMES = {
    'io_paths.c', 'debug_ctl.c', 'regression.py', 'legacy_gate.py',
    'IO_PATHS.md', 'REGRESSION.md', 'test_worker_batches.py',
}
PACKAGE_NAMES = {
    'bash', 'autoconf', 'automake', 'libtool', 'gmake', 'gettext-tools',
    'gettext-runtime', 'pkgconf', 'pkg', 'python3', 'python312',
}


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or str(path) != name:
        raise ValueError(f'unsafe archive path: {name}')
    return path


def snapshot(root, names):
    result = {}
    for name in sorted(names):
        safe_name(name)
        path = root / name
        if path.is_symlink():
            link = os.readlink(path)
            if Path(link).is_absolute() or not path.resolve().is_relative_to(root):
                raise ValueError(f'unsafe symlink: {name} -> {link}')
            result[name] = {'type': 'symlink', 'target': link}
        elif path.is_file():
            result[name] = {'type': 'file', 'sha256': sha(path),
                            'size': path.stat().st_size,
                            'mode': 0o755 if path.stat().st_mode & 0o111 else 0o644}
        else:
            raise ValueError(f'missing regular source file: {name}')
    return result


def archive(destination, root, prefix, files, epoch):
    # Manual TarInfo avoids macOS xattrs, AppleDouble, ACLs and owner metadata.
    with destination.open('wb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode='w', format=tarfile.PAX_FORMAT) as tar:
                for name, entry in sorted(files.items()):
                    info = tarfile.TarInfo(f'{prefix}/{name}')
                    info.uid = info.gid = 0
                    info.uname = info.gname = 'root'
                    info.mtime = epoch
                    if entry['type'] == 'symlink':
                        info.type = tarfile.SYMTYPE
                        info.linkname = entry['target']
                        info.mode = 0o777
                        tar.addfile(info)
                    else:
                        data = (root / name).read_bytes()
                        if hashlib.sha256(data).hexdigest() != entry['sha256']:
                            raise ValueError(f'source changed during archive: {name}')
                        info.size = len(data)
                        info.mode = entry['mode']
                        tar.addfile(info, io.BytesIO(data))


def conformance_names(root):
    names = {'SHA256SUMS'}
    for line in (root / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        safe_name(name)
        if sha(root / name) != digest:
            raise ValueError(f'conformance digest mismatch: {name}')
        names.add(name)
    provenance = json.loads((root / 'reference/provenance.json').read_text())
    if provenance['commit'] != MICROSOFT_COMMIT:
        raise ValueError('wrong Microsoft reference revision')
    return names


def packages(cache):
    result = {}
    for path in sorted(cache.rglob('*.pkg')):
        if not path.is_file():
            continue
        if path.is_symlink() and not path.resolve().is_relative_to(cache):
            raise ValueError(f'package alias leaves the isolated cache: {path}')
        name = re.sub(r'~[A-Za-z0-9]+(?=\.pkg$)', '', path.name)
        if not re.fullmatch(r'[A-Za-z0-9._+-]+\.pkg', name) or '..' in name:
            raise ValueError(f'unsafe package basename: {path.name}')
        digest = sha(path)
        if name in result and result[name][1] != digest:
            raise ValueError(f'conflicting package versions for {name}')
        result[name] = (path, digest)
    for package in PACKAGE_NAMES:
        pattern = re.compile(re.escape(package) + r'-[0-9][^.]*.*\.pkg$')
        matches = [name for name in result if pattern.fullmatch(name)]
        if len(matches) != 1:
            raise ValueError(f'expected exactly one {package} package: {matches}')
    return result


def make_iso(media, output):
    if output.exists():
        raise ValueError(f'refusing existing ISO: {output}')
    if sys.platform == 'darwin':
        subprocess.run(['hdiutil', 'makehybrid', '-iso', '-joliet',
                        '-iso-volume-name', 'CHUNKDEDUP', '-joliet-volume-name',
                        'CHUNKDEDUP', '-o', str(output), str(media)], check=True)
    elif sys.platform.startswith('freebsd'):
        subprocess.run(['makefs', '-t', 'cd9660', '-o', 'rockridge', '-o',
                        'label=CHUNKDEDUP', str(output), str(media)], check=True)
    else:
        subprocess.run(['xorriso', '-as', 'mkisofs', '-R', '-J', '-V',
                        'CHUNKDEDUP', '-o', str(output), str(media)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--conformance', type=Path, required=True)
    parser.add_argument('--tests', type=Path, required=True)
    parser.add_argument('--packages', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='new isolated payload directory; must not exist')
    parser.add_argument('--iso', type=Path, help='optional new ISO path')
    args = parser.parse_args()
    repo, reference, tests, cache = [p.resolve() for p in
                                   (args.repo, args.conformance, args.tests, args.packages)]
    output = args.output.resolve()
    if output.exists() or any(output.is_relative_to(p) for p in (repo, reference, tests, cache)):
        raise ValueError('output must be new and outside the input trees')
    if args.iso and args.iso.resolve().is_relative_to(output):
        raise ValueError('ISO must be outside the payload tree')
    head = run('git', 'rev-parse', 'HEAD', cwd=repo).decode().strip()
    tag = run('git', 'rev-parse', BASE_TAG + '^{commit}', cwd=repo).decode().strip()
    if head != BASE_COMMIT or tag != BASE_COMMIT:
        raise ValueError(f'expected frozen {BASE_TAG} base {BASE_COMMIT}; found {head}')
    epoch = int(run('git', 'show', '-s', '--format=%ct', 'HEAD', cwd=repo))
    tracked = set()
    omitted_submodules = {}
    for entry in run('git', 'ls-files', '--stage', '-z', cwd=repo).decode().rstrip('\0').split('\0'):
        metadata, name = entry.split('\t', 1)
        mode, object_id, stage = metadata.split()
        if stage != '0':
            raise ValueError('unresolved Git index entries')
        if mode == '160000':
            omitted_submodules[name] = object_id
        else:
            tracked.add(name)
    if not NEW_SOURCE.issubset(tracked):
        raise ValueError('new prototype sources must be tracked or added with git add -N so source.patch includes them')
    names = tracked | NEW_SOURCE
    source = snapshot(repo, names)
    reference_files = snapshot(reference, conformance_names(reference))
    test_files = snapshot(tests, TEST_NAMES)
    for name in ('chunkdedup_chunker.c', 'chunkdedup_chunker.h', 'rabin64_table.h'):
        if sha(repo / 'cmd/zfs' / name) != sha(reference / 'chunker' / name):
            raise ValueError(f'chunker differs from checked conformance source: {name}')
    if sha(repo / 'cmd/zfs/CHUNKDEDUP-MIT-LICENSE') != sha(reference / 'chunker/LICENSE'):
        raise ValueError('chunker MIT license mismatch')
    dependencies = packages(cache)
    patch = run('git', 'diff', '--binary', 'HEAD', cwd=repo)
    output.mkdir(parents=True)
    archive(output / 'openzfs.tar.gz', repo, 'openzfs', source, epoch)
    archive(output / 'conformance.tar.gz', reference, 'conformance', reference_files, epoch)
    archive(output / 'tests.tar.gz', tests, 'tests', test_files, epoch)
    recipe = Path(__file__).resolve().parent
    for name in ('guest-build.sh', 'verify-media.sh'):
        shutil.copyfile(recipe / name, output / name)
        (output / name).chmod(0o755)
    (output / 'LAB-MEDIA').write_text('ZFS chunkdedup offline media v2\n')
    (output / 'source.patch').write_bytes(patch)
    (output / 'packages').mkdir()
    for name, (path, digest) in dependencies.items():
        destination = output / 'packages' / name
        shutil.copyfile(path, destination)
        if sha(destination) != digest:
            raise ValueError(f'package changed during copy: {name}')
    if snapshot(repo, names) != source or snapshot(reference, reference_files) != reference_files or snapshot(tests, test_files) != test_files:
        raise ValueError('inputs changed during freeze; discard this incomplete payload and retry after source freeze')
    if run('git', 'diff', '--binary', 'HEAD', cwd=repo) != patch:
        raise ValueError('source patch changed during freeze')
    if run('git', 'rev-parse', 'HEAD', cwd=repo).decode().strip() != head:
        raise ValueError('source base changed during freeze')
    record = {'base_tag': BASE_TAG, 'base_commit': head, 'source_epoch': epoch,
              'microsoft_commit': MICROSOFT_COMMIT, 'files': source,
              'test_files': test_files, 'conformance_files': reference_files,
              'packages': {name: digest for name, (_, digest) in dependencies.items()},
              'omitted_test_image_submodules': omitted_submodules,
              'source_patch_sha256': hashlib.sha256(patch).hexdigest()}
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    files = sorted(p for p in output.rglob('*') if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.relative_to(output).as_posix()}\n' for p in files))
    for path in output.rglob('*'):
        if path.is_file():
            path.chmod(0o555 if path.name in ('guest-build.sh', 'verify-media.sh') else 0o444)
    if args.iso:
        make_iso(output, args.iso.resolve())
        print(f'ISO SHA256 {sha(args.iso.resolve())}')
    print(f'Frozen {len(source)} source entries, {len(test_files)} test files and {len(dependencies)} package archives into {output}')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))

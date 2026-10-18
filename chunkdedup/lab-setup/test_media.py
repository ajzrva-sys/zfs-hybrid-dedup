#!/usr/bin/env python3
"""Local packaging tests; uses synthetic package files, never installs them."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('media_builder', HERE / 'build-media.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def reject(callback):
    try:
        callback()
    except ValueError:
        return
    raise AssertionError('unsafe fixture unexpectedly accepted')


with tempfile.TemporaryDirectory(prefix='media-check-', dir=HERE) as temporary:
    root = Path(temporary)
    source = root / 'source'
    source.mkdir()
    (source / 'run.sh').write_text('#!/bin/sh\nexit 0\n')
    (source / 'run.sh').chmod(0o755)
    (source / 'data').write_bytes(b'checked bytes')
    (source / 'link').symlink_to('data')
    if hasattr(os, 'setxattr'):
        try:
            os.setxattr(source / 'data', 'user.chunkdedup-test', b'never archived')
        except OSError:
            pass
    files = builder.snapshot(source, {'run.sh', 'data', 'link'})
    builder.archive(root / 'a.tar.gz', source, 'source', files, 123456)
    builder.archive(root / 'b.tar.gz', source, 'source', files, 123456)
    assert builder.sha(root / 'a.tar.gz') == builder.sha(root / 'b.tar.gz')
    with tarfile.open(root / 'a.tar.gz') as archive:
        entries = archive.getmembers()
        assert [x.name for x in entries] == ['source/data', 'source/link', 'source/run.sh']
        assert all(x.uid == x.gid == 0 and not x.pax_headers for x in entries)
        assert entries[1].issym() and entries[1].linkname == 'data'
        assert entries[2].mode == 0o755
    (source / 'outside').symlink_to('../outside')
    reject(lambda: builder.snapshot(source, {'outside'}))
    reject(lambda: builder.snapshot(source, {'../outside'}))
    (source / 'data').write_bytes(b'changed source')
    reject(lambda: builder.archive(root / 'changed.tar.gz', source, 'source', files, 123456))
    cache = root / 'cache'
    (cache / 'Hashed').mkdir(parents=True)
    for name in builder.PACKAGE_NAMES:
        (cache / 'Hashed' / f'{name}-1.0~abc123.pkg').write_text(f'synthetic:{name}')
    (cache / 'bash-1.0.pkg').write_text('synthetic:bash')
    dependencies = builder.packages(cache)
    assert len(dependencies) == len(builder.PACKAGE_NAMES)
    assert 'bash-1.0.pkg' in dependencies
    (cache / 'bash-1.0.pkg').write_text('conflicting bytes')
    reject(lambda: builder.packages(cache))
    (cache / 'bash-1.0.pkg').write_text('synthetic:bash')
    (cache / 'bash-2.0.pkg').write_text('synthetic:another version')
    reject(lambda: builder.packages(cache))
    (cache / 'bash-2.0.pkg').unlink()
    (root / 'outside-1.pkg').write_text('must not package outside the cache')
    (cache / 'outside-1.pkg').symlink_to('../outside-1.pkg')
    reject(lambda: builder.packages(cache))
    (cache / 'outside-1.pkg').unlink()
    # Test the base-tools verifier on every platform using a sha256 shim only.
    media = root / 'media'
    (media / 'packages').mkdir(parents=True)
    for name in ('SOURCE.json', 'source.patch', 'openzfs.tar.gz', 'conformance.tar.gz',
                 'tests.tar.gz', 'guest-build.sh', 'verify-media.sh'):
        (media / name).write_text('synthetic verifier fixture\n')
    (media / 'LAB-MEDIA').write_text('ZFS chunkdedup offline media v2\n')
    for name in ('pkg-1.pkg', 'bash-1.pkg'):
        (media / 'packages' / name).write_text('synthetic package\n')
    manifest = ''.join(f'{builder.sha(p)}  {p.relative_to(media).as_posix()}\n'
                       for p in sorted(media.rglob('*')) if p.is_file())
    (media / 'SHA256SUMS').write_text(manifest)
    shim = root / 'bin'
    shim.mkdir()
    (shim / 'sha256').write_text('#!/usr/bin/env python3\nimport hashlib,sys\n'
                               'print(hashlib.sha256(open(sys.argv[-1],"rb").read()).hexdigest())\n')
    (shim / 'sha256').chmod(0o755)
    environment = dict(os.environ, PATH=str(shim) + os.pathsep + os.environ['PATH'])
    def verify(ok):
        result = subprocess.run(['sh', str(HERE / 'verify-media.sh'), str(media)],
                                env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
    verify(True)
    (media / 'packages/unlisted-1.pkg').write_text('must reject')
    verify(False)
    (media / 'packages/unlisted-1.pkg').unlink()
    (media / 'SHA256SUMS').write_text(manifest + manifest.splitlines()[0] + '\n')
    verify(False)
    (media / 'SHA256SUMS').write_text(manifest)
    (media / 'tests.tar.gz').write_text('checksum mismatch')
    verify(False)

print('Packaging determinism, metadata exclusion, source-change rejection, safe symlinks,')
print('flat package alias/conflict/version checks and complete media checksum verification PASS')

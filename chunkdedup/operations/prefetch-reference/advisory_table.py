"""Bounded independent RAM lookup copy, never a worker checkpoint or resume."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import re
import stat
import time

IDENTITY = ('object', 'generation', 'size', 'mtime_sec', 'mtime_nsec')
MAX_TABLE = 8 << 30
CHUNK = 1 << 20


def identity(record):
    return {k: record[k] for k in IDENTITY}


def require_guard(guard):
    result = guard()
    if result is not True and not (isinstance(result, dict) and result.get('ok') is True):
        raise ValueError('guard refused table copy')


class RamTable:
    def __init__(self, data, receipt):
        self.data, self.receipt = data, receipt

    def read_at(self, offset, length):
        if offset < 0 or length < 0 or offset + length > len(self.data):
            raise ValueError('RAM lookup outside independently copied table')
        return bytes(self.data[offset:offset + length])


def capture(device, expected, guard, *, record_size=1608, max_bytes=MAX_TABLE,
            progress=None, allocator=bytearray, now=time.monotonic):
    """Pin metadata throughout a sequential copy; never silently rebind on error.

    This is advisory lookup input. Source identity rechecks are not an atomic or
    cryptographic snapshot. Every selected data record still needs fresh QUERY
    and the native worker remains its sole content verifier/modifier.
    """
    expected = identity(expected)
    n = expected['size']
    if (not 0 < n <= min(max_bytes, MAX_TABLE) or n % record_size
            or record_size != 1608):
        raise ValueError('table size/layout exceeds admitted RAM bound')
    require_guard(guard)
    if identity(device.stat(expected['object'])) != expected:
        raise ValueError('table identity changed before copy')
    started = now()
    data = allocator(n)
    if len(data) != n:
        raise ValueError('RAM allocation returned unexpected size')
    digest = hashlib.sha256()
    calls = 0
    try:
        for at in range(0, n, CHUNK):
            require_guard(guard)
            size = min(CHUNK, n - at)
            part = device.read(expected, at, size)
            if len(part) != size:
                raise ValueError('short table read')
            data[at:at + size] = part
            digest.update(part)
            calls += 1
            if progress is not None:
                progress({'copied_bytes': at + size, 'total_bytes': n,
                          'seconds': now() - started})
        if identity(device.stat(expected['object'])) != expected:
            raise ValueError('table identity changed after copy; warming held')
        require_guard(guard)
    except BaseException:
        del data
        raise
    return RamTable(data, {'status': 'ADVISORY_RAM_LOOKUP_COPY',
                          'identity': expected, 'bytes': n,
                          'sha256': digest.hexdigest(), 'read_calls': calls,
                          'seconds': now() - started,
                          'checkpoint_or_scan_resume': False,
                          'atomic_snapshot_claimed': False,
                          'live_descriptor_operations': False})


EXPORT_STAT = ('dev', 'ino', 'size', 'mtime_ns', 'ctime_ns')
EXPORT_CHUNK = 64 << 20


def export_stat(value):
    return {name: getattr(value, 'st_' + name) for name in EXPORT_STAT}


def open_export(path, owner):
    """Walk directory descriptors; no parent or final symlink is followed."""
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
    directory = os.open('/', flags | os.O_DIRECTORY)
    try:
        for part in (*Path(path).parts[1:-1], None):
            current = os.fstat(directory)
            if not stat.S_ISDIR(current.st_mode) or not owner(current) or current.st_mode & 0o022:
                raise ValueError('unsafe export parent ownership/mode')
            if part is None:
                entry = os.stat(Path(path).name, dir_fd=directory, follow_symlinks=False)
                if (not stat.S_ISREG(entry.st_mode) or not owner(entry)
                        or entry.st_nlink != 1 or entry.st_mode & 0o022):
                    raise ValueError('unsafe export file ownership/type/mode')
                result = os.open(Path(path).name, flags | os.O_NONBLOCK | os.O_NOCTTY, dir_fd=directory)
                break
            following = os.open(part, flags | os.O_DIRECTORY, dir_fd=directory)
            os.close(directory)
            directory = following
    finally:
        os.close(directory)
    return result


def capture_export(device, expected, guard, export, *, record_size=1608,
                   max_bytes=MAX_TABLE, progress=None, allocator=bytearray,
                   now=time.monotonic, trusted_owner=None):
    """Hash/copy one admitted immutable SSD export into the same bounded RAM.

    The caller separately admits its read-only dataset/anchor. Ownership
    injection is only for local fixtures; production leaves trusted_owner=None.
    Every source record still requires fresh QUERY; there is no READOBJECT
    fallback and this does not create a checkpoint or restore worker progress.
    """
    if trusted_owner is None:
        if os.geteuid() != 0:
            raise ValueError('root required for admitted export capture')
        trusted_owner = lambda value: value.st_uid == 0
    expected = identity(expected)
    n = expected['size']
    if (type(n) is not int or not 0 < n <= min(max_bytes, MAX_TABLE)
            or record_size != 1608 or n % record_size):
        raise ValueError('table size/layout exceeds admitted RAM bound')
    if not isinstance(export, dict) or set(export) != {'path', 'sha256', 'stat'}:
        raise ValueError('exact export path/hash/stat configuration required')
    path = export['path']
    if (not isinstance(path, str) or not Path(path).is_absolute()
            or str(Path(path)) != path or '..' in Path(path).parts or path == '/'):
        raise ValueError('canonical absolute export path required')
    if (not isinstance(export['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', export['sha256'])
            or not isinstance(export['stat'], dict) or set(export['stat']) != set(EXPORT_STAT)
            or any(type(v) is not int or v < 0 for v in export['stat'].values())
            or export['stat']['size'] != n):
        raise ValueError('known exact export hash/size/stat required')

    def checked(fd):
        value = os.fstat(fd)
        if (not stat.S_ISREG(value.st_mode) or not trusted_owner(value)
                or value.st_nlink != 1 or value.st_mode & 0o022
                or export_stat(value) != export['stat']):
            raise ValueError('export identity/ownership/mode changed')
        return value

    require_guard(guard)
    if identity(device.stat(expected['object'])) != expected:
        raise ValueError('table identity changed before export copy')
    fd = open_export(path, trusted_owner)
    started = now()
    try:
        checked(fd)
        data = allocator(n)
        if len(data) != n:
            raise ValueError('RAM allocation returned unexpected size')
        digest = hashlib.sha256()
        calls = 0
        try:
            with os.fdopen(fd, 'rb', buffering=0, closefd=False) as source:
                for at in range(0, n, EXPORT_CHUNK):
                    require_guard(guard)
                    size = min(EXPORT_CHUNK, n - at)
                    view = memoryview(data)[at:at + size]
                    try:
                        if source.readinto(view) != size:
                            raise ValueError('short export table read')
                        digest.update(view)
                    finally:
                        view.release()
                    calls += 1
                    if progress is not None:
                        progress({'copied_bytes': at + size, 'total_bytes': n,
                                  'seconds': now() - started})
                require_guard(guard)
                if source.read(1) or digest.hexdigest() != export['sha256']:
                    raise ValueError('export table checksum/EOF mismatch')
            checked(fd)
            # Independently reopen the absolute path, so replacement of a
            # parent or named file cannot pass via the original held FD.
            final = open_export(path, trusted_owner)
            try:
                checked(final)
            finally:
                os.close(final)
            if identity(device.stat(expected['object'])) != expected:
                raise ValueError('table identity changed after export copy; warming held')
            require_guard(guard)
        except BaseException:
            del data
            raise
    finally:
        os.close(fd)
    return RamTable(data, {'status': 'ADVISORY_RAM_LOOKUP_COPY',
                          'identity': expected, 'bytes': n,
                          'sha256': digest.hexdigest(), 'read_calls': calls,
                          'seconds': now() - started,
                          'source': 'ADMITTED_IMMUTABLE_SSD_EXPORT',
                          'export': export,
                          'catalog_readobject_calls': 0,
                          'checkpoint_or_scan_resume': False,
                          'atomic_snapshot_claimed': False,
                          'live_descriptor_operations': False})

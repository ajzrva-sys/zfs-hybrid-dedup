"""Offline bounded native-catalog warming planner; performs no kernel operations.

read_at callbacks consume captured data or an independently owned READOBJECT
stream. They must never wrap, duplicate, or seek a production descriptor.
Native catalog `object` is record size, NOT the DMU object number.
"""
from __future__ import annotations
from collections import OrderedDict
from dataclasses import dataclass
import hashlib
import json
import re
import stat
from typing import Callable

ROW_BYTES = 191
U64_MAX = (1 << 64) - 1
MAX_RECORD = 16 << 20
MAX_ENTRIES = 256
MAX_BATCH_BYTES = 256 << 20
# Admitted native amd64 FreeBSD probe output; layout compatibility is exact.
ADMITTED_LAYOUT_CANONICAL_SHA256 = "15f89146470c388a1d0f6a26bb8edf09261ed11714e13a2c8b0c6349916a65bb"
ADMITTED_LAYOUT_FILE_SHA256 = "f96e4f81db0ce7e69bf0da77570fe598f25406bcc7638c415bcddfef0dddb4bc"
ADMITTED_PROBE_SHA256 = "1572cb09e16e38179857805ec129186021b8bc98f2e6e243c6891c36ca0afef0"
ADMITTED_DECLARATION_SHA256 = "ab71bb330015e90af4ebbd9672cf77cfdbf87b34b4bd9f4fa9d21e58407c010e"

ROW_RE = re.compile(rb'([0-9]{20}) ([0-9a-f]{64}) ([0-9]{20}) ([0-9]{20}) ([0-9]{20}) ([0-9]{20}) ([0-9]{20})\n')

class InvalidCapture(ValueError):
    pass

@dataclass(frozen=True)
class Layout:
    record_size: int
    fields: dict
    block_method: int
    byteorder: str

    @classmethod
    def from_json(cls, raw: dict) -> 'Layout':
        try:
            encoded = json.dumps(raw, sort_keys=True, separators=(',', ':'),
                                 allow_nan=False).encode('utf8')
        except (TypeError, ValueError) as error:
            raise InvalidCapture('Invalid native layout schema') from error
        if hashlib.sha256(encoded).hexdigest() != ADMITTED_LAYOUT_CANONICAL_SHA256:
            raise InvalidCapture('Native layout does not match admitted amd64 FreeBSD probe')
        # Own a copy: later edits to the caller's dictionary cannot alter offsets.
        fields = {name: dict(value) for name, value in raw['fields'].items()}
        return cls(1608, fields, 1, 'little')

    @classmethod
    def from_admitted_bytes(cls, data: bytes) -> 'Layout':
        if hashlib.sha256(data).hexdigest() != ADMITTED_LAYOUT_FILE_SHA256:
            raise InvalidCapture('Native layout receipt file hash mismatch')
        try:
            return cls.from_json(json.loads(data))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise InvalidCapture('Invalid native layout receipt') from error

    def number(self, data: bytes, field: str, signed=False) -> int:
        f = self.fields[field]
        return int.from_bytes(data[f['offset']:f['offset'] + f['size']], self.byteorder, signed=signed)

@dataclass(frozen=True)
class FileRecord:
    dataset: str
    dataset_id: int
    object: int
    generation: int
    size: int
    mtime_sec: int
    mtime_nsec: int
    block_size: int
    method: int
    chunked: int

    def prefetch(self, offset: int, length: int) -> dict:
        return {'dataset': self.dataset, 'object': self.object,
                'generation': self.generation, 'size': self.size,
                'mtime_sec': self.mtime_sec, 'mtime_nsec': self.mtime_nsec,
                'offset': offset, 'length': length}

class FileTable:
    """Random lookup in an independent capture, with a bounded decoded LRU."""
    def __init__(self, read_at: Callable[[int, int], bytes], total_bytes: int,
                 layout: Layout, cache_records=128):
        if total_bytes < 0 or total_bytes % layout.record_size or not 1 <= cache_records <= 4096:
            raise InvalidCapture('Bad captured file-table size/cache bound')
        self.read_at, self.total_bytes, self.layout = read_at, total_bytes, layout
        self.count = total_bytes // layout.record_size
        self.capacity = cache_records
        self.cache: OrderedDict[int, FileRecord] = OrderedDict()
        self.peak_cache_records = 0

    def get(self, file_id: int) -> FileRecord:
        if not 0 <= file_id < self.count:
            raise InvalidCapture('File ID outside captured table')
        if file_id in self.cache:
            self.cache.move_to_end(file_id)
            return self.cache[file_id]
        b = self.read_at(file_id * self.layout.record_size, self.layout.record_size)
        if len(b) != self.layout.record_size:
            raise InvalidCapture('Truncated captured file record')
        f = self.layout.fields['dataset']; name = b[f['offset']:f['offset'] + f['size']]
        if b'\0' not in name:
            raise InvalidCapture('Unterminated dataset')
        try:
            dataset = name.split(b'\0', 1)[0].decode('utf8', 'strict')
        except UnicodeDecodeError as e:
            raise InvalidCapture('Invalid dataset encoding') from e
        n = lambda key, signed=False: self.layout.number(b, key, signed)
        out = FileRecord(dataset, n('dataset_id'), n('st_ino'), n('st_gen'),
                         n('st_size', True), n('st_mtim.tv_sec', True),
                         n('st_mtim.tv_nsec', True), n('block_size'), n('method'), n('chunked'))
        if (not dataset or '\n' in dataset or out.dataset_id != n('st_dev')
                or not stat.S_ISREG(n('st_mode')) or out.object == 0
                or out.size <= 0 or out.mtime_sec < 0 or not 0 <= out.mtime_nsec < 10**9
                or not 0 < out.block_size <= MAX_RECORD or out.method not in (0, 1, 2)
                or out.chunked not in (0, 1)):
            raise InvalidCapture('Invalid captured file identity/geometry')
        self.cache[file_id] = out
        self.cache.move_to_end(file_id)
        while len(self.cache) > self.capacity:
            self.cache.popitem(last=False)
        self.peak_cache_records = max(self.peak_cache_records, len(self.cache))
        return out

@dataclass(frozen=True)
class NativeRow:
    dataset_id: int
    hash: str
    length: int
    file_id: int
    offset: int
    allocated: int
    block_size: int

    @property
    def key(self):
        return self.dataset_id, self.hash, self.length

class NativeCatalog:
    def __init__(self, read_at: Callable[[int, int], bytes], total_bytes: int):
        if total_bytes < 0 or total_bytes % ROW_BYTES:
            raise InvalidCapture('Native capture is not complete fixed 191-byte rows')
        self.read_at, self.total_bytes = read_at, total_bytes

    def row(self, cursor: int) -> tuple[NativeRow, bytes]:
        if cursor < 0 or cursor % ROW_BYTES or cursor >= self.total_bytes:
            raise InvalidCapture('Cursor not a captured native row boundary')
        b = self.read_at(cursor, ROW_BYTES)
        m = ROW_RE.fullmatch(b)
        if not m:
            raise InvalidCapture('Malformed/truncated native row')
        groups = m.groups()
        nums = [int(groups[i]) for i in (0, 2, 3, 4, 5, 6)]
        if any(n > U64_MAX for n in nums):
            raise InvalidCapture('Native integer exceeds uint64')
        d, length, file_id, offset, allocation, block_size = nums
        if not 0 < length <= block_size <= MAX_RECORD or offset > U64_MAX - length:
            raise InvalidCapture('Invalid native record geometry')
        return NativeRow(d, groups[1].decode(), length, file_id, offset, allocation, block_size), b

    def next_group_boundary(self, cursor: int, max_rows=8192) -> int | None:
        """Round ahead of a live/read-ahead cursor, then skip an incomplete run.

        Returns None after a bounded giant-run walk; caller must hold/advance,
        never guess a canonical from the middle of a run.
        """
        if cursor < 0 or not 1 <= max_rows <= 1_000_000:
            raise InvalidCapture('Bad boundary search bounds')
        at = ((cursor + ROW_BYTES - 1) // ROW_BYTES) * ROW_BYTES
        if at >= self.total_bytes:
            return self.total_bytes
        if at == 0:
            return 0
        previous, _ = self.row(at - ROW_BYTES)
        for _ in range(max_rows):
            row, _ = self.row(at)
            if row.key != previous.key:
                return at
            at += ROW_BYTES
            if at == self.total_bytes:
                return at
        return None

@dataclass
class Continuation:
    next_cursor: int
    previous_raw: bytes | None = None
    key: tuple | None = None
    canonical: NativeRow | None = None
    canonical_file: FileRecord | None = None
    canonical_emitted: bool = False
    canonical_cursor: int | None = None

class Planner:
    def __init__(self, native: NativeCatalog, files: FileTable):
        self.native, self.files = native, files

    def start(self, cursor=0) -> Continuation:
        if cursor < 0 or cursor % ROW_BYTES or cursor > self.native.total_bytes:
            raise InvalidCapture('Start cursor must be row-aligned')
        previous_raw = None
        if 0 < cursor < self.native.total_bytes:
            previous, previous_raw = self.native.row(cursor - ROW_BYTES)
            row, _ = self.native.row(cursor)
            if previous.key == row.key:
                raise InvalidCapture('Cannot start mid-group without continuation')
        return Continuation(cursor, previous_raw)

    def window(self, state: Continuation, *, max_rows=8192, max_entries=256,
               max_bytes=MAX_BATCH_BYTES, include_pairs=True) -> dict:
        if not 1 <= max_rows <= 1_000_000 or not 2 <= max_entries <= MAX_ENTRIES or not 2 <= max_bytes <= MAX_BATCH_BYTES:
            raise InvalidCapture('Invalid planner bounds')
        start = state.next_cursor
        records, seen, record_budgets = [], set(), []
        pairs, seen_pairs = [], set()
        rows, total_bytes = 0, 0
        while state.next_cursor < self.native.total_bytes and rows < max_rows:
            row, raw = self.native.row(state.next_cursor)
            if state.previous_raw is not None and raw < state.previous_raw:
                raise InvalidCapture('Captured native catalog is not sorted')
            file = self.files.get(row.file_id)
            if (file.dataset_id != row.dataset_id or file.block_size != row.block_size
                    or row.offset % file.block_size or row.offset + row.length > file.size
                    or row.length != min(file.block_size, file.size - row.offset)):
                raise InvalidCapture('Native row/file-table identity or geometry mismatch')
            new_key = row.key != state.key
            if new_key:
                state.key, state.canonical, state.canonical_file = row.key, None, None
                state.canonical_emitted = False
                state.canonical_cursor = None
            eligible = file.method == self.files.layout.block_method and not file.chunked
            canonical = state.canonical
            additions = []
            if eligible and canonical is not None and row.block_size == canonical.block_size and (file.dataset_id, file.object, row.offset) != (state.canonical_file.dataset_id, state.canonical_file.object, canonical.offset):
                source_record = state.canonical_file.prefetch(canonical.offset, canonical.length)
                destination_record = file.prefetch(row.offset, row.length)
                pair_key = (record_key(source_record), record_key(destination_record))
                new_pair = pair_key not in seen_pairs
                if new_pair and len(pairs) == max_entries:
                    break  # Bounded pair metadata even when flat hints deduplicate.
                if not state.canonical_emitted:
                    additions.append((state.canonical_file, canonical.offset, canonical.length))
                additions.append((file, row.offset, row.length))
                unique = []
                for item_file, offset, length in additions:
                    entry = item_file.prefetch(offset, length)
                    key = tuple(entry.values())
                    if key not in seen and all(key != k for k, _, _ in unique):
                        unique.append((key, entry, item_file.block_size))
                # Full native block size conservatively bounds copied-BP logical size;
                # short-tail hint lengths alone would undercount the kernel cap.
                need_bytes = sum(budget for _, _, budget in unique)
                if len(unique) > max_entries or need_bytes > max_bytes:
                    raise InvalidCapture('One candidate pair exceeds supplied window budget')
                if len(records) + len(unique) > max_entries or total_bytes + need_bytes > max_bytes:
                    break  # Do not consume this destination; continuation retries it.
                for key, entry, budget in unique:
                    seen.add(key); records.append(entry); record_budgets.append(budget)
                total_bytes += need_bytes
                if new_pair:
                    seen_pairs.add(pair_key)
                    pairs.append({'source': source_record, 'destination': destination_record,
                                  'source_catalog_cursor': state.canonical_cursor,
                                  'destination_catalog_cursor': state.next_cursor,
                                  'source_budget_bytes': state.canonical_file.block_size,
                                  'destination_budget_bytes': file.block_size})
                state.canonical_emitted = True
            elif eligible and canonical is None:
                state.canonical, state.canonical_file = row, file
                state.canonical_cursor = state.next_cursor
            state.previous_raw = raw
            state.next_cursor += ROW_BYTES
            rows += 1
        # State holds at most one canonical, one preceding row, and fixed scalars.
        return {'format': 'CDSP_PREFETCH_PLAN_V2', 'cursor_start': start,
                'cursor_end': state.next_cursor, 'rows_consumed': rows,
                'eof': state.next_cursor == self.native.total_bytes,
                'continuing_group': state.key if state.canonical is not None else None,
                'canonical_emitted': state.canonical_emitted,
                'request_count': len(records),
                'prefetch_bytes': sum(record['length'] for record in records),
                'budgeted_bytes': total_bytes, 'record_budget_bytes': record_budgets,
                'records': records, 'pair_mapping_included': bool(include_pairs),
                'pairs': pairs if include_pairs else None, 'pair_count': len(pairs),
                'advisory_only': True,
                'kernel_operations_performed': False}


RECORD_IDENTITY_FIELDS = ('dataset', 'object', 'generation', 'size',
                          'mtime_sec', 'mtime_nsec', 'offset', 'length')


def record_key(record: dict) -> tuple:
    """Stable mapping key for one QUERY/PREFETCH input identity and range."""
    try:
        return tuple(record[name] for name in RECORD_IDENTITY_FIELDS)
    except KeyError as error:
        raise InvalidCapture('Incomplete prefetch record identity') from error


def filter_shared_pairs(plan: dict, query_results: dict, *, max_entries=256,
                        max_bytes=MAX_BATCH_BYTES) -> dict:
    """Pure post-QUERY filtering; no ioctls, reads or state changes.

    Only exact physical birth + both raw first-DVA words establish the native
    already-shared fast path. Incomplete physical metadata keeps warming the
    pair. Missing/failed/non-REAL QUERY coverage omits that pair with an explicit
    coverage error; it never counts as already shared or successful warming.
    QUERY can race later sharing: all hints remain advisory.
    """
    if (plan.get('format') != 'CDSP_PREFETCH_PLAN_V2'
            or not plan.get('pair_mapping_included')
            or not isinstance(plan.get('pairs'), list)
            or len(plan['pairs']) > MAX_ENTRIES
            or not 2 <= max_entries <= MAX_ENTRIES
            or not 2 <= max_bytes <= MAX_BATCH_BYTES):
        raise InvalidCapture('Bounded V2 pair mapping and valid batch caps required')
    needed = OrderedDict()
    coverage_errors = []
    shared = verify = uncertain = 0
    for number, pair in enumerate(plan['pairs']):
        source, destination = pair['source'], pair['destination']
        sk, dk = record_key(source), record_key(destination)
        source_bp, destination_bp = query_results.get(sk), query_results.get(dk)
        def valid(bp, record):
            return (isinstance(bp, dict) and bp.get('error') == 0
                    and type(bp.get('flags')) is int and bp['flags'] & 1
                    and type(bp.get('block_size')) is int
                    and 0 < record['length'] <= bp['block_size'] <= MAX_RECORD)
        if sk == dk or not valid(source_bp, source) or not valid(destination_bp, destination):
            coverage_errors.append({'pair': number, 'reason': 'missing/failed/non-REAL QUERY or invalid geometry'})
            continue
        if source_bp['block_size'] != destination_bp['block_size']:
            coverage_errors.append({'pair': number, 'reason': 'QUERY native block sizes no longer compatible'})
            continue
        def exact_physical(bp):
            names = ('birth', 'dva0_word0', 'dva0_word1')
            if not all(type(bp.get(name)) is int and 0 <= bp[name] <= U64_MAX for name in names):
                return None
            if bp['birth'] == 0:
                return None
            return tuple(bp[name] for name in names)
        source_physical = exact_physical(source_bp)
        destination_physical = exact_physical(destination_bp)
        if source_physical is not None and source_physical == destination_physical:
            shared += 1
            continue
        verify += 1
        uncertain += source_physical is None or destination_physical is None
        for role, record, bp in [('source', source, source_bp), ('destination', destination, destination_bp)]:
            budget = pair.get(role + '_budget_bytes')
            if type(budget) is not int or not 0 < budget <= MAX_RECORD:
                raise InvalidCapture('Invalid pair budget metadata')
            budget = max(budget, bp['block_size'])
            key = record_key(record)
            if key in needed:
                needed[key] = (record, max(needed[key][1], budget))
            else:
                needed[key] = (record, budget)
    # A continued canonical can be absent from flat records but must be rewarmed
    # if any remaining destination needs verification. At most one extra record.
    if len(needed) > MAX_ENTRIES + 1:
        raise InvalidCapture('Pair mapping exceeds bounded window inventory')
    batches, records, budgeted = [], [], 0
    for record, budget in needed.values():
        if budget > max_bytes:
            raise InvalidCapture('One filtered hint exceeds supplied batch budget')
        if len(records) == max_entries or budgeted + budget > max_bytes:
            batches.append({'records': records, 'budgeted_bytes': budgeted})
            records, budgeted = [], 0
        records.append(record); budgeted += budget
    if records:
        batches.append({'records': records, 'budgeted_bytes': budgeted})
    return {'format': 'CDSP_FILTERED_PREFETCH_V1',
            'status': 'PARTIAL_QUERY_COVERAGE' if coverage_errors else 'PASS_OFFLINE_QUERY_FILTER',
            'cursor_start': plan['cursor_start'], 'cursor_end': plan['cursor_end'],
            'pair_count': len(plan['pairs']), 'already_shared_pairs': shared,
            'needs_verification_pairs': verify, 'uncertain_physical_pairs': uncertain,
            'coverage_errors': coverage_errors, 'records': [r for r, _ in needed.values()],
            'batches': batches, 'request_count': len(needed),
            'advisory_only': True, 'kernel_operations_performed': False,
            'query_race_may_add_unnecessary_io': True}

"""Packed incremental conflict proof for immutable BLOCK catalogs; no host controls.

Only a strict successor closes a group. The previous closed group then proves
submission, which retires older conflicting groups, never the witness itself.
"""
from array import array
import hashlib
import json
import struct
import sys

ROW = 191
NONE = 0xffffffff
MAGIC = b'SHARE-SPARSE-1\0\0'
ARRAYS = ('starts', 'ends', 'ordinals', 'heads', 'previous', 'following',
          'owners', 'members', 'free')


def need(ok, message):
    if not ok:
        raise ValueError(message)


def parse_row(raw, files):
    need(len(raw) == ROW, 'short native row')
    need(all(raw[i] == 32 for i in (20, 85, 106, 127, 148, 169))
         and raw[190] == 10, 'native row separators')
    numbers = []
    for at in (0, 86, 107, 128, 149, 170):
        field = raw[at:at + 20]
        need(field.isdigit(), 'noncanonical decimal')
        value = int(field)
        need(value < 1 << 64, 'decimal overflow')
        numbers.append(value)
    digest = raw[21:85]
    need(all(c in b'0123456789abcdef' for c in digest), 'noncanonical hash')
    need(numbers[1] > 0 and numbers[2] < files, 'length/file outside bounds')
    return (numbers[0], digest.decode('ascii'), numbers[1]), numbers[2]


class SparseState:
    def __init__(self, file_classes, classes, *, origin=0):
        need(sys.byteorder == 'little' and array('I').itemsize == 4
             and array('Q').itemsize == 8, 'unsupported packed-state ABI')
        need(0 < classes <= len(file_classes) <= 3_000_000, 'mapping counts')
        need(all(x < classes for x in file_classes), 'mapping class out of bounds')
        need(type(origin) is int and origin >= 0 and origin % ROW == 0, 'invalid group-boundary origin')
        self.origin = origin
        self.file_classes = file_classes
        self.classes = classes
        self.capacity = classes
        for name in ('starts', 'ends', 'ordinals'):
            setattr(self, name, array('Q', [0]) * classes)
        for name in ('heads', 'previous', 'following', 'owners', 'members'):
            setattr(self, name, array('I', [NONE]) * classes)
        self.free = array('I', range(classes - 1, -1, -1))
        self.first = self.last = NONE
        self.unresolved = self.closed = 0
        self.boundary = self.offset = origin
        self.pending = self.current = None
        self.last_key = None

    def _remove(self, slot):
        identity = self.heads[slot]
        while identity != NONE:
            need(self.owners[identity] == slot, 'owner chain corrupt')
            nxt = self.members[identity]
            self.owners[identity] = self.members[identity] = NONE
            identity = nxt
        before, after = self.previous[slot], self.following[slot]
        if before == NONE:
            self.first = after
        else:
            self.following[before] = after
        if after == NONE:
            self.last = before
        else:
            self.previous[after] = before
        self.heads[slot] = self.previous[slot] = self.following[slot] = NONE
        self.free.append(slot)
        self.unresolved -= 1

    def _insert(self, group):
        need(bool(self.free), 'packed unresolved capacity exhausted')
        slot = self.free.pop()
        self.starts[slot], self.ends[slot] = group['start'], group['end']
        self.ordinals[slot] = group['ordinal']
        self.previous[slot], self.following[slot] = self.last, NONE
        if self.last == NONE:
            self.first = slot
        else:
            need(self.ends[self.last] <= group['start'], 'unresolved ordering')
            self.following[self.last] = slot
        self.last = slot
        self.heads[slot] = NONE
        for identity in sorted(group['ids']):
            need(self.owners[identity] == NONE, 'unresolved identities overlap')
            self.owners[identity] = slot
            self.members[identity] = self.heads[slot]
            self.heads[slot] = identity
        self.unresolved += 1

    def seed(self, group):
        """Seed authenticated replay groups in order; keep final one pending."""
        need(self.current is None and group['end'] > group['start']
             and group['ordinal'] > self.closed and bool(group['ids']), 'invalid seed')
        if self.pending is not None:
            self._insert(self.pending)
        self.pending = group
        self.closed = group['ordinal']
        self.boundary = self.offset = group['end']
        self.last_key = tuple(group['key'])

    def close(self, group):
        need(bool(group['ids']) and group['count'] <= 8192, 'empty/giant group')
        if self.pending is not None:
            for identity in self.pending['ids']:
                slot = self.owners[identity]
                if slot != NONE:
                    self._remove(slot)
            self._insert(self.pending)
        self.closed += 1
        group['ordinal'] = self.closed
        self.pending = group
        self.boundary = group['end']

    def consume(self, raw):
        key, file_id = parse_row(raw, len(self.file_classes))
        if self.last_key is not None:
            need(key >= tuple(self.last_key), 'catalog sort order regressed')
        if self.current is None:
            if self.pending is not None:
                need(key > tuple(self.pending['key']), 'seed is not a group boundary')
            self.current = dict(start=self.offset, key=key, ids=set(), count=0)
        elif key != tuple(self.current['key']):
            self.current['end'] = self.offset
            self.close(self.current)
            self.current = dict(start=self.offset, key=key, ids=set(), count=0)
        self.current['ids'].add(self.file_classes[file_id])
        self.current['count'] += 1
        need(self.current['count'] <= 8192, 'giant group requires separate proof')
        self.last_key = key
        self.offset += ROW

    def advance(self, fp, limit, charge=lambda n: None):
        """fp is an independently opened immutable copy, never a worker FD."""
        need(type(limit) is int and limit >= self.offset, 'safe limit regressed')
        stop = max(0, (limit - 1) // ROW * ROW)
        fp.seek(self.offset)
        digest = hashlib.sha256()
        start = self.offset
        while self.offset < stop:
            count = min(4096 * ROW, stop - self.offset)
            charge(count)
            raw = fp.read(count)
            need(len(raw) == count, 'short immutable catalog tail')
            digest.update(raw)
            for at in range(0, count, ROW):
                self.consume(raw[at:at + ROW])
        return dict(start=start, end=self.offset, sha256=digest.hexdigest(),
                    boundary=self.boundary, closed_groups=self.closed)

    def holes(self):
        slot = self.first
        seen = 0
        while slot != NONE:
            need(seen < self.unresolved, 'unresolved list cycle')
            yield self.starts[slot], self.ends[slot]
            slot = self.following[slot]
            seen += 1
        need(seen == self.unresolved, 'unresolved list length')
        if self.pending is not None:
            yield self.pending['start'], self.pending['end']

    def proven_ranges(self):
        position = self.origin
        for start, end in self.holes():
            need(position <= start < end <= self.boundary, 'hole partition')
            if position < start:
                yield position, start
            position = end
        if position < self.boundary:
            yield position, self.boundary

    @property
    def proven_groups(self):
        return self.closed - self.unresolved - int(self.pending is not None)

    def chunks(self):
        def serial(group):
            return None if group is None else {**group, 'ids': sorted(group['ids'])}
        metadata = dict(classes=self.classes, origin=self.origin, first=self.first, last=self.last,
                        unresolved=self.unresolved, closed=self.closed,
                        boundary=self.boundary, offset=self.offset,
                        pending=serial(self.pending), current=serial(self.current),
                        last_key=self.last_key,
                        lengths={name: len(getattr(self, name)) for name in ARRAYS})
        raw = json.dumps(metadata, sort_keys=True, separators=(',', ':')).encode()
        need(len(raw) <= 1 << 20, 'state metadata exceeded bound')
        yield MAGIC + struct.pack('<Q', len(raw)) + raw
        for name in ARRAYS:
            view = memoryview(getattr(self, name)).cast('B')
            for at in range(0, len(view), 1 << 20):
                yield view[at:at + (1 << 20)]

    @classmethod
    def restore(cls, fp, file_classes, charge=lambda n: None):
        # This reader is called only after whole-state checksum authentication.
        def read(n):
            charge(n)
            data = fp.read(n)
            need(len(data) == n, 'short packed state')
            return data
        need(read(16) == MAGIC, 'packed state magic')
        length, = struct.unpack('<Q', read(8))
        need(length <= 1 << 20, 'packed metadata bound')
        meta = json.loads(read(length))
        classes = meta['classes']
        need(0 < classes <= len(file_classes) <= 3_000_000, 'packed counts')
        obj = cls.__new__(cls)
        obj.file_classes, obj.classes, obj.capacity = file_classes, classes, classes
        for name in ARRAYS:
            count = meta['lengths'][name]
            need(type(count) is int and (0 <= count <= classes if name == 'free'
                 else count == classes), 'packed array length')
            target = array('Q' if name in ('starts', 'ends', 'ordinals') else 'I')
            for at in range(0, count, 65536):
                target.frombytes(read(min(65536, count - at) * target.itemsize))
            setattr(obj, name, target)
        need(fp.read(1) == b'', 'packed state trailing bytes')
        for name in ('origin', 'first', 'last', 'unresolved', 'closed', 'boundary', 'offset', 'last_key'):
            setattr(obj, name, meta[name])
        for name in ('pending', 'current'):
            group = meta[name]
            if group is not None:
                group['ids'] = set(group['ids'])
                group['key'] = tuple(group['key'])
                need(0 < len(group['ids']) <= group['count'] <= 8192
                     and all(0 <= x < classes for x in group['ids']), 'packed group')
            setattr(obj, name, group)
        need(obj.offset % ROW == 0 and obj.origin % ROW == 0
             and 0 <= obj.origin <= obj.boundary <= obj.offset
             and obj.unresolved + len(obj.free) == classes, 'packed state partition')
        # Validate all live list and membership links without Python per-group objects.
        live = bytearray(classes)
        identity_seen = bytearray(classes)
        slot, previous, count = obj.first, NONE, 0
        last_end = obj.origin
        while slot != NONE:
            need(slot < classes and not live[slot] and obj.previous[slot] == previous,
                 'packed live list invalid')
            live[slot] = 1
            need(last_end <= obj.starts[slot] < obj.ends[slot] <= obj.boundary,
                 'packed live ranges invalid')
            identity, members = obj.heads[slot], 0
            while identity != NONE:
                need(identity < classes and not identity_seen[identity]
                     and obj.owners[identity] == slot, 'packed owner invalid')
                identity_seen[identity] = 1
                identity = obj.members[identity]
                members += 1
            need(0 < members <= 8192, 'packed empty/giant membership')
            previous, last_end = slot, obj.ends[slot]
            slot, count = obj.following[slot], count + 1
        need(previous == obj.last and count == obj.unresolved, 'packed live count')
        for slot in obj.free:
            need(slot < classes and not live[slot], 'packed free slots invalid')
            live[slot] = 1
        need(all(live) and all((owner == NONE) == (not identity_seen[i])
             for i, owner in enumerate(obj.owners)), 'packed owner/free partition')
        return obj

"""One bounded PRIVATE-GUEST prefetch window. No production admission exists.

Uses object reads through the companion; never opens a worker descriptor or
production file vnode. Native verification remains outside this helper.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('cdsp_planner', HERE / 'planner/planner.py')
planner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = planner
spec.loader.exec_module(planner)
FIELDS = ('object', 'generation', 'size', 'mtime_sec', 'mtime_nsec', 'offset', 'length')
MAX_READ = 1 << 20
MAX_REQUEST_RECORDS = 2048
MAX_REQUEST_BYTES = 256 << 20
MAX_BLOCK_BYTES = 16 << 20


def query_batches(records):
    """Bound QUERY metadata requests by both record count and input lengths."""
    selected, charged = [], 0
    for record in records:
        length = record.get('length')
        if type(length) is not int or not 0 < length <= MAX_BLOCK_BYTES:
            raise ValueError('invalid QUERY record length')
        if len(selected) == MAX_REQUEST_RECORDS or charged + length > MAX_REQUEST_BYTES:
            yield selected
            selected, charged = [], 0
        selected.append(record)
        charged += length
    if selected:
        yield selected


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class Device:
    def __init__(self, client, dataset, pool_guid, dataset_guid, runner=subprocess.run):
        self.client, self.dataset = str(client), dataset
        self.binding = [dataset, str(pool_guid), str(dataset_guid)]
        self.runner = runner
        self.calls = {'stat': 0, 'read': 0, 'query': 0, 'warm': 0}

    def command(self, op, args=(), records=None, raw=False):
        data = None
        if records is not None:
            if not 1 <= len(records) <= MAX_REQUEST_RECORDS:
                raise ValueError('bounded nonempty request required')
            lengths = [r.get('length') for r in records]
            if (any(type(n) is not int or not 0 < n <= MAX_BLOCK_BYTES for n in lengths)
                    or sum(lengths) > MAX_REQUEST_BYTES):
                raise ValueError('request record lengths exceed block/batch bounds')
            if any(r['dataset'] != self.dataset for r in records):
                raise ValueError('cross-dataset request refused')
            data = ''.join(' '.join(str(r[k]) for k in FIELDS) + '\n' for r in records).encode()
        self.calls[op] += 1
        # No timeout kills or restarts a kernel IO. The private VM runner owns
        # its independent host deadline and reports the actual VM state.
        p = self.runner([self.client, op, *self.binding, *map(str, args)],
                        input=data, capture_output=True)
        if raw:
            if p.returncode:
                raise RuntimeError(p.stderr.decode('utf8', 'backslashreplace'))
            return p.stdout
        try:
            result = json.loads(p.stdout)
        except (ValueError, TypeError):
            raise RuntimeError({'op': op, 'exit': p.returncode,
                                'stderr': p.stderr.decode('utf8', 'backslashreplace')})
        if not isinstance(result, dict):
            raise ValueError('client response must be an object')
        if p.returncode and not (op in ('query', 'warm') and isinstance(result.get('records'), list)):
            raise RuntimeError({'op': op, 'exit': p.returncode, 'result': result})
        result['client_exit_code'] = p.returncode
        return result

    def stat(self, object_id):
        r = self.command('stat', [object_id])
        if r['object'] != object_id or r['size'] < 0:
            raise ValueError('unexpected object identity')
        return r

    def read(self, identity, offset, length):
        if not 0 <= offset <= identity['size'] or not 0 < length <= MAX_READ or length > identity['size'] - offset:
            raise ValueError('object read outside capture')
        args = [identity[k] for k in FIELDS[:5]] + [offset, length]
        data = self.command('read', args, raw=True)
        if len(data) != length:
            raise ValueError('truncated object read')
        return data

    def query(self, records):
        out = {}
        for selected in query_batches(records):
            answer = self.command('query', records=selected)
            if answer['count'] != len(selected) or len(answer['records']) != len(selected):
                raise ValueError('QUERY count mismatch')
            if answer['client_exit_code'] and not any(r.get('error') for r in answer['records']):
                raise ValueError('QUERY client failed without a record-level error')
            for original, result in zip(selected, answer['records']):
                if any(original[k] != result[k] for k in FIELDS):
                    raise ValueError('QUERY changed input identity')
                out[planner.record_key(original)] = result
        return out


def warm_response_errors(answer, submitted):
    """Drainage is proved only for the exact submitted request and every record."""
    errors = []
    records = answer.get('records')
    if answer.get('client_exit_code') != 0:
        errors.append('client exit was nonzero')
    if not isinstance(records, list):
        return errors + ['missing record responses']
    if answer.get('count') != len(submitted) or len(records) != len(submitted):
        errors.append('response does not cover the submitted batch')
    for original, returned in zip(submitted, records):
        if not isinstance(returned, dict):
            errors.append('invalid record response')
            continue
        if any(original[k] != returned.get(k) for k in FIELDS):
            errors.append('response changed input identity')
        if returned.get('error') != 0 or returned.get('flags', 0) & 7 != 7:
            errors.append('record lacks successful real-block issue and completion')
    if (answer.get('issued_arc_requests') != len(submitted)
            or answer.get('completed_requests') != len(submitted)):
        errors.append('issue/completion counts do not cover the submitted batch')
    return errors


class ObjectReader:
    def __init__(self, device, identity, chunk_size=MAX_READ, cache_chunks=16):
        from collections import OrderedDict
        if not 1 <= chunk_size <= MAX_READ or not 1 <= cache_chunks <= 64:
            raise ValueError('bounded object cache required')
        self.device, self.identity = device, identity
        self.chunk_size, self.limit, self.cache = chunk_size, cache_chunks, OrderedDict()

    def read_at(self, offset, length):
        if offset < 0 or length < 0 or offset + length > self.identity['size']:
            raise ValueError('read outside source binding')
        result = bytearray()
        while length:
            base = offset // self.chunk_size * self.chunk_size
            if base not in self.cache:
                n = min(self.chunk_size, self.identity['size'] - base)
                self.cache[base] = self.device.read(self.identity, base, n)
            self.cache.move_to_end(base)
            data = self.cache[base]
            take = min(length, len(data) - (offset - base))
            result += data[offset - base:offset - base + take]
            offset += take
            length -= take
            while len(self.cache) > self.limit:
                self.cache.popitem(last=False)
        return bytes(result)


def physical_batches(records, queried):
    ordered = sorted(records, key=lambda r: (queried[planner.record_key(r)]['vdev'],
                                             queried[planner.record_key(r)]['dva_offset'],
                                             queried[planner.record_key(r)]['birth']))
    batches, selected, charged = [], [], 0
    for r in ordered:
        charge = queried[planner.record_key(r)]['block_size']
        if not 0 < charge <= MAX_BLOCK_BYTES:
            raise ValueError('invalid full-block budget')
        if len(selected) == MAX_REQUEST_RECORDS or charged + charge > MAX_REQUEST_BYTES:
            batches.append(selected)
            selected, charged = [], 0
        selected.append(r)
        charged += charge
    if selected:
        batches.append(selected)
    return batches


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--client-sha256', required=True)
    p.add_argument('--dataset', required=True)
    p.add_argument('--pool-guid', type=int, required=True)
    p.add_argument('--dataset-guid', type=int, required=True)
    p.add_argument('--catalog-dataset')
    p.add_argument('--catalog-pool-guid', type=int)
    p.add_argument('--catalog-dataset-guid', type=int)
    p.add_argument('--file-object', type=int, required=True)
    p.add_argument('--native-object', type=int, required=True)
    p.add_argument('--layout', type=Path, required=True)
    p.add_argument('--layout-sha256', required=True)
    p.add_argument('--cursor', type=int, default=0)
    p.add_argument('--max-rows', type=int, default=8192)
    p.add_argument('--execute', action='store_true')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if (os.geteuid() != 0 or socket.gethostname().split('.')[0] != 'cdsprefetch-native-20261024'
            or subprocess.check_output(['sysctl', '-n', 'kern.vm_guest']).strip() != b'bhyve'
            or not a.dataset.startswith('cdspnative_') or a.pool_guid <= 0 or a.dataset_guid <= 0):
        raise SystemExit('Private guest and uniquely owned fixture dataset required; production is not admitted')
    catalog = (a.catalog_dataset, a.catalog_pool_guid, a.catalog_dataset_guid)
    if any(v is not None for v in catalog):
        if (any(v is None for v in catalog) or not a.catalog_dataset.startswith('cdspnative_')
                or a.catalog_pool_guid <= 0 or a.catalog_dataset_guid <= 0):
            raise SystemExit('complete private catalog dataset binding required')
    else:
        catalog = (a.dataset, a.pool_guid, a.dataset_guid)
    if digest(a.client) != a.client_sha256 or not a.client.is_file() or a.client.is_symlink():
        raise SystemExit('client hash/path mismatch')
    if a.output.exists() or a.output.is_symlink():
        raise SystemExit('new output path required')
    if digest(a.layout) != a.layout_sha256:
        raise SystemExit('layout hash mismatch')
    layout = planner.Layout.from_admitted_bytes(a.layout.read_bytes())
    dev = Device(a.client, a.dataset, a.pool_guid, a.dataset_guid)
    catalog_dev = Device(a.client, *catalog)
    file_id, native_id = catalog_dev.stat(a.file_object), catalog_dev.stat(a.native_object)
    file_reader = ObjectReader(catalog_dev, file_id, chunk_size=16384, cache_chunks=64)
    native_reader = ObjectReader(catalog_dev, native_id)
    files = planner.FileTable(file_reader.read_at, file_id['size'], layout, cache_records=4096)
    native = planner.NativeCatalog(native_reader.read_at, native_id['size'])
    at = native.next_group_boundary(a.cursor)
    if at is None:
        raise SystemExit('bounded group-boundary discovery incomplete; warming held')
    state = planner.Planner(native, files).start(at)
    plan = planner.Planner(native, files).window(state, max_rows=a.max_rows, include_pairs=True)
    inventory = {}
    for pair in plan['pairs']:
        for role in ('source', 'destination'):
            r = pair[role]
            inventory[planner.record_key(r)] = r
    queried = dev.query(list(inventory.values()))
    filtered = planner.filter_shared_pairs(plan, queried)
    batches = physical_batches(filtered['records'], queried)
    results = []
    for batch in batches:
        if a.execute:
            started = time.monotonic()
            response = dev.command('warm', records=batch)
            results.append({'seconds': time.monotonic() - started, 'response': response,
                            'submitted_count': len(batch),
                            'validation_errors': warm_response_errors(response, batch)})
    failed_warm = any(x['validation_errors'] for x in results)
    partial = bool(filtered['coverage_errors']) or failed_warm
    status = ('PRIVATE_WINDOW_PARTIAL_OR_ERROR' if partial else
              'PRIVATE_WINDOW_PREFETCH_REQUESTS_DRAINED' if a.execute else
              'PRIVATE_WINDOW_PLANNED_ONLY')
    result = {'status': status,
              'client_sha256': a.client_sha256, 'layout_sha256': a.layout_sha256,
              'source_objects': {'file_table': file_id, 'native': native_id},
              'catalog_binding': {'dataset': catalog[0], 'pool_guid': catalog[1], 'dataset_guid': catalog[2]},
              'plan': plan, 'filter': filtered, 'physical_batch_count': len(batches),
              'calls': dev.calls, 'catalog_calls': catalog_dev.calls, 'warming': results,
              'worker_signals_or_descriptor_operations': False,
              'limits': ['ARC requests/completion are not successful sharing or measured speedup.',
                         'BP mappings may change after QUERY; all hints remain advisory.',
                         'This entry point refuses production hosts.']}
    # New path only; preserve every partial/failure receipt rather than replace.
    with a.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({'status': result['status'], 'request_count': filtered['request_count'],
                      'already_shared_pairs': filtered['already_shared_pairs'], 'calls': dev.calls}))


if __name__ == '__main__':
    main()

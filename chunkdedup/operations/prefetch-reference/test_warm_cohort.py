"""Generated catalog and injected-device tests; no kernel or production access."""
import unittest
from dataclasses import asdict
import warm_cohort as c

p = c.p


class Files:
    def __init__(self, count, block):
        self.count, self.block = count, block
        self.layout = type('Layout', (), {'block_method': 1})()
        self.calls = 0

    def get(self, number):
        if not 0 <= number < self.count:
            raise ValueError('bad fixture file ID')
        self.calls += 1
        return p.FileRecord('fixture/fs', 7, number + 100, 1,
                            self.block, 1700000000, 1, self.block, 1, 0)


def fixture(count=700, block=4096):
    rows = b''.join((f'{7:020d} {"a"*64} {block:020d} {i:020d} '
                     f'{0:020d} {block:020d} {block:020d}\n').encode()
                    for i in range(count))
    native = p.NativeCatalog(lambda at, n: rows[at:at+n], len(rows))
    return p.Planner(native, Files(count, block))


def query(records):
    return {p.record_key(r): {**r, 'error': 0, 'flags': 1,
                             'block_size': r['length'], 'vdev': r['object'] % 2,
                             'dva_offset': 10000000-r['object']*4096,
                             'birth': r['object'], 'dva0_word0': r['object'],
                             'dva0_word1': 123}
            for r in records}


class Device:
    def __init__(self, broken=False):
        self.calls, self.broken = [], broken

    def command(self, op, records):
        assert op == 'warm'
        self.calls.append(records)
        return {'count': len(records), 'client_exit_code': 0,
                'issued_arc_requests': len(records),
                'completed_requests': len(records),
                'records': [{**r, 'error': 0, 'flags': 1 if self.broken else 7}
                            for r in records]}


class Clock:
    value = 0

    def __call__(self):
        return self.value


class CohortTests(unittest.TestCase):
    def test_split_giant_group_preserves_canonical_and_all_rows(self):
        q = fixture()
        initial = q.start()
        original = asdict(initial)
        cohort = c.build_cohort(q, initial, query)
        self.assertEqual(asdict(initial), original)
        self.assertTrue(cohort['eof'])
        self.assertGreater(len(cohort['plans']), 2)
        self.assertEqual(sum(x['rows_consumed'] for x in cohort['plans']), 700)
        self.assertEqual(sum(x['pair_count'] for x in cohort['plans']), 699)
        self.assertEqual(len(cohort['inventory']), 700)
        self.assertEqual(len(cohort['records']), 700)
        self.assertTrue(all(pair['source']['object'] == 100
                            for plan in cohort['plans'] for pair in plan['pairs']))

    def test_rejected_preview_does_not_drop_rows_or_canonical(self):
        q = fixture()
        state = q.start()
        all_pairs = []
        while state.next_cursor < q.native.total_bytes:
            cohort = c.build_cohort(q, state, query, max_records=300)
            self.assertLessEqual(len(cohort['inventory']), 300)
            all_pairs.extend(pair['destination_catalog_cursor']
                             for plan in cohort['plans'] for pair in plan['pairs'])
            self.assertGreater(cohort['next_state'].next_cursor, state.next_cursor)
            state = cohort['next_state']
        self.assertEqual(all_pairs, list(range(191, 700*191, 191)))

    def test_catalog_walk_cap_and_continuation(self):
        q = fixture(50)
        one = c.build_cohort(q, q.start(), query, max_catalog_bytes=10*191)
        two = c.build_cohort(q, one['next_state'], query, max_catalog_bytes=10*191)
        self.assertEqual(one['catalog_bytes_walked'], 10*191)
        self.assertEqual(two['cursor_start'], one['cursor_end'])
        self.assertEqual(two['plans'][0]['pairs'][0]['source']['object'], 100)
        self.assertEqual(two['plans'][0]['pairs'][0]['destination_catalog_cursor'], 10*191)

    def test_global_physical_order_and_record_caps(self):
        cohort = c.build_cohort(fixture(), p.Continuation(0), query)
        flat = [r for batch in cohort['batches'] for r in batch]
        positions = [(cohort['queried'][p.record_key(r)]['vdev'],
                      cohort['queried'][p.record_key(r)]['dva_offset']) for r in flat]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(len(flat), 700)
        self.assertTrue(all(len(b) <= 2048 for b in cohort['batches']))
        self.assertEqual(cohort['worst_case_issued_logical_bytes'],
                         len(cohort['batches'])*(256 << 20))

    def test_full_block_byte_caps(self):
        q = fixture(100, block=16 << 20)
        cohort = c.build_cohort(q, q.start(), query)
        self.assertLessEqual(cohort['predicted_inventory_bytes'], 512 << 20)
        self.assertGreater(len(cohort['batches']), 1)
        for batch in cohort['batches']:
            self.assertLessEqual(sum(r['length'] for r in batch), 256 << 20)

    def test_shared_pairs_are_omitted_across_windows(self):
        def same(records):
            out = query(records)
            for bp in out.values():
                bp.update(birth=1, dva0_word0=2, dva0_word1=3)
            return out
        q = fixture()
        cohort = c.build_cohort(q, q.start(), same)
        self.assertFalse(cohort['records'])
        self.assertEqual(sum(f['already_shared_pairs'] for f in cohort['filters']), 699)

    def test_missing_query_is_coverage_not_warming_success(self):
        q = fixture(3)
        def missing(records):
            out = query(records)
            del out[p.record_key(records[-1])]
            return out
        cohort = c.build_cohort(q, q.start(), missing)
        self.assertEqual(len(cohort['coverage_errors']), 1)
        self.assertEqual(len(cohort['records']), 2)
        dev = Device()
        out = c.execute_cohort(cohort, dev, lambda: True, 100, now=Clock())
        self.assertEqual(out['status'], 'PARTIAL_QUERY_COVERAGE')
        self.assertFalse(dev.calls)
        self.assertEqual(out['batches_drained_successfully'], 0)

    def test_deadline_after_guard_prevents_call(self):
        q, clock, dev = fixture(), Clock(), Device()
        cohort = c.build_cohort(q, q.start(), query)
        def guard():
            clock.value = 2
            return True
        out = c.execute_cohort(cohort, dev, guard, 1, now=clock)
        self.assertEqual(out['status'], 'DEADLINE')
        self.assertFalse(dev.calls)

    def test_guard_between_batches_stops_without_cancel(self):
        q, dev = fixture(3000), Device()
        cohort = c.build_cohort(q, q.start(), query)
        allowed = iter([{'ok': True}, {'ok': False, 'reason': 'caught up'}])
        out = c.execute_cohort(cohort, dev, lambda: next(allowed), 100, now=Clock())
        self.assertEqual(out['status'], 'GUARD_REFUSED')
        self.assertEqual(len(dev.calls), 1)
        self.assertEqual(out['batches_drained_successfully'], 1)
        self.assertFalse(out['signals_or_cancel_used'])

    def test_invalid_response_stops_and_never_claims_drain(self):
        q, dev = fixture(), Device(broken=True)
        cohort = c.build_cohort(q, q.start(), query)
        out = c.execute_cohort(cohort, dev, lambda: True, 100, now=Clock())
        self.assertEqual(out['status'], 'WARM_RESPONSE_ERROR')
        self.assertEqual(out['batches_drained_successfully'], 0)
        self.assertEqual(len(dev.calls), 1)

    def test_guard_exception_prevents_all_calls(self):
        q, dev = fixture(), Device()
        cohort = c.build_cohort(q, q.start(), query)
        def guard():
            raise RuntimeError('observer failed')
        out = c.execute_cohort(cohort, dev, guard, 100, now=Clock())
        self.assertEqual(out['status'], 'GUARD_ERROR')
        self.assertFalse(dev.calls)

    def test_all_batches_drained(self):
        q, dev = fixture(), Device()
        cohort = c.build_cohort(q, q.start(), query)
        out = c.execute_cohort(cohort, dev, lambda: True, 100, now=Clock())
        self.assertEqual(out['status'], 'DRAINED')
        self.assertEqual(out['batches_drained_successfully'], len(cohort['batches']))

    def test_cursor_drops_consumed_destinations_but_keeps_future_canonical(self):
        q, dev = fixture(700), Device()
        cohort = c.build_cohort(q, q.start(), query)
        cursor = 350*191
        out = c.execute_cohort(cohort, dev, lambda: {'ok': True, 'cursor': cursor},
                               100, now=Clock())
        sent = [r for batch in dev.calls for r in batch]
        objects = {r['object'] for r in sent}
        self.assertIn(100, objects)  # Canonical row0 still needed by row699.
        self.assertEqual(objects - {100}, set(range(451, 800)))
        self.assertEqual(out['obsolete_records_omitted'], 350)
        self.assertEqual(out['status'], 'DRAINED')

    def test_catch_up_uses_end_not_start(self):
        q, dev = fixture(3000), Device()
        cohort = c.build_cohort(q, q.start(), query)
        positions = iter([{'ok': True, 'cursor': cohort['cursor_start']},
                          {'ok': True, 'cursor': cohort['cursor_end']}])
        out = c.execute_cohort(cohort, dev, lambda: next(positions), 100, now=Clock())
        self.assertEqual(len(dev.calls), 1)
        self.assertEqual(out['status'], 'CAUGHT_UP')

    def test_empty_obsolete_batch_is_skipped(self):
        q, dev = fixture(), Device()
        cohort = c.build_cohort(q, q.start(), query)
        cursor = cohort['cursor_end'] - 1
        out = c.execute_cohort(cohort, dev, lambda: {'ok': True, 'cursor': cursor},
                               100, now=Clock())
        self.assertFalse(dev.calls)
        self.assertEqual(out['empty_obsolete_batches_skipped'], len(cohort['batches']))

    def test_bad_guard_cursor_refuses_calls(self):
        q, dev = fixture(), Device()
        cohort = c.build_cohort(q, q.start(), query)
        out = c.execute_cohort(cohort, dev, lambda: {'ok': True, 'cursor': '1'},
                               100, now=Clock())
        self.assertEqual(out['status'], 'GUARD_ERROR')
        self.assertFalse(dev.calls)

    def test_later_shared_pairs_do_not_extend_canonical_lifetime(self):
        q, dev = fixture(700), Device()
        def mixed(records):
            out = query(records)
            for record in records:
                if record['object'] != 101:
                    out[p.record_key(record)].update(birth=1, dva0_word0=2, dva0_word1=3)
            return out
        cohort = c.build_cohort(q, q.start(), mixed)
        self.assertEqual({r['object'] for r in cohort['records']}, {100, 101})
        canonical = next(r for r in cohort['records'] if r['object'] == 100)
        self.assertEqual(cohort['needed_until'][p.record_key(canonical)], 191)
        out = c.execute_cohort(cohort, dev,
                               lambda: {'ok': True, 'cursor': 350*191},
                               100, now=Clock())
        self.assertFalse(dev.calls)
        self.assertEqual(out['obsolete_records_omitted'], 2)


if __name__ == '__main__':
    unittest.main()

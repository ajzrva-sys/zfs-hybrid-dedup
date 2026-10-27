import hashlib
import unittest
import advisory_table as a


class Device:
    def __init__(self, rows=1400):
        self.data = bytes(range(256)) * (1608 * rows // 256) + bytes(range(256))[:1608 * rows % 256]
        self.meta = dict(object=4, generation=3, size=len(self.data), mtime_sec=10, mtime_nsec=11)
        self.calls = []
        self.stats = 0
        self.mutate_after = False
        self.short = False

    def stat(self, obj):
        self.stats += 1
        r = dict(self.meta)
        if self.mutate_after and self.stats == 2:
            r['mtime_nsec'] += 1
        return r

    def read(self, expected, offset, size):
        self.calls.append((offset, size))
        out = self.data[offset:offset + size]
        return out[:-1] if self.short else out


class TestRamCopy(unittest.TestCase):
    def test_sequential_bounded_copy_and_random_lookup(self):
        d = Device(); r = a.capture(d, d.meta, lambda: True)
        self.assertEqual(r.receipt['sha256'], hashlib.sha256(d.data).hexdigest())
        self.assertEqual(r.read_at(1607,1609), d.data[1607:3216])
        self.assertEqual(d.calls[0],(0,1<<20))
        self.assertTrue(all(n <= 1<<20 for _,n in d.calls))
        self.assertEqual(d.calls[-1][0]+d.calls[-1][1],len(d.data))
        self.assertFalse(r.receipt['checkpoint_or_scan_resume'])

    def test_size_refusal_precedes_allocation_and_reads(self):
        d=Device()
        with self.assertRaisesRegex(ValueError,'size/layout'):
            a.capture(d,d.meta,lambda: True,max_bytes=100,allocator=lambda n:self.fail('allocated'))
        self.assertEqual(d.calls,[])

    def test_explicit_dict_refusal_precedes_allocation(self):
        d=Device()
        with self.assertRaisesRegex(ValueError,'guard refused'):
            a.capture(d,d.meta,lambda:{'ok':False},allocator=lambda n:self.fail('allocated'))
        self.assertEqual(d.calls,[])

    def test_changed_initial_identity_refuses(self):
        d=Device(); expected=dict(d.meta); expected['generation']+=1
        with self.assertRaisesRegex(ValueError,'before copy'):
            a.capture(d,expected,lambda: True)
        self.assertEqual(d.calls,[])

    def test_changed_final_identity_refuses_instead_of_rebinding(self):
        d=Device(); d.mutate_after=True
        with self.assertRaisesRegex(ValueError,'after copy'):
            a.capture(d,d.meta,lambda: True)
        self.assertEqual(d.stats,2)

    def test_short_reads_refuse(self):
        d=Device(); d.short=True
        with self.assertRaisesRegex(ValueError,'short'):
            a.capture(d,d.meta,lambda: True)

    def test_guard_stops_before_more_reads(self):
        d=Device(); count=0
        def guard():
            nonlocal count
            count+=1;return count < 3
        with self.assertRaisesRegex(ValueError,'guard refused'):
            a.capture(d,d.meta,guard)
        self.assertEqual(len(d.calls),1)

    def test_bounds_on_lookup(self):
        r=a.capture(Device(2),Device(2).meta,lambda: True)
        with self.assertRaisesRegex(ValueError,'outside'):
            r.read_at(len(r.data)-1,2)


if __name__=='__main__':unittest.main(verbosity=2)

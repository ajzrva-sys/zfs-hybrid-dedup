import unittest

from impact import Impact


def groups_at(at, idle=None, factor=None):
    idle, factor = idle or {}, factor or {}
    total = 0
    for cycle in range(int(at // 300) + 1):
        span = min(300, max(0, at - cycle * 300))
        rate = idle.get(cycle, 100)
        total += rate * (min(span, 60) * factor.get(cycle, 1) + max(0, span - 60))
    return int(total)


def feed(impact, end=1200, *, idle=None, factor=None, offset=0, missing=()):
    results = []
    for at in range(offset, end + 1, 10):
        if at in missing:
            continue
        result = impact.observe(at, groups_at(at, idle, factor))
        if result is not None:
            results.append(result)
    return results


class ImpactTests(unittest.TestCase):
    def test_initial_partial_and_two_windows_are_insufficient(self):
        self.assertEqual(feed(Impact(0), end=900), [])

    def test_three_regular_complete_windows_record_observation_not_causation(self):
        result = feed(Impact(0))[-1]
        self.assertEqual(result['state'], 'Observed')
        self.assertFalse(result['hold'])
        self.assertAlmostEqual(result['observed_slowdown_pct'], 0)
        self.assertFalse(result['causal'])
        self.assertFalse(result['groups_are_durable_progress'])

    def test_three_individual_and_aggregate_slowdowns_hold(self):
        result = feed(Impact(0), factor={1: .5, 2: .5, 3: .5})[-1]
        self.assertTrue(result['hold'])
        self.assertGreater(result['observed_slowdown_pct'], 5)
        self.assertTrue(all(w['observed_slowdown_pct'] > 5 for w in result['windows']))

    def test_one_burst_cannot_hold_even_when_aggregate_exceeds_threshold(self):
        result = feed(Impact(0), factor={1: 0})[-1]
        self.assertGreater(result['observed_slowdown_pct'], 5)
        self.assertFalse(result['hold'])

    def test_weighted_baseline_uses_projected_seconds_not_mean_rates(self):
        result = feed(Impact(0), idle={1: 100, 2: 200, 3: 300},
                      factor={1: .2, 2: .5, 3: .8})[-1]
        windows = result['windows']
        total = sum(w['groups'] for w in windows)
        expected = total / sum(w['groups'] / w['idle_rate'] for w in windows)
        self.assertAlmostEqual(result['weighted_idle_groups_per_second'], expected)
        self.assertNotAlmostEqual(expected, sum(w['idle_rate'] for w in windows) / 3)
        self.assertAlmostEqual(result['groups_per_second'],
                               total / sum(w['seconds'] for w in windows))

    def test_jitter_uses_actual_bracketing_duration_and_only_idle_samples(self):
        result = feed(Impact(0), end=1207, offset=7)[-1]
        self.assertTrue(all(w['seconds'] == 310 for w in result['windows']))
        self.assertTrue(all(w['idle_seconds'] == 170 for w in result['windows']))
        self.assertAlmostEqual(result['observed_slowdown_pct'], 0)

    def test_gaps_and_zero_counters_are_insufficient_not_a_pass(self):
        self.assertEqual(feed(Impact(0), missing=range(650, 720, 10)), [])
        impact = Impact(0)
        self.assertTrue(all(impact.observe(at, 0) is None for at in range(0, 1201, 10)))

    def test_counter_or_clock_regression_holds_and_is_latched(self):
        for next_point in [(11, 99), (9, 101)]:
            impact = Impact(0)
            impact.observe(10, 100)
            result = impact.observe(*next_point)
            self.assertTrue(result['hold'])
            self.assertEqual(impact.observe(20, 200), result)


if __name__ == '__main__':
    unittest.main(verbosity=2)

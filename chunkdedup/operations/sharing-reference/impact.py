"""Observed saver cost from activity counters; no reads or active controls.

Full-window endpoints bracket nominal300s periods (at most20s boundary slop).
Idle endpoints stay strictly inside phase120..300; actual elapsed times are used.
Insufficient, stalled or gapped samples never establish acceptable performance.
"""
import math


class Impact:
    def __init__(self, origin, period=300, active=60):
        if not math.isfinite(origin) or period != 300 or active != 60:
            raise ValueError('unqualified impact window profile')
        self.origin, self.period, self.active = origin, period, active
        self.points, self.windows, self.index, self.held = [], [], None, None

    def observe(self, monotonic, groups):
        if self.held is not None:
            return dict(self.held)
        if (type(monotonic) not in (int, float) or not math.isfinite(monotonic) or
                monotonic < self.origin or type(groups) is not int or groups < 0 or
                self.points and (monotonic <= self.points[-1][0] or groups < self.points[-1][1])):
            self.held = dict(state='Hold', hold=True, reason='Activity counter/time regressed or is invalid', observed_only=True)
            return dict(self.held)
        self.points.append((monotonic, groups))
        if self.index is None:
            self.index = int((monotonic - self.origin) // self.period) + 1
        start, end = self.origin + self.index * self.period, self.origin + (self.index + 1) * self.period
        if monotonic < end:
            return None
        left = [p for p in self.points if p[0] <= start]
        idle = [p for p in self.points if start + 2 * self.active <= p[0] < end]
        covered = [p for p in self.points if (left and p[0] >= left[-1][0])]
        valid = (left and len(idle) >= 2 and start - left[-1][0] <= 20 and monotonic - end <= 20
                 and idle[-1][0] - idle[0][0] >= 150 and
                 all(b[0] - a[0] <= 30 for a, b in zip(covered, covered[1:])))
        if valid:
            g, seconds = groups - left[-1][1], monotonic - left[-1][0]
            ig, idle_seconds = idle[-1][1] - idle[0][1], idle[-1][0] - idle[0][0]
            valid = g > 0 and ig > 0 and seconds >= self.period
        if valid:
            rate = ig / idle_seconds
            self.windows = (self.windows + [dict(groups=g, seconds=seconds, idle_rate=rate,
                idle_seconds=idle_seconds, observed_slowdown_pct=100 * (1 - g / seconds / rate))])[-3:]
        else:
            self.windows.clear()
        self.index = int((monotonic - self.origin) // self.period)
        self.points = [p for p in self.points if p[0] >= end - 20]
        if len(self.windows) < 3:
            return None
        total = sum(w['groups'] for w in self.windows)
        assisted = total / sum(w['seconds'] for w in self.windows)
        baseline = total / sum(w['groups'] / w['idle_rate'] for w in self.windows)
        slowdown = 100 * (1 - assisted / baseline)
        hold = slowdown > 5 and all(w['observed_slowdown_pct'] > 5 for w in self.windows)
        result = dict(state='Hold' if hold else 'Observed', hold=hold, complete_windows=3,
                      groups_per_second=assisted, weighted_idle_groups_per_second=baseline,
                      observed_slowdown_pct=slowdown, windows=list(self.windows),
                      observed_only=True, causal=False, groups_are_durable_progress=False)
        if hold:
            self.held = dict(result, reason='Three full windows show observed slowdown above5%')
            return dict(self.held)
        return result

"""Meter this helper's combined input/output; sleep never controls the worker."""
import time


class Budget:
    def __init__(self, *, rate=64 << 20, active=300, period=300,
                 clock=time.monotonic, pause=time.sleep, guard=lambda: None):
        if not (0 < rate <= 64 << 20 and active == period == 300):
            raise ValueError('I/O budget exceeds admission')
        self.rate, self.active, self.period = rate, active, period
        self.clock, self.pause, self.guard = clock, pause, guard
        self.origin = self.next = clock()
        self.total = 0

    def wait(self, until):
        while self.clock() < until:
            self.guard()
            self.pause(min(5, max(0, until - self.clock())))

    def charge(self, size):
        if not isinstance(size, int) or not 0 <= size <= 1 << 20:
            raise ValueError('I/O charge exceeds one buffer')
        while size:
            now = self.clock()
            epoch = int((now - self.origin) // self.period)
            end = self.origin + epoch * self.period + self.active
            finish = max(now, self.next) + size / self.rate
            if finish <= end:
                self.wait(finish)
                if self.clock() > end:
                    continue
                self.next = finish
                self.total += size
                return
            self.wait(self.origin + (epoch + 1) * self.period)

    def charge_large(self, size):
        while size:
            count = min(size, 1 << 20)
            self.charge(count)
            size -= count

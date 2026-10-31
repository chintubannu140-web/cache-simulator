"""Independent list-based reference model, randomized traces, and hierarchy regressions."""
import csv
import pathlib
import random
import subprocess
import tempfile


# Lists hold block numbers in oldest-to-newest order; dirty bits are separate.
# The simple reference intentionally uses no timestamps or iterator/hash LRU index.
class Reference:
    def __init__(self, size, block, ways, policy, wb, wa, latency, lower):
        self.block = block
        self.ways = ways or size // block
        self.sets = [[] for _ in range(size // block // self.ways)]
        self.dirty = set()
        self.seen = set()
        self.shadow = []
        self.capacity = size // block
        self.policy, self.wb, self.wa = policy, wb, wa
        self.latency, self.lower = latency, lower
        self.s = dict.fromkeys(['accesses', 'hits', 'misses', 'compulsory', 'capacity', 'conflict',
                               'reads', 'writes', 'writebacks'], 0)

    def access(self, addr, write=False, full=False):
        s = self.s
        s['accesses'] += 1
        s['writes' if write else 'reads'] += 1
        block = addr // self.block
        first, shadow_hit = block not in self.seen, block in self.shadow
        self.seen.add(block)
        if shadow_hit:
            self.shadow.remove(block)
        self.shadow.append(block)
        self.shadow = self.shadow[-self.capacity:]
        group = self.sets[block % len(self.sets)]
        cycles = self.latency
        if block in group:
            s['hits'] += 1
            if self.policy == 'LRU':
                group.remove(block)
                group.append(block)
        else:
            s['misses'] += 1
            s['compulsory' if first else 'conflict' if shadow_hit else 'capacity'] += 1
            if write and not self.wa:
                return cycles + self.lower(addr, True, full)
            if len(group) == self.ways:
                old = group.pop(0)
                if old in self.dirty:
                    self.dirty.remove(old)
                    s['writebacks'] += 1
                    cycles += self.lower(old*self.block, True, True)
            if not full:
                cycles += self.lower(block*self.block, False, False)
            group.append(block)
        if write:
            if self.wb:
                self.dirty.add(block)
            else:
                cycles += self.lower(addr, True, full)
        return cycles


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    trace, output = root/'trace.txt', root/'out.csv'

    def simulate(accesses, *, size=32, block=16, ways=1, policy='LRU', wb=True, wa=True, l2=False):
        trace.write_text(''.join(f'{"W" if w else "R"} {a}\n' for a, w in accesses))
        args = ['./cache_sim', '--trace', str(trace), '--csv', str(output), '--size', str(size),
                '--block', str(block), '--assoc', str(ways), '--policy', policy,
                '--write-policy', 'WB' if wb else 'WT', '--allocation', 'WA' if wa else 'NWA']
        if l2:
            args += ['--l2-size', '64', '--l2-assoc', '2', '--l2-hit-time', '10', '--l2-policy', 'FIFO']
        subprocess.run(args, check=True, capture_output=True, text=True)
        with output.open() as stream:
            return next(csv.DictReader(stream))

    # Hand-calculated traffic and latency. Dirty state is not flushed at end.
    cases = [
        ([(0, True), (0, True), (32, True)], True, True, (2, 1, 1, 1, 303)),
        ([(0, True), (0, True), (32, True)], False, True, (2, 3, 0, 0, 503)),
        ([(0, True), (0, True), (32, True)], True, False, (0, 3, 0, 0, 303)),
        ([(0, True), (0, False)], True, False, (1, 1, 0, 0, 202)),
    ]
    for accesses, wb, wa, expected in cases:
        row = simulate(accesses, wb=wb, wa=wa)
        actual = tuple(int(float(row[k])) for k in ['memory_reads', 'memory_writes', 'writebacks', 'dirty_lines', 'total_cycles'])
        assert actual == expected, (actual, expected)
    row = simulate([(0, False), (32, False), (0, False)], l2=True)
    assert tuple(int(row[k]) for k in ['misses', 'l2_accesses', 'l2_hits', 'l2_misses', 'memory_reads']) == (3, 3, 1, 2, 2)
    assert float(row['total_cycles']) == 233
    # Non-inclusive L2 may evict a block still dirty in L1. A later full-block
    # L1 writeback miss in L2 must not read that block from memory first.
    trace.write_text('W 0\nR 16\nR 32\nR 48\nW 64\n')
    subprocess.run(['./cache_sim', '--trace', str(trace), '--csv', str(output),
                    '--size', '64', '--block', '16', '--assoc', '0',
                    '--l2-size', '16', '--l2-assoc', '1'], check=True, capture_output=True)
    with output.open() as stream:
        row = next(csv.DictReader(stream))
    assert tuple(int(row[k]) for k in ['memory_reads', 'memory_writes', 'writebacks', 'l2_writebacks']) == (5, 1, 1, 1)
    assert float(row['total_cycles']) == 665
    print('PASS hand-calculated write traffic, L1/L2 latency, and non-inclusive dirty eviction')

    rng = random.Random(2026)
    count = 0
    for l2 in [False, True]:
        for wb in [False, True]:
            for wa in [False, True]:
                for policy in ['FIFO', 'LRU']:
                    for ways in [1, 2, 0]:
                        accesses = [(rng.randrange(16)*8, bool(rng.randrange(2))) for _ in range(120)]
                        memory = dict(reads=0, writes=0)

                        def lower(addr, write, full):
                            memory['writes' if write else 'reads'] += 1
                            return 100

                        second = Reference(64, 16, 2, 'FIFO', wb, wa, 10, lower) if l2 else None
                        first = Reference(32, 16, ways, policy, wb, wa, 1, second.access if second else lower)
                        cycles = sum(first.access(a, w) for a, w in accesses)
                        row = simulate(accesses, ways=ways, policy=policy, wb=wb, wa=wa, l2=l2)
                        for key, value in first.s.items():
                            assert int(row[key]) == value, (key, row[key], value)
                        assert int(row['dirty_lines']) == len(first.dirty)
                        assert int(row['memory_reads']) == memory['reads']
                        assert int(row['memory_writes']) == memory['writes']
                        assert float(row['total_cycles']) == cycles
                        assert abs(float(row['amat']) - cycles/len(accesses)) < 1e-9
                        assert int(row['hits']) + int(row['misses']) == len(accesses)
                        assert sum(int(row[k]) for k in ['compulsory', 'capacity', 'conflict']) == int(row['misses'])
                        if second:
                            for key in ['accesses', 'hits', 'misses', 'writebacks']:
                                assert int(row['l2_' + key]) == second.s[key]
                            assert int(row['l2_dirty_lines']) == len(second.dirty)
                        count += 1
    print(f'PASS {count} deterministic randomized reference comparisons across write policies and levels')

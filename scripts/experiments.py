#!/usr/bin/env python3
"""Reproducible cache sweep; emits raw CSV, a measured report, and optional plots."""
import argparse
import csv
import itertools
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=pathlib.Path, default=ROOT/'artifacts')
    parser.add_argument('--plot', action='store_true', help='generate PNG and SVG using Matplotlib')
    args = parser.parse_args()
    out = args.out.resolve()
    traces = out/'traces'
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['make', 'cache_sim', 'workload_runner'], cwd=ROOT, check=True)
    subprocess.run([str(ROOT/'workload_runner'), str(traces)], check=True)
    rows = []
    workloads = ['matmul_naive', 'matmul_blocked', 'array', 'linked_list']
    # One-factor sweeps plus write-policy and hierarchy comparisons at the baseline.
    baseline = dict(size=4096, block=64, assoc=4, policy='LRU', write='WB', allocation='WA', l2=0)
    configurations = []
    for field, values in [('size', [1024, 4096, 16384, 32768]), ('block', [16, 32, 64, 128]),
                          ('assoc', [1, 2, 4, 8]), ('policy', ['FIFO', 'LRU'])]:
        for value in values:
            configurations.append({**baseline, field: value})
    for write, allocation in itertools.product(['WB', 'WT'], ['WA', 'NWA']):
        configurations.append({**baseline, 'write': write, 'allocation': allocation})
    configurations.append({**baseline, 'l2': 32768})
    configurations = list({tuple(c.items()): c for c in configurations}.values())
    with tempfile.TemporaryDirectory() as directory:
        csv_path = pathlib.Path(directory)/'run.csv'
        for workload in workloads:
            for c in configurations:
                command = [str(ROOT/'cache_sim'), '--trace', str(traces/f'{workload}.txt'),
                           '--size', str(c['size']), '--block', str(c['block']), '--assoc', str(c['assoc']),
                           '--policy', c['policy'], '--write-policy', c['write'], '--allocation', c['allocation'],
                           '--csv', str(csv_path)]
                if c['l2']:
                    command += ['--l2-size', str(c['l2']), '--l2-assoc', '8']
                subprocess.run(command, check=True, capture_output=True, text=True)
                with csv_path.open() as stream:
                    row = next(csv.DictReader(stream))
                row['trace'] = f'traces/{workload}.txt'
                rows.append({'workload': workload, **row})
            print(f'Completed {workload}: {len(configurations)} configurations', flush=True)
    with (out/'sweep.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    report(rows, out)
    if args.plot:
        plot(rows, out)
    print(f'Results: {out}/sweep.csv and {out}/report.md')


def select(rows, workload, **changes):
    config = dict(cache_size='4096', block_size='64', associativity='4', policy='LRU',
                  write_policy='WB', allocation='WA', l2_size='0')
    config.update({k: str(v) for k, v in changes.items()})
    return next(r for r in rows if r['workload'] == workload and all(r[k] == v for k, v in config.items()))


def report(rows, out):
    lines = ['# Reproducible workload experiment', '',
             'Run: `make experiments` (charts: `make plots PYTHON=.venv/bin/python`).', '',
             'These are source-instrumented logical data traces from executed C++ computations, '
             'not hardware captures. Initialization, stack, instruction fetches, compiler transformations, '
             'and tracer operations are excluded. Addresses are normalized for reproducibility. '
             'All caches start cold; remaining dirty lines are not flushed.', '',
             'Matrices: 32 × 32 doubles, 8 × 8 tiles, identical multiply/update operations and checked results. '
             'Traversal: 1,024 values, four passes; array elements are 8 bytes apart, '
             'linked-list logical nodes are 64 bytes apart in seeded shuffled order. '
             'This deliberately compares both layout and traversal, not just pointer overhead.', '',
             'Baseline: 4 KiB, 64-byte blocks, 4-way LRU, WB/WA; hit 1 cycle, memory 100 cycles.', '',
             '| Workload | Accesses | Miss rate | AMAT (cycles) | Memory reads | Memory writes |',
             '|---|---:|---:|---:|---:|---:|']
    for workload in ['matmul_naive', 'matmul_blocked', 'array', 'linked_list']:
        r = select(rows, workload)
        lines.append(f'| {workload} | {r["accesses"]} | {float(r["miss_ratio"]):.2%} | {float(r["amat"]):.3f} | {r["memory_reads"]} | {r["memory_writes"]} |')
    naive, blocked = select(rows, 'matmul_naive'), select(rows, 'matmul_blocked')
    lines += ['', f'At the baseline, blocking changes miss rate from {float(naive["miss_ratio"]):.2%} '
              f'to {float(blocked["miss_ratio"]):.2%}, and simulated AMAT from {float(naive["amat"]):.3f} '
              f'to {float(blocked["amat"]):.3f} cycles. Tiles reuse smaller portions of A, B, and C before moving on. '
              'Set conflicts can limit the benefit; cache capacity alone does not predict every result.', '',
              '| Cache size | Naive miss rate | Blocked miss rate |', '|---|---:|---:|']
    for size in [1024, 4096, 16384, 32768]:
        a, b = [select(rows, w, cache_size=size) for w in ['matmul_naive', 'matmul_blocked']]
        lines.append(f'| {size} B | {float(a["miss_ratio"]):.2%} | {float(b["miss_ratio"]):.2%} |')
    lines += ['', 'The total matrix working set is 24 KiB. The size sweep shows whether blocking still '
              'helps when that working set fits. The CSV also varies block size, associativity, replacement, '
              'write policy, and allocation independently; these are one-factor comparisons, not an exhaustive search.', '',
              '## Write policies on blocked multiplication', '',
              '| Policy | Allocation | AMAT | Memory reads | Memory writes | Pending L1 dirty lines |',
              '|---|---|---:|---:|---:|---:|']
    for wp, alloc in itertools.product(['WB', 'WT'], ['WA', 'NWA']):
        r = select(rows, 'matmul_blocked', write_policy=wp, allocation=alloc)
        lines.append(f'| {wp} | {alloc} | {float(r["amat"]):.3f} | {r["memory_reads"]} | {r["memory_writes"]} | {r["dirty_lines"]} |')
    r = select(rows, 'matmul_naive', l2_size=32768)
    lines += ['', '## L1/L2 example', '',
              f'Naive multiplication with baseline L1 and a 32 KiB, 8-way LRU L2 (10-cycle lookup): '
              f'L1 miss rate {float(r["miss_ratio"]):.2%}, L2 local miss rate {float(r["l2_local_miss_ratio"]):.2%}, '
              f'global fill miss ratio {float(r["global_fill_miss_ratio"]):.2%}, AMAT {float(r["amat"]):.3f} cycles.', '',
              'L2 local statistics include L1 dirty writebacks. Global fill miss ratio means memory block reads '
              'per CPU access, not write-bypass traffic. Latency is a serial blocking model; there are no '
              'write buffers, prefetching, bandwidth effects, or overlapping misses. These are simulated '
              'latencies, not measured program runtime or CPU speedups.', '']
    (out/'report.md').write_text('\n'.join(lines))


def plot(rows, out):
    os.environ.setdefault('MPLCONFIGDIR', str(out / '.matplotlib'))
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit('CSV/report saved. Install requirements.txt in a virtual environment for --plot.') from exc
    plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
    for field, values, label, filename in [
        ('cache_size', [1024, 4096, 16384, 32768], 'Cache size (bytes)', 'cache_size'),
        ('block_size', [16, 32, 64, 128], 'Block size (bytes)', 'block_size'),
        ('associativity', [1, 2, 4, 8], 'Associativity (ways)', 'associativity')]:
        fig, axes = plt.subplots(1, 2, figsize=(11, 4), layout='constrained')
        for workload in ['matmul_naive', 'matmul_blocked', 'array', 'linked_list']:
            data = [select(rows, workload, **{field: v}) for v in values]
            axes[0].plot(range(len(values)), [100*float(r['miss_ratio']) for r in data], marker='o', label=workload)
            axes[1].plot(range(len(values)), [float(r['amat']) for r in data], marker='o', label=workload)
        for ax, ylabel in zip(axes, ['Miss rate (%)', 'Simulated AMAT (cycles)']):
            ax.set(xticks=range(len(values)), xticklabels=values, xlabel=label, ylabel=ylabel)
            ax.grid(alpha=.2)
        axes[1].legend(fontsize=8)
        fig.suptitle('Source-instrumented workloads · LRU · WB/WA · one-factor sweep')
        for extension in ['png', 'svg']:
            fig.savefig(out/f'{filename}.{extension}', dpi=160)
        plt.close(fig)


if __name__ == '__main__':
    main()

"""End-to-end checks for classification, CSV output, and invalid inputs."""
import csv
import pathlib
import subprocess
import tempfile


def run(*args, success=True):
    result = subprocess.run(['./cache_sim', *map(str, args)], capture_output=True, text=True)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    if not success:
        assert 'Error:' in result.stderr, result.stderr
    return result


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    trace = root / 'trace,"sample.txt'
    output = root / 'results.csv'

    def simulate(addresses, size=64, assoc=1, policy='LRU', compare=False):
        trace.write_text('\n'.join(map(str, addresses)) + '\n')
        args = ['--trace', trace, '--size', size, '--block', 16,
                '--assoc', assoc, '--policy', policy, '--csv', output]
        if compare:
            args.append('--compare')
        run(*args)
        with output.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        for row in rows:
            assert row['trace'] == str(trace)
            assert int(row['misses']) == sum(int(row[k]) for k in ['compulsory', 'capacity', 'conflict'])
            assert int(row['accesses']) == int(row['hits']) + int(row['misses'])
        return rows

    row = simulate([0, 64, 0, 64])[0]
    assert [int(row[k]) for k in ['compulsory', 'capacity', 'conflict']] == [2, 0, 2]
    row = simulate([0, 16, 32, 0], size=32, assoc=0)[0]
    assert [int(row[k]) for k in ['compulsory', 'capacity', 'conflict']] == [3, 1, 0]
    row = simulate([0, 16, 0, 32, 0], size=32, assoc=0, policy='FIFO')[0]
    assert [int(row[k]) for k in ['compulsory', 'capacity', 'conflict']] == [3, 0, 1]
    row = simulate([0, 4])[0]
    assert float(row['hit_ratio']) == 0.5 and float(row['amat']) == 51
    assert len(simulate([0, 16, 0, 32], compare=True)) == 8
    print('PASS miss classification and CSV metrics, escaping, and comparison rows')

    trace.write_text('# comment\nR 0x10 # hex\nW 0016\n')
    run('--trace', trace, '--csv', output)
    with output.open() as stream:
        assert next(csv.DictReader(stream))['hits'] == '1'

    for option, value in [('--size', '-1'), ('--size', '64junk'), ('--size', '18446744073709551616'),
                          ('--size', '0'), ('--block', '3'), ('--assoc', '3'),
                          ('--policy', 'typo'), ('--hit-time', '-1'), ('--hit-time', 'nan'),
                          ('--miss-penalty', 'inf'), ('--miss-penalty', '5junk'),
                          ('--write-policy', 'bad'), ('--allocation', 'bad'),
                          ('--l2-size', '0'), ('--l2-size', '3'), ('--l2-assoc', '2')]:
        run('--trace', trace, option, value, success=False)
    for contents in ['garbage\n', 'R\n', 'R 0x10 extra\n', '-1\n', '0x\n', '18446744073709551616\n']:
        trace.write_text(contents)
        result = run('--trace', trace, success=False)
        assert f'{trace}:1:' in result.stderr
    trace.write_text('# empty\n')
    run('--trace', trace, success=False)
    trace.write_text('0\n')
    alias = root / 'alias.txt'
    alias.symlink_to(trace)
    run('--trace', trace, '--csv', alias, success=False)
    assert trace.read_text() == '0\n'
    run('--trace', trace, '--csv', root / 'missing' / 'results.csv', success=False)
    run('--trace', root / 'missing.txt', success=False)
    run('--trace', trace, '--size', success=False)
    run('--unknown', success=False)
    run(success=False)
    run('--help')
    print('PASS strict inputs, trace line diagnostics, and output errors')

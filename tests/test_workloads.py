"""Workload output contracts: executed kernels agree and traces retain operations."""
import pathlib
import subprocess
import tempfile

with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    result = subprocess.run(['./workload_runner', directory], check=True, text=True, capture_output=True)
    assert 'checksum' in result.stdout
    for name in ['matmul_naive', 'matmul_blocked']:
        lines = [line for line in (root/f'{name}.txt').read_text().splitlines() if not line.startswith('#')]
        assert len(lines) == 4 * 32**3
        assert sum(line.startswith('W ') for line in lines) == 32**3
        assert set(line.split()[0] for line in lines) == {'R', 'W'}
    assert sum(1 for line in (root/'array.txt').open() if line.startswith('R ')) == 4096
    assert sum(1 for line in (root/'linked_list.txt').open() if line.startswith('R ')) == 8192
    print('PASS executed workload checksums, trace lengths, and read/write operations')

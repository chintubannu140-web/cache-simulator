# Five-minute interview demo

## 1. Explain the problem

“I built a trace-driven C++ cache simulator to investigate how access order, cache organization, and write policies affect miss rates, memory traffic, and simulated latency.”

## 2. Build and verify

```bash
make test
make traces
./cache_sim --trace traces/conflict.txt --compare
```

Explain why four competing blocks cannot coexist in one direct-mapped location, while a four-way set can hold them. Replacement does not affect a direct-mapped cache because there is only one possible victim.

## 3. Run executed workloads

```bash
make experiments
cat artifacts/report.md
```

The matrix kernels perform the same updates in different traversal orders and check their results. At the documented baseline, blocked multiplication has a much lower miss rate. Show the size sweep: blocking's advantage is workload/configuration dependent and disappears in the larger-cache cases here.

Be precise: these are source-instrumented logical data accesses, not complete machine traces. The report is simulated cache behavior, not a measured application speedup.

## 4. Show write traffic and L2

```bash
./cache_sim --trace artifacts/traces/matmul_blocked.txt \
  --size 4096 --block 64 --assoc 4 --write-policy WT --allocation WA
./cache_sim --trace artifacts/traces/matmul_blocked.txt \
  --size 4096 --block 64 --assoc 4 --write-policy WB --allocation WA
./cache_sim --trace artifacts/traces/matmul_naive.txt \
  --size 4096 --block 64 --assoc 4 --l2-size 32768 --l2-assoc 8
```

Explain dirty bits, writeback on eviction, and why pending dirty state is reported without a final flush. Both matrix variants read C before writing it, so WA versus NWA need not change this workload's results; use the small write-miss tests to demonstrate that distinction.

## 5. Discuss engineering choices

- Cache sets are vectors of lines; access and victim selection cost O(associativity).
- The miss classifier uses an allocating fully associative LRU reference cache.
- The L2 is non-inclusive/non-exclusive; no L1 invalidation is required on L2 eviction.
- Timing is blocking and serial; there are no hidden write buffers or overlapping misses.
- A simpler Python model checks randomized traces against the C++ implementation.
- CSV sweeps and optional plots make experiments reproducible.

Prepare to walk through a tiny trace by hand and explain a test before presenting a feature you have not yet studied. Describe AI assistance honestly and distinguish your own decisions and understanding.

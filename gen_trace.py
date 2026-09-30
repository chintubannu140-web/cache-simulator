"""Generates sample memory-access traces into traces/ (one hex address per line)."""
import random
random.seed(1)

def write(name, addrs):
    with open(f"traces/{name}.txt", "w") as f:
        f.write(f"# {name}: {len(addrs)} accesses\n")
        for a in addrs:
            f.write(f"{a:#x}\n")

# 1. Sequential: walks through an array (good spatial locality)
write("sequential", [0x1000 + 4 * i for i in range(4096)])

# 2. Loop: re-reads a small working set many times (good temporal locality)
write("loop", [0x2000 + 4 * (i % 128) for i in range(8192)])

# 3. Random: no locality
write("random", [random.randrange(0, 1 << 16) & ~3 for _ in range(8192)])

# 4. Conflict: addresses 1 KB apart map to the same set in a 1 KB direct-mapped cache
write("conflict", [0x4000 + 1024 * (i % 4) + 4 * (i // 4 % 2) for i in range(8192)])

# 5. Hot + scan: a few "hot" blocks reused often, mixed with a streaming scan.
#    LRU keeps the hot blocks; FIFO evicts them once they get old.
hot = [0x8000 + 16 * k for k in range(3)]
addrs = []
for i in range(6000):
    addrs.append(hot[i % 3])
    addrs.append(0x10000 + 16 * i)   # scan: never reused
write("hot_scan", addrs)

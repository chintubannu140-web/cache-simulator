#!/bin/bash
# Tiny hand-checked tests. Cache: 64 B, block 16 B => 4 blocks.
fail=0
check() {  # name expected output
  if echo "$3" | grep -q "$2" ; then echo "PASS $1"; else echo "FAIL $1 (wanted: $2)"; echo "$3"; fail=1; fi
}
t=$(mktemp)

# Test 1: same block twice -> 1 compulsory miss then 1 hit => 50% hit
printf "0x0\n0x4\n" > $t
out=$(./cache_sim --trace $t --size 64 --block 16 --assoc 1)
check "spatial locality" "50.00%" "$out"

# Test 2: direct mapped conflict. 0x00 and 0x40 map to the same set => all misses
printf "0x00\n0x40\n0x00\n0x40\n" > $t
out=$(./cache_sim --trace $t --size 64 --block 16 --assoc 1)
check "direct mapped conflict" "0.00%" "$out"

# Test 3: same trace, 2-way set associative => both blocks coexist => 2 hits of 4 = 50%
out=$(./cache_sim --trace $t --size 64 --block 16 --assoc 2)
check "2-way removes conflict" "50.00%" "$out"

# Test 4: LRU vs FIFO. Fully associative, 2 blocks (size 32). Pattern A B A C A:
#   LRU:  A miss, B miss, A hit, C miss (evicts B), A hit  => 2 hits = 40%
#   FIFO: A miss, B miss, A hit, C miss (evicts A), A miss => 1 hit  = 20%
printf "0x00\n0x10\n0x00\n0x20\n0x00\n" > $t
lru=$(./cache_sim --trace $t --size 32 --block 16 --assoc 0 --policy LRU)
fifo=$(./cache_sim --trace $t --size 32 --block 16 --assoc 0 --policy FIFO)
check "LRU keeps recently used block (40%)" "40.00%" "$lru"
check "FIFO evicts oldest block (20%)" "20.00%" "$fifo"

# Test 5: AMAT = hit_time + miss_ratio * penalty = 1 + 0.5 * 100 = 51.00
printf "0x0\n0x4\n" > $t
out=$(./cache_sim --trace $t --size 64 --block 16 --assoc 1 --hit-time 1 --miss-penalty 100)
check "AMAT formula" "51.00" "$out"

rm -f $t
exit $fail

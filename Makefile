CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

cache_sim: cache_sim.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

traces:
	python3 gen_trace.py

test: cache_sim
	bash tests/run_tests.sh

clean:
	rm -f cache_sim

.PHONY: traces test clean

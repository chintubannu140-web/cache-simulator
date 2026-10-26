CXX = g++
PYTHON ?= python3
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

cache_sim: cache_sim.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

traces:
	mkdir -p traces
	$(PYTHON) gen_trace.py

test: cache_sim workload_runner
	bash tests/run_tests.sh
	$(PYTHON) tests/test_features.py
	$(PYTHON) tests/test_model.py
	$(PYTHON) tests/test_workloads.py

workload_runner: workloads/trace_workloads.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

experiments: cache_sim workload_runner
	$(PYTHON) scripts/experiments.py

plots: cache_sim workload_runner
	$(PYTHON) scripts/experiments.py --plot

clean:
	rm -f cache_sim workload_runner

.PHONY: traces test clean experiments plots

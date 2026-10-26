// Source-instrumented workloads: actual computations, normalized logical addresses.
// These are explicit data accesses, not a complete machine-instruction trace.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;
constexpr uint64_t A = 0x100000, B = 0x200000, C = 0x300000;
struct Trace {
    ofstream file;
    explicit Trace(const string& path) : file(path) {
        if (!file) throw runtime_error("cannot write " + path);
        file << "# source-instrumented logical data accesses; one address per operation\n";
    }
    void read(uint64_t address) { file << "R " << address << '\n'; }
    void write(uint64_t address) { file << "W " << address << '\n'; }
    void finish() { file.close(); if (!file) throw runtime_error("trace write failed"); }
};
static double multiply(const string& path, int n, int tile) {
    vector<double> a(n*n), b(n*n), c(n*n, 0);
    for (int i = 0; i < n*n; ++i) { a[i] = i % 7; b[i] = i % 11; }
    Trace trace(path);
    // Both variants use the same per-update accesses; only loop traversal changes.
    for (int ii = 0; ii < n; ii += tile)
        for (int jj = 0; jj < n; jj += tile)
            for (int kk = 0; kk < n; kk += tile)
                for (int i = ii; i < min(ii+tile, n); ++i)
                    for (int j = jj; j < min(jj+tile, n); ++j)
                        for (int k = kk; k < min(kk+tile, n); ++k) {
                            int ai = i*n+k, bi = k*n+j, ci = i*n+j;
                            trace.read(A + 8*ai); trace.read(B + 8*bi);
                            trace.read(C + 8*ci);
                            c[ci] += a[ai] * b[bi];
                            trace.write(C + 8*ci);
                        }
    trace.finish();
    return accumulate(c.begin(), c.end(), 0.0);
}
struct alignas(64) Node {
    uint64_t value = 0;
    Node* next = nullptr;
};
static_assert(sizeof(Node) == 64 && offsetof(Node, next) == 8,
              "The demonstration assumes 64-byte nodes with an 8-byte next-field offset");

static uint64_t traverse(const string& path, int count, bool linked) {
    vector<uint64_t> values(count);
    iota(values.begin(), values.end(), 1);
    vector<Node> nodes(count);
    vector<int> order(count);
    iota(order.begin(), order.end(), 0);
    if (linked) { mt19937 rng(42); shuffle(order.begin(), order.end(), rng); }
    for (int i = 0; i < count; ++i) nodes[i].value = values[i];
    for (int i = 0; i+1 < count; ++i) nodes[order[i]].next = &nodes[order[i+1]];
    Trace trace(path);
    uint64_t checksum = 0;
    for (int repeat = 0; repeat < 4; ++repeat) {
        if (linked) {
            for (Node* node = &nodes[order[0]]; node; node = node->next) {
                uint64_t address = A + (node - nodes.data()) * sizeof(Node);
                trace.read(address);
                checksum += node->value;
                trace.read(address + offsetof(Node, next));
            }
        } else {
            for (int i = 0; i < count; ++i) {
                trace.read(A + sizeof(uint64_t)*i);
                checksum += values[i];
            }
        }
    }
    trace.finish();
    return checksum;
}
int main(int argc, char** argv) try {
    string out = argc > 1 ? argv[1] : "artifacts/traces";
    filesystem::create_directories(out);
    constexpr int n = 32, tile = 8;
    double naive = multiply(out + "/matmul_naive.txt", n, n);
    double blocked = multiply(out + "/matmul_blocked.txt", n, tile);
    uint64_t array = traverse(out + "/array.txt", 1024, false);
    uint64_t linked = traverse(out + "/linked_list.txt", 1024, true);
    if (naive != blocked || array != linked) throw runtime_error("workload checksum mismatch");
    ofstream metadata(out + "/metadata.txt");
    metadata << "Source-instrumented logical traces; initialization and tracer accesses excluded.\n"
             << "matrices: n=" << n << ", tile=" << tile << ", element bytes=8, checksum=" << naive << '\n'
             << "traversal: 1024 values, 4 passes, array stride=8, linked-node spacing=64, checksum=" << array << '\n';
    metadata.close();
    if (!metadata) throw runtime_error("metadata write failed");
    cout << "Wrote four source-instrumented traces to " << out << "\nMatrix checksum: " << naive
         << "; traversal checksum: " << array << '\n';
    return 0;
} catch (const exception& e) { cerr << e.what() << '\n'; return 1; }

// Configurable cache simulator: Direct Mapped / Set Associative / Fully Associative,
// FIFO and LRU replacement. Reports hit ratio, miss ratio and AMAT.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
using namespace std;

enum class Policy { FIFO, LRU };

struct Line {
    bool valid = false;
    uint64_t tag = 0;
    uint64_t lastUsed = 0;   // updated on every access (used by LRU)
    uint64_t insertedAt = 0; // set when block is loaded (used by FIFO)
};

struct Stats {
    uint64_t accesses = 0, hits = 0, misses = 0, compulsory = 0;
};

class Cache {
public:
    Cache(uint64_t cacheSize, uint64_t blockSize, uint64_t assoc, Policy p)
        : blockSize_(blockSize), policy_(p) {
        uint64_t numBlocks = cacheSize / blockSize;
        if (assoc == 0 || assoc > numBlocks) assoc = numBlocks;  // 0 = fully associative
        assoc_ = assoc;
        numSets_ = numBlocks / assoc;
        sets_.assign(numSets_, vector<Line>(assoc_));
        offsetBits_ = log2u(blockSize_);
        indexBits_ = log2u(numSets_);
    }

    // returns true on hit
    bool access(uint64_t addr) {
        stats.accesses++;
        clock_++;
        uint64_t blockAddr = addr >> offsetBits_;
        uint64_t index = blockAddr & (numSets_ - 1);   // set number
        uint64_t tag = blockAddr >> indexBits_;
        vector<Line>& set = sets_[index];

        for (Line& l : set) {
            if (l.valid && l.tag == tag) {              // HIT
                stats.hits++;
                if (policy_ == Policy::LRU) l.lastUsed = clock_;
                return true;
            }
        }

        stats.misses++;                                 // MISS
        if (seen_.insert(blockAddr).second) stats.compulsory++;

        Line* victim = nullptr;
        for (Line& l : set) if (!l.valid) { victim = &l; break; }   // free slot first
        if (!victim) {
            victim = &set[0];
            for (Line& l : set) {
                uint64_t a = (policy_ == Policy::LRU) ? l.lastUsed : l.insertedAt;
                uint64_t b = (policy_ == Policy::LRU) ? victim->lastUsed : victim->insertedAt;
                if (a < b) victim = &l;
            }
        }
        victim->valid = true;
        victim->tag = tag;
        victim->lastUsed = clock_;
        victim->insertedAt = clock_;
        return false;
    }

    Stats stats;
    uint64_t numSets() const { return numSets_; }
    uint64_t assoc() const { return assoc_; }

private:
    static uint64_t log2u(uint64_t x) { uint64_t n = 0; while (x > 1) { x >>= 1; n++; } return n; }
    uint64_t blockSize_, assoc_ = 1, numSets_ = 1, offsetBits_ = 0, indexBits_ = 0, clock_ = 0;
    Policy policy_;
    vector<vector<Line>> sets_;
    unordered_set<uint64_t> seen_;
};

static bool isPow2(uint64_t x) { return x && !(x & (x - 1)); }

// Trace format: one address per line (hex like 0x1A2B or decimal). Optional leading R/W. '#' = comment.
static bool loadTrace(const string& path, vector<uint64_t>& out) {
    ifstream f(path);
    if (!f) return false;
    string line;
    while (getline(f, line)) {
        size_t c = line.find('#');
        if (c != string::npos) line = line.substr(0, c);
        istringstream ss(line);
        string tok, last;
        while (ss >> tok) last = tok;                   // address is the last token
        if (last.empty()) continue;
        out.push_back(strtoull(last.c_str(), nullptr, 0));
    }
    return true;
}

static void printRow(const string& name, const string& pol, const Stats& s, double hitT, double missP) {
    double hr = s.accesses ? (double)s.hits / s.accesses : 0;
    double mr = 1.0 - hr;
    double amat = hitT + mr * missP;
    cout << left << setw(22) << name << setw(6) << pol << right << fixed << setprecision(2)
         << setw(10) << hr * 100 << "%" << setw(10) << mr * 100 << "%"
         << setw(12) << s.compulsory << setw(10) << amat << "\n";
}

static void usage() {
    cout << "Usage: cache_sim --trace FILE [--size BYTES] [--block BYTES] [--assoc N] [--policy LRU|FIFO]\n"
            "                 [--hit-time CYC] [--miss-penalty CYC] [--compare]\n"
            "  --assoc 1 = direct mapped, N = N-way set associative, 0 = fully associative\n"
            "  --compare runs all organizations and both policies in one table\n"
            "Defaults: size 1024, block 16, assoc 1, policy LRU, hit-time 1, miss-penalty 100\n";
}

int main(int argc, char** argv) {
    uint64_t size = 1024, block = 16, assoc = 1;
    Policy policy = Policy::LRU;
    double hitT = 1, missP = 100;
    string trace;
    bool compare = false;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto next = [&]() -> string { if (i + 1 >= argc) { usage(); exit(1); } return argv[++i]; };
        if (a == "--trace") trace = next();
        else if (a == "--size") size = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--block") block = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--assoc") assoc = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--policy") { string p = next(); policy = (p == "FIFO" || p == "fifo") ? Policy::FIFO : Policy::LRU; }
        else if (a == "--hit-time") hitT = atof(next().c_str());
        else if (a == "--miss-penalty") missP = atof(next().c_str());
        else if (a == "--compare") compare = true;
        else { usage(); return 1; }
    }
    if (trace.empty()) { usage(); return 1; }
    if (!isPow2(size) || !isPow2(block) || block > size) {
        cerr << "Error: size and block must be powers of two, and block <= size.\n"; return 1;
    }
    uint64_t numBlocks = size / block;
    if (assoc != 0 && (!isPow2(assoc) || assoc > numBlocks)) {
        cerr << "Error: assoc must be 0 (fully) or a power of two <= " << numBlocks << ".\n"; return 1;
    }
    vector<uint64_t> addrs;
    if (!loadTrace(trace, addrs) || addrs.empty()) { cerr << "Error: cannot read trace '" << trace << "'.\n"; return 1; }

    cout << "Trace: " << trace << " (" << addrs.size() << " accesses)  cache " << size << " B, block "
         << block << " B, hit time " << hitT << ", miss penalty " << missP << "\n\n";
    cout << left << setw(22) << "Organization" << setw(6) << "Policy" << right << setw(11) << "Hit ratio"
         << setw(11) << "Miss ratio" << setw(12) << "Compulsory" << setw(10) << "AMAT" << "\n"
         << string(72, '-') << "\n";

    auto run = [&](uint64_t as, Policy p) {
        Cache c(size, block, as, p);
        for (uint64_t a : addrs) c.access(a);
        string name = (c.assoc() == 1) ? "Direct Mapped"
                    : (c.numSets() == 1) ? "Fully Associative"
                    : to_string(c.assoc()) + "-way Set Assoc";
        printRow(name, p == Policy::LRU ? "LRU" : "FIFO", c.stats, hitT, missP);
    };

    if (compare) {
        run(1, Policy::FIFO); run(1, Policy::LRU);      // policy does not matter for direct mapped
        for (uint64_t as : {2ull, 4ull})
            if (as <= numBlocks) { run(as, Policy::FIFO); run(as, Policy::LRU); }
        run(0, Policy::FIFO); run(0, Policy::LRU);
    } else {
        run(assoc, policy);
    }
    return 0;
}

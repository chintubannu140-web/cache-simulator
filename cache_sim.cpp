// Blocking trace-driven cache model. See README.md for timing and write assumptions.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace std;

enum class Policy { FIFO, LRU };
struct Access { uint64_t address; bool write; };
struct Config {
    uint64_t size = 1024, block = 16, assoc = 1;
    Policy policy = Policy::LRU;
    double hitTime = 1;
    bool writeBack = true, writeAllocate = true;
};
struct Line {
    bool valid = false, dirty = false;
    uint64_t block = 0, lastUsed = 0, insertedAt = 0;
};
struct Stats {
    uint64_t accesses = 0, hits = 0, misses = 0, compulsory = 0, capacity = 0, conflict = 0;
    uint64_t reads = 0, writes = 0, writebacks = 0;
};
// A transfer is a full block only for a dirty eviction. These transfers need no read-for-fill.
using LowerAccess = function<double(uint64_t, bool, bool)>;

class Cache {
public:
    Cache(Config config, LowerAccess lower) : config_(config), lower_(move(lower)) {
        capacity_ = config_.size / config_.block;
        ways_ = config_.assoc ? config_.assoc : capacity_;
        sets_.assign(capacity_ / ways_, vector<Line>(ways_));
    }
    double access(uint64_t address, bool write, bool fullBlock = false) {
        ++stats.accesses;
        write ? ++stats.writes : ++stats.reads;
        ++clock_;
        uint64_t block = address / config_.block;
        auto& set = sets_[block % sets_.size()];
        bool first = seen_.insert(block).second;
        auto previous = referenceIndex_.find(block);
        bool referenceHit = previous != referenceIndex_.end();
        if (referenceHit) reference_.erase(previous->second);
        reference_.push_front(block);
        referenceIndex_[block] = reference_.begin();
        if (reference_.size() > capacity_) {
            referenceIndex_.erase(reference_.back());
            reference_.pop_back();
        }
        for (auto& line : set) {
            if (line.valid && line.block == block) {
                ++stats.hits;
                line.lastUsed = clock_;
                if (!write) return config_.hitTime;
                if (config_.writeBack) { line.dirty = true; return config_.hitTime; }
                return config_.hitTime + lower_(address, true, fullBlock);
            }
        }
        ++stats.misses;
        if (first) ++stats.compulsory;
        else if (!referenceHit) ++stats.capacity;
        else ++stats.conflict;
        if (write && !config_.writeAllocate)
            return config_.hitTime + lower_(address, true, fullBlock);

        Line* victim = nullptr;
        for (auto& line : set) if (!line.valid) { victim = &line; break; }
        if (!victim) {
            victim = &set.front();
            for (auto& line : set) {
                auto age = [&](const Line& l) {
                    return config_.policy == Policy::LRU ? l.lastUsed : l.insertedAt;
                };
                if (age(line) < age(*victim)) victim = &line;
            }
        }
        double cycles = config_.hitTime;
        if (victim->valid && victim->dirty) {
            ++stats.writebacks;
            cycles += lower_(victim->block * config_.block, true, true);
        }
        if (!fullBlock) cycles += lower_(block * config_.block, false, false);
        *victim = {true, write && config_.writeBack, block, clock_, clock_};
        if (write && !config_.writeBack) cycles += lower_(address, true, fullBlock);
        return cycles;
    }
    uint64_t dirtyLines() const {
        uint64_t count = 0;
        for (const auto& set : sets_) for (const auto& line : set) count += line.valid && line.dirty;
        return count;
    }
    uint64_t assoc() const { return ways_; }
    uint64_t numSets() const { return sets_.size(); }
    Stats stats;
private:
    Config config_;
    LowerAccess lower_;
    uint64_t capacity_ = 0, ways_ = 0, clock_ = 0;
    vector<vector<Line>> sets_;
    unordered_set<uint64_t> seen_;
    list<uint64_t> reference_;
    unordered_map<uint64_t, list<uint64_t>::iterator> referenceIndex_;
};
static bool isPow2(uint64_t x) { return x && !(x & (x - 1)); }

// Parse the complete token: decimal or explicitly prefixed hexadecimal.
static uint64_t parseUnsigned(const string& token) {
    if (token.empty() || token[0] == '-' || token[0] == '+')
        throw invalid_argument("expected an unsigned integer: " + token);
    bool hex = token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
    string digits = hex ? token.substr(2) : token;
    if (digits.empty() || digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != string::npos)
        throw invalid_argument("invalid integer: " + token);
    try {
        return stoull(token, nullptr, hex ? 16 : 10);
    } catch (const out_of_range&) {
        throw out_of_range("integer exceeds 64-bit unsigned range: " + token);
    }
}

static double parseTiming(const string& token) {
    size_t end = 0;
    double value;
    try {
        value = stod(token, &end);
    } catch (const exception&) {
        throw invalid_argument("invalid timing value: " + token);
    }
    if (end != token.size() || !isfinite(value) || value < 0)
        throw invalid_argument("timing must be finite and nonnegative: " + token);
    return value;
}

static void loadTrace(const string& path, vector<Access>& out) {
    ifstream f(path);
    if (!f) throw runtime_error("cannot read trace '" + path + "'");
    string line;
    uint64_t lineNumber = 0;
    while (getline(f, line)) {
        ++lineNumber;
        line = line.substr(0, line.find('#'));
        istringstream ss(line);
        string address, extra;
        if (!(ss >> address)) continue;
        try {
            bool write = address == "W";
            if (address == "R" || address == "W") {
                if (!(ss >> address)) throw invalid_argument("missing address");
            }
            if (ss >> extra) throw invalid_argument("unexpected extra token");
            out.push_back({parseUnsigned(address), write});
        } catch (const exception& e) {
            throw runtime_error(path + ":" + to_string(lineNumber) + ": " + e.what());
        }
    }
    if (f.bad()) throw runtime_error("failed reading trace '" + path + "'");
    if (out.empty()) throw runtime_error("trace contains no accesses: " + path);
}

static string csvQuote(const string& value) {
    string result = "\"";
    for (char c : value) { if (c == '"') result += '"'; result += c; }
    return result + "\"";
}

static Policy parsePolicy(const string& value) {
    if (value == "LRU" || value == "lru") return Policy::LRU;
    if (value == "FIFO" || value == "fifo") return Policy::FIFO;
    throw invalid_argument("unknown policy: " + value);
}
static void validate(const Config& c, const string& level) {
    if (!isPow2(c.size) || !isPow2(c.block) || c.block > c.size)
        throw invalid_argument(level + ": size and block must be powers of two, block <= size");
    if (c.assoc && (!isPow2(c.assoc) || c.assoc > c.size / c.block))
        throw invalid_argument(level + ": associativity must be 0 or a power of two <= line count");
}
static double fraction(uint64_t part, uint64_t total) { return total ? double(part) / total : 0; }
static void usage() {
    cout << "Usage: cache_sim --trace FILE [options]\n"
            "  --size BYTES --block BYTES --assoc N --policy LRU|FIFO\n"
            "  --write-policy WB|WT --allocation WA|NWA\n"
            "  --hit-time CYC --miss-penalty CYC --compare --csv FILE\n"
            "  --l2-size BYTES [--l2-assoc N --l2-policy LRU|FIFO --l2-hit-time CYC]\n"
            "Defaults: L1 1024 B, block 16 B, assoc 1, LRU, WB, WA, hit 1, memory 100.\n"
            "L2 is optional; defaults: assoc 4, LRU, hit 10; shares block/write policies.\n"
            "Assoc 0 = fully associative. --compare varies L1 organization/replacement.\n";
}

int main(int argc, char** argv) try {
    Config config, l2config;
    l2config.assoc = 4;
    l2config.hitTime = 10;
    uint64_t l2size = 0;
    double memoryTime = 100;
    string trace, csvPath;
    bool compare = false, l2Options = false;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        auto next = [&]() -> string {
            if (i + 1 >= argc) throw invalid_argument("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--trace") trace = next();
        else if (arg == "--size") config.size = parseUnsigned(next());
        else if (arg == "--block") config.block = parseUnsigned(next());
        else if (arg == "--assoc") config.assoc = parseUnsigned(next());
        else if (arg == "--policy") config.policy = parsePolicy(next());
        else if (arg == "--hit-time") config.hitTime = parseTiming(next());
        else if (arg == "--miss-penalty") memoryTime = parseTiming(next());
        else if (arg == "--write-policy") {
            string value = next();
            if (value != "WB" && value != "WT") throw invalid_argument("write policy must be WB or WT");
            config.writeBack = value == "WB";
        } else if (arg == "--allocation") {
            string value = next();
            if (value != "WA" && value != "NWA") throw invalid_argument("allocation must be WA or NWA");
            config.writeAllocate = value == "WA";
        } else if (arg == "--l2-size") {
            l2size = parseUnsigned(next());
            if (!l2size) throw invalid_argument("L2 size must be positive");
        } else if (arg == "--l2-assoc") { l2config.assoc = parseUnsigned(next()); l2Options = true; }
        else if (arg == "--l2-policy") { l2config.policy = parsePolicy(next()); l2Options = true; }
        else if (arg == "--l2-hit-time") { l2config.hitTime = parseTiming(next()); l2Options = true; }
        else if (arg == "--csv") { csvPath = next(); if (csvPath.empty()) throw invalid_argument("empty CSV path"); }
        else if (arg == "--compare") compare = true;
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else throw invalid_argument("unknown option: " + arg);
    }
    if (trace.empty()) throw invalid_argument("--trace FILE is required");
    validate(config, "L1");
    if (l2Options && !l2size) throw invalid_argument("L2 options require --l2-size");
    l2config.size = l2size;
    l2config.block = config.block;
    l2config.writeBack = config.writeBack;
    l2config.writeAllocate = config.writeAllocate;
    if (l2size) validate(l2config, "L2");
    vector<Access> accesses;
    loadTrace(trace, accesses);
    ofstream csv;
    if (!csvPath.empty()) {
        error_code ec;
        if (filesystem::equivalent(trace, csvPath, ec)) throw invalid_argument("CSV output must differ from trace path");
        csv.open(csvPath);
        if (!csv) throw runtime_error("cannot write CSV '" + csvPath + "'");
        csv << "trace,cache_size,block_size,associativity,policy,accesses,hits,misses,compulsory,capacity,conflict,hit_ratio,miss_ratio,hit_time,miss_penalty,amat,write_policy,allocation,reads,writes,writebacks,dirty_lines,memory_reads,memory_writes,total_cycles,l2_size,l2_associativity,l2_policy,l2_hit_time,l2_accesses,l2_hits,l2_misses,l2_local_miss_ratio,l2_writebacks,l2_dirty_lines,global_fill_miss_ratio\n";
        csv << setprecision(17);
    }
    cout << "Trace: " << trace << " (" << accesses.size() << " accesses), "
         << (config.writeBack ? "WB" : "WT") << '/' << (config.writeAllocate ? "WA" : "NWA") << '\n';
    cout << left << setw(22) << "Organization" << setw(6) << "Policy" << right << setw(11) << "Hit ratio"
         << setw(11) << "Miss ratio" << setw(12) << "Compulsory" << setw(12) << "Capacity"
         << setw(12) << "Conflict" << setw(12) << "AMAT" << '\n';

    auto run = [&](uint64_t assoc, Policy policy) {
        uint64_t memoryReads = 0, memoryWrites = 0;
        LowerAccess memory = [&](uint64_t, bool write, bool) {
            write ? ++memoryWrites : ++memoryReads;
            return memoryTime;
        };
        unique_ptr<Cache> l2;
        LowerAccess lower = memory;
        if (l2size) {
            l2 = make_unique<Cache>(l2config, memory);
            lower = [&](uint64_t address, bool write, bool fullBlock) { return l2->access(address, write, fullBlock); };
        }
        Config current = config;
        current.assoc = assoc;
        current.policy = policy;
        Cache l1(current, lower);
        double cycles = 0;
        for (auto a : accesses) cycles += l1.access(a.address, a.write);
        if (!isfinite(cycles)) throw overflow_error("total simulated cycles overflowed");
        const auto& s = l1.stats;
        Stats second = l2 ? l2->stats : Stats{};
        double hr = fraction(s.hits, s.accesses), mr = fraction(s.misses, s.accesses);
        double amat = cycles / s.accesses;
        string name = l1.assoc() == 1 ? "Direct Mapped" : l1.numSets() == 1 ? "Fully Associative" : to_string(l1.assoc()) + "-way Set Assoc";
        string pol = policy == Policy::LRU ? "LRU" : "FIFO";
        cout << left << setw(22) << name << setw(6) << pol << right << fixed << setprecision(2)
             << setw(10) << hr * 100 << '%' << setw(10) << mr * 100 << '%'
             << setw(12) << s.compulsory << setw(12) << s.capacity << setw(12) << s.conflict << setw(12) << amat << '\n';
        cout << "  Memory reads/writes: " << memoryReads << '/' << memoryWrites
             << "; L1 writebacks: " << s.writebacks << "; pending dirty: " << l1.dirtyLines() << '\n';
        if (l2) cout << "  L2 accesses/hits/misses: " << second.accesses << '/' << second.hits << '/' << second.misses
                     << "; local miss ratio: " << fraction(second.misses, second.accesses) * 100 << "%"
                     << "; L2 writebacks: " << second.writebacks << "; pending dirty: " << l2->dirtyLines() << '\n';
        if (csv.is_open()) {
            csv << csvQuote(trace) << ',' << current.size << ',' << current.block << ',' << l1.assoc() << ',' << pol << ','
                << s.accesses << ',' << s.hits << ',' << s.misses << ',' << s.compulsory << ',' << s.capacity << ',' << s.conflict << ','
                << hr << ',' << mr << ',' << current.hitTime << ',' << memoryTime << ',' << amat << ','
                << (current.writeBack ? "WB" : "WT") << ',' << (current.writeAllocate ? "WA" : "NWA") << ','
                << s.reads << ',' << s.writes << ',' << s.writebacks << ',' << l1.dirtyLines() << ',' << memoryReads << ',' << memoryWrites << ',' << cycles << ','
                << l2size << ',' << (l2 ? l2->assoc() : 0) << ',' << (l2config.policy == Policy::LRU ? "LRU" : "FIFO") << ','
                << (l2 ? l2config.hitTime : 0) << ',' << second.accesses << ',' << second.hits << ',' << second.misses << ','
                << fraction(second.misses, second.accesses) << ',' << second.writebacks << ',' << (l2 ? l2->dirtyLines() : 0) << ','
                << fraction(memoryReads, s.accesses) << '\n';
        }
    };
    if (compare) {
        run(1, Policy::FIFO); run(1, Policy::LRU);
        for (uint64_t ways : {2ull, 4ull}) if (ways <= config.size / config.block) {
            run(ways, Policy::FIFO); run(ways, Policy::LRU);
        }
        run(0, Policy::FIFO); run(0, Policy::LRU);
    } else run(config.assoc, config.policy);
    if (csv.is_open()) {
        csv.close();
        if (!csv) throw runtime_error("failed writing CSV '" + csvPath + "'");
    }
    return 0;
} catch (const exception& e) {
    cerr << "Error: " << e.what() << '\n';
    return 1;
}


#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

static constexpr size_t PAGE_BYTES = 4096;
static constexpr size_t UNIT_BYTES = 256 * 1024;
static constexpr size_t PAGES_PER_UNIT = UNIT_BYTES / PAGE_BYTES;

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static uint64_t recipe_word(uint64_t page_index, uint64_t word_index) {
    return splitmix64(page_index * 0xD6E8FEB86659FD93ull ^
                      word_index * 0xA0761D6478BD642Full ^
                      0x535441544552414Dull);
}

static unsigned char recipe_byte(uint64_t page_index, size_t byte_offset) {
    uint64_t w = recipe_word(page_index, byte_offset / 8);
    return (unsigned char)((w >> ((byte_offset % 8) * 8)) & 0xffu);
}

static void fill_recipe_page(unsigned char* p, uint64_t page_index) {
    auto* w = reinterpret_cast<uint64_t*>(p);
    for (size_t i = 0; i < PAGE_BYTES / 8; ++i) {
        w[i] = recipe_word(page_index, i);
    }
}

static double now_ms() {
    static LARGE_INTEGER freq = []{
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return 1000.0 * (double)q.QuadPart / (double)freq.QuadPart;
}

struct CpuSampler {
    ULARGE_INTEGER last_idle{}, last_kernel{}, last_user{};
    bool initialized = false;

    static ULARGE_INTEGER cvt(FILETIME f) {
        ULARGE_INTEGER x{};
        x.LowPart = f.dwLowDateTime;
        x.HighPart = f.dwHighDateTime;
        return x;
    }

    double sample() {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;
        auto i = cvt(idle), k = cvt(kernel), u = cvt(user);
        if (!initialized) {
            last_idle = i; last_kernel = k; last_user = u;
            initialized = true;
            return 0.0;
        }
        uint64_t di = i.QuadPart - last_idle.QuadPart;
        uint64_t dk = k.QuadPart - last_kernel.QuadPart;
        uint64_t du = u.QuadPart - last_user.QuadPart;
        last_idle = i; last_kernel = k; last_user = u;
        uint64_t total = dk + du;
        if (!total) return 0.0;
        double busy = 100.0 * (double)(total - di) / (double)total;
        return std::clamp(busy, 0.0, 100.0);
    }
};

static bool install_self_job_limit(size_t bytes, std::string& why) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        why = "CreateJobObject failed " + std::to_string(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    info.ProcessMemoryLimit = bytes;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        why = "SetInformationJobObject failed " + std::to_string(GetLastError());
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, GetCurrentProcess())) {
        why = "AssignProcessToJobObject failed " + std::to_string(GetLastError());
        CloseHandle(job);
        return false;
    }
    // Deliberately keep handle open for process lifetime.
    static HANDLE g_job = nullptr;
    g_job = job;
    why = "active";
    return true;
}

struct DeltaPage {
    std::array<unsigned char, PAGE_BYTES> bytes{};
};

struct UnitMeta {
    enum class State : uint8_t { Reserved, Materializing, Resident };
    State state = State::Reserved;
    uint64_t dirty_mask = 0;
    uint64_t last_use = 0;
    uint32_t pins = 0;
    uint64_t generation = 0;
};

struct HeapEntry {
    uint64_t last_use;
    uint64_t unit;
    uint64_t generation;
    bool operator>(const HeapEntry& o) const {
        if (last_use != o.last_use) return last_use > o.last_use;
        return unit > o.unit;
    }
};

class StateArena {
public:
    StateArena(size_t logical_bytes, size_t initial_budget)
        : logical_bytes_(logical_bytes),
          unit_count_((logical_bytes + UNIT_BYTES - 1) / UNIT_BYTES),
          meta_(unit_count_),
          budget_bytes_(initial_budget)
    {
        base_ = reinterpret_cast<unsigned char*>(
            VirtualAlloc(nullptr, logical_bytes_, MEM_RESERVE, PAGE_NOACCESS)
        );
        if (!base_) {
            throw std::runtime_error("VirtualAlloc(MEM_RESERVE) failed: " +
                                     std::to_string(GetLastError()));
        }
        worker_ = std::thread([this]{ worker_loop(); });
    }

    ~StateArena() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        if (base_) VirtualFree(base_, 0, MEM_RELEASE);
    }

    size_t logical_bytes() const { return logical_bytes_; }
    size_t unit_count() const { return unit_count_; }

    void set_budget(size_t bytes) {
        std::unique_lock<std::mutex> lk(mu_);
        budget_bytes_ = std::max(bytes, UNIT_BYTES * 2);
        evict_until_room_locked(0, UINT64_MAX);
    }

    size_t budget() const {
        std::lock_guard<std::mutex> lk(mu_);
        return budget_bytes_;
    }

    void set_prefetch_enabled(bool on) {
        prefetch_enabled_.store(on, std::memory_order_relaxed);
        if (!on) {
            std::lock_guard<std::mutex> lk(mu_);
            prefetch_q_.clear();
        }
    }

    void enqueue_prefetch(uint64_t unit) {
        if (!prefetch_enabled_.load(std::memory_order_relaxed)) return;
        if (unit >= unit_count_) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (prefetch_q_.size() < 256) {
            prefetch_q_.push_back(unit);
            cv_.notify_all();
        }
    }

    bool touch(uint64_t unit, bool do_write, uint8_t expected_xor, uint64_t& integrity_errors) {
        if (unit >= unit_count_) return false;
        if (!ensure_resident(unit, false)) return false;

        unsigned char* ptr = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto& m = meta_[unit];
            if (m.state != UnitMeta::State::Resident) return false;
            ++m.pins;
            m.last_use = ++clock_;
            ++m.generation;
            lru_.push({m.last_use, unit, m.generation});
            ptr = base_ + unit * UNIT_BYTES;
        }

        const uint64_t first_page = (unit * UNIT_BYTES) / PAGE_BYTES;

        // Strong cheap validation: one byte on every page.
        for (size_t p = 0; p < PAGES_PER_UNIT; ++p) {
            unsigned char expected = recipe_byte(first_page + p, 0);
            if (ptr[p * PAGE_BYTES] != expected) {
                ++integrity_errors;
            }
        }

        // Dirty canary lives at offset 13 of the first page.
        unsigned char base_canary = recipe_byte(first_page, 13);
        if (ptr[13] != (unsigned char)(base_canary ^ expected_xor)) {
            ++integrity_errors;
        }

        if (do_write) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                meta_[unit].dirty_mask |= 1ull; // first 4K page
            }
            ptr[13] ^= 0x5a;
        }

        volatile unsigned char sink = ptr[0];
        (void)sink;

        {
            std::lock_guard<std::mutex> lk(mu_);
            auto& m = meta_[unit];
            if (m.pins) --m.pins;
            cv_.notify_all();
        }
        return true;
    }

    uint64_t materializations() const { return materializations_.load(); }
    uint64_t evictions() const { return evictions_.load(); }
    uint64_t prefetches() const { return prefetches_.load(); }
    uint64_t prefetch_hits() const { return prefetch_hits_.load(); }
    uint64_t cpu_backoffs() const { return cpu_backoffs_.load(); }
    uint64_t commit_failures() const { return commit_failures_.load(); }
    size_t peak_committed() const { return peak_committed_.load(); }
    size_t current_committed() const { return committed_bytes_.load(); }
    size_t delta_pages() const {
        std::lock_guard<std::mutex> lk(mu_);
        return deltas_.size();
    }

private:
    bool ensure_resident(uint64_t unit, bool background) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            while (meta_[unit].state == UnitMeta::State::Materializing) {
                cv_.wait(lk);
            }
            if (meta_[unit].state == UnitMeta::State::Resident) {
                if (background) prefetch_hits_.fetch_add(1);
                meta_[unit].last_use = ++clock_;
                ++meta_[unit].generation;
                lru_.push({meta_[unit].last_use, unit, meta_[unit].generation});
                return true;
            }

            if (!evict_until_room_locked(UNIT_BYTES, unit)) {
                return false;
            }
            meta_[unit].state = UnitMeta::State::Materializing;
            pending_bytes_ += UNIT_BYTES;
        }

        unsigned char* addr = base_ + unit * UNIT_BYTES;
        void* p = VirtualAlloc(addr, UNIT_BYTES, MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            std::lock_guard<std::mutex> lk(mu_);
            meta_[unit].state = UnitMeta::State::Reserved;
            pending_bytes_ -= UNIT_BYTES;
            commit_failures_.fetch_add(1);
            cv_.notify_all();
            return false;
        }

        const uint64_t first_page = (unit * UNIT_BYTES) / PAGE_BYTES;
        for (size_t pg = 0; pg < PAGES_PER_UNIT; ++pg) {
            fill_recipe_page(addr + pg * PAGE_BYTES, first_page + pg);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            // Reapply saved dirty pages.
            uint64_t mask = meta_[unit].dirty_mask;
            while (mask) {
                unsigned bit = 0;
                unsigned long idx = 0;
#if defined(_MSC_VER)
                _BitScanForward64(&idx, mask);
                bit = (unsigned)idx;
#else
                bit = (unsigned)__builtin_ctzll(mask);
#endif
                auto it = deltas_.find(unit * PAGES_PER_UNIT + bit);
                if (it != deltas_.end()) {
                    std::memcpy(addr + bit * PAGE_BYTES, it->second.bytes.data(), PAGE_BYTES);
                }
                mask &= (mask - 1);
            }

            pending_bytes_ -= UNIT_BYTES;
            committed_bytes_.fetch_add(UNIT_BYTES);
            size_t cur = committed_bytes_.load();
            size_t old = peak_committed_.load();
            while (cur > old && !peak_committed_.compare_exchange_weak(old, cur)) {}

            auto& m = meta_[unit];
            m.state = UnitMeta::State::Resident;
            m.last_use = ++clock_;
            ++m.generation;
            lru_.push({m.last_use, unit, m.generation});
            materializations_.fetch_add(1);
            if (background) prefetches_.fetch_add(1);
            cv_.notify_all();
        }
        return true;
    }

    bool evict_until_room_locked(size_t need, uint64_t protect) {
        size_t guard = 0;
        while (committed_bytes_.load() + pending_bytes_ + need > budget_bytes_) {
            bool evicted = false;
            while (!lru_.empty()) {
                HeapEntry e = lru_.top();
                lru_.pop();
                if (e.unit >= unit_count_) continue;
                auto& m = meta_[e.unit];
                if (m.state != UnitMeta::State::Resident) continue;
                if (m.generation != e.generation || m.last_use != e.last_use) continue;
                if (e.unit == protect || m.pins != 0) continue;

                unsigned char* addr = base_ + e.unit * UNIT_BYTES;
                uint64_t mask = m.dirty_mask;
                while (mask) {
                    unsigned bit = 0;
                    unsigned long idx = 0;
#if defined(_MSC_VER)
                    _BitScanForward64(&idx, mask);
                    bit = (unsigned)idx;
#else
                    bit = (unsigned)__builtin_ctzll(mask);
#endif
                    DeltaPage& dp = deltas_[e.unit * PAGES_PER_UNIT + bit];
                    std::memcpy(dp.bytes.data(), addr + bit * PAGE_BYTES, PAGE_BYTES);
                    mask &= (mask - 1);
                }

                if (!VirtualFree(addr, UNIT_BYTES, MEM_DECOMMIT)) {
                    return false;
                }
                m.state = UnitMeta::State::Reserved;
                committed_bytes_.fetch_sub(UNIT_BYTES);
                evictions_.fetch_add(1);
                evicted = true;
                break;
            }
            if (!evicted) {
                if (++guard > unit_count_ + 4) return false;
                return false;
            }
        }
        return true;
    }

    void worker_loop() {
        CpuSampler sampler;
        sampler.sample();
        for (;;) {
            uint64_t unit = UINT64_MAX;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(20), [&]{
                    return stop_ || (!prefetch_q_.empty() &&
                           prefetch_enabled_.load(std::memory_order_relaxed));
                });
                if (stop_) return;
                if (!prefetch_enabled_.load(std::memory_order_relaxed) || prefetch_q_.empty()) {
                    continue;
                }
                unit = prefetch_q_.front();
                prefetch_q_.pop_front();
            }

            double cpu = sampler.sample();
            if (cpu > 55.0) {
                cpu_backoffs_.fetch_add(1);
                Sleep(8);
                continue;
            }
            ensure_resident(unit, true);
        }
    }

private:
    unsigned char* base_ = nullptr;
    size_t logical_bytes_ = 0;
    size_t unit_count_ = 0;
    std::vector<UnitMeta> meta_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint64_t> prefetch_q_;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> lru_;
    std::unordered_map<uint64_t, DeltaPage> deltas_;

    uint64_t clock_ = 0;
    size_t budget_bytes_ = 0;
    size_t pending_bytes_ = 0;
    bool stop_ = false;
    std::thread worker_;
    std::atomic<bool> prefetch_enabled_{true};

    std::atomic<size_t> committed_bytes_{0};
    std::atomic<size_t> peak_committed_{0};
    std::atomic<uint64_t> materializations_{0};
    std::atomic<uint64_t> evictions_{0};
    std::atomic<uint64_t> prefetches_{0};
    std::atomic<uint64_t> prefetch_hits_{0};
    std::atomic<uint64_t> cpu_backoffs_{0};
    std::atomic<uint64_t> commit_failures_{0};
};

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double x = (v.size() - 1) * p / 100.0;
    size_t lo = (size_t)std::floor(x), hi = (size_t)std::ceil(x);
    if (lo == hi) return v[lo];
    return v[lo] * (hi - x) + v[hi] * (x - lo);
}

struct ProcMem {
    double working_set_mb = 0;
    double peak_working_set_mb = 0;
    double private_mb = 0;
};

static ProcMem process_memory() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    ProcMem out{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        out.working_set_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
        out.peak_working_set_mb = (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
        out.private_mb = (double)pmc.PrivateUsage / (1024.0 * 1024.0);
    }
    return out;
}

struct Args {
    int logical_gb = 8;
    int objects = 24;
    int steps = 3000;
    int max_budget_mb = 543;
    int min_budget_mb = 256;
    int safety_reserve_mb = 878;
    int hard_limit_mb = 4096;
    int shadow_depth = 4;
    uint64_t seed = 20260924;
    std::string profile = "normal";
    std::string json = "result.json";
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + k);
            return argv[++i];
        };
        if (k == "--logical-gb") a.logical_gb = std::stoi(next());
        else if (k == "--objects") a.objects = std::stoi(next());
        else if (k == "--steps") a.steps = std::stoi(next());
        else if (k == "--max-budget-mb") a.max_budget_mb = std::stoi(next());
        else if (k == "--min-budget-mb") a.min_budget_mb = std::stoi(next());
        else if (k == "--safety-reserve-mb") a.safety_reserve_mb = std::stoi(next());
        else if (k == "--hard-limit-mb") a.hard_limit_mb = std::stoi(next());
        else if (k == "--shadow-depth") a.shadow_depth = std::stoi(next());
        else if (k == "--seed") a.seed = std::stoull(next());
        else if (k == "--profile") a.profile = next();
        else if (k == "--json") a.json = next();
        else throw std::runtime_error("unknown argument: " + k);
    }
    return a;
}

static int choose_next_object(int cur, int objects, const std::string& profile,
                              std::mt19937_64& rng) {
    uint64_t r = rng() % 1000;
    if (profile == "high-switch") {
        if (r < 750) return (cur + 1 + (int)(rng() % (objects - 1))) % objects;
        return cur;
    }
    if (profile == "low-locality") {
        return (int)(rng() % objects);
    }
    if (profile == "bursty") {
        if (r < 300) return (cur + 1 + (int)(rng() % 5)) % objects;
        return cur;
    }
    if (r < 180) return (cur + 1 + (int)(rng() % 3)) % objects;
    return cur;
}

int main(int argc, char** argv) {
    try {
        Args a = parse_args(argc, argv);
        const size_t logical_bytes = (size_t)a.logical_gb * 1024ull * 1024ull * 1024ull;
        const uint64_t units = (logical_bytes + UNIT_BYTES - 1) / UNIT_BYTES;
        if (a.objects < 1 || units < (uint64_t)a.objects) {
            throw std::runtime_error("invalid objects/logical size");
        }

        std::string job_why;
        bool hard_limit_active = install_self_job_limit(
            (size_t)a.hard_limit_mb * 1024ull * 1024ull, job_why);

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);

        size_t initial_budget = (size_t)a.max_budget_mb * 1024ull * 1024ull;
        StateArena arena(logical_bytes, initial_budget);

        const uint64_t units_per_obj = units / (uint64_t)a.objects;
        std::vector<uint8_t> canary_xor(units, 0);
        std::vector<std::vector<uint32_t>> transitions(
            a.objects, std::vector<uint32_t>(a.objects, 0));

        std::mt19937_64 rng(a.seed);
        int current_obj = 0;
        uint64_t cur_in_obj = 0;

        std::vector<double> lat;
        lat.reserve(a.steps);
        uint64_t integrity_errors = 0;
        uint64_t budget_updates = 0;
        uint64_t emergency_shrinks = 0;
        double peak_ws = 0.0, peak_private = 0.0;
        size_t min_observed_budget = arena.budget();
        size_t max_observed_budget = arena.budget();

        auto update_governor = [&]() {
            MEMORYSTATUSEX st{};
            st.dwLength = sizeof(st);
            if (!GlobalMemoryStatusEx(&st)) return;

            size_t reserve = (size_t)a.safety_reserve_mb * 1024ull * 1024ull;
            size_t minb = (size_t)a.min_budget_mb * 1024ull * 1024ull;
            size_t maxb = (size_t)a.max_budget_mb * 1024ull * 1024ull;
            size_t avail = (size_t)st.ullAvailPhys;

            size_t target = minb;
            if (avail > reserve) target = std::min(maxb, avail - reserve);
            target = std::clamp(target, minb, maxb);

            bool danger = (st.dwMemoryLoad >= 90 ||
                           avail < 512ull * 1024ull * 1024ull);
            if (danger) {
                target = minb;
                arena.set_prefetch_enabled(false);
                ++emergency_shrinks;
            } else {
                arena.set_prefetch_enabled(true);
            }
            arena.set_budget(target);
            min_observed_budget = std::min(min_observed_budget, target);
            max_observed_budget = std::max(max_observed_budget, target);
            ++budget_updates;
        };

        update_governor();

        for (int step = 0; step < a.steps; ++step) {
            if ((step % 50) == 0) update_governor();

            int next_obj = choose_next_object(current_obj, a.objects, a.profile, rng);
            if (next_obj != current_obj) {
                transitions[current_obj][next_obj]++;
                current_obj = next_obj;
                cur_in_obj = (uint64_t)(rng() % std::min<uint64_t>(units_per_obj, 16));
            } else {
                if (a.profile == "low-locality") {
                    cur_in_obj = rng() % units_per_obj;
                } else {
                    uint64_t advance = 1 + (rng() % (a.profile == "bursty" ? 6 : 3));
                    cur_in_obj = (cur_in_obj + advance) % units_per_obj;
                }
            }

            uint64_t unit = (uint64_t)current_obj * units_per_obj + cur_in_obj;
            if (unit >= units) unit %= units;

            bool do_write = (step % 17) == 0;
            double t0 = now_ms();
            bool ok = arena.touch(unit, do_write, canary_xor[unit], integrity_errors);
            double dt = now_ms() - t0;
            if (!ok) throw std::runtime_error("StateRAM touch failed");
            if (do_write) canary_xor[unit] ^= 0x5a;
            lat.push_back(dt);

            // Conservative weak-CPU shadow: a few nearby 256 KiB units.
            for (int d = 1; d <= a.shadow_depth; ++d) {
                uint64_t q = (uint64_t)current_obj * units_per_obj +
                             ((cur_in_obj + (uint64_t)d) % units_per_obj);
                if (q < units) arena.enqueue_prefetch(q);
            }

            // Learned object-transition hint: prefetch one entry unit of the
            // most common next object, not a full object.
            uint32_t best = 0;
            int best_obj = -1;
            for (int j = 0; j < a.objects; ++j) {
                if (transitions[current_obj][j] > best) {
                    best = transitions[current_obj][j];
                    best_obj = j;
                }
            }
            if (best_obj >= 0) {
                uint64_t q = (uint64_t)best_obj * units_per_obj;
                if (q < units) arena.enqueue_prefetch(q);
            }

            if ((step % 100) == 0) {
                auto pm = process_memory();
                peak_ws = std::max(peak_ws, pm.peak_working_set_mb);
                peak_private = std::max(peak_private, pm.private_mb);
            }
        }

        // Deterministic dirty-rematerialization check:
        // touch a window larger than the 543 MiB budget, dirty every 31st unit,
        // then revisit it. This forces Windows commit/decommit churn.
        uint64_t check_units = std::min<uint64_t>(units, 4096); // 1 GiB logical window
        for (uint64_t u = 0; u < check_units; ++u) {
            bool wr = (u % 31) == 0;
            if (!arena.touch(u, wr, canary_xor[u], integrity_errors)) {
                throw std::runtime_error("coverage touch failed");
            }
            if (wr) canary_xor[u] ^= 0x5a;
        }
        for (uint64_t u = 0; u < check_units; ++u) {
            if (!arena.touch(u, false, canary_xor[u], integrity_errors)) {
                throw std::runtime_error("coverage revisit failed");
            }
        }

        Sleep(100);
        auto pm = process_memory();
        peak_ws = std::max(peak_ws, pm.peak_working_set_mb);
        peak_private = std::max(peak_private, pm.private_mb);

        double p50 = percentile(lat, 50);
        double p95 = percentile(lat, 95);
        double p99 = percentile(lat, 99);
        uint64_t over10 = 0, over50 = 0;
        for (double x : lat) {
            if (x > 10.0) ++over10;
            if (x > 50.0) ++over50;
        }

        bool pass =
            integrity_errors == 0 &&
            arena.commit_failures() == 0 &&
            arena.evictions() > 0 &&
            arena.materializations() > 0 &&
            arena.peak_committed() <= (size_t)a.max_budget_mb * 1024ull * 1024ull + UNIT_BYTES &&
            hard_limit_active;

        std::ofstream out(a.json, std::ios::binary);
        out << "{\n";
        out << "  \"phase\": \"Windows Phase 2 user-mode engine\",\n";
        out << "  \"logical_gb\": " << a.logical_gb << ",\n";
        out << "  \"unit_kb\": 256,\n";
        out << "  \"profile\": \"" << a.profile << "\",\n";
        out << "  \"steps\": " << a.steps << ",\n";
        out << "  \"hard_limit_active\": " << (hard_limit_active ? "true" : "false") << ",\n";
        out << "  \"hard_limit_note\": \"" << job_why << "\",\n";
        out << "  \"max_budget_mb\": " << a.max_budget_mb << ",\n";
        out << "  \"min_budget_mb\": " << a.min_budget_mb << ",\n";
        out << "  \"safety_reserve_mb\": " << a.safety_reserve_mb << ",\n";
        out << "  \"p50_ms\": " << std::fixed << std::setprecision(4) << p50 << ",\n";
        out << "  \"p95_ms\": " << p95 << ",\n";
        out << "  \"p99_ms\": " << p99 << ",\n";
        out << "  \"over_10ms_pct\": " << (100.0 * over10 / std::max<size_t>(1, lat.size())) << ",\n";
        out << "  \"over_50ms_pct\": " << (100.0 * over50 / std::max<size_t>(1, lat.size())) << ",\n";
        out << "  \"materializations\": " << arena.materializations() << ",\n";
        out << "  \"evictions\": " << arena.evictions() << ",\n";
        out << "  \"prefetches\": " << arena.prefetches() << ",\n";
        out << "  \"prefetch_hits\": " << arena.prefetch_hits() << ",\n";
        out << "  \"cpu_backoffs\": " << arena.cpu_backoffs() << ",\n";
        out << "  \"commit_failures\": " << arena.commit_failures() << ",\n";
        out << "  \"delta_pages\": " << arena.delta_pages() << ",\n";
        out << "  \"integrity_errors\": " << integrity_errors << ",\n";
        out << "  \"peak_engine_committed_mb\": "
            << ((double)arena.peak_committed() / (1024.0*1024.0)) << ",\n";
        out << "  \"peak_working_set_mb\": " << peak_ws << ",\n";
        out << "  \"peak_private_mb\": " << peak_private << ",\n";
        out << "  \"budget_updates\": " << budget_updates << ",\n";
        out << "  \"emergency_shrinks\": " << emergency_shrinks << ",\n";
        out << "  \"min_observed_budget_mb\": "
            << ((double)min_observed_budget / (1024.0*1024.0)) << ",\n";
        out << "  \"max_observed_budget_mb\": "
            << ((double)max_observed_budget / (1024.0*1024.0)) << ",\n";
        out << "  \"pass\": " << (pass ? "true" : "false") << "\n";
        out << "}\n";
        out.close();

        std::cout << "StateRAM Windows Phase 2\n";
        std::cout << " logical=" << a.logical_gb << " GiB"
                  << " unit=256 KiB profile=" << a.profile << "\n";
        std::cout << " hard process commit limit: "
                  << (hard_limit_active ? "ACTIVE" : "NOT ACTIVE")
                  << " (" << job_why << ")\n";
        std::cout << " engine peak committed="
                  << (double)arena.peak_committed()/(1024.0*1024.0) << " MiB"
                  << " evictions=" << arena.evictions()
                  << " delta_pages=" << arena.delta_pages() << "\n";
        std::cout << " P50=" << p50 << " ms"
                  << " P95=" << p95 << " ms"
                  << " P99=" << p99 << " ms"
                  << " >10ms=" << (100.0*over10/std::max<size_t>(1,lat.size())) << "%"
                  << " >50ms=" << (100.0*over50/std::max<size_t>(1,lat.size())) << "%\n";
        std::cout << " integrity_errors=" << integrity_errors
                  << " commit_failures=" << arena.commit_failures() << "\n";
        std::cout << " WIN_STATERAM_PHASE2=" << (pass ? "PASS" : "FAIL") << "\n";
        return pass ? 0 : 10;
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 2;
    }
}

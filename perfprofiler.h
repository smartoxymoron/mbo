#ifndef ZR_UTIL_PERFPROFILER
#define ZR_UTIL_PERFPROFILER
// clang-format off

#include <cstdint>
#include <string>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cassert>
#include <x86intrin.h>
#include <algorithm>
#include <unistd.h>
#include <string_view>

/* Simplified PerfProfiler that uses malloc instead of SHM */

class PerfProfiler {
  public:
    struct alignas(64) stat_t {
        static constexpr size_t NAMELEN = 31;
        char name[NAMELEN];
        uint8_t rsvd;
        uint64_t count;
        uint64_t sum;
        uint64_t min;
        uint64_t max;

        inline void reset(struct stat_t* old = nullptr) {
            if (old)
                *old = *this;
            count = 0;
            sum = 0;
            min = UINT64_MAX;
            max = 0;
        }

        inline void accum(uint64_t value) {
            if (value > 32000) // Ignore outliers >32k cycles, approx 10us
                return;

            count++;
            sum += value;
            min = value < min ? value : min;
            max = value > max ? value : max;
        }
    };
    static_assert(64 == sizeof(stat_t), "For performance each stat_t should occupy one cacheline");

    struct page_t {
        void init() {
            count = 0;
            next_report_ns = 0;
            locked.clear();
            for (uint32_t index = 0; index < LIMIT; index++)
                stats[index].reset();
        }

        static constexpr uint32_t LIMIT = 1024;
        uint32_t count;
        uint32_t padding;
        uint64_t next_report_ns;
        std::atomic_flag locked = ATOMIC_FLAG_INIT;
        stat_t stats[LIMIT];
    };

    static void create(std::string name, uint64_t report_ms, std::string path = "") {
        if (nullptr == s_singleton)
            s_singleton = new PerfProfiler(name, report_ms, path);
    }

    static inline PerfProfiler& singleton() {
        if (nullptr == s_singleton)
            create("default", 0);
        return *s_singleton;
    }

    static inline uint64_t tsc() {
        return (uint64_t)__rdtsc();
    }

    static inline uint64_t clock_gettime_ns(bool coarse = false) {
        struct timespec t;
        clock_gettime(coarse ? CLOCK_REALTIME_COARSE : CLOCK_REALTIME, &t);
        return (uint64_t)t.tv_sec * 1'000'000'000UL + (uint64_t)t.tv_nsec;
    }

    inline uint64_t tsc2ns(uint64_t tsc) {
        return (tsc * m_tsc2ns) / 65536;
    }

    stat_t* get(const char* name) {
        if (strnlen(name, stat_t::NAMELEN) >= stat_t::NAMELEN) return &m_drain;
        if (nullptr == m_page) return &m_drain;

        // Try reading without lock first
        uint32_t current_count = m_page->count;
        for (uint32_t i = 0; i < current_count; ++i) {
            if (0 == strncmp(name, m_page->stats[i].name, stat_t::NAMELEN))
                return &m_page->stats[i];
        }

        // Need to add new stat, take spinlock
        while (m_page->locked.test_and_set(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }

        // Re-check after taking lock
        current_count = m_page->count;
        for (uint32_t i = 0; i < current_count; ++i) {
            if (0 == strncmp(name, m_page->stats[i].name, stat_t::NAMELEN)) {
                m_page->locked.clear(std::memory_order_release);
                return &m_page->stats[i];
            }
        }

        if (current_count >= m_page->LIMIT) {
            m_page->locked.clear(std::memory_order_release);
            return &m_drain;
        }

        stat_t* stat = &m_page->stats[m_page->count];
        strncpy(stat->name, name, stat_t::NAMELEN - 1);
        stat->name[stat_t::NAMELEN - 1] = 0;
        stat->reset();
        m_page->count++;
        
        m_page->locked.clear(std::memory_order_release);
        return stat;
    }

    inline void report(bool polling = false) {
        if (nullptr == m_page) return;
        uint64_t now = clock_gettime_ns(true);

        if (0 == m_page->count || (polling && now < m_page->next_report_ns))
            return;

        if (0 == m_report_ms && polling) {
            return;
        }

        static uint64_t last = 0;
        time_t t = time(nullptr);
        char buffer[32];
        fprintf(m_fileout, "\n%-31s %9s %9s %9s %9s %9s %9s\tafter %5lums at %s",
            "PerfProfiler", "Format", "Count", "Total", "Average", "Min", "Max",
            last ? (now - last) / 1'000'000 : 0, ctime_r(&t, buffer));
        last = now;
        if (m_report_ms) m_page->next_report_ns = now + m_report_ms * 1'000'000;

        uint32_t count = std::min(m_page->count, m_page->LIMIT);
        for (uint32_t i = 0; i < count; ++i) {
            stat_t s;
            m_page->stats[i].reset(&s); // This is non-atomic but ok for reporting

            char* format_ptr = s.name;
            char* stat_name = strsep(&format_ptr, "|");
            const char* format = format_ptr ? format_ptr : "n";
            
            auto conv = [this, format](uint64_t v) -> uint64_t {
                if (format[0] == 'n') return this->tsc2ns(v);
                if (format[0] == 'u') return this->tsc2ns(v) / 1000;
                if (format[0] == 'm') return this->tsc2ns(v) / 1000000;
                return v;
            };

            fprintf(m_fileout, "%-31s %8s| %9lu %9lu %9lu %9lu %9lu\n", stat_name ? stat_name : "-", format,
                s.count, conv(s.sum), s.count ? conv(s.sum) / s.count : 0, s.count ? conv(s.min) : 0, conv(s.max));
        }
        fflush(m_fileout);
    }

    PerfProfiler(std::string name, uint64_t report_ms = 0, std::string path = "");
    ~PerfProfiler();
    static inline PerfProfiler* s_singleton = nullptr;
    
  private:
    std::string m_name;
    uint64_t m_report_ms;
    page_t* m_page{nullptr};
    stat_t m_drain;
    uint64_t m_tsc2ns;
    FILE* m_fileout;
    FILE* m_fileerr;
};

inline PerfProfiler::PerfProfiler(std::string name, uint64_t report_ms, std::string path)
    : m_name(name), m_report_ms(report_ms), m_fileout(stdout), m_fileerr(stderr) {
    (void)path;
    m_page = new page_t();
    m_page->init();
    
    uint64_t t1 = tsc();
    usleep(100'000);
    uint64_t t2 = tsc();
    m_tsc2ns = (65536UL * 100'000'000) / (t2 - t1);
}

inline PerfProfiler::~PerfProfiler() {
    delete m_page;
}

template <typename T>
class ScopedAction {
    T m_action;

  public:
    ScopedAction(T action) : m_action(std::move(action)) {
    }
    ~ScopedAction() {
        m_action();
    }
};

struct PerfProfileBaton {
    inline void set(uint64_t tsc = PerfProfiler::tsc()) {
        m_timestamp[0] = m_timestamp[1] = static_cast<uint32_t>(tsc);
    }

    inline void pass(uint64_t tsc = PerfProfiler::tsc()) {
        m_timestamp[0] = static_cast<uint32_t>(tsc);
    }

    inline uint32_t get(uint64_t index = 0) {
        return m_timestamp[index];
    }

    uint32_t m_timestamp[2]{0, 0};  // [1] has timestamp at start
};

#define __PPCCAT2(a, b)   a##b
#define __PPCCAT(a, b)    __PPCCAT2(a, b)
#define __PPUNIQUE(_name) __PPCCAT(_name, __LINE__)

#define __PPSTAT          __PPUNIQUE(__ppstat)
#define __PPTSC           __PPUNIQUE(__pptsc)
#define __PPACTION        __PPUNIQUE(__ppaction)

#define PerfProfile(_name)                                                                                             \
    PerfProfiler::stat_t* __PPSTAT = nullptr;                                                                          \
    {                                                                                                                  \
        static thread_local PerfProfiler::stat_t* __ppstat                                                             \
            = PerfProfiler::singleton().get(std::string_view(_name).data());                                           \
        __PPSTAT = __ppstat;                                                                                           \
    };                                                                                                                 \
    uint64_t __PPTSC = PerfProfiler::tsc();                                                                            \
    ScopedAction __PPACTION([&__PPSTAT, __PPTSC]() -> void {                                                           \
        __PPSTAT->accum(PerfProfiler::tsc() - __PPTSC);                                                                \
    });

#define PerfProfileSample(_name, _value)                                                                               \
    {                                                                                                                  \
        static thread_local PerfProfiler::stat_t* __PPSTAT                                                             \
            = PerfProfiler::singleton().get(std::string_view(_name).data());                                           \
        if (__PPSTAT)                                                                                                  \
            __PPSTAT->accum(_value);                                                                                   \
    }

#define PerfProfileCount(_name, _value)                                                                                \
    {                                                                                                                  \
        static thread_local PerfProfiler::stat_t* __PPSTAT                                                             \
            = PerfProfiler::singleton().get((std::string(_name) + "|").c_str());                                       \
        if (__PPSTAT)                                                                                                  \
            __PPSTAT->accum(_value);                                                                                   \
    }

#define PerfProfileRelay(_name, _baton)                                                                                \
    {                                                                                                                  \
        static thread_local PerfProfiler::stat_t* __PPSTAT                                                             \
            = PerfProfiler::singleton().get((std::string(_name) + "|n").c_str());                                      \
        if (_baton.get()) [[likely]] {                                                                                 \
            uint32_t tsc = static_cast<uint32_t>(PerfProfiler::tsc());                                                 \
            __PPSTAT->accum(tsc - _baton.get());                                                                       \
            _baton.pass(tsc);                                                                                          \
        }                                                                                                              \
    }

#define PerfProfileRelayTotal(_name, _baton)                                                                           \
    {                                                                                                                  \
        static thread_local PerfProfiler::stat_t* __PPSTAT                                                             \
            = PerfProfiler::singleton().get((std::string(_name) + "|n").c_str());                                      \
        if (_baton.get(1)) [[likely]] {                                                                                \
            uint32_t tsc = static_cast<uint32_t>(PerfProfiler::tsc());                                                 \
            __PPSTAT->accum(tsc - _baton.get(1));                                                                      \
            _baton.pass(tsc);                                                                                          \
        }                                                                                                              \
    }

#define PerfProfileTsc()        PerfProfiler::tsc()
#define PerfProfileNs()         PerfProfiler::clock_gettime_ns()
#define PerfProfileExaToNs(_t)  (static_cast<uint64_t>(_t) - 37000000000UL)

#define PerfProfilerStatic(_name, _report_ms) [[maybe_unused]] static PerfProfiler* __pp_init = PerfProfiler::s_singleton = new PerfProfiler(_name, _report_ms)
#define PerfProfilerReport(...) PerfProfiler::singleton().report(__VA_ARGS__)

#endif

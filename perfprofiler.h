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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <x86intrin.h>

/* Design goals: Minimum overhead, easy to use, thread-safe, support multi-process, crash-safe, nodeps/~header-only for
 * easy addition to any codebase, very C for WYSIWYG for clarity
 *
 * Requirements: Expects (constant) tsc to be synced across CPUs, C++, Linux environment, security (no-one but
 * PerfProfiler should change shm - we trust its contents)
 *
 * To use:
 * PerfProfile()        scoped timer total/count/avg/min/max
 * PerfProfilerSample() raw tsc sample accumulate interface
 * PerfProfileLite()    TODO lighter-weight scoped timer with only total/count/avg
 * PerfProfileCount()   TODO simple counter interface with total/count/avg/min/max
 * PerfProfileHit()     TODO same as above but only counts true/false
 * PerfProfileLimit()   TODO scoped timer that emits rate-limited warning message on threshold breach
 * PerfProfileSwatch    TODO stopwatch interface for fine control over where time is measured, TBD
 * PerfProfileSlow()    TODO scoped timer that only tracks one in N samples, needs a better name... PerfProfileN()?
 * PerfProfileMt()      TODO similar to PerfProfile() but uses atomic instructions so multiple threads can write to it;
 *                      TBD, future could use multiple thread_local aliases that are accumulated by reporter, fused
 *                      total+count, thread_local buffering or something else for improved efficiency
 * PerfProfileFdr()     TODO flight data recorder, tracking and recording relays that took too long, TBD
 * TBD()                TODO monitoring and rate-limited warnings for things like polling frequency/latency
 *
 * Concepts:
 * - A 'page' containing multiple stats is accessible to multiple processes, implemented as a deliberately named shm
 * - A 'stat' identified by a unique string tracks some statistics (usually count/sum/min/max) for a number (usually
 *   bytes/occurrences) or a duration (in tsc)
 * - Each page is exclusively owned by a designated 'reporter' process (say LH) responsible for reporting/resetting
 *   stats with a single thread doing the reporting
 * - A page can also be modified by 'contributor' processes that can only accumulate to a stat (which the reporter is
 *   also free to do)
 * - The lifetime of the reporter encompasses the lifetime of contributors - reporter must start first, contributors
 *   can join/leave any time as long as reporter is live
 * - Typical use will see a stat updated non-atomically by a single thread though the reporter will occasionally
 *   read/reset it without a lock (fast but a bit unsafe)
 *
 * Implementation:
 * - Each process has a singleton PerfProfiler instance that is explicity initialized as a reporter or as a contributor
 * - The reporter instance will stamp ownership on the shm, which also informs the contributor instances of the
 *   reporters liveness
 * - As soon the reporter takes ownership, it will reset the stats and possibly recreate them if the version is to be
 *   upgraded
 * - Contributors only update to stats if they understand the version of the shm and there is a live reporter
 * - Stats are typically updated by a single process (so non-atomic is ok) but, unfortunately, the reporter will
 *   report/reset them async (because min/max need reset) which can race and fail to reset
 * - Currently expected overhead for basic stat (non-atomic, rdtsc, single threaded access, count/sum/min/max) is ~20ns,
 *   plan accordingly
 * - The idea of a one reporter and multiple contributor processes maps roughly to our process dependency chain and
 *   logical separation
 * - With multi-process deployment, LH+strats will be monitored with one page and pubs will have a page each
 * - With single-process deployment, LH+strat+pub is a single process and should write to the same page but, for
 *   uniformity, we can keep the pub stats separate
 * - The crash-safe requirement adds some complexity
 * - Durations will be reported in nanoseconds, other information (perhaps byte counts) will be reported as recorded
 *
 * Usage:
 * - Call PerfProfilerCreate(name, report_ms) once per process to instantiate and point the PerfProfiler instance to the
 *   appropriate shm and designate it as reporter (non-zero report_ms)
 * - To use in ad-hoc projects, it may be easier to just include the header and use PerfProfilerStatic(name, report_ms)
 *   as a global definition
 * - Shm used will be /dev/shm/perfprofiler-{name}, with process name being the default name
 * - Output is appended to perfprofiler-{name}-{yyyymmdd}.txt (if path provided, else stdout) / stderr
 * - Use PerfProfile("stat_name") as a scoped timer
 * - Use PerfProfileTsc() explicitly to read tsc and then accumulate the stats manually with
 *   PerfProfileSample("stat_name", tsc_diff)
 * - Feel free to use PerfProfileNs() (ns since epoch, uint64_t) and ScopedAction as you see fit
 *
 * Limits:
 * - A v1 page is limited to 63 stats
 * - A stat name is limited to 30 ascii characters - _recommend_ shortening names by using '.' instead of '::' etc.
 * - Currently a PerfProfiler instance can only operate on a single page (so, for instance, a strat cannot use a
 *   different shm/report for some private stats)
 * - The shm contents are _trusted_
 * - TODO The stat name 'format' implementation ('|') is currently half-baked and not properly specified nor validated -
 *   don't do anything stupid
 *
 * Sample of per-second information reported:
 * PerfProfiler                       Format     Count     Total   Average       Min       Max	after  1007ms from test_linehandler(2223622) at Fri Aug 30 13:55:11 2024
 * AdapterNSE0.send                       n|        85    262766      3091      1612      7850
 * AdapterNSE1.send                       n|        98    315017      3214      1642      6719  //Note different stat name for each thread, single-thread contributing to each
 * PerfProfiler.report_tsc                t|         1     99247     99247     99247     99247
 * PerfProfiler.report                    n|         1     23652     23652     23652     23652
 * report                                 u|         1        23        23        23        23
 * AdapterNSE0.poll_dequeue                |        40        85         2         1         7
 * AdapterNSE1.poll_dequeue                |        47        98         2         1         7
 */
[[maybe_unused]] static int debug_count = 0;

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
        void init(uint64_t stamp = 0) {
            count.store(0, std::memory_order_relaxed);
            owned_ns.store(stamp, std::memory_order_relaxed);
            for (uint32_t index = 0; index < LIMIT; index++)
                stats[index].reset();
            version.store(page_t::VERSION, std::memory_order_release);
        }

        static constexpr uint32_t HEADER_SIZE = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t);
        static constexpr uint32_t VERSION = 1;
        static constexpr uint32_t LIMIT = 127;
        static constexpr uint64_t OWNED_GRACE_NS = 1e9; // Wait ~1 second for lease to expire
        std::atomic<uint32_t> version;              // Version, count and reportat_ns are the permanent header format
        std::atomic<uint32_t> count;                // Monotonic
        std::atomic<uint64_t> owned_ns;             // Lease for reporter instance, epoch in ns; uint64_t epoch in ns will not wrap
        uint64_t reserved[6];
        stat_t stats[LIMIT];                        // 127 stats will total to 8KB
    };
    static_assert(64 * (page_t::LIMIT + 1) == sizeof(page_t), "page_t header occupies one cache line to fit neatly with stat_t");

    static void create(std::string name, uint64_t report_ms, std::string path = "") {
        // If create() is used via PerfProfilerSetup(), it must be done before any other (non-static) interface is used
        extern char* __progname; // Dynamic initialization defaults to using the program name as instance name
        assert(nullptr == s_singleton or nullptr == s_singleton->m_page);
        s_singleton = new PerfProfiler(name.size() ? name : std::string(__progname), report_ms, path);
    }

    static inline PerfProfiler& singleton() {
        // If create() is not used, then PerfProfilerStatic() should be used to statically setup PerfProfiler
        return *s_singleton;
    }

    PerfProfiler(void)
        : m_name("invalid"), m_report_ms(0), m_page(nullptr), m_ownedtill(0), m_fileout(stdout), m_fileerr(stderr) {
    }

    static inline uint64_t tsc(bool use_rdtscp = false) {
        if (use_rdtscp) {
            unsigned int cpu;
            return (uint64_t)__rdtscp(&cpu);
        } else {
            return (uint64_t)__rdtsc();
        }
    }

    static inline uint64_t clock_gettime_ns(bool coarse = false) {
        struct timespec t;
        clock_gettime(coarse ? CLOCK_REALTIME_COARSE : CLOCK_REALTIME, &t);
        return (uint64_t)t.tv_sec * 1'000'000'000UL + (uint64_t)t.tv_nsec;
    }

    // Public but not efficient because must be accessed via singleton
    inline uint64_t tsc2ns(uint64_t tsc) {
        return (tsc * m_tsc2ns) / 65536;
    }

    inline uint64_t ns2tsc(uint64_t ns) {
        return (ns * m_ns2tsc) / 65536;
    }

    // Will never return nullptr, will never block indefinitely, crash-proof (fingers crossed)
    stat_t* get(const char* name) {

        if (strnlen(name, stat_t::NAMELEN) >= stat_t::NAMELEN) {
            fprintf(m_fileerr, "PerfProfiler stat name too long, %s will not be collected\n", name);
            return &m_drain;
        }

        if (nullptr == m_page) {
            fprintf(m_fileerr, "PerfProfiler inactive, %s will not be collected - use PerfProfilerSetup()/PerfProfilerStatic()\n", name);
            return &m_drain;
        }

        auto count = m_page->count.load(std::memory_order_acquire);
        stat_t* stat = nullptr;

        do {
            count = std::min(count, m_page->LIMIT);
            // Look for an existing stat with the same name; boost::static_vector is neater but keeping POD for IPC
            for (stat = &m_page->stats[0]; stat < &m_page->stats[count]; stat++)
                if (0 == strncmp(name, stat->name, stat->NAMELEN))
                    return stat;

            if (stat - m_page->stats >= m_page->LIMIT) { // Not found, all counters in use
                fprintf(m_fileerr,
                        "PerfProfiler all %u counters in use, %s will not be collected\n",
                        m_page->LIMIT,
                        name);
                return &m_drain;
            }
        } while (!m_page->count.compare_exchange_strong(count, count + 1, std::memory_order_release, std::memory_order_acquire));

        // This is racy and non-atomic - we've incremented count to take ownership but the name is updated async
        strncpy(stat->name, name, stat->NAMELEN - 1);
        stat->name[stat->NAMELEN - 1] = 0;
        count++;
        stat->reset();
        // Crude 'val = val, release' operation, should give improve odds of expected behaviour but may still produce
        // duplicate counters/false matches
        m_page->count.compare_exchange_strong(count, count, std::memory_order_release);
        return stat;
    }

    // Typically report() will be invoked every report_ms with polling=false; Alternatively it can be called in a loop
    // with polling=true and report will be emitted when due
    inline void report(bool polling = false) {
        uint64_t now = clock_gettime_ns(true);

        // If nothing to report or not yet time to report (for polling mode) then do nothing
        if (nullptr == m_page or 0 == m_page->count.load(std::memory_order_relaxed)
            or (polling and now < m_page->owned_ns.load(std::memory_order_relaxed)))
            return;

        if (0 == m_report_ms) {
            fprintf(m_fileerr, "Non-reporter PerfProfiler instance trying to report\n");
            return;
        }

        if (nullptr == m_page) {
            fprintf(m_fileerr, "PerfProfiler inactive, will not report on %s\n", m_name.c_str());
            return;
        }

        if (!owned(m_ownedtill, m_report_ms * 1'000'000)) {
            fprintf(m_fileerr, "PerfProfiler no ownership, will not report on %s\n", m_name.c_str());
            return;
        }

        extern char* __progname;
        static uint64_t last = 0; // Static works because report() is called from a single thread
        time_t t = time(nullptr);
        char buffer[32];
        fprintf(m_fileout, "\n%-31s %9s %9s %9s %9s %9s %9s\tafter %5lums from %s(%d) at %s",
            "PerfProfiler", "Format", "Count", "Total", "Average", "Min", "Max",
            last ? (now - last) / 1'000'000 : 0, __progname, ::gettid(), ctime_r(&t, buffer));
        last = now;

        uint32_t count = std::min(m_page->count.load(std::memory_order_acquire), m_page->LIMIT);
        stat_t s;
        for (auto stat = &m_page->stats[0]; count > 0; count--, stat++) {
            stat->reset(&s);

            char* format = s.name; // Tokenize (destructively) name in copy to find any conversion specifiers (default tsc->ns)
            char* name;
            name = strsep(&format, "|");
            format = format ? format : (char*)"n";
            auto conv = [this, format](uint64_t v) -> uint64_t {
                switch (format[0]) {
                    case 't': return v; // tsc
                    case 'n': return this->tsc2ns((v)); // ns
                    case 'u': return this->tsc2ns((v)) / 1000; // us
                    case 'm': return this->tsc2ns((v)) / 1000000; // ms
                }
                return v; // Default, number as-is, eg. counter
            };
            if (s.min > (1UL << 32)) {
                fprintf(m_fileout, "%-31s %8s| %9lu\t// min > 2^32 suggests invalid data\n",
                    name ? name : "-", format ? format : "-", s.count);
                continue;
            }
            fprintf(m_fileout, "%-31s %8s| %9lu %9lu %9lu %9lu %9lu\n", name ? name : "-", format ? format : "-",
                s.count, conv(s.sum), s.count ? conv(s.sum) / s.count : 0, s.count ? conv(s.min) : 0, conv(s.max));
        }
        ::fflush(m_fileout);
    }

  private:
    PerfProfiler(std::string name, uint64_t report_ms = 0, std::string path = "")
        : m_name(name), m_report_ms(report_ms), m_page(nullptr), m_ownedtill(0), m_fileout(stdout), m_fileerr(stderr) {

        if (setup()) {
            uint64_t delta = tsc();
            usleep(100'000); // 100ms
            delta = tsc() - delta;
            m_tsc2ns = (65536UL * 100'000'000) / delta;
            m_ns2tsc = (65536UL * delta) / 100'000'000;

            std::string filename = m_fileout == stdout ? "stdout" : "stderr";
            if (path.size()) {
                time_t t = time(nullptr);
                char dt[32];
                strftime(dt, 32, "%Y%m%d", localtime(&t));
                filename = std::string("perfprofiler") + (m_name.size() ? ("-" + m_name) : "") + "-" + dt + ".txt";
                m_fileout = fopen((path + "/" + filename).c_str(), "a");
                if (nullptr == m_fileout) {
                    fprintf(m_fileerr, "PerfProfiler open failed for %s\n", (path + "/" + filename).c_str());
                    filename = "stderr";
                    m_fileout = stderr; // Default zr logging output stream
                }
            }

            fprintf(m_fileerr, "PerfProfiler setup complete for %s/%lums to %s, calib over %luns/%lutsc, tsc2ns/error tsc*%lu>>16/%.4lf%%\n",
                m_name.c_str(), m_report_ms, filename.c_str(), tsc2ns(delta), ns2tsc(100'000'000), m_tsc2ns, (100 / (double)m_tsc2ns));
        }
    }

    ~PerfProfiler() {
        if (m_page)
            ::munmap(m_page, sizeof(page_t));
        if (m_fileout != stdout)
            fclose(m_fileout);
    }

    bool setup(void) {
        // This routine is overly complicated given that we're just reporting non-critical stats, apologies
        std::string filename = m_name.size() ? ("/perfprofiler-" + m_name) : std::string("/perfprofiler");
        bool is_reporter = (0 != m_report_ms);
        int fd;

        // If reporter, first try creating new shm and resizing, else try opening existing shm
        if (is_reporter) {
            fd = ::shm_open(filename.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
            if (fd >= 0 and 0 != ::ftruncate(fd, sizeof(page_t))) {
                fprintf(m_fileerr, "PerfProfiler failed to open/resize %s for %s/%lums\n",
                        filename.c_str(), m_name.c_str(), m_report_ms);
                goto fatal;
            }
        }
        if (not is_reporter or (fd < 0 and EEXIST == errno)) {
            fd = ::shm_open(filename.c_str(), O_RDWR, 0666);
        }
        if (fd < 0) {
            fprintf(m_fileerr, "PerfProfiler failed to open %s for %s/%lums\n",
                    filename.c_str(), m_name.c_str(), m_report_ms);
            goto fatal;
        }

        // Check file is big enough to at least contain header
        struct ::stat s;
        fstat(fd, &s);
        if (s.st_size < page_t::HEADER_SIZE) {
            fprintf(m_fileerr, "PerfProfiler file %s too small for %s/%lums\n",
                    filename.c_str(), m_name.c_str(), m_report_ms);
            goto fatal;
        }

        // mmap() the shm - note that we may be mapping past EOF but we will not be accessing
        m_page = static_cast<page_t*>(::mmap(NULL, sizeof(page_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (MAP_FAILED == m_page) {
            m_page = nullptr;
            fprintf(m_fileerr, "PerfProfiler failed to mmap %s for %s/%lums\n",
                    filename.c_str(), m_name.c_str(), m_report_ms);
            goto fatal;
        }

        // Check ownership - will grab if reporter, else just check liveness
        if (not owned(m_ownedtill, m_report_ms * 1'000'000)) {
            fprintf(m_fileerr, "PerfProfiler ownership check on %s failed for %s/%lums\n",
                    filename.c_str(), m_name.c_str(), m_report_ms);
            goto fatal;
        }

        // Check version is acceptable - reporter can upgrade, contributor requires a match
        if ((m_page->version.load(std::memory_order_relaxed) > page_t::VERSION)
            or (not is_reporter and m_page->version.load(std::memory_order_relaxed) != page_t::VERSION)) {
            fprintf(m_fileerr, "PerfProfiler version check failed for %s - is %u, expect %u %s\n",
                    filename.c_str(), m_page->version.load(std::memory_order_relaxed), page_t::VERSION, is_reporter ? "or below" : "");
            goto fatal;
        }

        // Reset/upgrade while retaining ownership, contributor just checks if version is expected
        // Note that changing file size when mmap:ed is UB
        if (is_reporter) {
            ::munmap(m_page, sizeof(page_t));
            if (0 != ::ftruncate(fd, sizeof(page_t))) {
                fprintf(m_fileerr, "PerfProfiler failed to resize %s for %s/%lums\n",
                        filename.c_str(), m_name.c_str(), m_report_ms);
                goto fatal;
            }

            m_page = static_cast<page_t*>(::mmap(NULL, sizeof(page_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            if (MAP_FAILED == m_page) {
                m_page = nullptr;
                fprintf(m_fileerr, "PerfProfiler failed to mmap %s for %s/%lums\n",
                        filename.c_str(), m_name.c_str(), m_report_ms);
                goto fatal;
            }
            m_page->init(m_ownedtill);
        }
        ::close(fd); // Can close after mmap success

        return owned(m_ownedtill, m_report_ms * 1'000'000);

    fatal:
        if (m_page) {
            ::munmap(m_page, sizeof(page_t));
            m_page = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
        }
        m_ownedtill = 0;
        return false;
    }

    // Shm liveness check
    bool owned(void) {
        uint64_t dummy = 0;
        return owned(dummy, 0);
    }

    // Implementation of shm ownership update and ownership test
    // (0, 0) returns true if owned, i.e. reporter live; (0, N) returns true if unowned and took ownership, updates prev
    // (P, N) returns true if could extend ownership from P to N (N == 0 would relinquish ownership), updates prev
    bool owned(uint64_t& prev, uint64_t lease_ns) {
        uint64_t now = clock_gettime_ns(true); // Coarse time is good enough
        uint64_t grace = m_page->OWNED_GRACE_NS;
        uint64_t owned_till = m_page->owned_ns.load(std::memory_order_relaxed);
        bool owned = (now <= owned_till + grace);

        if (0 == prev and 0 == lease_ns) // Want to check if someone owns this shm
            return owned;

        if (0 == prev and true == owned) // Want to take ownership but someone else has it
            return false;

        uint64_t expected = prev ? prev : owned_till;
        uint64_t next = lease_ns ? now + lease_ns: 0;
        prev = 0;                       // Reset prev for case where we lose ownership

        if (!m_page->owned_ns.compare_exchange_strong(expected, next, std::memory_order_relaxed))
            return false;

        prev = next;
        return true;
    }

    static PerfProfiler* s_singleton;

    std::string m_name;
    uint64_t m_report_ms;
    page_t* m_page{nullptr};
    uint64_t m_ownedtill;
    stat_t m_drain;
    uint64_t m_tsc2ns;
    uint64_t m_ns2tsc;
    FILE* m_fileout;
    FILE* m_fileerr;
};

template <typename T>
class ScopedAction { // Example: ScopedAction unlock([this]()->void{this->m_page->unlock();});
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

// How this works: the 'hidden scope' pointer to stat_t is initialized at first execution and guaranteed to be non-null
// at use; Then we have a ScopedAction with unique name based on line number containing a lambda that captures the start
// time and increments the sum/count etc. of the stat_t in destructor at scope end
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

#define PerfProfilerStatic(...) PerfProfiler* PerfProfiler::s_singleton = new PerfProfiler(__VA_ARGS__)
#define PerfProfilerCreate(...) PerfProfiler::create(__VA_ARGS__)
#define PerfProfilerReport(...) PerfProfiler::singleton().report(__VA_ARGS__)

#ifdef PERFPROFILER_DISABLE
#undef PerfProfile
#undef PerfProfileSample
#undef PerfProfileCount
#undef PerfProfileRelay
#undef PerfProfileRelayTotal
#undef PerfProfilerStatic
#undef PerfProfilerCreate
#undef PerfProfilerReport

#define PerfProfile(_name) do{(void)(_name);}while(0)
#define PerfProfileSample(_name, _value) do{(void)(_name); (void)(_value);}while(0)
#define PerfProfileCount(_name, _value) do{(void)(_name); (void)(_value);}while(0)
#define PerfProfileRelay(_name, _baton)  do{(void)(_name); (void)(_baton);}while(0)
#define PerfProfileRelayTotal(_name, _baton)  do{(void)(_name); (void)(_baton);}while(0)
#define PerfProfilerStatic(...)
#define PerfProfilerCreate(...)
#define PerfProfilerReport(...)
#endif /* PERFPROFILER_DISABLE */

// clang-format on
#endif /* ZR_UTIL_PERFPROFILER */


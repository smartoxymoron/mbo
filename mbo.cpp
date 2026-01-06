#include <iostream>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <boost/unordered_map.hpp>
#include "perfprofiler.h"

using namespace std;

// --- PerfProfiler Initialization ---
PerfProfilerStatic("mbo", 0);

// --- Data Types ---
using OrderId = uint64_t;
using Token = uint32_t;
using Price = int32_t;
using Qty = int32_t;
using AggQty = int64_t;
using Count = int32_t;

// --- Binary Formats ---
struct InputRecord {
    uint32_t record_idx;
    uint64_t order_id;
    uint64_t order_id2;
    uint8_t token[3];
    uint8_t op_type : 7;
    uint8_t is_ask : 1;
    Price price;
    Qty qty;

    Token get_token() const {
        return (uint32_t(token[0])) | (uint32_t(token[1]) << 8) | (uint32_t(token[2]) << 16);
    }
} __attribute__((packed));
static_assert(sizeof(InputRecord) == 32);

struct Level {
    Price price;
    AggQty qty;
} __attribute__((packed));

struct Book {
    uint32_t record_idx;
    uint32_t token;
    char op_type;
    uint8_t padding[3];
    Level bids[20];
    Level asks[20];
} __attribute__((packed));
static_assert(sizeof(Book) == 492);

struct OrderInfo {
    bool is_ask;
    Price price;
    Qty qty;
};

// --- Global Settings ---
inline bool g_crossing_enabled = false;

// --- PriceLevels ---
template<bool IsAsk>
class PriceLevels {
public:
    PriceLevels() {
        levels_.reserve(1000);
    }

    void add(Price p, Qty qty) {
        auto& entry = levels_[p];
        entry.first += qty;
        entry.second++;
    }

    void remove(Price p, Qty qty) {
        auto it = levels_.find(p);
        if (it != levels_.end()) {
            it->second.first -= qty;
            it->second.second--;
            if (it->second.first <= 0) {
                levels_.erase(it);
            }
        }
    }

    int get_top_levels(Level* out, int n) const {
        if (levels_.empty()) return 0;

        vector<Price> prices;
        prices.reserve(levels_.size());
        for (const auto& it : levels_) {
            prices.push_back(it.first);
        }

        if constexpr (IsAsk) {
            sort(prices.begin(), prices.end()); // Ascending for asks
        } else {
            sort(prices.begin(), prices.end(), greater<Price>()); // Descending for bids
        }

        int count = min((int)prices.size(), n);
        for (int i = 0; i < count; ++i) {
            out[i].price = prices[i];
            out[i].qty = levels_.at(prices[i]).first;
        }
        return count;
    }

    Price best_price() const {
        if (levels_.empty()) return 0;
        
        Price best = 0;
        bool first = true;
        for (const auto& it : levels_) {
            Price price = it.first;
            if (first) {
                best = price;
                first = false;
            } else {
                if constexpr (IsAsk) {
                    if (price < best) best = price;
                } else {
                    if (price > best) best = price;
                }
            }
        }
        return best;
    }

private:
    boost::unordered_map<Price, pair<AggQty, Count>> levels_;
};

// --- MBO ---
class MBO {
public:
    MBO(Token token) : token_(token) {
        order_map_.reserve(10000);
        memset(&book_, 0, sizeof(Book));
    }

    void new_order(OrderId id, bool is_ask, Price price, Qty qty);
    void modify_order(OrderId id, Price new_price, Qty new_qty);
    void cancel_order(OrderId id);
    void trade(OrderId id, Qty fill_qty);

    bool has_crossed() const;
    void infer_and_apply_cross();

    const Book& get_book() const { return book_; }
    void update_book(uint32_t record_idx, char op_type);

private:
    Token token_;
    PriceLevels<false> bids_;
    PriceLevels<true> asks_;
    boost::unordered_map<OrderId, OrderInfo> order_map_;
    deque<OrderId> pending_cross_fills_;
    Book book_;
};

void MBO::new_order(OrderId id, bool is_ask, Price price, Qty qty) {
    if (id == 0) return;
    order_map_[id] = {is_ask, price, qty};
    if (is_ask) asks_.add(price, qty);
    else bids_.add(price, qty);
    
    if (g_crossing_enabled) {
        infer_and_apply_cross();
    }
}

void MBO::modify_order(OrderId id, Price new_price, Qty new_qty) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    if (info.price != new_price) {
        // Price changed
        if (info.is_ask) {
            asks_.remove(info.price, info.qty);
            asks_.add(new_price, new_qty);
        } else {
            bids_.remove(info.price, info.qty);
            bids_.add(new_price, new_qty);
        }
    } else {
        // Same price, qty change
        Qty delta = new_qty - info.qty;
        if (info.is_ask) asks_.add(info.price, delta);
        else bids_.add(info.price, delta);
    }
    info.price = new_price;
    info.qty = new_qty;

    if (g_crossing_enabled) {
        infer_and_apply_cross();
    }
}

void MBO::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    if (info.is_ask) asks_.remove(info.price, info.qty);
    else bids_.remove(info.price, info.qty);
    
    order_map_.erase(it);
}

void MBO::trade(OrderId id, Qty fill_qty) {
    if (id == 0) return;

    if (!pending_cross_fills_.empty() && pending_cross_fills_.front() == id) {
        pending_cross_fills_.pop_front();
        return;
    }

    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    if (info.is_ask) asks_.remove(info.price, fill_qty);
    else bids_.remove(info.price, fill_qty);

    if (fill_qty >= info.qty) {
        order_map_.erase(it);
    } else {
        info.qty -= fill_qty;
    }
}

bool MBO::has_crossed() const {
    Price bid = bids_.best_price();
    Price ask = asks_.best_price();
    if (bid == 0 || ask == 0) return false;
    return bid > ask;
}

void MBO::infer_and_apply_cross() {
    // Stage 2 implementation
}

void MBO::update_book(uint32_t record_idx, char op_type) {
    book_.record_idx = record_idx;
    book_.token = token_;
    book_.op_type = op_type;
    memset(book_.bids, 0, sizeof(book_.bids));
    memset(book_.asks, 0, sizeof(book_.asks));
    bids_.get_top_levels(book_.bids, 20);
    asks_.get_top_levels(book_.asks, 20);
}

// --- Runner ---
class Runner {
public:
    Runner() {
        mbos_.reserve(100);
    }
    const Book& process_record(const InputRecord& rec);

private:
    boost::unordered_map<Token, unique_ptr<MBO>> mbos_;
};

const Book& Runner::process_record(const InputRecord& rec) {
    PerfProfile("process_record");
    PerfProfileCount("records_processed", 1);

    Token token = rec.get_token();
    auto it = mbos_.find(token);
    if (it == mbos_.end()) {
        it = mbos_.emplace(token, make_unique<MBO>(token)).first;
    }

    MBO& mbo = *it->second;
    switch (rec.op_type) {
        case 'N': mbo.new_order(rec.order_id, rec.is_ask, rec.price, rec.qty); break;
        case 'M': mbo.modify_order(rec.order_id, rec.price, rec.qty); break;
        case 'X': mbo.cancel_order(rec.order_id); break;
        case 'T': mbo.trade(rec.order_id, rec.qty); break;
    }

    mbo.update_book(rec.record_idx, rec.op_type);
    return mbo.get_book();
}

// --- Main ---
int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <test_data.bin> [--crossing] [--reference <ref.bin>]" << endl;
        return 1;
    }

    const char* input_file = argv[1];
    const char* reference_file = nullptr;

    for (int i = 2; i < argc; ++i) {
        if (string(argv[i]) == "--crossing") {
            g_crossing_enabled = true;
        } else if (string(argv[i]) == "--reference" && i + 1 < argc) {
            reference_file = argv[++i];
        }
    }

    int fd = open(input_file, O_RDONLY);
    if (fd < 0) { perror("open input"); return 1; }
    struct stat sb;
    fstat(fd, &sb);
    void* mapped = mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) { perror("mmap input"); close(fd); return 1; }
    madvise(mapped, sb.st_size, MADV_WILLNEED);
    const InputRecord* records = static_cast<const InputRecord*>(mapped);
    size_t num_records = sb.st_size / sizeof(InputRecord);

    const Book* ref_books = nullptr;
    void* ref_mapped = nullptr;
    size_t num_ref_books = 0;
    if (reference_file) {
        int rfd = open(reference_file, O_RDONLY);
        if (rfd >= 0) {
            struct stat rsb;
            fstat(rfd, &rsb);
            ref_mapped = mmap(nullptr, rsb.st_size, PROT_READ, MAP_PRIVATE, rfd, 0);
            if (ref_mapped != MAP_FAILED) {
                ref_books = static_cast<const Book*>(ref_mapped);
                num_ref_books = rsb.st_size / sizeof(Book);
            }
            close(rfd);
        }
    }

    Runner runner;
    for (size_t i = 0; i < num_records; ++i) {
        const Book& book = runner.process_record(records[i]);

        if (ref_books && i < num_ref_books) {
            if (memcmp(&book, &ref_books[i], sizeof(Book)) != 0) {
                cerr << "MISMATCH at record " << i << " (idx: " << records[i].record_idx << ")" << endl;
                return 1;
            }
        }
        fwrite(&book, sizeof(Book), 1, stdout);
    }

    PerfProfilerReport();

    munmap(mapped, sb.st_size);
    if (ref_mapped && ref_mapped != MAP_FAILED) munmap(ref_mapped, num_ref_books * sizeof(Book));
    close(fd);
    return 0;
}

#include <iostream>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <span>
#include <boost/unordered_map.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/static_vector.hpp>
#include "perfprofiler.h"

using namespace std;

// --- PerfProfiler Initialization ---
PerfProfilerStatic("mbo", 0);

// --- Data Types ---
using OrderId = uint64_t;
using Token = uint32_t;
using Price = int64_t;
using Qty = int32_t;
using AggQty = int64_t;
using Count = int32_t;


// TEST DATA GENERATION START
// --- Binary Formats ---
struct OutputLevel {
    int64_t price;
    int32_t qty;
    int32_t num_orders;

    void print(const char* side, int lvl) const {
        if (price == 0) return;
        printf("  %s[%2d] p:%10ld q:%8d n:%4d\n", side, lvl, price, qty, num_orders);
    }
} __attribute__((packed));
static_assert(sizeof(OutputLevel) == 16);

// InputRecord captures exchange-observable events
// tick_type mapping (from TickType enum in mds_objects.h):
//   'N' = newOrderMsg      'M' = modOrderMsg      'T' = tradeMsg       'X' = cancelOrderMsg
//   'S' = cxlOrderSelfTrade 'A' = newOrderCross   'B' = modOrderCross  'C' = cxlOrderCross
//   'D' = iocOrderCross    'E' = mktOrderCross
struct InputRecord {
    uint32_t record_idx;     // 4 bytes
    uint32_t token;          // 4 bytes
    int64_t order_id;        // 8 bytes - primary order id (buy for trade)
    int64_t order_id2;       // 8 bytes - secondary order id (sell for trade)
    int64_t price;           // 8 bytes - order/trade price
    int32_t qty;             // 4 bytes - order/trade quantity
    char tick_type;          // 1 byte - see mapping above
    uint8_t is_ask;          // 1 byte - 0=bid, 1=ask (unused for trade)
    uint8_t padding[2];      // 2 bytes padding
    // Total: 4+4+8+8+8+4+1+1+2 = 40 bytes

    void print() const {
        printf("INPUT [%u] tok:%u id:%ld id2:%ld p:%ld q:%d type:%c side:%s\n",
               record_idx, token, order_id, order_id2, price, qty, tick_type, is_ask ? "ASK" : "BID");
    }
} __attribute__((packed));
static_assert(sizeof(InputRecord) == 40);

// OutputRecord captures the complete book state after an event
struct OutputRecord {
    uint32_t record_idx;     // 4 bytes
    uint32_t token;          // 4 bytes
    InputRecord event;       // 40 bytes - event from TickInfo (not exchange input!)
    int64_t ltp;             // 8 bytes - last trade price
    int32_t ltq;             // 4 bytes - last trade quantity
    int8_t bid_affected_lvl; // 1 byte - which bid level was affected
    int8_t ask_affected_lvl; // 1 byte - which ask level was affected
    int8_t bid_filled_lvls;  // 1 byte - how many bid levels are filled
    int8_t ask_filled_lvls;  // 1 byte - how many ask levels are filled
    uint8_t is_ask;          // 1 byte - side affected (0=bid, 1=ask)
    uint8_t padding[3];      // 3 bytes padding
    OutputLevel bids[20];    // 320 bytes
    OutputLevel asks[20];    // 320 bytes
    // Total: 4+4+40+8+4+1+1+1+1+1+3+320+320 = 708 bytes

    void print() const {
        printf("OUTPUT [%u] tok:%u side:%s affected_bid:%d affected_ask:%d filled_bid:%d filled_ask:%d ltp:%ld ltq:%d\n",
               record_idx, token, is_ask ? "ASK" : "BID", bid_affected_lvl, ask_affected_lvl, 
               bid_filled_lvls, ask_filled_lvls, ltp, ltq);
        for(int i=0; i<20; ++i) if (bids[i].price != 0) bids[i].print("BID", i);
        for(int i=0; i<20; ++i) if (asks[i].price != 0) asks[i].print("ASK", i);
    }

    bool compare(const OutputRecord& other) const {
        if (record_idx != other.record_idx) return false;
        if (token != other.token) return false;
        if (is_ask != other.is_ask) return false;
        if (bid_filled_lvls != other.bid_filled_lvls) return false;
        if (ask_filled_lvls != other.ask_filled_lvls) return false;

        // Side determined by reference as primary must match exactly
        if (other.is_ask) {
            if (ask_affected_lvl != other.ask_affected_lvl) return false;
            // Secondary side: if we have 20, accept anything. If we set a value, it must match.
            if (bid_affected_lvl != 20 && bid_affected_lvl != other.bid_affected_lvl) return false;
        } else {
            if (bid_affected_lvl != other.bid_affected_lvl) return false;
            if (ask_affected_lvl != 20 && ask_affected_lvl != other.ask_affected_lvl) return false;
        }
        
        for (int i = 0; i < 20; ++i) {
            if (bids[i].price != other.bids[i].price) return false;
            if (bids[i].qty != other.bids[i].qty) return false;
            if (bids[i].num_orders != other.bids[i].num_orders) return false;
            if (asks[i].price != other.asks[i].price) return false;
            if (asks[i].qty != other.asks[i].qty) return false;
            if (asks[i].num_orders != other.asks[i].num_orders) return false;
        }
        return true;
    }
} __attribute__((packed));
static_assert(sizeof(OutputRecord) == 708);
// TEST DATA GENERATION END

// --- Delta Message Structures ---
enum DeltaType : uint8_t {
    TickInfo = 0,    // Event metadata (always present, always first)
    Update = 1,      // Modify existing level (implicit delete if qty/count → 0)
    Insert = 2,      // Add level at index (with optional shift)
};

struct TickInfoDelta {
    uint8_t type;              // = 0
    char tick_type;            // 'N','M','X','T','A','B','C','D','E','S'
    uint8_t exch_side_flags;   // bit 0: is_exch_tick, bit 1: side
    uint8_t reserved;
    int64_t price;             // For trades: this IS the LTP
    int64_t qty;               // For trades: this IS the LTQ
    
    friend std::ostream& operator<<(std::ostream& os, const TickInfoDelta& d) {
        return os << "TickInfo{type=" << d.tick_type 
                  << ", side=" << ((d.exch_side_flags & 0x02) ? "ask" : "bid")
                  << ", exch=" << (d.exch_side_flags & 0x01)
                  << ", p=" << d.price << ", q=" << d.qty << "}";
    }
} __attribute__((packed));
static_assert(sizeof(TickInfoDelta) == 20);

struct UpdateDelta {
    uint8_t type;              // = 1
    uint8_t side_index;        // bits 0-4: index (0-19), bit 5: side
    int16_t count_delta;
    int64_t qty_delta;
    
    friend std::ostream& operator<<(std::ostream& os, const UpdateDelta& d) {
        bool side = (d.side_index >> 5) & 1;
        uint8_t idx = d.side_index & 0x1F;
        return os << "Update{side=" << (side ? "ask" : "bid")
                  << ", idx=" << (int)idx
                  << ", qty_delta=" << d.qty_delta
                  << ", count_delta=" << d.count_delta << "}";
    }
} __attribute__((packed));
static_assert(sizeof(UpdateDelta) == 12);

struct InsertDelta {
    uint8_t type;              // = 2
    uint8_t side_index_shift;  // bits 0-4: index, bit 5: side, bit 6: shift
    uint16_t reserved;
    int32_t count;             // Absolute count (32-bit for high-liquidity levels)
    int64_t price;
    int64_t qty;               // Absolute qty
    
    friend std::ostream& operator<<(std::ostream& os, const InsertDelta& d) {
        bool side = (d.side_index_shift >> 5) & 1;
        uint8_t idx = d.side_index_shift & 0x1F;
        bool shift = (d.side_index_shift >> 6) & 1;
        return os << "Insert{side=" << (side ? "ask" : "bid")
                  << ", idx=" << (int)idx
                  << ", shift=" << shift
                  << ", p=" << d.price
                  << ", q=" << d.qty
                  << ", count=" << d.count << "}";
    }
} __attribute__((packed));
static_assert(sizeof(InsertDelta) == 24);

struct DeltaChunk {
    uint32_t token = 0;
    uint16_t record_idx = 0;
    uint8_t flags = 0;             // bit 0: final (book ready for strategy)
    uint8_t num_deltas = 0;        // Number of deltas in this chunk (1-N)
    uint8_t payload[56] = {};      // Variable-length delta sequence
    
    friend std::ostream& operator<<(std::ostream& os, const DeltaChunk& chunk) {
        os << "Chunk[rec=" << chunk.record_idx 
           << ", tok=" << chunk.token 
           << ", final=" << (chunk.flags & 1) << "]: ";
        
        // Iterate through deltas in payload
        size_t offset = 0;
        size_t total_bytes = 8;  // Header is 8 bytes (token:4 + record_idx:2 + flags:1 + num_deltas:1)
        
        for (uint8_t i = 0; i < chunk.num_deltas && offset < 56; ++i) {
            if (i > 0) os << " + ";
            
            uint8_t dtype = chunk.payload[offset];
            
            if (dtype == DeltaType::TickInfo) {
                if (offset + sizeof(TickInfoDelta) > 56) break;
                const TickInfoDelta* delta = reinterpret_cast<const TickInfoDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(TickInfoDelta);
                offset += sizeof(TickInfoDelta);
            } else if (dtype == DeltaType::Update) {
                if (offset + sizeof(UpdateDelta) > 56) break;
                const UpdateDelta* delta = reinterpret_cast<const UpdateDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(UpdateDelta);
                offset += sizeof(UpdateDelta);
            } else if (dtype == DeltaType::Insert) {
                if (offset + sizeof(InsertDelta) > 56) break;
                const InsertDelta* delta = reinterpret_cast<const InsertDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(InsertDelta);
                offset += sizeof(InsertDelta);
            } else {
                os << "Unknown{type=" << (int)dtype << "}";
                break;
            }
        }
        
        os << " = " << total_bytes << "B";
        return os;
    }
} __attribute__((packed));
static_assert(sizeof(DeltaChunk) == 64);

// --- Bitmask Helper Functions ---
inline uint8_t pack_side_index(bool is_ask, uint8_t index) {
    return (index & 0x1F) | ((is_ask ? 1 : 0) << 5);
}

inline uint8_t pack_side_index_shift(bool is_ask, uint8_t index, bool shift) {
    return (index & 0x1F) | ((is_ask ? 1 : 0) << 5) | ((shift ? 1 : 0) << 6);
}

inline bool unpack_side(uint8_t packed) {
    return (packed >> 5) & 1;
}

inline uint8_t unpack_index(uint8_t packed) {
    return packed & 0x1F;
}

inline bool unpack_shift(uint8_t packed) {
    return (packed >> 6) & 1;
}

// --- DeltaEmitter ---
class DeltaEmitter {
private:
    static constexpr size_t MAX_CHUNKS = 20;  // Worst case: snapshot with 40 levels = ~18 chunks + buffer
    boost::container::static_vector<DeltaChunk, MAX_CHUNKS> chunks_;
    size_t current_offset_;  // Offset into chunks_.back().payload
    Token token_;
    uint16_t record_idx_;
    
    template<typename DeltaT>
    void append_delta(const DeltaT& delta) {
        // Ensure we have a chunk to work with
        if (chunks_.empty() || current_offset_ + sizeof(DeltaT) > 56) [[unlikely]] {
            // Start new chunk (default initialized to zeros)
            chunks_.emplace_back();
            DeltaChunk& chunk = chunks_.back();
            chunk.token = token_;
            chunk.record_idx = record_idx_;
            current_offset_ = 0;
        }
        
        // Build delta directly in vector's last chunk
        DeltaChunk& chunk = chunks_.back();
        memcpy(&chunk.payload[current_offset_], &delta, sizeof(DeltaT));
        current_offset_ += sizeof(DeltaT);
        chunk.num_deltas++;
    }
    
public:
    DeltaEmitter() : current_offset_(0), token_(0), record_idx_(0) {}
    
    void set_event(Token token, uint16_t record_idx) {
        token_ = token;
        record_idx_ = record_idx;
    }
    
    void emit_tick_info(char tick_type, bool is_ask, bool is_exch, Price price, Qty qty) {
        TickInfoDelta delta;
        delta.type = DeltaType::TickInfo;
        delta.tick_type = tick_type;
        delta.exch_side_flags = (is_exch ? 0x01 : 0x00) | (is_ask ? 0x02 : 0x00);
        delta.reserved = 0;
        delta.price = price;
        delta.qty = qty;
        append_delta(delta);
    }
    
    void emit_update(bool is_ask, uint8_t index, int64_t qty_delta, int16_t count_delta) {
        // TickInfo must be first delta in sequence
        assert(!chunks_.empty() && 
               "emit_tick_info() must be called before emit_update()");
        
        // Only emit if index is in top 20 levels
        if (index >= 20) return;
        
        UpdateDelta delta;
        delta.type = DeltaType::Update;
        delta.side_index = pack_side_index(is_ask, index);
        delta.count_delta = count_delta;
        delta.qty_delta = qty_delta;
        append_delta(delta);
    }
    
    void emit_insert(bool is_ask, uint8_t index, bool shift, Price price, int64_t qty, int32_t count) {
        // TickInfo must be first delta in sequence
        assert(!chunks_.empty() && 
               "emit_tick_info() must be called before emit_insert()");
        
        // Only emit if index is in top 20 levels
        if (index >= 20) return;
        
        InsertDelta delta;
        delta.type = DeltaType::Insert;
        delta.side_index_shift = pack_side_index_shift(is_ask, index, shift);
        delta.reserved = 0;
        delta.count = count;
        delta.price = price;
        delta.qty = qty;
        append_delta(delta);
    }
    
    void finalize() {
        // Mark last chunk as final
        if (!chunks_.empty()) {
            chunks_.back().flags = 1;
        }
    }
    
    std::span<const DeltaChunk> get_chunks() const {
        return std::span<const DeltaChunk>(chunks_.data(), chunks_.size());
    }
    
    void clear() {
        chunks_.clear();
        current_offset_ = 0;
    }
};

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
    using Comparator = typename std::conditional<IsAsk, std::less<Price>, std::greater<Price>>::type;
    using MapType = boost::container::flat_map<Price, pair<AggQty, Count>, Comparator>;

    PriceLevels() : emitter_(nullptr) {
        levels_.reserve(1000);
    }
    
    void set_emitter(DeltaEmitter* e) {
        emitter_ = e;
    }

    void add(Price p, Qty qty, Count count_delta) {
        auto it = levels_.lower_bound(p);
        bool inserted = (it == levels_.end() || it->first != p);
        
        if (inserted) {
            it = levels_.emplace_hint(it, p, std::make_pair(qty, count_delta));
        } else {
            it->second.first += qty;
            it->second.second += count_delta;
        }
        
        int idx = it - levels_.begin();
        
        // Emit delta
        if (inserted) {
            bool shift = (idx < (int)levels_.size() - 1);
            emitter_->emit_insert(IsAsk, idx, shift, p, it->second.first, it->second.second);
        } else {
            emitter_->emit_update(IsAsk, idx, qty, count_delta);
        }
        
        // Handle deletion (invariant: if count == 0, qty must be 0)
        if (it->second.first <= 0) {
            assert(it->second.second <= 0 && "qty=0 implies count=0");
            levels_.erase(it);
            
            // Emit refill if needed (level 20 now visible)
            if (levels_.size() >= 20) {
                auto refill = levels_.begin() + 19;
                emitter_->emit_insert(IsAsk, 19, false, refill->first, 
                                     refill->second.first, refill->second.second);
            }
        }
    }

    void remove(Price p, Qty qty, Count count_delta) {
        auto it = levels_.find(p);
        if (it == levels_.end()) return;
        
        int idx = it - levels_.begin();
        
        it->second.first -= qty;
        it->second.second -= count_delta;
        
        // Emit update delta
        emitter_->emit_update(IsAsk, idx, -qty, -count_delta);
        
        // Handle deletion (invariant: if count == 0, qty must be 0)
        if (it->second.first <= 0) {
            assert(it->second.second <= 0 && "qty=0 implies count=0");
            levels_.erase(it);
            
            // Emit refill if needed (level 20 now visible)
            if (levels_.size() >= 20) {
                auto refill = levels_.begin() + 19;
                emitter_->emit_insert(IsAsk, 19, false, refill->first, 
                                     refill->second.first, refill->second.second);
            }
        }
    }

    // Returns: vector of {price, qty_consumed}
    vector<pair<Price, Qty>> cross(Price target, Qty fill_qty) {
        vector<pair<Price, Qty>> consumed;
        Qty remaining = fill_qty;

        while (remaining > 0 && !levels_.empty()) {
            Price best = best_price();
            bool crosses = false;
            if constexpr (IsAsk) crosses = (best <= target);
            else crosses = (best >= target);

            if (!crosses) break;

            auto it = levels_.find(best);
            Qty consume_qty = min(remaining, (Qty)it->second.first);
            consumed.push_back({best, consume_qty});
            
            it->second.first -= consume_qty;
            remaining -= consume_qty;
            
            if (it->second.first <= 0) {
                levels_.erase(it);
            }
        }
        return consumed;
    }

    int get_top_levels(OutputLevel* out, int n) const {
        if (levels_.empty()) return 0;

        int count = min((int)levels_.size(), n);
        auto it = levels_.begin();
        for (int i = 0; i < count; ++i, ++it) {
            out[i].price = it->first;
            out[i].qty = (int32_t)it->second.first;
            out[i].num_orders = it->second.second;
        }
        return count;
    }

    Price best_price() const {
        if (levels_.empty()) return 0;
        return levels_.begin()->first;
    }

    // Get level index for a price (0-based), returns -1 if not found
    int get_level_index(Price p) const {
        auto it = levels_.find(p);
        if (it == levels_.end()) return -1;
        return it - levels_.begin();
    }

private:
    MapType levels_;
    DeltaEmitter* emitter_;
};

// --- MBO ---
class MBO {
public:
    MBO(Token token) : token_(token), last_affected_bid_price_(0), last_affected_ask_price_(0) {
        order_map_.reserve(10000);
        memset(&book_, 0, sizeof(OutputRecord));
        
        // Wire up delta emission
        bids_.set_emitter(&emitter_);
        asks_.set_emitter(&emitter_);
    }

    void new_order(OrderId id, bool is_ask, Price price, Qty qty);
    void modify_order(OrderId id, Price new_price, Qty new_qty);
    void cancel_order(OrderId id);
    void trade(OrderId id1, OrderId id2, Price price, Qty fill_qty);

    bool has_crossed() const;
    void infer_and_apply_cross();

    const OutputRecord& get_book() const { return book_; }
    void update_book(const InputRecord& event);
    
    std::span<const DeltaChunk> get_delta_chunks() const {
        return emitter_.get_chunks();
    }
    
    void prepare_deltas(Token token, uint16_t record_idx) {
        emitter_.clear();
        emitter_.set_event(token, record_idx);
    }
    
    void finalize_deltas() {
        emitter_.finalize();
    }

private:
    Token token_;
    DeltaEmitter emitter_;
    PriceLevels<false> bids_;
    PriceLevels<true> asks_;
    boost::unordered_map<OrderId, OrderInfo> order_map_;
    deque<OrderId> pending_cross_fills_;
    OutputRecord book_;
    
    // Track last affected levels (0 = not affected)
    Price last_affected_bid_price_;
    Price last_affected_ask_price_;
};

void MBO::new_order(OrderId id, bool is_ask, Price price, Qty qty) {
    if (id == 0) return;
    
    // Emit TickInfo delta
    emitter_.emit_tick_info('N', is_ask, true, price, qty);
    
    order_map_[id] = {is_ask, price, qty};
    if (is_ask) {
        asks_.add(price, qty, 1);
        last_affected_ask_price_ = price;
        last_affected_bid_price_ = 0;
    } else {
        bids_.add(price, qty, 1);
        last_affected_bid_price_ = price;
        last_affected_ask_price_ = 0;
    }
    
    if (g_crossing_enabled) {
        infer_and_apply_cross();
    }
}

void MBO::modify_order(OrderId id, Price new_price, Qty new_qty) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    
    // Emit TickInfo delta
    emitter_.emit_tick_info('M', info.is_ask, true, new_price, new_qty);
    if (info.price != new_price) {
        // Price changed - emit add before remove so first delta reflects new resting level
        if (info.is_ask) {
            asks_.add(new_price, new_qty, 1);
            asks_.remove(info.price, info.qty, 1);
            last_affected_ask_price_ = new_price;
            last_affected_bid_price_ = 0;
        } else {
            bids_.add(new_price, new_qty, 1);
            bids_.remove(info.price, info.qty, 1);
            last_affected_bid_price_ = new_price;
            last_affected_ask_price_ = 0;
        }
    } else {
        // Same price, qty change
        Qty delta = new_qty - info.qty;
        if (info.is_ask) {
            asks_.add(info.price, delta, 0);
            last_affected_ask_price_ = info.price;
            last_affected_bid_price_ = 0;
        } else {
            bids_.add(info.price, delta, 0);
            last_affected_bid_price_ = info.price;
            last_affected_ask_price_ = 0;
        }
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
    
    // Emit TickInfo delta
    emitter_.emit_tick_info('X', info.is_ask, true, info.price, info.qty);
    if (info.is_ask) {
        asks_.remove(info.price, info.qty, 1);
        last_affected_ask_price_ = info.price;
        last_affected_bid_price_ = 0;
    } else {
        bids_.remove(info.price, info.qty, 1);
        last_affected_bid_price_ = info.price;
        last_affected_ask_price_ = 0;
    }
    
    order_map_.erase(it);
}

void MBO::trade(OrderId id1, OrderId id2, Price price, Qty fill_qty) {
    // Track which sides are filled to determine passive/aggressor
    bool bid_filled = false;
    bool ask_filled = false;
    
    // First pass: determine which sides are involved
    for (OrderId id : {id1, id2}) {
        if (id == 0) continue;
        if (!pending_cross_fills_.empty() && pending_cross_fills_.front() == id) continue;
        auto it = order_map_.find(id);
        if (it != order_map_.end()) {
            if (it->second.is_ask) ask_filled = true;
            else bid_filled = true;
        }
    }
    
    // Determine side for trade: if only bid filled, aggressor is ask (and vice versa)
    bool trade_side_is_ask = bid_filled && !ask_filled;
    
    // Emit TickInfo delta FIRST (for trades, price/qty become LTP/LTQ)
    emitter_.emit_tick_info('T', trade_side_is_ask, true, price, fill_qty);
    
    // Reset affected prices
    last_affected_bid_price_ = 0;
    last_affected_ask_price_ = 0;
    
    // Second pass: apply the fills
    for (OrderId id : {id1, id2}) {
        if (id == 0) continue;

        if (!pending_cross_fills_.empty() && pending_cross_fills_.front() == id) {
            pending_cross_fills_.pop_front();
            continue;
        }

        auto it = order_map_.find(id);
        if (it == order_map_.end()) continue;

        OrderInfo& info = it->second;
        
        // Track affected price
        if (info.is_ask) {
            last_affected_ask_price_ = info.price;
        } else {
            last_affected_bid_price_ = info.price;
        }
        
        bool fully_filled = (fill_qty >= info.qty);
        Count count_delta = fully_filled ? 1 : 0;

        if (info.is_ask) asks_.remove(info.price, fill_qty, count_delta);
        else bids_.remove(info.price, fill_qty, count_delta);

        if (fully_filled) {
            order_map_.erase(it);
        } else {
            info.qty -= fill_qty;
        }
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

void MBO::update_book(const InputRecord& event) {
    book_.record_idx = event.record_idx;
    book_.token = token_;
    book_.event = event;
    
    // For trades, determine side from which orders were filled (set in trade())
    // For other operations, use event.is_ask
    if (event.tick_type == 'T') {
        // For trades: if only bid filled, aggressor is ask
        bool bid_filled = (last_affected_bid_price_ != 0);
        bool ask_filled = (last_affected_ask_price_ != 0);
        book_.is_ask = bid_filled && !ask_filled;
        
        // Set LTP/LTQ from event
        book_.ltp = event.price;
        book_.ltq = event.qty;
    } else {
        book_.is_ask = event.is_ask;
    }
    
    // Check bid side
    if (last_affected_bid_price_ != 0) {
        int bid_idx = bids_.get_level_index(last_affected_bid_price_);
        if (bid_idx < 0) {
            book_.bid_affected_lvl = 0;  // Deleted
        } else if (bid_idx >= 20) {
            book_.bid_affected_lvl = 20;  // Outside top 20 = not affected
        } else {
            book_.bid_affected_lvl = bid_idx;
        }
    } else {
        book_.bid_affected_lvl = 20;  // Not affected
    }
    
    // Check ask side
    if (last_affected_ask_price_ != 0) {
        int ask_idx = asks_.get_level_index(last_affected_ask_price_);
        if (ask_idx < 0) {
            book_.ask_affected_lvl = 0;  // Deleted
        } else if (ask_idx >= 20) {
            book_.ask_affected_lvl = 20;  // Outside top 20 = not affected
        } else {
            book_.ask_affected_lvl = ask_idx;
        }
    } else {
        book_.ask_affected_lvl = 20;  // Not affected
    }
    
    memset(book_.bids, 0, sizeof(book_.bids));
    memset(book_.asks, 0, sizeof(book_.asks));
    book_.bid_filled_lvls = bids_.get_top_levels(book_.bids, 20);
    book_.ask_filled_lvls = asks_.get_top_levels(book_.asks, 20);
}

// --- Delta Reconstruction (for validation) ---
void apply_deltas_to_book(OutputRecord& rec, std::span<const DeltaChunk> chunks) {
    // Track first delta index on each side for affected_lvl reconstruction
    int8_t bid_affected_lvl = 20;  // 20 = not affected
    int8_t ask_affected_lvl = 20;
    bool bid_seen = false;
    bool ask_seen = false;
    
    // Process all chunks
    for (const auto& chunk : chunks) {
        rec.record_idx = chunk.record_idx;
        rec.token = chunk.token;
        
        size_t offset = 0;
        for (uint8_t i = 0; i < chunk.num_deltas && offset < 56; ++i) {
            uint8_t dtype = chunk.payload[offset];
            
            if (dtype == DeltaType::TickInfo) {
                const TickInfoDelta* delta = reinterpret_cast<const TickInfoDelta*>(&chunk.payload[offset]);
                
                // Copy event metadata
                rec.event.tick_type = delta->tick_type;
                rec.event.is_ask = (delta->exch_side_flags >> 1) & 1;
                rec.event.price = delta->price;
                rec.event.qty = delta->qty;
                rec.is_ask = rec.event.is_ask;
                
                // For trades, extract LTP/LTQ
                if (delta->tick_type == 'T') {
                    rec.ltp = delta->price;
                    rec.ltq = delta->qty;
                }
                
                offset += sizeof(TickInfoDelta);
                
            } else if (dtype == DeltaType::Update) {
                const UpdateDelta* delta = reinterpret_cast<const UpdateDelta*>(&chunk.payload[offset]);
                bool is_ask = unpack_side(delta->side_index);
                uint8_t idx = unpack_index(delta->side_index);
                
                OutputLevel* book = is_ask ? rec.asks : rec.bids;
                
                // Track first delta on each side for affected_lvl
                if (!is_ask && !bid_seen) {
                    bid_seen = true;
                    // Check if this update causes deletion
                    if (book[idx].qty + delta->qty_delta <= 0) {
                        bid_affected_lvl = 0;  // Deletion
                    } else {
                        bid_affected_lvl = idx;
                    }
                }
                if (is_ask && !ask_seen) {
                    ask_seen = true;
                    if (book[idx].qty + delta->qty_delta <= 0) {
                        ask_affected_lvl = 0;  // Deletion
                    } else {
                        ask_affected_lvl = idx;
                    }
                }
                
                // Apply update
                book[idx].qty += delta->qty_delta;
                book[idx].num_orders += delta->count_delta;
                
                // Handle implicit deletion
                if (book[idx].qty <= 0) {
                    memmove(&book[idx], &book[idx+1], (19-idx) * sizeof(OutputLevel));
                    memset(&book[19], 0, sizeof(OutputLevel));
                }
                
                offset += sizeof(UpdateDelta);
                
            } else if (dtype == DeltaType::Insert) {
                const InsertDelta* delta = reinterpret_cast<const InsertDelta*>(&chunk.payload[offset]);
                bool is_ask = unpack_side(delta->side_index_shift);
                uint8_t idx = unpack_index(delta->side_index_shift);
                bool shift = unpack_shift(delta->side_index_shift);
                
                OutputLevel* book = is_ask ? rec.asks : rec.bids;
                
                // Track first delta on each side for affected_lvl
                if (!is_ask && !bid_seen) {
                    bid_seen = true;
                    bid_affected_lvl = idx;
                }
                if (is_ask && !ask_seen) {
                    ask_seen = true;
                    ask_affected_lvl = idx;
                }
                
                // Apply insert
                if (shift) {
                    // Shift levels down before inserting
                    memmove(&book[idx+1], &book[idx], (19-idx) * sizeof(OutputLevel));
                }
                
                book[idx].price = delta->price;
                book[idx].qty = delta->qty;
                book[idx].num_orders = delta->count;
                
                offset += sizeof(InsertDelta);
                
            } else {
                // Unknown delta type, skip
                break;
            }
        }
    }
    
    // Set affected_lvl fields
    rec.bid_affected_lvl = bid_affected_lvl;
    rec.ask_affected_lvl = ask_affected_lvl;
    
    // Count filled levels
    rec.bid_filled_lvls = 0;
    rec.ask_filled_lvls = 0;
    for (int i = 0; i < 20; ++i) {
        if (rec.bids[i].price != 0) rec.bid_filled_lvls++;
        if (rec.asks[i].price != 0) rec.ask_filled_lvls++;
    }
}

// --- Runner ---
class Runner {
public:
    Runner() {
        mbos_.reserve(100);
        reconstructed_books_.reserve(100);
    }
    const OutputRecord& process_record(const InputRecord& rec);

private:
    boost::unordered_map<Token, unique_ptr<MBO>> mbos_;
    boost::unordered_map<Token, OutputRecord> reconstructed_books_;  // For delta validation
};

const OutputRecord& Runner::process_record(const InputRecord& rec) {
    PerfProfile("process_record");
    PerfProfileCount("records_processed", 1);

    rec.print();

    Token token = rec.token;
    auto it = mbos_.find(token);
    if (it == mbos_.end()) {
        it = mbos_.emplace(token, make_unique<MBO>(token)).first;
    }

    MBO& mbo = *it->second;
    
    // Prepare delta emission
    mbo.prepare_deltas(token, rec.record_idx);
    
    // Dispatch to appropriate operation
    switch (rec.tick_type) {
        case 'N': mbo.new_order(rec.order_id, rec.is_ask, rec.price, rec.qty); break;
        case 'M': mbo.modify_order(rec.order_id, rec.price, rec.qty); break;
        case 'X': mbo.cancel_order(rec.order_id); break;
        case 'T': mbo.trade(rec.order_id, rec.order_id2, rec.price, rec.qty); break;
    }
    
    // Update book snapshot and finalize deltas
    mbo.update_book(rec);
    mbo.finalize_deltas();
    
    // Get delta chunks and print them
    auto chunks = mbo.get_delta_chunks();
    for (const auto& chunk : chunks) {
        std::cout << "  " << chunk << "\n";
    }
    
    // Validate: apply deltas to reconstructed book and compare
    auto& reconstructed = reconstructed_books_[token];
    apply_deltas_to_book(reconstructed, chunks);
    const OutputRecord& direct = mbo.get_book();
    
    if (!reconstructed.compare(direct)) {
        printf("\n*** DELTA RECONSTRUCTION MISMATCH ***\n");
        printf("Record: %u, Token: %u\n", rec.record_idx, token);
        printf("\nDIRECT (from MBO):\n");
        direct.print();
        printf("\nRECONSTRUCTED (from deltas):\n");
        reconstructed.print();
        printf("\nDelta chunks:\n");
        for (const auto& chunk : chunks) {
            std::cout << "  " << chunk << "\n";
        }
        fflush(stdout);
        exit(1);
    }
    
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

    const OutputRecord* ref_books = nullptr;
    void* ref_mapped = nullptr;
    size_t num_ref_books = 0;
    if (reference_file) {
        int rfd = open(reference_file, O_RDONLY);
        if (rfd >= 0) {
            struct stat rsb;
            fstat(rfd, &rsb);
            ref_mapped = mmap(nullptr, rsb.st_size, PROT_READ, MAP_PRIVATE, rfd, 0);
            if (ref_mapped != MAP_FAILED) {
                ref_books = static_cast<const OutputRecord*>(ref_mapped);
                num_ref_books = rsb.st_size / sizeof(OutputRecord);
            }
            close(rfd);
        }
    }

    Runner runner;
    for (size_t i = 0; i < num_records; ++i) {
        const OutputRecord& book = runner.process_record(records[i]);
        book.print();

        if (ref_books && i < num_ref_books) {
            if (!book.compare(ref_books[i])) {
                printf("MISMATCH at record %lu (idx: %u)\n", i, records[i].record_idx);
                printf("REFERENCE:\n");
                ref_books[i].print();
                fflush(stdout);
                while(1);
                return 1;
            }
        }
    }

    PerfProfilerReport();

    munmap(mapped, sb.st_size);
    if (ref_mapped && ref_mapped != MAP_FAILED) munmap(ref_mapped, num_ref_books * sizeof(OutputRecord));
    close(fd);
    return 0;
}

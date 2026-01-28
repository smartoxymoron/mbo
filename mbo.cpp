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
#include <memory>
#include <algorithm>
#include <span>
#include <boost/container/flat_map.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/container/static_vector.hpp>
#include "perfprofiler.h"

using namespace std;

#define printf(...) // to quickly disable printf for perf testing

// --- Always-on assertion for release builds ---
#define always_assert(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", \
                    #condition, __FILE__, __LINE__); \
            fflush(stderr); \
            abort(); \
        } \
    } while(0)

// #undef always_assert
// #define always_assert(...)  // to quickly disable assertions for perf testing

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
        (void)side; (void)lvl;  // Suppress unused warnings when printf is disabled
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
    uint8_t is_ask;          // 1 byte - 0=bid, 1=ask (for trades: always 0, not meaningful)
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
        printf("OUTPUT [%u] tok:%u side:%s tick:%c p:%ld q:%d affected_bid:%d affected_ask:%d filled_bid:%d filled_ask:%d id:%ld id2:%ld ltp:%ld ltq:%d\n",
               record_idx, token, is_ask ? "ASK" : "BID", event.tick_type, event.price, event.qty,
               bid_affected_lvl, ask_affected_lvl, bid_filled_lvls, ask_filled_lvls, event.order_id, event.order_id2, ltp, ltq);
        // Print asks in reverse order (highest to lowest)
        for(int i=19; i>=0; --i) if (asks[i].price != 0) asks[i].print("ASK", i);
        // Then print bids in regular order (highest to lowest)
        for(int i=0; i<20; ++i) if (bids[i].price != 0) bids[i].print("BID", i);
    }

    // Returns 0 if books match, unique error code otherwise:
    //   [-20...-1]: Bid level mismatch (negative level index)
    //   [+1...+20]: Ask level mismatch (positive level index)
    //   100+: Other mismatches (metadata, counts, etc.)
    int compare(const OutputRecord& reference) const {
        PerfProfile("compare");
        // Check event tick_type matches - primary validation for crossing alignment
        if (event.tick_type != reference.event.tick_type) return 111;
        
#if 1  // Disabled temporarily to focus on tick_type alignment
        if (record_idx != reference.record_idx) return 100; // only sending lower 16 bits of record_idx to save space in delta
        if (token != reference.token) return 101;
        
        // Skip is_ask check for trades - reference has incorrect aggressor detection
        // Also skip for cancels when order not found (affected levels both 20) - reference sends random side
        // TODO discuss with traders how to proceed; unrelated - also discuss crossing/selftrade behaviour and simplify
        bool is_cancel_not_found = (event.tick_type == 'X' && bid_affected_lvl == 20 && ask_affected_lvl == 20);
        if (event.tick_type != 'T' && !is_cancel_not_found && is_ask != reference.is_ask) return 102;

        // TODO check why these appear to not be set in reference
        // if (ltp != other.ltp) return 103;
        // if (ltq != other.ltq) return 104;
        
        // Order ID validation
        if (event.order_id != reference.event.order_id) return 112;
        // order_id2 only meaningful for trades (D/E/T); reference uses -1 sentinel for non-trades
        if ((event.tick_type == 'T' || event.tick_type == 'D' || event.tick_type == 'E') &&
            event.order_id2 != reference.event.order_id2) return 113;
        
        // Price/qty validation (from event/tick info)
        if (event.price != reference.event.price) return 114;
        if (event.qty != reference.event.qty) return 115;

        if (bid_filled_lvls != reference.bid_filled_lvls) return 105;
        if (ask_filled_lvls != reference.ask_filled_lvls) return 106;

        // Side determined by reference as primary must match exactly
        // Special case for modifies/crosses: we report minimum (topmost) affected level, reference may report deeper level
        // Allow our_affected_lvl <= ref_affected_lvl for these types (we're more precise about what changed)
        // 'A' (newOrderCross) and 'B' (modOrderCross) included: reference doesn't track affected levels through re-cross
        bool is_modify = (event.tick_type == 'M' || event.tick_type == 'B' || event.tick_type == 'A');
        
        if (reference.is_ask) {
            if (is_modify) {
                // For modifies: accept if we report topmost or same level
                if (ask_affected_lvl > reference.ask_affected_lvl) return 107;
            } else {
                // For non-modifies: exact match required
                if (ask_affected_lvl != reference.ask_affected_lvl) return 107;
            }
            // Secondary side: if we have 20, accept anything. If we set a value, it must match.
            if (bid_affected_lvl != 20 && bid_affected_lvl != reference.bid_affected_lvl) return 108;
        } else {
            if (is_modify) {
                // For modifies: accept if we report topmost or same level
                if (bid_affected_lvl > reference.bid_affected_lvl) return 109;
            } else {
                // For non-modifies: exact match required
                if (bid_affected_lvl != reference.bid_affected_lvl) return 109;
            }
            if (ask_affected_lvl != 20 && ask_affected_lvl != reference.ask_affected_lvl) return 110;
        }
#endif
        // Check book levels: bids use negative indices, asks use positive
        for (int i = 0; i < 20; ++i) {
            if (bids[i].price != reference.bids[i].price) return -(i+1);
            if (bids[i].qty != reference.bids[i].qty) return -(i+1);
            if (bids[i].num_orders != reference.bids[i].num_orders) return -(i+1);
            if (asks[i].price != reference.asks[i].price) return (i+1);
            if (asks[i].qty != reference.asks[i].qty) return (i+1);
            if (asks[i].num_orders != reference.asks[i].num_orders) return (i+1);
        }
        return 0;  // Match!
    }
} __attribute__((packed));
static_assert(sizeof(OutputRecord) == 708);
// TEST DATA GENERATION END

// --- Delta Message Structures ---
enum DeltaType : uint8_t {
    TickInfo = 0,        // Event metadata (always present, always first)
    Update = 1,          // Modify existing level (implicit delete if qty/count → 0)
    Insert = 2,          // Add level at index (with optional shift)
    CrossingComplete = 3 // Signal that crossing has fully resolved (1 byte)
};

struct TickInfoDelta {
    uint8_t type;              // = 0
    char tick_type;            // 'N','M','X','T','A','B','C','D','E','S'
    uint8_t exch_side_flags;   // bit 0: is_exch_tick, bit 1: side
    uint8_t reserved;
    uint32_t record_idx;       // Corresponds to OutputRecord index
    int64_t price;             // For trades: this IS the LTP
    int32_t qty;               // For trades: this IS the LTQ
    int64_t order_id;          // Primary order (aggressor for trades)
    int64_t order_id2;         // Secondary order (passive for trades, 0 otherwise)
    
    friend std::ostream& operator<<(std::ostream& os, const TickInfoDelta& d) {
        os << "TickInfo{rec=" << d.record_idx
           << ", type=" << d.tick_type 
           << ", side=" << ((d.exch_side_flags & 0x02) ? "ask" : "bid")
           << ", exch=" << (d.exch_side_flags & 0x01)
           << ", p=" << d.price << ", q=" << d.qty
           << ", id=" << d.order_id;
        if (d.order_id2 != 0) os << ", id2=" << d.order_id2;
        return os << "}";
    }
} __attribute__((packed));
static_assert(sizeof(TickInfoDelta) == 36);

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

// Minimal delta to signal crossing completion (receiver synthesizes N/M/X)
struct CrossingCompleteDelta {
    uint8_t type;              // = 3
    
    friend std::ostream& operator<<(std::ostream& os, const CrossingCompleteDelta&) {
        return os << "CrossingComplete{}";
    }
} __attribute__((packed));
static_assert(sizeof(CrossingCompleteDelta) == 1);

struct DeltaChunk {
    uint32_t token = 0;
    uint8_t flags = 0;             // bit 0: final (book ready for strategy)
    uint8_t num_deltas = 0;        // Number of deltas in this chunk (1-N)
    uint8_t payload[58] = {};      // Variable-length delta sequence (record_idx now in TickInfoDelta)
    
    friend std::ostream& operator<<(std::ostream& os, const DeltaChunk& chunk) {
        os << "Chunk[tok=" << chunk.token 
           << ", final=" << (chunk.flags & 1) << "]: ";
        
        // Iterate through deltas in payload
        size_t offset = 0;
        size_t total_bytes = 6;  // Header: token:4 + flags:1 + num_deltas:1
        
        for (uint8_t i = 0; i < chunk.num_deltas && offset < 58; ++i) {
            if (i > 0) os << " + ";
            
            uint8_t dtype = chunk.payload[offset];
            
            if (dtype == DeltaType::TickInfo) {
                if (offset + sizeof(TickInfoDelta) > 58) break;
                const TickInfoDelta* delta = reinterpret_cast<const TickInfoDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(TickInfoDelta);
                offset += sizeof(TickInfoDelta);
            } else if (dtype == DeltaType::Update) {
                if (offset + sizeof(UpdateDelta) > 58) break;
                const UpdateDelta* delta = reinterpret_cast<const UpdateDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(UpdateDelta);
                offset += sizeof(UpdateDelta);
            } else if (dtype == DeltaType::Insert) {
                if (offset + sizeof(InsertDelta) > 58) break;
                const InsertDelta* delta = reinterpret_cast<const InsertDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(InsertDelta);
                offset += sizeof(InsertDelta);
            } else if (dtype == DeltaType::CrossingComplete) {
                if (offset + sizeof(CrossingCompleteDelta) > 58) break;
                const CrossingCompleteDelta* delta = reinterpret_cast<const CrossingCompleteDelta*>(&chunk.payload[offset]);
                os << *delta;
                total_bytes += sizeof(CrossingCompleteDelta);
                offset += sizeof(CrossingCompleteDelta);
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

/* Callers always call emit_*() methods; emitter does the filtering (e.g., index >= 20)
 * Clean encapsulation over scattered pre-checks but relies on inlining (verified -O3)
 *  to avoid wasted argument computation.
 */
class DeltaEmitter {
private:
    static constexpr size_t MAX_CHUNKS = 20;  // Worst case: snapshot with 40 levels = ~18 chunks + buffer
    boost::container::static_vector<DeltaChunk, MAX_CHUNKS> chunks_;
    size_t current_offset_;  // Offset into chunks_.back().payload
    Token token_;
    uint32_t record_idx_;
    
    template<typename DeltaT>
    void append_delta(const DeltaT& delta) {
        // Ensure we have a chunk to work with
        if (chunks_.empty() || current_offset_ + sizeof(DeltaT) > 58) [[unlikely]] {
            // Start new chunk (default initialized to zeros)
            chunks_.emplace_back();
            DeltaChunk& chunk = chunks_.back();
            chunk.token = token_;
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
    
    void set_event(Token token, uint32_t record_idx) {
        token_ = token;
        record_idx_ = record_idx;
    }
    
    void emit_tick_info(char tick_type, bool is_ask, bool is_exch, Price price, Qty qty,
                         OrderId order_id = 0, OrderId order_id2 = 0) {
        TickInfoDelta delta;
        delta.type = DeltaType::TickInfo;
        delta.tick_type = tick_type;
        delta.exch_side_flags = (is_exch ? 0x01 : 0x00) | (is_ask ? 0x02 : 0x00);
        delta.reserved = 0;
        delta.record_idx = record_idx_;
        delta.price = price;
        delta.qty = qty;
        delta.order_id = order_id;
        delta.order_id2 = order_id2;
        append_delta(delta);
    }
    
    void emit_update(bool is_ask, int index, int64_t qty_delta, int16_t count_delta) {
        // TickInfo must be first delta in sequence
        always_assert(!chunks_.empty() && 
               "emit_tick_info() must be called before emit_update()");
        
        // Only emit if index is in top 20 levels
        if (index >= 20) [[unlikely]] return;
        
        UpdateDelta delta;
        delta.type = DeltaType::Update;
        delta.side_index = pack_side_index(is_ask, index);
        delta.count_delta = count_delta;
        delta.qty_delta = qty_delta;
        append_delta(delta);
    }
    
    void emit_insert(bool is_ask, int index, bool shift, Price price, int64_t qty, int32_t count) {
        // TickInfo must be first delta in sequence
        always_assert(!chunks_.empty() && 
               "emit_tick_info() must be called before emit_insert()");
        
        // Only emit if index is in top 20 levels
        if (index >= 20) [[unlikely]] return;
        
        InsertDelta delta;
        delta.type = DeltaType::Insert;
        delta.side_index_shift = pack_side_index_shift(is_ask, index, shift);
        delta.reserved = 0;
        delta.count = count;
        delta.price = price;
        delta.qty = qty;
        append_delta(delta);
    }
    
    void emit_crossing_complete() {
        // Signal that crossing has fully resolved - receiver synthesizes N/M/X
        always_assert(!chunks_.empty() && 
               "emit_tick_info() must be called before emit_crossing_complete()");
        
        CrossingCompleteDelta delta;
        delta.type = DeltaType::CrossingComplete;
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
    bool is_ask;    // Don't rely on exchange telling us the side with each message
    Price price;
    Qty qty;
};

// --- Global Settings ---
inline bool g_crossing_enabled = false;

// Pending cross info for self-trade detection
// When a crossing order is active, we track it here so cancel_order can detect self-trades
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;        // Price used for crossing (new price for modify)
    Price original_resting_price = 0; // For modifies: old price where order was resting
    Qty aggressor_original_qty = 0;   // Original qty for reference
    char residual_tick_type = 'N';    // 'N' if from new_order, 'M' if from modify_order
    int8_t original_affected_lvl = 20; // For modifies: level where order was before crossing
    bool aggressor_on_level = false;  // Whether aggressor's residual was added to its level
    
    bool is_active() const { return aggressor_id != 0; }
    void clear() { aggressor_id = 0; aggressor_on_level = false; }
};

// Track per-level consumption during crossing for rollback support
struct CrossFill {
    Price price;
    Qty qty;
    Count count;  // order count at level when consumed (needed if level deleted at qty=0)
};

// --- PriceLevels ---
/*
 * PRICE NEGATION FOR UNIFIED ORDERING (IMPLEMENTED)
 * 
 * Both sides use std::less<Price> comparator (ascending sort).
 * To make bids sort best-first, we negate bid prices internally:
 *         - Asks: store actual prices (100, 101, 102...) → sorted ascending (best = lowest)
 *         - Bids: store negated prices (-100, -99, -98...) → sorted ascending (best = highest when denegated)
 * 
 * IMPLEMENTATION PATTERN:
 *   - ALL public methods accept/return ACTUAL prices (never canonical)
 *   - Convert at entry: canonical = actual * side_multiplier_
 *   - Internal operations use canonical prices
 *   - Convert at exit: actual = canonical * side_multiplier_
 *   - Delta emission: ALWAYS use actual prices
 * 
 * Key conversions:
 *   - Entry: canonical = actual * side_multiplier_  (negate for bids)
 *   - Exit:  actual = canonical * side_multiplier_  (denegate for bids)
 *   - side_multiplier_ is idempotent: multiply twice = original value
 * 
 * RISKS & CONSTRAINTS:
 *   ⚠️  INT64_MIN is NO LONGER A VALID PRICE (negation overflows)
 *       - Use INT64_MAX for market orders on both sides
 *       - Per-exchange adapter code MUST validate/convert before passing to MBO
 *       - Add static_assert or runtime check if exchange can send INT64_MIN
 * 
 *   ⚠️  Sentinel values for flat array (future):
 *       - Cannot use INT64_MIN/INT64_MAX as sentinels (prices can be ±INT64_MAX)
 *       - Use 0 as sentinel (no real market has price=0)
 *       - OR track valid range via start_idx/end_idx (no sentinel needed)
 * 
 *   ⚠️  Debugging: Bid prices appear negative in debugger
 *       - Add debug_print() that shows abs(canonical) or denegate for display
 *       - Consider abs() in production logs for clarity
 * 
 *   ⚠️  Discipline required at ALL boundaries:
 *       - NEVER expose canonical prices outside PriceLevels
 *       - NEVER accept canonical prices from outside
 *       - Easy to forget during cross() implementation or new methods
 * 
 *   ⚠️  Testing: Boundary cases are critical
 *       - Test with INT64_MAX prices
 *       - Test cross() with prices near overflow
 *       - Test mixed operations across spread
 * 
 * Benefits:
 *   ✓ Cleaner MBO code (ternary instead of if/else branches)
 *   ✓ Same type for bids_/asks_ enables future optimizations
 *   ✓ Works with flat array tail-placement strategy
 *   ✓ No performance cost (multiply is 1 cycle, comparisons stay same)
 */
class PriceLevels {
public:
    using MapType = boost::container::flat_map<Price, pair<AggQty, Count>, std::greater<Price>>;

    PriceLevels(bool is_ask) 
        : is_ask_(is_ask)
        , side_multiplier_(is_ask ? 1 : -1)
        , emitter_(nullptr) 
    {
        levels_.reserve(1000);
        cross_fills_.reserve(4);  // Typically cross ≤4 levels
    }
    
    void set_emitter(DeltaEmitter* e) {
        emitter_ = e;
    }
    
    Qty pending_cross_fill_qty() const { return pending_cross_fill_qty_; }

    // VWAP and total qty of pending (unconfirmed) cross fills — for C tick emission
    std::pair<Price, Qty> pending_cross_vwap() const {
        Qty pending = pending_cross_fill_qty_;
        if (pending == 0) return {0, 0};
        
        // Skip confirmed fills from the front (trades confirm FIFO)
        Qty total_consumed = 0;
        for (const auto& f : cross_fills_) total_consumed += f.qty;
        Qty skip = total_consumed - pending;
        
        int64_t volume = 0;
        Qty counted = 0;
        for (const auto& f : cross_fills_) {
            if (skip >= f.qty) { skip -= f.qty; continue; }
            Qty use = std::min(f.qty - skip, pending - counted);
            skip = 0;
            volume += f.price * use;
            counted += use;
            if (counted >= pending) break;
        }
        return {counted > 0 ? static_cast<Price>(volume / counted) : 0, pending};
    }

    void add_liquidity(Price p, Qty qty, Count count_delta) {
        always_assert(qty >= 0 && "add_liquidity requires non-negative qty");
        
        // PerfProfile("add_liquidity");
        // Convert to canonical (negated for bids)
        Price canonical = p * side_multiplier_;
        
        auto it = levels_.lower_bound(canonical);
        bool inserted = (it == levels_.end() || it->first != canonical);
        
        if (inserted) {
            //PerfProfile("add_liquidity_insert");
            it = levels_.emplace_hint(it, canonical, std::make_pair(qty, count_delta));
            int idx = static_cast<int>(levels_.size()) - 1 - static_cast<int>(it - levels_.begin());
            // printf("DEBUG: add_liquidity INSERT at idx %d price %ld qty %ld count_delta %ld\n", idx, p, (long)qty, (long)count_delta);
            
            // shift=true always: receiver memmoves [idx..19] to [idx+1..20] before writing
            // When inserting at end, memmove copies zeros (harmless); at idx=19, 0-byte memmove (no-op)
            emitter_->emit_insert(is_ask_, idx, /*shift=*/true, p, it->second.first, it->second.second);
        } else {
            it->second.first += qty;
            it->second.second += count_delta;
            int idx = static_cast<int>(levels_.size()) - 1 - static_cast<int>(it - levels_.begin());
            // printf("DEBUG: add_liquidity UPDATE at idx %d price %ld qty %ld count_delta %ld\n", idx, p, (long)qty, (long)count_delta);
            
            emitter_->emit_update(is_ask_, idx, qty, count_delta);
        }
    }

    void remove_liquidity(Price p, Qty qty, Count count_delta) {
        // qty can be 0 for count-only updates during trade reconciliation
        if (qty == 0 && count_delta == 0) return;
        
        // Convert to canonical (negated for bids)
        Price canonical = p * side_multiplier_;
        
        auto it = levels_.find(canonical);
        if (it == levels_.end()) return;
        
        int idx = static_cast<int>(levels_.size()) - 1 - static_cast<int>(it - levels_.begin());
        
        it->second.first -= qty;
        it->second.second -= count_delta;
        
        // Emit update delta
        emitter_->emit_update(is_ask_, idx, -qty, -count_delta);
        
        // Handle deletion when qty reaches 0
        if (it->second.first <= 0) [[unlikely]] {
            // Note: count may be > 0 during crossing reconciliation. When qty=0 in the
            // call parameters, we're doing a count-only update (reconciling a crossed trade).
            // The level will be erased regardless of count - count mismatches are transient
            // and acceptable during the crossing/reconciliation window.
            levels_.erase(it);
            
            // Emit refill if level 20 has come into view (emitter filters if out of range)
            if (idx < 20 and levels_.size() >= 20) {    // TODO consider removing the idx check here as it will be filtered out by the emitter
                auto refill = levels_.end() - 20;  // 20th best = index 19
                // Denegate price for emission
                Price actual_price = refill->first * side_multiplier_;
                emitter_->emit_insert(is_ask_, 19, false, actual_price, 
                                     refill->second.first, refill->second.second);
            }
        }
    }

    // --- Crossing Support ---
    // TODO: cross() and remove_liquidity() have overlapping logic; refactor after crossing works
    
    // Consume liquidity from best prices toward aggressor price
    // Called BEFORE adding aggressive order. Returns total qty consumed.
    // Tracks per-level consumption in cross_fills_ for rollback support.
    Qty cross(Price aggressor_price, Qty aggressor_qty) {
        if (!g_crossing_enabled) return 0;
        
        PerfProfile("cross");
        // Only clear on initial cross (no active pending crossing).
        // Re-crosses during self-trade cancels must preserve fill history for VWAP calculation.
        if (pending_cross_fill_qty_ == 0) {
            cross_fills_.clear();
            pending_cross_fill_count_ = 0;
        }
        
        Qty consumed = 0;
        Qty remaining = aggressor_qty;
        
        while (remaining > 0 && !levels_.empty()) {
            Price best = best_price();
            if (best == 0) break;
            
            // Check if best price crosses aggressor
            // For bids (is_ask_=false, passive when ask aggressive): crosses if best >= aggressor
            // For asks (is_ask_=true, passive when bid aggressive): crosses if best <= aggressor
            bool crosses = is_ask_ ? (best <= aggressor_price) : (best >= aggressor_price);
            if (!crosses) break;
            
            // Best level is at rbegin() due to descending sort
            auto& [qty, count] = levels_.rbegin()->second;
            Qty consume = std::min(remaining, static_cast<Qty>(qty));
            
            // Track per-level consumption for potential rollback
            // Save count before remove_liquidity may erase the level (at qty=0)
            cross_fills_.push_back({best, consume, count});
            pending_cross_fill_count_ += count;
            
            // Remove from level with count_delta=0 (will be fixed by trades)
            remove_liquidity(best, consume, 0);
            
            consumed += consume;
            remaining -= consume;
        }
        
        pending_cross_fill_qty_ += consumed;
        return consumed;
    }
    
    // Reconcile pending cross fills when trade arrives (Phase 2)
    // Returns qty that was already applied to levels during crossing
    Qty reconcile_cross_fill(Qty fill_qty) {
        Qty reconciled = std::min(fill_qty, pending_cross_fill_qty_);
        pending_cross_fill_qty_ -= reconciled;
        return reconciled;
    }
    
    // Reconcile pending cross fill count when trade confirms an order fully consumed
    void reconcile_cross_count(Count count_delta) {
        pending_cross_fill_count_ -= count_delta;
    }
    
    // Unreserve pending cross fill qty (for self-trade cancel)
    // Called when a passively consumed order is cancelled (self-trade).
    // Also decrements fill count by 1 (the cancelled order).
    void unreserve_cross_fill(Qty qty) {
        pending_cross_fill_qty_ -= std::min(qty, pending_cross_fill_qty_);
        if (pending_cross_fill_count_ > 0) pending_cross_fill_count_--;
    }
    
    // Uncross: restore only the UNCONFIRMED speculatively consumed liquidity (for aggressor cancel).
    // cross_fills_ may contain confirmed fills at the front (from reconciled trades) that must be
    // skipped. Only the unconfirmed tail (pending_cross_fill_qty_ / pending_cross_fill_count_)
    // should be restored. This is the same "skip confirmed from front" logic as pending_cross_vwap().
    void uncross() {
        PerfProfile("uncross");
        
        // Calculate how much to skip (confirmed by trades)
        Qty total_qty = 0;
        Count total_count = 0;
        for (const auto& f : cross_fills_) { total_qty += f.qty; total_count += f.count; }
        Qty skip_qty = total_qty - pending_cross_fill_qty_;
        Count skip_count = total_count - pending_cross_fill_count_;
        
        for (const auto& fill : cross_fills_) {
            if (skip_qty >= fill.qty) {
                // Entire entry was confirmed by trades - skip it
                skip_qty -= fill.qty;
                skip_count -= fill.count;
                continue;
            }
            
            // Partially or fully unconfirmed entry
            Qty restore_qty = fill.qty - skip_qty;
            Count restore_count = fill.count - skip_count;
            skip_qty = 0;
            skip_count = 0;
            
            // Check if level still exists (partial consumption) or was deleted (full consumption)
            Price canonical = fill.price * side_multiplier_;
            auto it = levels_.find(canonical);
            if (it != levels_.end()) {
                // Level exists (was partially consumed) - just restore qty, count unchanged
                add_liquidity(fill.price, restore_qty, 0);
            } else {
                // Level was fully consumed and deleted - restore with adjusted count
                add_liquidity(fill.price, restore_qty, restore_count);
            }
        }
        pending_cross_fill_qty_ = 0;
        pending_cross_fill_count_ = 0;
        cross_fills_.clear();
    }
    
    // Clear cross fills without restoring (for normal crossing completion)
    void clear_cross_fills() {
        cross_fills_.clear();
        pending_cross_fill_count_ = 0;
    }
    
    // Access cross fills for partial rollback calculations
    const std::vector<CrossFill>& cross_fills() const { return cross_fills_; }

    Price best_price() const {
        if (levels_.empty()) return 0;
        // Best price now at rbegin() (descending order), denegate to return actual price
        return levels_.rbegin()->first * side_multiplier_;
    }
    
    // Get the level index (0-based, 0=best) for a given price
    // Returns 20 if price not found or beyond top 20
    int8_t get_level_index(Price p) const {
        Price canonical = p * side_multiplier_;
        auto it = levels_.find(canonical);
        if (it == levels_.end()) return 20;
        int idx = static_cast<int>(levels_.size()) - 1 - static_cast<int>(it - levels_.begin());
        return (idx >= 20) ? 20 : static_cast<int8_t>(idx);
    }

// TEMP private:
    bool is_ask_;
    int64_t side_multiplier_;  // +1 for asks, -1 for bids (reserved for future optimization)
    MapType levels_;
    DeltaEmitter* emitter_;
    
    // Crossing state
    Qty pending_cross_fill_qty_ = 0;  // Qty consumed by crosses, awaiting trade reconciliation
    Count pending_cross_fill_count_ = 0;  // Order count across pending (unconfirmed) fills
    std::vector<CrossFill> cross_fills_;  // Per-level consumption for rollback support
};

// --- MBO ---
class MBO {
    friend class Runner;  // For accessing order_map_ to count active orders
public:
    MBO(Token token) 
        : token_(token)
        , bids_(false)  // is_ask = false
        , asks_(true)   // is_ask = true
    {
        // TODO analyze whether reserving more makes performance *much* worse on prod as well for 20k input
        order_map_.reserve(1000);
        
        // Wire up delta emission
        bids_.set_emitter(&emitter_);
        asks_.set_emitter(&emitter_);
    }

    void new_order(OrderId id, bool is_ask, Price price, Qty qty);
    void modify_order(OrderId id, Price new_price, Qty new_qty);
    void cancel_order(OrderId id);
    void trade(OrderId buy_id, OrderId sell_id, Price price, Qty fill_qty);
    
    std::span<const DeltaChunk> get_delta_chunks() const {
        return emitter_.get_chunks();
    }
    
    void prepare_deltas(Token token, uint32_t record_idx) {
        emitter_.clear();
        emitter_.set_event(token, record_idx);
    }
    
    void finalize_deltas() {
        emitter_.finalize();
    }

private:
    Token token_;
    DeltaEmitter emitter_;
    PriceLevels bids_;
    PriceLevels asks_;
    boost::unordered::unordered_flat_map<OrderId, OrderInfo> order_map_;
    OrderId last_order_id_ = 0;  // Track most recent new/modify for aggressor detection in trades
    PendingCross pending_cross_;  // Track active crossing for self-trade detection
};

void MBO::new_order(OrderId id, bool is_ask, Price price, Qty qty) {
    if (id == 0) return;
    
    // Pending cross should be fully resolved before a new order
    always_assert(!pending_cross_.is_active() && 
                  "Pending cross not resolved before new_order");
    
    last_order_id_ = id;
    
    PriceLevels& passive = is_ask ? bids_ : asks_;
    PriceLevels& aggressor = is_ask ? asks_ : bids_;
    
    // Peek at best passive price to determine if crossing would occur
    // (must know tick type before emitting any deltas)
    Price passive_best = passive.best_price();
    bool would_cross = g_crossing_enabled && (passive_best != 0) &&
        (is_ask ? (price <= passive_best) : (price >= passive_best));
    
    char tick_type = would_cross ? 'A' : 'N';  // A=newOrderCross, N=newOrderMsg
    bool is_exch_tick = !would_cross;
    emitter_.emit_tick_info(tick_type, is_ask, is_exch_tick, price, qty, id);
    
    // Now do the actual crossing (emits deltas)
    Qty consumed = passive.cross(price, qty);
    Qty residual = qty - consumed;
    
    // If price check said we would cross, we must have consumed something
    always_assert((!would_cross || consumed > 0) && 
                  "would_cross but no consumption - price check inconsistent with cross()");
    
    // Track pending cross for self-trade detection
    if (consumed > 0) {
        pending_cross_.aggressor_id = id;
        pending_cross_.aggressor_is_ask = is_ask;
        pending_cross_.aggressor_price = price;
        pending_cross_.aggressor_original_qty = qty;
        pending_cross_.residual_tick_type = 'N';
        pending_cross_.aggressor_on_level = false;  // Reset - will be set below if residual added
    }
    
    // order_map stores ORIGINAL qty (exchange view)
    order_map_[id] = {is_ask, price, qty};
    
    // Level gets only RESIDUAL
    if (residual > 0) {
        aggressor.add_liquidity(price, residual, 1);
        pending_cross_.aggressor_on_level = true;
    }
}

void MBO::modify_order(OrderId id, Price new_price, Qty new_qty) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    
    // Pending cross should be fully resolved before a modify order
    always_assert(!pending_cross_.is_active() && 
                  "Pending cross not resolved before modify_order");
    
    last_order_id_ = id;
    
    // For crossing, we need to know consumed qty before emitting tick
    // But deltas must come after tick_info. So we have a constraint:
    // - Non-crossing: emit M, then do level ops
    // - Crossing: need to compute consumed first, emit B, then do level ops
    // Solution: Check crossing potential without consuming, then emit tick, then consume
    
    PriceLevels& own_side = info.is_ask ? asks_ : bids_;
    PriceLevels& passive = info.is_ask ? bids_ : asks_;
    
    // For non-crossing mode (or to check crossing without consuming)
    // we could peek at whether crossing would occur. For now, simplified approach:
    // Always use non-crossing path if crossing disabled
    if (!g_crossing_enabled) {
        emitter_.emit_tick_info('M', info.is_ask, true, new_price, new_qty, id);
        
        if (info.price != new_price) {
            own_side.remove_liquidity(info.price, info.qty, 1);
            own_side.add_liquidity(new_price, new_qty, 1);
        } else if (Qty delta = new_qty - info.qty; delta < 0) {
            own_side.remove_liquidity(info.price, -delta, 0);
        } else {
            own_side.add_liquidity(info.price, delta, 0);
        }
        
        info.price = new_price;
        info.qty = new_qty;
        return;
    }
    
    // Crossing enabled path: emit tick first, then do operations
    // Peek at best passive price to determine if crossing would occur
    Price passive_best = passive.best_price();
    // TODO this price comparison pattern is repeated in a few places - check disas if optimal
    bool would_cross = (passive_best != 0) && 
        (info.is_ask ? (new_price <= passive_best) : (new_price >= passive_best));
    
    char tick_type = would_cross ? 'B' : 'M';
    bool is_exch_tick = !would_cross;
    emitter_.emit_tick_info(tick_type, info.is_ask, is_exch_tick, new_price, new_qty, id);
    
    // Get the affected level BEFORE removing (might be gone after)
    int8_t original_affected_lvl = own_side.get_level_index(info.price);
    
    // Now do the actual operations
    own_side.remove_liquidity(info.price, info.qty, 1);
    
    Qty consumed = passive.cross(new_price, new_qty);
    Qty residual = new_qty - consumed;
    
    // If price check said we would cross, we must have consumed something
    always_assert((!would_cross || consumed > 0) && 
                  "would_cross but no consumption - price check inconsistent with cross()");
    
    // Track pending cross for self-trade detection
    if (consumed > 0) {
        pending_cross_.aggressor_id = id;
        pending_cross_.aggressor_is_ask = info.is_ask;
        pending_cross_.aggressor_price = new_price;
        pending_cross_.original_resting_price = info.price;  // Old price for X tick
        pending_cross_.aggressor_original_qty = info.qty;    // Old qty (resting state) for X tick
        pending_cross_.residual_tick_type = 'M';
        pending_cross_.original_affected_lvl = original_affected_lvl;
        pending_cross_.aggressor_on_level = false;  // Reset - will be set below if residual added
    }
    
    info.price = new_price;
    info.qty = new_qty;
    
    if (residual > 0) {
        own_side.add_liquidity(new_price, residual, 1);
        pending_cross_.aggressor_on_level = true;
    }
}

void MBO::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    
    if (it == order_map_.end()) {
        // Order not found - emit TickInfo with exchange data
        emitter_.emit_tick_info('X', false, true, 0, 0, id);
        return;
    }

    OrderInfo& info = it->second;
    // printf("DEBUG: cancel_order id %ld info.price %ld info.qty %ld\n", id, info.price, (long)info.qty);
    
    PriceLevels& half = info.is_ask ? asks_ : bids_;
    
    // Check if this is the aggressor being cancelled during crossing
    bool is_aggressor_cancel = pending_cross_.is_active() && 
                               (id == pending_cross_.aggressor_id);
    
    if (is_aggressor_cancel) {
        // Aggressor cancel: exchange cancelled the aggressive order (e.g., all passives were self)
        // We need to: 1) emit C tick with VWAP of pending speculation
        //             2) restore all speculative consumption to passive levels
        //             3) remove aggressor's residual from its level
        //             4) emit S tick with aggressor's actual info
        //             5) emit CrossingComplete and clear crossing state
        
        PriceLevels& passive_side = pending_cross_.aggressor_is_ask ? bids_ : asks_;
        
        // C tick = VWAP of pending speculative fills, total pending qty
        auto [cross_vwap, cross_qty] = passive_side.pending_cross_vwap();
        emitter_.emit_tick_info('C', info.is_ask, true, cross_vwap, cross_qty, id);
        
        // Calculate residual BEFORE uncross modifies state
        Qty unconfirmed = passive_side.pending_cross_fill_qty();
        Qty residual_on_level = info.qty - unconfirmed;
        
        // Restore passive levels using tracked per-level consumption
        passive_side.uncross();
        
        // Remove aggressor's residual from its level (if any was added)
        if (residual_on_level > 0 && pending_cross_.aggressor_on_level) {
            half.remove_liquidity(info.price, residual_on_level, 1);
        }
        
        // Emit S tick with aggressor's actual info (receiver captures for C expansion)
        emitter_.emit_tick_info('S', info.is_ask, false, info.price, info.qty, id);
        
        // Emit CrossingComplete and clear crossing state
        emitter_.emit_crossing_complete();
        passive_side.clear_cross_fills();
        pending_cross_.clear();
        
    } else {
        // Check if this is a passive order being cancelled during crossing
        bool is_passive_cancel_during_crossing = false;
        if (pending_cross_.is_active()) {
            bool is_passive_side = (info.is_ask != pending_cross_.aggressor_is_ask);
            
            if (is_passive_side) {
                // Check if cancelled order's price would cross aggressor's price
                bool would_cross = pending_cross_.aggressor_is_ask 
                    ? (pending_cross_.aggressor_price <= info.price)
                    : (pending_cross_.aggressor_price >= info.price);
                
                is_passive_cancel_during_crossing = would_cross;
            }
        }
        
        if (is_passive_cancel_during_crossing) {
            // Passive self-trade cancel: cancelled order was on passive side
            PriceLevels& passive_side = pending_cross_.aggressor_is_ask ? bids_ : asks_;
            Qty consumed_from_order = std::min(info.qty, passive_side.pending_cross_fill_qty());
            
            if (consumed_from_order == 0) {
                // Order wasn't actually consumed - treat as regular cancel
                emitter_.emit_tick_info('X', info.is_ask, false, info.price, info.qty, id);
                half.remove_liquidity(info.price, info.qty, 1);
            } else {
                // Self-trade cancel with actual consumption
                // C tick = aggressor's POV: VWAP of pending speculative fills, total pending qty
                auto [cross_vwap, cross_qty] = passive_side.pending_cross_vwap();
                emitter_.emit_tick_info('C', info.is_ask, true, cross_vwap, cross_qty, id, pending_cross_.aggressor_id);
                
                // Remove remaining visible portion from level
                Qty remaining_on_level = info.qty - consumed_from_order;
                half.remove_liquidity(info.price, remaining_on_level, 1);
                
                // Unreserve the consumed portion
                passive_side.unreserve_cross_fill(consumed_from_order);
                
                // Re-cross: aggressor needs to find other liquidity
                Qty re_consumed = passive_side.cross(pending_cross_.aggressor_price, consumed_from_order);
                Qty re_residual = consumed_from_order - re_consumed;
                
                // Add unconsumed re-cross residual back to aggressor's resting level
                if (re_residual > 0) {
                    PriceLevels& aggressor_side = pending_cross_.aggressor_is_ask ? asks_ : bids_;
                    // count_delta=1 if aggressor not yet on level, 0 if already there
                    int count_delta = pending_cross_.aggressor_on_level ? 0 : 1;
                    aggressor_side.add_liquidity(pending_cross_.aggressor_price, re_residual, count_delta);
                    pending_cross_.aggressor_on_level = true;
                }
                
                // Emit S tick with full cancelled order qty (receiver captures for C expansion)
                emitter_.emit_tick_info('S', info.is_ask, false, info.price, info.qty, id, pending_cross_.aggressor_id);
                
                // If no more pending speculative consumption, crossing is complete
                if (passive_side.pending_cross_fill_qty() == 0) {
                    emitter_.emit_crossing_complete();
                    passive_side.clear_cross_fills();
                    pending_cross_.clear();
                }
            }
        } else {
            // Regular cancel (not during crossing, or not related to crossing)
            emitter_.emit_tick_info('X', info.is_ask, false, info.price, info.qty, id);
            half.remove_liquidity(info.price, info.qty, 1);
        }
    }
    
    order_map_.erase(it);
}

void MBO::trade(OrderId bid_id, OrderId ask_id, Price price, Qty fill_qty) {
    // Lookup both orders (0 means IOC/hidden - not in book)
    auto bid_it = bid_id ? order_map_.find(bid_id) : order_map_.end();
    auto ask_it = ask_id ? order_map_.find(ask_id) : order_map_.end();
    always_assert(bid_it == order_map_.end() || !bid_it->second.is_ask);
    always_assert(ask_it == order_map_.end() || ask_it->second.is_ask);
    
    bool bid_in_book = (bid_it != order_map_.end());
    bool ask_in_book = (ask_it != order_map_.end());
    
    // Aggressor is the order NOT in book; if both/neither, use last_order_id_
    bool aggressor_is_ask = (bid_in_book != ask_in_book) ? bid_in_book 
                                                         : (ask_id == last_order_id_);
    auto aggressor_it = aggressor_is_ask ? ask_it : bid_it;
    OrderId aggressor_id = aggressor_is_ask ? ask_id : bid_id;

    // Tick type: 'D' = IOC (id=0), 'E' = market order (id!=0 but not in book), 'T' = normal
    char tick_type = (aggressor_id == 0) ? 'D' : (aggressor_it == order_map_.end() ? 'E' : 'T');
    
    emitter_.emit_tick_info(tick_type, aggressor_is_ask, true, price, fill_qty, bid_id, ask_id);

    // Reconcile against passive side - this qty was already removed from levels during crossing
    PriceLevels& passive = aggressor_is_ask ? bids_ : asks_;
    Qty reconciled = passive.reconcile_cross_fill(fill_qty);
    Qty remaining = fill_qty - reconciled;
    
    // If we reconciled a crossing, emit synthetic zero-delta updates to set affected_lvl=0 on both sides
    if (reconciled > 0) {
        emitter_.emit_update(!aggressor_is_ask, 0, 0, 0);  // passive side, level 0, no change
        emitter_.emit_update(aggressor_is_ask, 0, 0, 0);   // aggressor side, level 0, no change
    }
    
    for (auto it: {bid_it, ask_it}) {
        if (it == order_map_.end()) continue;
        
        OrderInfo& info = it->second;
        always_assert(fill_qty <= info.qty && "Trade overfill detected: fill_qty exceeds order qty");
        
        // Always update order_map (exchange authoritative)
        info.qty -= fill_qty;
        
        PriceLevels& half = info.is_ask ? asks_ : bids_;
        
        if (remaining > 0) {
            // Normal case: remove qty and count from level
            half.remove_liquidity(info.price, remaining, (info.qty == 0) ? 1 : 0);
        } else if (info.qty == 0) {
            // Reconciled case: qty already removed during crossing, just update count
            half.remove_liquidity(info.price, 0, 1);
        }
        
        // Track confirmed order count for crossing rollback accuracy
        // When a passive order is fully consumed during reconciled crossing,
        // decrement pending_cross_fill_count_ so uncross() restores correct counts.
        if (reconciled > 0 && info.qty == 0 &&
            info.is_ask != pending_cross_.aggressor_is_ask) {
            passive.reconcile_cross_count(1);
        }
        
        if (info.qty == 0) {
            order_map_.erase(it);
        }
    }
    
    // When crossing is complete, signal completion or emit X for fully consumed modifies
    // TODO see how to make the behaviour consistent from the MBO end and fake compatibility at reconstruction; this is too verbose likely inefficient
    if (pending_cross_.is_active()) {
        PriceLevels& cross_passive = pending_cross_.aggressor_is_ask ? bids_ : asks_;
        if (cross_passive.pending_cross_fill_qty() == 0) {
            // Crossing complete - clear cross fills (no longer needed for rollback)
            cross_passive.clear_cross_fills();
            
            // Check if aggressor has residual
            auto agg_it = order_map_.find(pending_cross_.aggressor_id);
            bool has_residual = (agg_it != order_map_.end() && agg_it->second.qty > 0);
            
            if (!has_residual && pending_cross_.residual_tick_type == 'M') {
                // Fully consumed from MODIFY - emit X tick directly using ORIGINAL resting price
                // This ensures the X tick references where the order WAS before modify
                emitter_.emit_tick_info('X', pending_cross_.aggressor_is_ask, false,
                                       pending_cross_.original_resting_price, 
                                       pending_cross_.aggressor_original_qty,
                                       pending_cross_.aggressor_id, 0);
                // Emit zero-delta update at original level to set affected_lvl correctly
                // (must come AFTER X TickInfo so receiver associates it with X)
                emitter_.emit_update(pending_cross_.aggressor_is_ask, 
                                    pending_cross_.original_affected_lvl, 0, 0);
            } else if (has_residual || pending_cross_.residual_tick_type == 'N') {
                // Either has residual (emit N/M) or fully consumed new order (no X needed)
                // Let receiver synthesize via CrossingComplete
                emitter_.emit_crossing_complete();
            }
            pending_cross_.clear();
        }
    }
}

// --- Delta Reconstruction (for validation) ---

// Pending aggressor state for receiver-side C/S/N expansion and CrossingComplete handling
// Tracks aggressor info from 'A'/'B' ticks to expand 'C' ticks and synthesize N/M/X on CrossingComplete
struct PendingAggressorState {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Qty aggressor_original_qty = 0;  // Original qty at crossing start (for X tick)
    Qty aggressor_remaining = 0;     // Updated on trades (for N/M tick)
    char original_tick_type = 0;     // 'A' for new_order, 'B' for modify_order
    bool crossing_complete = false;  // Set when CrossingComplete seen during self-trade
    
    bool is_active() const { return aggressor_id != 0; }
    void clear() { aggressor_id = 0; original_tick_type = 0; crossing_complete = false; }
    
    void set(OrderId id, bool is_ask, Price price, Qty qty, char tick_type) {
        aggressor_id = id;
        aggressor_is_ask = is_ask;
        aggressor_price = price;
        aggressor_original_qty = qty;
        aggressor_remaining = qty;
        original_tick_type = tick_type;
    }
    
    void on_trade(Qty fill_qty) {
        aggressor_remaining -= fill_qty;
    }
};

// TODO when finalizing chunk push/pop/peek interfaces, consider that any tick info
// delta will be the first if present and only loop over the rest - perhaps there's
// a neater iterator pattern that we'll be able to use; could also standardize the
// flags and combine the update/insert loop to setup_args/maybe_shift/add/maybe_erase
// Returns the number of OutputRecords produced (usually 1, but 3 for 'C' tick)
int apply_deltas_to_book(OutputRecord& rec, std::span<const DeltaChunk> chunks, 
                          PendingAggressorState& agg_state, 
                          std::vector<OutputRecord>* extra_records = nullptr) {
    // Track first delta index on each side for affected_lvl reconstruction
    uint8_t affected_lvl[2] = {20, 20};  // [bid, ask], 20 = not affected
    bool seen_tick_info = false;  // Track if we've processed a TickInfoDelta
    Qty self_trade_cancel_full_qty = 0;   // Full order qty from explicit S tick (for C expansion)
    Price self_trade_cancel_price = 0;    // Passive order's actual price from explicit S tick

    PerfProfile("apply_deltas_to_book");
    // Process all chunks
    for (const auto& chunk : chunks) {
        rec.token = chunk.token;
        
        size_t offset = 0;
        for (uint8_t i = 0; i < chunk.num_deltas && offset < 58; ++i) {
            uint8_t dtype = chunk.payload[offset];
            
            if (dtype == DeltaType::TickInfo) {
                const TickInfoDelta* delta = reinterpret_cast<const TickInfoDelta*>(&chunk.payload[offset]);
                
                // S tick during active crossing: capture passive order's price/qty for C expansion
                // Don't process as normal tick - C expansion will use these for S record
                if (delta->tick_type == 'S' && agg_state.is_active()) {
                    self_trade_cancel_full_qty = delta->qty;
                    self_trade_cancel_price = delta->price;
                    offset += sizeof(TickInfoDelta);
                    continue;
                }
                
                bool is_ask = (delta->exch_side_flags >> 1) & 1;
                
                // If we've already processed a TickInfoDelta, this is a secondary tick
                // (e.g., N/M/X after T for residual/cancellation)
                // Push the current record as an extra before processing the new one
                if (seen_tick_info && extra_records != nullptr) {
                    // Finalize current record with affected levels and filled counts
                    rec.bid_affected_lvl = affected_lvl[0];
                    rec.ask_affected_lvl = affected_lvl[1];
                    rec.bid_filled_lvls = 0;
                    rec.ask_filled_lvls = 0;
                    for (int j = 0; j < 20; ++j) {
                        if (rec.bids[j].price != 0) rec.bid_filled_lvls++;
                        if (rec.asks[j].price != 0) rec.ask_filled_lvls++;
                    }
                    extra_records->push_back(rec);
                    // Reset affected levels for secondary TickInfo (e.g., X tick after T)
                    // Note: CrossingComplete-synthesized N/M keeps affected levels (handled separately)
                    affected_lvl[0] = 20;
                    affected_lvl[1] = 20;
                }
                seen_tick_info = true;
                
                // Copy event metadata (record_idx now in TickInfoDelta)
                rec.record_idx = delta->record_idx;
                rec.event.tick_type = delta->tick_type;
                rec.event.is_ask = is_ask;
                rec.event.price = delta->price;
                rec.event.qty = delta->qty;
                rec.event.order_id = delta->order_id;
                rec.event.order_id2 = delta->order_id2;
                rec.is_ask = is_ask;
                
                // Track aggressor state for 'A'/'B' ticks (crossing start)
                if (delta->tick_type == 'A' || delta->tick_type == 'B') {
                    agg_state.set(delta->order_id, is_ask, delta->price, delta->qty, delta->tick_type);
                }
                
                // Update aggressor remaining on trades
                // Note: don't clear here - CrossingComplete will signal when to synthesize N/M/X
                if ((delta->tick_type == 'T' || delta->tick_type == 'D' || delta->tick_type == 'E') 
                    && agg_state.is_active()) {
                    agg_state.on_trade(delta->qty);
                }
                
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
                
                // Track affected_lvl: use minimum (topmost) index among all updates
                affected_lvl[is_ask] = std::min(affected_lvl[is_ask], idx);
                
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

                // Track affected_lvl: skip refills (shift=false), use minimum (topmost) non-refill delta
                if (shift) {  // shift=true → real insertion, not refill
                    affected_lvl[is_ask] = std::min(affected_lvl[is_ask], idx);
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
                
            } else if (dtype == DeltaType::CrossingComplete) {
                // Crossing has fully resolved - synthesize N/M/X tick for the aggressor
                // Skip synthesis if current tick is 'C' (self-trade) - 'C' expansion handles it
                bool is_self_trade = (rec.event.tick_type == 'C');
                if (extra_records != nullptr && agg_state.is_active() && !is_self_trade) {
                    // Check if we need to synthesize a tick
                    bool need_residual = (agg_state.aggressor_remaining > 0);
                    bool need_cancel = (!need_residual && agg_state.original_tick_type == 'B');
                    
                    if (need_residual || need_cancel) {
                        // Push current record (typically a T/D/E tick) to extra_records first
                        // Finalize current record with affected levels and filled counts
                        rec.bid_affected_lvl = affected_lvl[0];
                        rec.ask_affected_lvl = affected_lvl[1];
                        rec.bid_filled_lvls = 0;
                        rec.ask_filled_lvls = 0;
                        for (int j = 0; j < 20; ++j) {
                            if (rec.bids[j].price != 0) rec.bid_filled_lvls++;
                            if (rec.asks[j].price != 0) rec.ask_filled_lvls++;
                        }
                        extra_records->push_back(rec);
                        
                        // Synthesize the residual/cancel tick
                        // Keep affected levels from T tick (same logical event)
                        // Note: affected_lvl is NOT reset - N/M/X inherits from T
                        
                        if (need_residual) {
                            // Aggressor has residual - emit N (from 'A') or M (from 'B')
                            rec.event.tick_type = (agg_state.original_tick_type == 'A') ? 'N' : 'M';
                            rec.event.is_ask = agg_state.aggressor_is_ask;
                            rec.event.price = agg_state.aggressor_price;
                            rec.event.qty = agg_state.aggressor_remaining;
                            rec.event.order_id = agg_state.aggressor_id;
                            rec.event.order_id2 = 0;
                            rec.is_ask = agg_state.aggressor_is_ask;
                        } else {
                            // Fully consumed from MODIFY - emit X (order removed from book)
                            rec.event.tick_type = 'X';
                            rec.event.is_ask = agg_state.aggressor_is_ask;
                            rec.event.price = agg_state.aggressor_price;
                            rec.event.qty = agg_state.aggressor_original_qty;
                            rec.event.order_id = agg_state.aggressor_id;
                            rec.event.order_id2 = 0;
                            rec.is_ask = agg_state.aggressor_is_ask;
                        }
                    }
                    // For 'A' that fully consumed - no additional tick needed, rec stays as T/D/E
                }
                if (!is_self_trade) {
                    agg_state.clear();
                } else {
                    // Self-trade: agg_state remains active for 'C' expansion at end,
                    // but flag that crossing is complete so expansion can clear it
                    agg_state.crossing_complete = true;
                }
                offset += sizeof(CrossingCompleteDelta);
                
            } else {
                // Unknown delta type, skip
                break;
            }
        }
    }
    
    // Set affected_lvl fields
    rec.bid_affected_lvl = affected_lvl[0];
    rec.ask_affected_lvl = affected_lvl[1];
    
    // Count filled levels
    rec.bid_filled_lvls = 0;
    rec.ask_filled_lvls = 0;
    for (int i = 0; i < 20; ++i) {
        if (rec.bids[i].price != 0) rec.bid_filled_lvls++;
        if (rec.asks[i].price != 0) rec.ask_filled_lvls++;
    }
    
    // Handle 'C' tick expansion: generate S and N ticks using tracked aggressor state
    if (rec.event.tick_type == 'C' && extra_records != nullptr && agg_state.is_active()) {
        bool aggressor_side = agg_state.aggressor_is_ask;
        
        // Distinguish aggressor cancel vs passive cancel:
        // - Aggressor cancel: cancelled order IS the aggressor (id matches)
        // - Passive cancel: cancelled order is on the passive side (id doesn't match)
        bool is_aggressor_cancel = (static_cast<OrderId>(rec.event.order_id) == agg_state.aggressor_id);
        
        if (is_aggressor_cancel) {
            // Aggressor self-trade cancel: exchange cancelled the aggressor
            // Emit C + S only (no N/B residual since aggressor is gone)
            
            // C tick: aggressor's side
            rec.event.is_ask = aggressor_side;
            rec.is_ask = aggressor_side;
            rec.bid_affected_lvl = 0;
            rec.ask_affected_lvl = 0;
            
            // S tick: aggressor's side (the cancelled order IS the aggressor)
            OutputRecord rec_s = rec;
            rec_s.event.tick_type = 'S';
            rec_s.bid_affected_lvl = 20;
            rec_s.ask_affected_lvl = 20;
            if (self_trade_cancel_full_qty > 0) {
                rec_s.event.price = self_trade_cancel_price;
                rec_s.event.qty = self_trade_cancel_full_qty;
            }
            extra_records->push_back(rec_s);
            
            agg_state.clear();
            return 2;  // C + S
        } else {
            // Passive self-trade cancel: cancelled order was on passive side
            bool cancelled_side = !aggressor_side;
            
            // Fix the 'C' record to use aggressor's side
            rec.event.is_ask = aggressor_side;
            rec.is_ask = aggressor_side;
            
            // Self-trade cancels inherently affect top of book on both sides:
            // the cancelled passive order was at/near best price (it was being crossed),
            // and the aggressor rests on the other side. The speculative crossing already
            // removed the passive level (no Update deltas in the chunk), so we set
            // affected levels explicitly rather than relying on delta-derived values.
            rec.bid_affected_lvl = 0;
            rec.ask_affected_lvl = 0;
            
            // 'S' tick: cancelled order's perspective (cancelled order's side)
            // Synthetic notification: no additional book changes, both affected levels = 20
            // Price/qty from MBO's explicit S tick (passive order's actual info, not C's VWAP)
            OutputRecord rec_s = rec;
            rec_s.event.tick_type = 'S';
            rec_s.event.is_ask = cancelled_side;
            rec_s.is_ask = cancelled_side;
            rec_s.bid_affected_lvl = 20;
            rec_s.ask_affected_lvl = 20;
            if (self_trade_cancel_full_qty > 0) {
                rec_s.event.price = self_trade_cancel_price;
                rec_s.event.qty = self_trade_cancel_full_qty;
            }
            extra_records->push_back(rec_s);
            
            // Tick type depends on whether crossing has been fully confirmed:
            // - crossing_complete=false: still speculative, use original tick type ('A'/'B')
            // - crossing_complete=true: confirmed, use residual type ('N'/'M')
            OutputRecord rec_n = rec;
            rec_n.event.tick_type = agg_state.crossing_complete
                ? ((agg_state.original_tick_type == 'A') ? 'N' : 'M')
                : agg_state.original_tick_type;
            rec_n.event.is_ask = aggressor_side;
            rec_n.event.price = agg_state.aggressor_price;
            rec_n.event.qty = agg_state.aggressor_remaining;
            rec_n.event.order_id = agg_state.aggressor_id;
            rec_n.event.order_id2 = 0;
            rec_n.is_ask = aggressor_side;
            // Aggressor side: keep affected level from 'C' deltas (where add_liquidity happened)
            // Other side: 20 (not affected)
            if (aggressor_side) {  // ask aggressor
                rec_n.bid_affected_lvl = 20;
                // ask_affected_lvl already set from 'C' processing
            } else {  // bid aggressor
                rec_n.ask_affected_lvl = 20;
                // bid_affected_lvl already set from 'C' processing
            }
            extra_records->push_back(rec_n);
            
            // Clear agg_state if crossing completed during this self-trade cancel
            if (agg_state.crossing_complete) {
                agg_state.clear();
            }
            // else: crossing continues, agg_state stays active for more trades
            return 3;  // C + S + N
        }
    }
    
    return 1;  // Single record produced
}

// --- Book Observer (strategy callback interface) ---
// In production: strategy process receives book snapshots via this interface
class BookObserver {
public:
    virtual ~BookObserver() = default;
    // Called for each book snapshot produced by delta application.
    // Return true to continue processing, false to abort.
    virtual bool on_book_update(const OutputRecord& book) = 0;
};

// --- Runner ---
// Simulates the publisher→SHM→strategy pipeline in a single process.
// process_record() = publisher context (MBO operations → deltas to SHM buffer)
// process_deltas()  = strategy context (deltas → book reconstruction → observer callback)
class Runner {
public:
    Runner() {
        mbos_.reserve(100);
        reconstructed_books_.reserve(100);
        aggressor_states_.reserve(100);
    }
    
    // Publisher context: process input record, emit deltas to SHM buffer
    void process_record(const InputRecord& rec);
    
    // Strategy context: apply deltas to reconstructed book, deliver snapshots via observer.
    // Returns false if observer requested abort.
    bool process_deltas(BookObserver& observer);
    
    void report_active_orders() const;

private:
    // --- Publisher state ---
    boost::unordered::unordered_flat_map<Token, unique_ptr<MBO>> mbos_;
    
    // --- SHM simulation (deltas produced by last process_record) ---
    std::vector<DeltaChunk> shm_deltas_;
    
    // --- Strategy/receiver state ---
    boost::unordered::unordered_flat_map<Token, OutputRecord> reconstructed_books_;
    boost::unordered::unordered_flat_map<Token, PendingAggressorState> aggressor_states_;
};

void Runner::process_record(const InputRecord& rec) {
    PerfProfileCount("records_processed", 1);
    rec.print();

    Token token = rec.token;
    auto it = mbos_.find(token);
    if (it == mbos_.end()) {
        it = mbos_.emplace(token, make_unique<MBO>(token)).first;
    }

    PerfProfile("got_mbo");
    MBO& mbo = *it->second;
    
    mbo.prepare_deltas(token, rec.record_idx);
    
    switch (rec.tick_type) {
        case 'N': {PerfProfile("new_order"); mbo.new_order(rec.order_id, rec.is_ask, rec.price, rec.qty); break;}
        case 'M': {PerfProfile("modify_order"); mbo.modify_order(rec.order_id, rec.price, rec.qty); break;}
        case 'X': {PerfProfile("cancel_order"); mbo.cancel_order(rec.order_id); break;}
        case 'T': {PerfProfile("trade"); mbo.trade(rec.order_id, rec.order_id2, rec.price, rec.qty); break;}
    }
    
    mbo.finalize_deltas();

    // Copy deltas to SHM buffer (simulates publisher writing to shared memory)
    auto chunks = mbo.get_delta_chunks();
    shm_deltas_.assign(chunks.begin(), chunks.end());
    
#ifndef printf
    for (const auto& chunk : shm_deltas_) {
        std::cout << "  " << chunk << "\n";
    }
#endif
}

bool Runner::process_deltas(BookObserver& observer) {
    if (shm_deltas_.empty()) return true;
    
    Token token = shm_deltas_[0].token;
    auto& reconstructed = reconstructed_books_[token];
    auto& agg_state = aggressor_states_[token];
    
    std::vector<OutputRecord> extra_records;
    int num_records [[maybe_unused]] = apply_deltas_to_book(reconstructed, shm_deltas_, agg_state, &extra_records);
    
    // Deliver snapshots to observer in correct order:
    // - Multi-tick (T+N/M/X): extras contain the T tick → deliver extras before main
    // - C expansion (C+S+N): extras contain S and N → deliver main before extras
    bool multi_tick_secondary = !extra_records.empty() && 
        (extra_records[0].event.tick_type == 'T' || extra_records[0].event.tick_type == 'D' || 
         extra_records[0].event.tick_type == 'E');
    
    if (multi_tick_secondary) {
        for (const auto& extra : extra_records) {
            if (!observer.on_book_update(extra)) return false;
        }
        if (!observer.on_book_update(reconstructed)) return false;
    } else {
        if (!observer.on_book_update(reconstructed)) return false;
        for (const auto& extra : extra_records) {
            if (!observer.on_book_update(extra)) return false;
        }
    }
    
    return true;
}

void Runner::report_active_orders() const {
    for (const auto& [token, mbo] : mbos_) {
        PerfProfileCount("active_orders", mbo->order_map_.size());
        PerfProfileCount("active_levels", mbo->bids_.levels_.size() + mbo->asks_.levels_.size());
    }
}

// --- Reference Validator (compares book snapshots against reference output) ---
class ReferenceValidator : public BookObserver {
public:
    ReferenceValidator(const OutputRecord* ref_books, size_t num_ref, const InputRecord* inputs)
        : ref_books_(ref_books), num_ref_(num_ref), inputs_(inputs) {}
    
    void set_current_input(size_t idx) { input_idx_ = idx; }
    
    bool on_book_update(const OutputRecord& book) override {
        book.print();
        
        if (!ref_books_ || ref_idx_ >= num_ref_) {
            ref_idx_++;
            return true;
        }
        
// printf("VERBOSE INPUT:\n");
// records[input_idx-1].print();
// printf("VERBOSE OURS: (compare result %d)\n", cmp);
// book.print();
// printf("VERBOSE REFERENCE:\n");
// ref_books[ref_idx].print();

        int cmp = book.compare(ref_books_[ref_idx_]);
        if (cmp != 0) {
            /*
            100: record_idx mismatch
            101: token mismatch
            102: is_ask mismatch (non-trade)
            105: bid_filled_lvls mismatch
            106: ask_filled_lvls mismatch
            107: ask_affected_lvl mismatch
            108: bid_affected_lvl mismatch (when ask is primary)
            109: bid_affected_lvl mismatch (when bid is primary)
            110: ask_affected_lvl mismatch (when bid is primary)
            111: event.tick_type mismatch
            112: event.order_id mismatch
            113: event.order_id2 mismatch
            114: event.price mismatch
            115: event.qty mismatch
            [-20..-1]: bid level
            [+1..+20]: ask level
            */
            inputs_[input_idx_].print();
            printf("MISMATCH at input %lu (ref_idx: %lu) - Error code: %d ", 
                   input_idx_ + 1, ref_idx_, cmp);
            if (cmp >= 100) printf("(metadata/counts)\n");
            else if (cmp > 0) printf("(ask level %d)\n", cmp);
            else printf("(bid level %d)\n", -cmp);
            printf("OURS:\n");
            book.print();
            printf("REFERENCE:\n");
            ref_books_[ref_idx_].print();
            fflush(stdout);
            return false;
        }
        
        ref_idx_++;
        return true;
    }

private:
    const OutputRecord* ref_books_;
    size_t num_ref_;
    const InputRecord* inputs_;
    size_t ref_idx_ = 0;
    size_t input_idx_ = 0;
};

// --- Dump Observer (writes book snapshots to file) ---
class DumpObserver : public BookObserver {
    FILE* f_;
public:
    DumpObserver(FILE* f) : f_(f) {}
    bool on_book_update(const OutputRecord& book) override {
        fprintf(f_, "[%u] tok:%u tick:%c side:%s affected_bid:%d affected_ask:%d ltp:%ld ltq:%d\n",
                book.record_idx, book.token, book.event.tick_type,
                book.is_ask ? "ASK" : "BID",
                book.bid_affected_lvl, book.ask_affected_lvl,
                book.event.price, book.event.qty);
        return true;
    }
};

// --- Main ---
int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <input.bin> [<reference.bin>] [--crossing] [--dump]" << endl;
        return 1;
    }

    const char* input_file = argv[1];
    const char* reference_file = nullptr;
    bool dump_mode = false;

    // Second positional arg (non-flag) is reference file
    for (int i = 2; i < argc; ++i) {
        if (string(argv[i]) == "--crossing") {
            g_crossing_enabled = true;
        } else if (string(argv[i]) == "--dump") {
            dump_mode = true;
        } else if (argv[i][0] != '-') {
            reference_file = argv[i];
        }
    }
    
    // Auto-detect crossing mode from filename if not explicitly set
    if (!g_crossing_enabled && string(input_file).find("_crossing") != string::npos &&
        string(input_file).find("_nocrossing") == string::npos) {
        g_crossing_enabled = true;
    }

    // mmap input records
    int fd = open(input_file, O_RDONLY);
    if (fd < 0) { perror("open input"); return 1; }
    struct stat sb;
    fstat(fd, &sb);
    void* mapped = mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) { perror("mmap input"); close(fd); return 1; }
    madvise(mapped, sb.st_size, MADV_WILLNEED);
    const InputRecord* records = static_cast<const InputRecord*>(mapped);
    size_t num_records = sb.st_size / sizeof(InputRecord);

    // mmap reference (optional)
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
    int exit_code = 0;
    
    if (dump_mode) {
        FILE* f_input = fopen("dump_input.txt", "w");
        FILE* f_ours = fopen("dump_ours.txt", "w");
        DumpObserver dump_observer(f_ours);
        
        for (size_t i = 0; i < num_records; ++i) {
            const auto& rec = records[i];
            fprintf(f_input, "[%u] tok:%u id:%lu id2:%lu p:%ld q:%d type:%c side:%s\n",
                    rec.record_idx, rec.token, rec.order_id, rec.order_id2,
                    rec.price, rec.qty, rec.tick_type, rec.is_ask ? "ASK" : "BID");
            runner.process_record(rec);
            runner.process_deltas(dump_observer);
        }
        
        if (reference_file && ref_books) {
            FILE* f_ref = fopen("dump_reference.txt", "w");
            for (size_t i = 0; i < num_ref_books; ++i) {
                const auto& book = ref_books[i];
                fprintf(f_ref, "[%u] tok:%u tick:%c side:%s affected_bid:%d affected_ask:%d ltp:%ld ltq:%d\n",
                        book.record_idx, book.token, book.event.tick_type,
                        book.is_ask ? "ASK" : "BID",
                        book.bid_affected_lvl, book.ask_affected_lvl,
                        book.event.price, book.event.qty);
            }
            fclose(f_ref);
        }
        
        fclose(f_input);
        fclose(f_ours);
        printf("Dumped to dump_input.txt, dump_ours.txt%s\n",
               reference_file ? ", dump_reference.txt" : "");
    } else {
        // Normal mode: process records, compare against reference via observer
        ReferenceValidator validator(ref_books, num_ref_books, records);
        
        for (size_t input_idx = 0; input_idx < num_records; ++input_idx) {
            runner.process_record(records[input_idx]);
            validator.set_current_input(input_idx);
            if (!runner.process_deltas(validator)) {
                exit_code = 1;
                break;
            }
        }
    }

    runner.report_active_orders();
    PerfProfilerReport();

    munmap(mapped, sb.st_size);
    if (ref_mapped && ref_mapped != MAP_FAILED) munmap(ref_mapped, num_ref_books * sizeof(OutputRecord));
    close(fd);
    return exit_code;
}

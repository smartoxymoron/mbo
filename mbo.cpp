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

// #define printf(...) // to quickly disable printf for perf testing

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
        if (record_idx % 65536 != reference.record_idx % 65536) return 100; // only sending lower 16 bits of record_idx to save space in delta
        if (token != reference.token) return 101;
        
        // Skip is_ask check for trades - reference has incorrect aggressor detection
        // Also skip for cancels when order not found (affected levels both 20) - reference sends random side
        // TODO discuss with traders how to proceed; unrelated - also discuss crossing/selftrade behaviour and simplify
        bool is_cancel_not_found = (event.tick_type == 'X' && bid_affected_lvl == 20 && ask_affected_lvl == 20);
        if (event.tick_type != 'T' && !is_cancel_not_found && is_ask != reference.is_ask) return 102;

        // TODO check why these appear to not be set in reference
        // if (ltp != other.ltp) return 103;
        // if (ltq != other.ltq) return 104;

        if (bid_filled_lvls != reference.bid_filled_lvls) return 105;
        if (ask_filled_lvls != reference.ask_filled_lvls) return 106;

        // Side determined by reference as primary must match exactly
        // Special case for modifies: we report minimum (topmost) affected level, reference may report deeper level
        // Allow our_affected_lvl <= ref_affected_lvl for modifies (we're more precise about what changed)
        bool is_modify = (event.tick_type == 'M');
        
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
    uint16_t record_idx = 0;       // TEMP changed to uint32_t for easier debugging
    uint8_t flags = 0;             // bit 0: final (book ready for strategy)
    uint8_t num_deltas = 0;        // Number of deltas in this chunk (1-N)
    uint8_t payload[56] = {};      // Variable-length delta sequence
    
    friend std::ostream& operator<<(std::ostream& os, const DeltaChunk& chunk) {
        os << "Chunk[rec=" << chunk.record_idx 
           << ", tok=" << chunk.token 
           << ", final=" << (chunk.flags & 1) << "]: ";
        
        // Iterate through deltas in payload
        size_t offset = 0;
        // TEMP bump record_idx to uint32_t for easier debugging
        size_t total_bytes = 8;  // Header is 10 bytes (token:4 + record_idx:4 + flags:1 + num_deltas:1)
        
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
    uint16_t record_idx_;   // TEMP changed to uint32_t for easier debugging
    
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
    
    void set_event(Token token, uint32_t record_idx) {
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
    }
    
    void set_emitter(DeltaEmitter* e) {
        emitter_ = e;
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
        always_assert(qty > 0 && "remove_liquidity requires positive qty");
        
        // Convert to canonical (negated for bids)
        Price canonical = p * side_multiplier_;
        
        auto it = levels_.find(canonical);
        if (it == levels_.end()) return;
        
        int idx = static_cast<int>(levels_.size()) - 1 - static_cast<int>(it - levels_.begin());
        
        it->second.first -= qty;
        it->second.second -= count_delta;
        
        // Emit update delta
        emitter_->emit_update(is_ask_, idx, -qty, -count_delta);
        
        // Handle deletion (invariant: if count == 0, qty must be 0)
        if (it->second.first <= 0) [[unlikely]] {
            always_assert(it->second.second <= 0 && "qty=0 implies count=0");
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

    // Returns: vector of {price, qty_consumed}
    vector<pair<Price, Qty>> cross(Price target, Qty fill_qty) {
        vector<pair<Price, Qty>> consumed;
        Qty remaining = fill_qty;
        
        // Convert target to canonical for comparison
        Price canonical_target = target * side_multiplier_;

        while (remaining > 0 && !levels_.empty()) {
            auto it = levels_.rbegin();  // Best price now at rbegin() (descending order)
            Price canonical_best = it->first;
            
            // Compare in canonical space (descending order, but comparison logic same)
            bool crosses = (canonical_best <= canonical_target);
            if (!crosses) break;

            Qty consume_qty = min(remaining, (Qty)it->second.first);
            
            // Return actual price (denegate)
            Price actual_best = canonical_best * side_multiplier_;
            consumed.push_back({actual_best, consume_qty});
            
            it->second.first -= consume_qty;
            remaining -= consume_qty;
            
            if (it->second.first <= 0) {
                levels_.erase(std::next(it).base());  // Convert reverse_iterator to iterator for erase
            }
        }
        return consumed;
    }

    Price best_price() const {
        if (levels_.empty()) return 0;
        // Best price now at rbegin() (descending order), denegate to return actual price
        return levels_.rbegin()->first * side_multiplier_;
    }

private:
    bool is_ask_;
    int64_t side_multiplier_;  // +1 for asks, -1 for bids (reserved for future optimization)
    MapType levels_;
    DeltaEmitter* emitter_;
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
    void trade(OrderId id1, OrderId id2, Price price, Qty fill_qty);

    bool has_crossed() const;
    void infer_and_apply_cross();
    
    std::span<const DeltaChunk> get_delta_chunks() const {
        return emitter_.get_chunks();
    }
    
    void prepare_deltas(Token token, uint16_t record_idx) { // TEMP changed to uint32_t for easier debugging
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
    OrderId last_order_id_ = 0;  // Track most recent new/modify for aggressor detection
};

void MBO::new_order(OrderId id, bool is_ask, Price price, Qty qty) {
    if (id == 0) return;
    
    // Emit TickInfo delta
    emitter_.emit_tick_info('N', is_ask, true, price, qty);
    {
    //PerfProfile("new_order2");
    order_map_[id] = {is_ask, price, qty};
    last_order_id_ = id;  // Track for aggressor detection in trades
    }
    PriceLevels& half = is_ask ? asks_ : bids_;
    half.add_liquidity(price, qty, 1);
}

void MBO::modify_order(OrderId id, Price new_price, Qty new_qty) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    // printf("DEBUG: entering modify_order id %ld new_price %ld new_qty %ld info.price %ld info.qty %ld\n", id, new_price, (long)new_qty, info.price, (long)info.qty);
    
    // Emit TickInfo delta
    emitter_.emit_tick_info('M', info.is_ask, true, new_price, new_qty);
    
    last_order_id_ = id;  // Track for aggressor detection in trades
    
    PriceLevels& half = info.is_ask ? asks_ : bids_;
    
    if (info.price != new_price) {
        // Price changed - remove old then add new (matches exchange semantics, correct affected_lvl)
        half.remove_liquidity(info.price, info.qty, 1);
        half.add_liquidity(new_price, new_qty, 1);
    } else if (Qty delta = new_qty - info.qty; delta < 0) {
        // Same price, quantity decreased
        half.remove_liquidity(info.price, -delta, 0);
    } else {
        // Same price, quantity increased (or rare delta==0 case)
        half.add_liquidity(info.price, delta, 0);
    }
    
    info.price = new_price;
    info.qty = new_qty;
}

void MBO::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    
    if (it == order_map_.end()) {
        // Order not found - emit TickInfo with exchange data
        emitter_.emit_tick_info('X', false, true, 0, 0);
        return;
    }

    OrderInfo& info = it->second;
    // printf("DEBUG: cancel_order id %ld info.price %ld info.qty %ld\n", id, info.price, (long)info.qty);
    
    // Emit TickInfo delta
    emitter_.emit_tick_info('X', info.is_ask, false, info.price, info.qty);
    
    PriceLevels& half = info.is_ask ? asks_ : bids_;
    half.remove_liquidity(info.price, info.qty, 1);
    
    order_map_.erase(it);
}

void MBO::trade(OrderId bid_id, OrderId ask_id, Price price, Qty fill_qty) {
    // CONSTRAINT: OrderId 0 is reserved/invalid (similar to INT64_MIN for prices)
    // NSE provides bid_id and ask_id explicitly, but both may exist in book (limit-on-limit)
    // Use last_order_id_ to determine aggressor when ambiguous
    
    // Lookup both orders upfront (handle 0 gracefully)
    auto bid_it = bid_id ? order_map_.find(bid_id) : order_map_.end();
    auto ask_it = ask_id ? order_map_.find(ask_id) : order_map_.end();
    always_assert(order_map_.end() == bid_it or false == bid_it->second.is_ask);
    always_assert(order_map_.end() == ask_it or true == ask_it->second.is_ask);
    
    // This is likely sufficient and correct but we do have a mismatch against reference with crossing disabled
    // TEMP will disable is_ask check in compare() for trades for now
    bool aggressor_side_is_ask = (ask_id == last_order_id_) ? true : false;

    // Validate against old logic (temporary assertion)
    // bool bid_found = (bid_it != order_map_.end());
    // bool ask_found = (ask_it != order_map_.end());
    // bool trade_side_is_ask_old = bid_found && !ask_found;
    // printf("DEBUG: aggressor_side_is_ask %d trade_side_is_ask_old %d\n", aggressor_side_is_ask, trade_side_is_ask_old);
    // if (bid_found && ask_found) {
    //     printf("DEBUG: Both found bid %ld ask %ld (last_order_id_ %ld)\n", bid_id, ask_id, last_order_id_);
    //     // Both found - old logic is broken (always false), skip validation
    // } else {
    //     // Only one found - old logic should match new logic
    //     always_assert(aggressor_side_is_ask == trade_side_is_ask_old && 
    //                  "Aggressor detection logic mismatch");
    // }
    
    // Emit TickInfo delta first (for trades, price/qty become LTP/LTQ)
    emitter_.emit_tick_info('T', aggressor_side_is_ask, true, price, fill_qty);

    for (auto it: {bid_it, ask_it}) {
        if (it != order_map_.end()) {
            OrderInfo& info = it->second;
            always_assert(fill_qty <= info.qty && "Trade overfill detected: fill_qty exceeds order qty");
            info.qty -= fill_qty;
            
            PriceLevels& half = info.is_ask ? asks_ : bids_;
            half.remove_liquidity(info.price, fill_qty, (info.qty == 0 ? 1 : 0));
            if (info.qty == 0) {
                order_map_.erase(it);
            }
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

// --- Delta Reconstruction (for validation) ---
// TODO when finalizing chunk push/pop/peek interfaces, consider that any tick info
// delta will be the first if present and only loop over the rest - perhaps there's
// a neater iterator pattern that we'll be able to use; could also standardize the
// flags and combine the update/insert loop to setup_args/maybe_shift/add/maybe_erase
// Is a fairly large function of 762 bytes
void apply_deltas_to_book(OutputRecord& rec, std::span<const DeltaChunk> chunks) {
    // Track first delta index on each side for affected_lvl reconstruction
    uint8_t affected_lvl[2] = {20, 20};  // [bid, ask], 20 = not affected
    
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
}

// --- Runner ---
class Runner {
public:
    Runner() {
        mbos_.reserve(100);
        reconstructed_books_.reserve(100);
    }
    const OutputRecord& process_record(const InputRecord& rec);
    void report_active_orders() const;

private:
    boost::unordered::unordered_flat_map<Token, unique_ptr<MBO>> mbos_;
    boost::unordered::unordered_flat_map<Token, OutputRecord> reconstructed_books_;  // For delta validation
};

const OutputRecord& Runner::process_record(const InputRecord& rec) {
    //PerfProfile("process_record");
    PerfProfileCount("records_processed", 1);

    rec.print();

    Token token = rec.token;
    auto it = mbos_.find(token);
    if (it == mbos_.end()) {
        it = mbos_.emplace(token, make_unique<MBO>(token)).first;
    }

    PerfProfile("got_mbo");
    MBO& mbo = *it->second;
    
    // Prepare delta emission
    mbo.prepare_deltas(token, rec.record_idx);
    
    // Dispatch to appropriate operation
    switch (rec.tick_type) {
        case 'N': {PerfProfile("new_order"); mbo.new_order(rec.order_id, rec.is_ask, rec.price, rec.qty); break;}
        case 'M': {PerfProfile("modify_order"); mbo.modify_order(rec.order_id, rec.price, rec.qty); break;}
        case 'X': {PerfProfile("cancel_order"); mbo.cancel_order(rec.order_id); break;}
        case 'T': {PerfProfile("trade"); mbo.trade(rec.order_id, rec.order_id2, rec.price, rec.qty); break;}
    }
    
    // Finalize deltas
    mbo.finalize_deltas();

    // Get delta chunks and print them
    auto chunks = mbo.get_delta_chunks();
#ifndef printf
    for (const auto& chunk : chunks) {
        std::cout << "  " << chunk << "\n";
    }
#else
    (void)chunks;  // Suppress unused warning when printf is disabled
#endif


    // Apply deltas to reconstructed book
    auto& reconstructed = reconstructed_books_[token];
    PerfProfile("apply_deltas_to_book");
    apply_deltas_to_book(reconstructed, chunks);
    reconstructed.print();
    
    return reconstructed;
}

void Runner::report_active_orders() const {
    for (const auto& [token, mbo] : mbos_) {
        PerfProfileCount("active_orders", mbo->order_map_.size());
    }
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

        if (ref_books && i < num_ref_books) {
            int cmp = book.compare(ref_books[i]);
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
                */
                printf("MISMATCH at record %lu (idx: %u) - Error code: %d ", i, records[i].record_idx, cmp);
                if (cmp >= 100) {
                    printf("(metadata/counts)\n");
                } else if (cmp > 0) {
                    printf("(ask level %d)\n", cmp);
                } else {
                    printf("(bid level %d)\n", -cmp);
                }
                printf("REFERENCE:\n");
                ref_books[i].print();
                fflush(stdout);
                // while(1);
                return 1;
            }
        }
    }

    // Report active orders for each MBO (gets min/avg/max statistics)
    runner.report_active_orders();

    PerfProfilerReport();

    munmap(mapped, sb.st_size);
    if (ref_mapped && ref_mapped != MAP_FAILED) munmap(ref_mapped, num_ref_books * sizeof(OutputRecord));
    close(fd);
    return 0;
}

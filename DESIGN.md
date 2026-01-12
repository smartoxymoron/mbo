# MBO Naive Reference Implementation Design

## Purpose
Correctness reference + basic timing. NOT for performance comparison (boost maps are too slow).
Detailed stats collection deferred to later phase with real NSE data on target machines.

## File Structure
```
mbo.cpp          // Main implementation (single file)
perfprofiler.h   // Existing timing/stats helper
test_data.bin    // Input: mmap'd binary records
test_reference.bin // Expected output for correctness testing
Makefile         // Build script
```

## Required Includes (mbo.cpp)
```cpp
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
#include <boost/unordered/unordered_flat_map.hpp>
#include "perfprofiler.h"

using namespace std;
using boost::unordered_flat_map;
```

## Data Types
```cpp
using OrderId = uint64_t;
using Token = uint32_t;    // instrument ID (3 bytes used, 1 padding)
using Price = int64_t;
using Qty = int32_t;
using AggQty = int64_t;
using Count = int32_t;

enum class Side : uint8_t { Bid, Ask };
```

## Input Record Format (40 bytes, packed)
```cpp
struct InputRecord {
    uint32_t record_idx;
    uint32_t token;
    uint64_t order_id;
    uint64_t order_id2;    // for trades (aggressor order)
    Price price;           // starts at offset 24 (8-byte aligned)
    Qty qty;               // starts at offset 32
    char op_type;          // offset 36 ('N', 'M', 'X', 'T')
    uint8_t is_ask;        // offset 37 (0 or 1)
    uint8_t padding[2];    // total 40 bytes
} __attribute__((packed));
static_assert(sizeof(InputRecord) == 40);
```

## Output Record Format

### Book (652 bytes) - Full Snapshot
```cpp
struct Level {
    Price price;
    AggQty qty;
};

struct Book {
    uint32_t record_idx;
    uint32_t token;
    char op_type;
    uint8_t padding[3];
    Level bids[20];   // sorted best to worst
    Level asks[20];   // sorted best to worst
} __attribute__((packed));
static_assert(sizeof(Book) == 652);
```
**Note**: If fewer than 20 levels exist, pad with {price=0, qty=0}.

### Delta Messages - Incremental Updates

**Design**: MBO emits fixed 64-byte delta chunks over SHM for efficient book reconstruction.

**Key properties**:
- Fixed 64-byte chunks (cache-aligned, simple ring buffer transport)
- 3 primitive delta types: TickInfo (20B), Update (12B), Insert (24B)
- Most operations (99%) fit in 1-2 chunks
- Multi-chunk sequences use `final` flag to signal completion
- Strategy reconstructs 20-level book by applying deltas sequentially

**Current interface**: MBO stores chunks in internal `static_vector`, caller retrieves via `get_delta_chunks()`.

**Future optimization (Phase 2)**: Runner provides pre-allocated DeltaChunk buffer per instrument, MBO writes directly (zero-copy). Current approach has cache pollution (~500KB/s for 500 instruments with per-MBO static array).

**Details**: See [DELTAS.md](DELTAS.md) for complete design, size analysis, and reconstruction semantics.

## Class Design

### OrderInfo (per order state)
```cpp
struct OrderInfo {
    bool is_ask;  // side is immutable for an order
    Price price;
    Qty qty;
};
```

### PriceLevels (one side of book)
```cpp
template<bool IsAsk>
class PriceLevels {
    // Naive: boost::unordered_flat_map<Price, pair<AggQty, Count>>
    // Reserve ~1000 entries at construction to avoid allocations
    // Comparator for sorting: bids descending (higher better), asks ascending (lower better)
    
public:
    void add(Price p, Qty qty);           // increment qty, increment count (create if new)
    void remove(Price p, Qty qty);        // decrement qty, decrement count; erase if empty
    
    // Cross: consume levels from best price toward target
    // Returns: number of levels fully consumed, partial fill qty on last level
    pair<int, Qty> cross(Price target, Qty fill_qty);
    
    // Get sorted top-N levels (best first), fills output array, returns count
    int get_top_levels(Level* out, int n) const;
    
    // Get best price (for cross detection), returns 0 if no levels
    Price best_price() const;
    
private:
    boost::unordered_flat_map<Price, pair<AggQty, Count>> levels_;
};
```

### MBO (one instrument)
```cpp
// Delta emission interface (TBD - exact signature to be determined)
// For now: MBO emits deltas via callback/interface during operations

class MBO {
public:
    MBO(Token token);
    
    // Operations - emit deltas inline
    void new_order(OrderId id, bool is_ask, Price price, Qty qty);
    void modify_order(OrderId id, Price new_price, Qty new_qty);  // lookup old price/qty
    void cancel_order(OrderId id);
    void trade(OrderId id, Qty fill_qty);
    
    // Cross detection and inference
    bool has_crossed() const;  // bid best > ask best
    void infer_and_apply_cross();  // detect, generate synthetic trades, apply them
    
    // Book reconstruction (for validation only, outside MBO)
    // TBD: May provide iterator interface or direct access to levels
    
private:
    Token token_;
    PriceLevels<false> bids_;  // IsAsk=false
    PriceLevels<true> asks_;   // IsAsk=true
    boost::unordered_flat_map<OrderId, OrderInfo> order_map_;  // reserve ~10000
    
    // Cross tracking (orders we've inferred as filled, waiting for real trade to reconcile)
    deque<OrderId> pending_cross_fills_;
    
    // No Book struct member - MBO is format-agnostic
    // Deltas emitted inline during operations
};
```

### Runner (all instruments)
```cpp
// Global settings
inline bool g_crossing_enabled = false;

class Runner {
public:
    Runner();
    
    // Process one record, emit deltas inline, return reconstituted Book for validation
    // Uses PerfProfile/PerfProfileCount internally for timing
    // TBD: Delta emission mechanism (callback, buffer, etc.)
    const Book& process_record(const InputRecord& rec);
    
private:
    boost::unordered_flat_map<Token, unique_ptr<MBO>> mbos_;  // reserve ~100
    
    // Book reconstitution from deltas (for validation against reference)
    Book current_book_;  // reconstituted from emitted deltas
};
```

## Main Flow (main.cpp)
```cpp
int main(int argc, char** argv) {
    // 1. Parse args
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
    
    // 2. mmap input file
    int fd = open(input_file, O_RDONLY);
    struct stat sb;
    fstat(fd, &sb);
    size_t file_size = sb.st_size;
    
    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    madvise(mapped, file_size, MADV_WILLNEED);  // preload
    
    const InputRecord* records = static_cast<const InputRecord*>(mapped);
    size_t num_records = file_size / sizeof(InputRecord);
    
    // 3. mmap reference file if provided
    const Book* ref_books = nullptr;
    size_t num_ref_books = 0;
    void* ref_mapped = nullptr;
    
    if (reference_file) {
        int ref_fd = open(reference_file, O_RDONLY);
        struct stat ref_sb;
        fstat(ref_fd, &ref_sb);
        ref_mapped = mmap(nullptr, ref_sb.st_size, PROT_READ, MAP_PRIVATE, ref_fd, 0);
        ref_books = static_cast<const Book*>(ref_mapped);
        num_ref_books = ref_sb.st_size / sizeof(Book);
        close(ref_fd);
    }
    
    // 4. Process records
    Runner runner;
    
    for (size_t i = 0; i < num_records; ++i) {
        // Process record: MBO emits deltas inline, Runner reconstitutes Book
        const Book& book = runner.process_record(records[i]);
        
        // Compare reconstituted Book against reference
        if (ref_books && i < num_ref_books) {
            if (memcmp(&book, &ref_books[i], sizeof(Book)) != 0) {
                cerr << "MISMATCH at record " << i << endl;
                // Print detailed diff for debugging
                // ... (implement diff logic)
                return 1;
            }
        }
        
        // Write reconstituted Book (for validation/reference generation)
        fwrite(&book, sizeof(Book), 1, stdout);
        
        // Note: In production, deltas would be sent over SHM, not Books
    }
    
    // 5. Report timing (PerfProfiler handles this)
    PerfProfilerReport();
    
    // 6. Cleanup
    if (ref_mapped) munmap(ref_mapped, num_ref_books * sizeof(Book));
    munmap(mapped, file_size);
    close(fd);
    
    return 0;
}
```

## Operation Logic

### New Order
```
1. Create OrderInfo{is_ask, price, qty} in order_map
2. is_ask ? asks_.add(price, qty) : bids_.add(price, qty)
3. If g_crossing_enabled: infer_and_apply_cross() if has_crossed()
```

### Modify Order
```
1. Lookup old OrderInfo from order_map (get old price, qty, is_ask)
2. If price changed:
   - Remove from old price: is_ask ? asks_.remove(old_price, old_qty) : bids_.remove(old_price, old_qty)
   - Add at new price: is_ask ? asks_.add(new_price, new_qty) : bids_.add(new_price, new_qty)
3. If price same (qty reduction):
   - Delta = new_qty - old_qty (negative)
   - is_ask ? asks_.add(price, delta) : bids_.add(price, delta)
4. Update OrderInfo{is_ask, new_price, new_qty}
5. If g_crossing_enabled: check and apply cross
```

### Cancel Order
```
1. Lookup OrderInfo
2. PriceLevels[side].remove(price, qty)
3. Erase from order_map
```

### Trade
```
1. If order_id == 0: ignore (invalid trade)
2. Check if order in pending_cross_fills_ (front of deque):
   - We already applied this as synthetic trade during cross
   - Reconcile: assert(actual_price == expected_price), qty already removed
   - Pop from pending_cross_fills_
   - Skip further processing
3. Else (real trade, not inferred):
   - Lookup OrderInfo from order_map
   - is_ask ? asks_.remove(price, fill_qty) : bids_.remove(price, fill_qty)
   - If fully filled: erase from order_map
   - Else: update qty in order_map
```

### Cross Detection & Inference
```
Called by: infer_and_apply_cross() after each new/modify operation

1. Check: bids_.best_price() > asks_.best_price()
2. If not crossed: return
3. If crossed:
   - Determine aggressor: the side that just added/modified (track last_op_is_ask)
   - Get aggressor qty at best price
   - Consume opposing side:
     * If aggressor is ask: bids_.cross(ask_best_price, aggressor_qty)
     * If aggressor is bid: asks_.cross(bid_best_price, aggressor_qty)
   - cross() returns (levels_consumed, partial_qty)
   - For each level consumed (assume 1 order per level for simplicity):
     * Generate synthetic trade: order_id=next_from_opposing_best, qty=level_qty
     * Add order_id to pending_cross_fills_
     * Process as trade immediately (will remove from levels, order_map)
4. Note: We don't have actual order IDs, so we'll need to track which orders
   were at consumed price levels. Use order_map to find orders at consumed prices.
```

## Cross Implementation Detail

### PriceLevels::cross(Price target, Qty fill_qty)
```
Input: target price, fill quantity
Returns: vector<pair<Price, Qty>> consumed_levels

1. Get all levels sorted by priority (best first)
2. Qty remaining = fill_qty
3. For each level from best:
   - If price doesn't reach target: consume fully
     * consumed_levels.push_back({price, level_qty})
     * remaining -= level_qty
     * Remove from map
   - Else if price reaches target:
     * Partial: consume_qty = min(remaining, level_qty)
     * consumed_levels.push_back({price, consume_qty})
     * Update level qty or remove if empty
     * Break
4. Return consumed_levels
```

### MBO::infer_and_apply_cross()
```
After new/modify operation:

1. Check has_crossed(): bids_.best_price() > asks_.best_price()
2. If not crossed: return
3. Determine aggressor side (track from last operation)
4. Call opposing.cross(aggressor_price, aggressor_qty)
   - Returns consumed_levels = [(price, qty), ...]
5. For each (price, qty) in consumed_levels:
   - Find all orders at this price in order_map (iterate, filter by price + side)
   - For each order at consumed price:
     * Create synthetic trade for this order
     * Call trade(order_id, order_qty) to remove from book
     * Add order_id to pending_cross_fills_
6. Note: We assume simple 1:1 order-level mapping for now
```

## Delta Emission (TBD - Implementation Details)

**High-level flow**:
1. MBO operations (add, remove, etc.) emit delta primitives inline
2. Deltas sent immediately (via SHM or callback)
3. Outside MBO: Deltas reconstituted into Book for validation
4. Production: Strategy consumes deltas directly (no Book reconstruction)

**Open questions for implementation**:
- Delta struct format (primitive type + parameters)
- Emission mechanism (callback, buffer queue, direct SHM write?)
- Book reconstitution logic location (Runner? separate utility?)

## Performance Profiling

Use `perfprofiler.h` macros in Runner::process_record():
```cpp
PerfProfile("process_record");
PerfProfileCount("records_processed");
// ... potentially more granular profiles if needed
```

At end of main(), call `PerfProfilerReport()` to print human-readable timing stats.

## Testing Against Reference
```bash
# Without reference (generate reconstituted books)
./mbo test_data.bin > output.bin

# With reference (programmatic comparison, stops at first mismatch)
./mbo test_data.bin --reference test_reference.bin > output.bin

# With crossing enabled
./mbo test_data.bin --crossing --reference test_reference.bin
```

**Note**: 
- Output is always reconstituted Books (for validation)
- MBO emits deltas inline during processing
- Runner reconstitutes deltas into Book for validation
- In production, strategies would consume deltas directly (no Book reconstitution)

## Compilation

Makefile:
```makefile
CXX = g++
CXXFLAGS = -std=c++20 -O3 -mavx2 -Wall -Wextra
LDFLAGS = 

mbo: mbo.cpp perfprofiler.h
	$(CXX) $(CXXFLAGS) -o mbo mbo.cpp $(LDFLAGS)

clean:
	rm -f mbo output.bin

run: mbo
	./mbo test_data.bin

.PHONY: clean run
```

## Staged Implementation

### Stage 1: Basic Operations + Delta Emission (g_crossing_enabled = false)
- Implement basic N/M/X/T operations
- Implement Runner, MBO, PriceLevels
- **TBD**: Define delta primitive format and emission mechanism
- Implement delta emission inline during MBO operations
- Implement Book reconstitution from deltas (in Runner for validation)
- Test correctness against reference output
- Verify timing with PerfProfiler

### Stage 2: Add Crossing (g_crossing_enabled = true)
- Implement has_crossed(), infer_and_apply_cross()
- Implement PriceLevels::cross()
- Track pending_cross_fills_ and reconcile in trade()
- Generate synthetic trades for consumed levels
- Ensure deltas emitted correctly during crossing
- Test correctness with crossing enabled
- Measure performance impact of crossing

### Stage 3 (Future): Multiple Deltas per Input
- Generate multiple delta streams per actual/synthetic trade (if required)
- Modify output to be 1:N instead of 1:1

## Implementation Details Resolved

1. **Output frequency**: One Book output per input record (1:1 mapping) to begin with
2. **Empty levels**: Pad with {price=0, qty=0}
3. **Unknown order ID (0)**: Ignore, skip processing
4. **Cross reconciliation**: Assert price matches, qty already removed
5. **Profiling**: perfprofiler.h handles timing and reporting

## Notes

- Reserve capacity in all boost maps upfront to eliminate allocations during processing
- Use `__attribute__((packed))` to ensure binary record layout matches expectations  
- mmap with MAP_PRIVATE for read-only access, no msync needed
- Use `madvise(MADV_WILLNEED)` to preload entire input file into memory upfront
- perfprofiler.h provides timing macros (PerfProfile, PerfProfileCount, PerfProfilerReport)
- Compile with -mavx2 (not -mavx512)
- Order side (is_ask) is immutable — never changes for a given order
- Cross reconciliation compares on qty, asserts on price
- pending_cross_fills_ is a deque (not set) for O(1) push/pop, no allocations
- Programmatic reference comparison stops at first mismatch with debug output
- **Delta emission**: MBO emits delta primitives inline (format-agnostic, no Book dependency)
- **Book reconstitution**: Runner reconstitutes deltas → Book for validation only
- **Production flow**: MBO deltas sent directly to strategy over SHM (no Book reconstitution)

## Implementation Checklist

For Gemini Flash to implement mbo.cpp:

1. **Data structures**: 
   - InputRecord, Book, Level (with static_assert sizes)
   - **Delta primitives**: TBD format (to be defined before implementation)

2. **OrderInfo struct**: {bool is_ask, Price price, Qty qty}

3. **PriceLevels<bool IsAsk> class**:
   - boost::unordered_flat_map<Price, pair<AggQty, Count>> levels_
   - Methods: add, remove, cross, get_top_levels, best_price
   - Reserve ~1000 capacity in constructor
   - **Emit deltas** during add/remove operations

4. **MBO class**:
   - Members: PriceLevels<false> bids_, PriceLevels<true> asks_
   - order_map_ (reserve ~10000), pending_cross_fills_ (deque)
   - **No Book member** - format-agnostic
   - Methods: new_order, modify_order, cancel_order, trade
   - Cross: has_crossed, infer_and_apply_cross
   - **Delta emission**: inline during operations (mechanism TBD)

5. **Runner class**:
   - mbos_ map (reserve ~100)
   - **Book reconstitution**: current_book_ (built from deltas for validation)
   - process_record() with PerfProfile wrapping
   - **Delta → Book reconstitution** logic

6. **main()**:
   - Parse args (--crossing, --reference)
   - mmap input + reference with MADV_WILLNEED
   - Process loop: emit deltas, reconstitute Book, compare
   - Output: reconstituted Books (for validation)
   - PerfProfilerReport()
   - Cleanup

7. **Makefile**: g++ -std=c++20 -O3 -mavx2

Key logic:
- modify = lookup old OrderInfo, remove old, add new
- trade = check pending_cross_fills_ first, reconcile or apply
- cross = detect, consume opposing levels, find affected orders, generate synthetic trades
- get_top_levels = sort prices by priority (bids desc, asks asc), fill array, return count
- **delta emission** = emit primitives inline from MBO operations
- **Book reconstitution** = apply deltas to Book in Runner (validation only)


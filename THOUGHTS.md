# OneRing: Universal MBO Book Builder

> *"One Ring to rule them all"* — A universal, exchange-agnostic MBO implementation

## Vision
**OneRing** is the canonical MBO implementation designed to be exchange-agnostic at its core. The architecture separates:
- **Core MBO (OneRing)**: Universal order book logic with clean semantics (one input event = one output)
- **Exchange Adapters**: Thin layers above for exchange-specific input parsing
- **Compatibility Layer**: Delta reconstruction that emulates legacy behaviors (e.g., reference MBO_v2) for transparent integration with existing strategies

This separation keeps the core implementation clean and maintainable while isolating legacy quirks (like emitting multiple callbacks for a single crossing event) to the compatibility layer. The goal is to build one MBO to rule them all, not import poorly-defined exchange-specific behaviors into the core.

## Process Guidelines
- **Be critical and accurate** — avoid speculative optimism, ground in data
- **Fork deliberately** — only when exploring genuinely divergent approaches
- **Match git branches** — code experiments should align with conversation forks
- **Keep THOUGHTS.md minimal** — only decisions, constraints, open questions

## Conversation Tracking
- **root**: Initial brainstorm, data structure selection, delta design
- Fork when: trying competing data structure approaches, need focused deep-dive

## Problem Summary
- Build 20-level order book (bid/ask with price-qty-count) from L3/MBO tick data
- C++ with AVX2, targets: Intel Raptor Lake (32MB L3), AMD 9950X3D (128MB L3)
- Single-threaded, isolated core, no preemption
- Performance: 30ns/packet target at 10M pkt/s peak (~150 cycles)

## Key Constraints
- 8B order ID, 8B price, 4B qty, 8B agg qty, 4B agg count
- Order ID → price/quantity lookup mandatory (NSE: cancel lacks price, mod lacks old price/qty)
- ~10,000s active orders per instrument expected
- Cache pressure observed (2x perf hit on Intel under load)
- **Crossing atomicity**: Must resolve cross on opposing side BEFORE inserting remainder

## Design Decisions (Phase 1)

### Data Structure: Flat Sorted Array [48]
- [x] **Structure**: `prices[48]` (sorted) + parallel `data[48]` (qty, count)
- [x] **Tracking**: `start_idx` / `end_idx` for valid range
- [x] **Ordering**: Asks ascending (best=lowest at start_idx), Bids descending (best=highest at start_idx)
- [x] **Sentinel**: `INT64_MAX` for asks, `INT64_MIN` for bids (empty slots)
- [x] **Lookup**: Linear scan Phase 0 → AVX2 scan Phase 1
- [x] **Insert/Delete**: memmove toward nearest empty region (start or end)
- [x] **Compact initially** (no holes) — holes as Phase 2 experiment
- [x] **Templated** for bid/ask (sentinel, comparator, sort direction)

### Overflow Management
- [x] **Soft limit (30-40)**: Lazy batched eviction during idle OR bounded incremental per-tick
- [x] **Hard limit (48)**: Eager synchronous eviction — cannot defer if full on insert
- [x] **Cold storage**: `boost::unordered_flat_map<Price, {qty, count}>` for levels beyond 48
- [x] **Pull from overflow**: Lazy — only when needed for output or room available

### Delta Emission
- [x] **Inline emission**: Deltas generated during MBO operations, not post-diff
- [x] **Sequential application**: Each delta applies to current book state (indices shift after DeleteLevel)
- [x] **Independent qty/count**: `UpdateLevel` has separate `qty_delta` and `count_delta` (can be 0 independently)
- [x] **Primitives**:
  - `InsertLevel(side, index, price, qty, count)` — count typically 1
  - `UpdateLevel(side, index, qty_delta, count_delta)` — independent deltas
  - `DeleteLevel(side, index)` — single level, emit multiple for bulk crossing
- [x] **Batch atomicity**: Crossing emits multiple deltas (opposing side deletes + incoming insert) as atomic batch

### Crossing
- [x] **Order**: Resolve opposing side first → then insert remainder → then emit deltas
- [x] **Delta sequence**: DeleteLevel(s) for consumed opposing levels, then InsertLevel for remainder (if any)

## Rejected/Deferred Approaches

### Chunk-based [8]×6 (Rejected)
- Inter-chunk rebalancing too complex
- "Keep one slot free" invariant adds bookkeeping
- Ordered-within-chunk still requires shifting
- Complexity cost outweighs benefits vs flat array

### Unordered [48] with AVX2 (Deferred to Phase 2)
- **Pro**: O(1) insert/delete (no shift), holes natural
- **Con**: No O(1) best-price access (critical for 99.9% trades at level 0)
- **Con**: Delta index requires popcount pass (can fuse with lookup, but no early exit)
- **Con**: Generating top-20 requires maintaining sorted [20] auxiliary array or additional lookup
- Worth benchmarking later, but ordered wins for Phase 1

### Index Cache (Deferred)
- 4-entry MRU for {price, arr_idx} — may help if profiling shows repeated lookups
- For [48] with AVX2 scan (~40 cycles), cache overhead may not pay off
- Defer until profiling shows need

## Ideas Backlog

### Phase 1 Optimizations
- AVX2 for price scan (4×64-bit per 256-bit register, 12 comparisons for 48)
- Exploit empty-slots-left for O(1) best-price insert (asks: new lower prices)
- Use neighboring hole for insert to avoid shift (if holes enabled)

### Phase 2 Experiments
- Holes + lazy compaction (tradeoff shift cost vs fragmentation)
- Unordered [48] with fused lookup+popcount, or a [48] array index to book index map that must be maintained
- Multiple chained [48] blocks if overflow is hot path

### Future (Order State)
- Order ID → OrderInfo map: `boost::unordered_flat_map<OrderId, {side, price, qty}>`
- Queue per price level (if strategy needs order priority)

## Next Steps
1. Update DESIGN.md with FlatBook struct layout
2. Implement `mbo.h` / `mbo.cpp` Stage 1 (FlatBook + basic ops + deltas)
3. Test harness with simulated data
4. Stage 2: Crossing logic
5. Profile on target machines, iterate

## Key Context for Fresh Conversations
- **Flat Array [48]** chosen: simple, cache-friendly, O(1) best-price access
- **ToB dominance**: 99.9% trades at level 0, optimize for this
- **Deltas are core**: Emitted inline, sequential application, independent qty/count
- **Crossing is atomic**: Opposing side resolved before inserting remainder
- **Hard limit eviction**: Cannot defer if array full — synchronous eviction required

---
*Last updated: Conversation root — Opus/Gemini/GPT/Sonnet consensus*

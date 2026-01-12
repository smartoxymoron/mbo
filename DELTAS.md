# MBO Delta Message Design

## Purpose
Efficiently communicate order book updates from MBO to strategy over SHM with minimal latency and predictable fixed-size chunks.

## Design Goals
- **Cache-aligned**: Most operations fit in 64 bytes (1 cache line)
- **Fixed chunks**: 64-byte DeltaChunk enables simple ring buffer transport
- **Minimal primitives**: Reduce complexity, improve generation/reconstruction performance
- **No redundant lookups**: Index determined at generation point, not via find()
- **Complete information**: Strategy can reconstruct full 20-level book state

## Final Design: Three Delta Primitives

```cpp
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
} __attribute__((packed));
static_assert(sizeof(TickInfoDelta) == 20);

struct UpdateDelta {
    uint8_t type;              // = 1
    uint8_t side_index;        // bits 0-4: index (0-19), bit 5: side
    int16_t count_delta;
    int64_t qty_delta;
} __attribute__((packed));
static_assert(sizeof(UpdateDelta) == 12);

struct InsertDelta {
    uint8_t type;              // = 2
    uint8_t side_index_shift;  // bits 0-4: index, bit 5: side, bit 6: shift
    uint16_t reserved;
    int32_t count;             // Absolute count (32-bit for high-liquidity levels)
    int64_t price;
    int64_t qty;               // Absolute qty
} __attribute__((packed));
static_assert(sizeof(InsertDelta) == 24);

struct DeltaChunk {
    uint32_t token;
    uint16_t record_idx;
    uint8_t flags;             // bit 0: final (book ready for strategy)
    uint8_t num_deltas;        // Number of deltas in this chunk (1-N)
    uint8_t payload[56];       // Variable-length delta sequence
} __attribute__((packed));
static_assert(sizeof(DeltaChunk) == 64);
```

## Reconstruction Semantics

**TickInfo**: Always first delta in sequence. For trade events, price/qty become LTP/LTQ (no separate LTP delta needed).

**Update**: Apply deltas to existing level.
```cpp
Level& lvl = book[side][index];
lvl.qty += qty_delta;
lvl.num_orders += count_delta;
if (lvl.qty <= 0) {  // Invariant: qty=0 implies count=0
    memmove(&book[side][index], &book[side][index+1], (19-index)*sizeof(Level));
    book[side][19] = {0, 0, 0};
}
```

**Insert**: Set level at index.
- `shift=1` (bit 6 of side_index_shift): Mid-book insertion (memmove levels down first, then set)
- `shift=0`: Direct set at index (for refills at end of book)

## Reconstructing Full OutputRecord

The remote (Runner) can reconstruct all OutputRecord fields from deltas without price lookups:

**bid_filled_lvls / ask_filled_lvls**: Count non-zero price levels after applying all deltas.

**bid_affected_lvl / ask_affected_lvl**: Use the index from the **first** Update/Insert delta on each side:
- If first delta is Update and causes deletion (qty → 0): `affected_lvl = 0`
- Otherwise: `affected_lvl = first_delta_index`
- If side has no deltas: `affected_lvl = 20` (not affected)
- **Note**: DeltaEmitter filters deltas with idx >= 20, so if an operation affects a level outside the top 20, no deltas are emitted and affected_lvl correctly remains 20.

**Convention**: For modify with price change, emit add (Insert) **before** remove (Update) so the first delta reflects the new resting level, not the old deleted level.

**ltp / ltq**: Extract from TickInfo.price/qty when tick_type='T' (trade events).

## Key Decisions & Reasoning

**Why 3 primitives instead of 5+?**
- Update handles both qty changes AND deletions (implicit when → 0)
- Insert handles both mid-book AND refills (via shift flag)
- TickInfo embeds LTP/LTQ (reference code always sets from trade price/qty)
- Result: Simpler generation logic, smaller messages

**Why implicit delete in Update?**
- Natural: qty=0 always means "level gone" in book semantics
- Avoids redundant "Delete then refill" → just "Update(→0) then refill"
- Saves 4+ bytes per deletion
- Tradeoff: Reconstruction must check qty/count ≤ 0 (acceptable)

**Why int64_t qty instead of int32_t?**
- Some exchanges/instruments trade billions of quantity
- 4 extra bytes per delta acceptable for universality
- Aggregate quantities (especially accumulated over multiple levels) can overflow int32

**Why shrink UpdateDelta to 12 bytes?**
- Enables critical case (Modify price change) to fit exactly in 56-byte payload
- Most common operations now fit 1 chunk (99% vs 85%)
- Tradeoff: count_delta limited to int16_t (±32K), acceptable since single update rarely changes count by >32K

**Why fixed 64-byte chunks with variable deltas?**
- Fixed size simplifies SHM ring buffer (no size prefix parsing)
- Most operations (99%) fit in 1-2 chunks
- Multi-chunk sequences use `final` flag to signal completion
- Alternative (variable-size messages) requires complex framing

**Why num_deltas in header instead of 0xFF end markers?**
- Cleaner: no sentinel scanning required
- Enables validation: chunk parsing can assert exactly num_deltas consumed
- 1 byte well-spent for robustness

## Size Analysis

**Payload limit: 56 bytes per chunk** (64-byte chunk - 8-byte header)

| Event | Deltas | Payload Bytes | Chunks | Total Bytes |
|-------|--------|---------------|--------|-------------|
| New order (passive, new level) | Tick + Insert | 20+24 = 44 | 1 | 64 |
| New order (passive, existing) | Tick + Update | 20+12 = 32 | 1 | 64 |
| Cancel (level deleted) | Tick + Update(→0) | 20+12 = 32 | 1 | 64 |
| Modify (same price) | Tick + Update | 20+12 = 32 | 1 | 64 |
| Modify (price change) | Tick + Update + Insert | 20+12+24 = **56** | 1 | 64 |
| Trade (both partial) | Tick + Update + Update | 20+12+12 = 44 | 1 | 64 |
| Trade (one full fill) | Tick + Update(→0) + Update | 20+12+12 = 44 | 1 | 64 |
| Cross (1 lvl) + residual (no refill) | Tick + Update + Insert | 20+12+24 = **56** | 1 | 64 |
| Cross (1 lvl) + refill + residual | Tick + Update + 2×Insert | 20+12+48 = 80 | 2 | 128 |
| Cross (3 lvl) + residual | Tick + 3×Update + Insert | 20+36+24 = 80 | 2 | 128 |
| Cross (3 lvl) + 3 refill + residual | Tick + 3×Update + 4×Insert | 20+36+96 = 152 | 3 | 192 |
| Aggressive mod (worst case) | Tick + 4×Update + 4×Insert | 20+48+96 = 164 | 3 | 192 |
| Post-crossing trade | Tick only | 20 | 1 | 64 |
| Snapshot (40 levels) | Tick + 40×Insert | 20+960 = 980 | 18 | 1152 |

**Distribution estimate (NSE data):**
- 1 chunk: ~99% of events (passive orders, cancels, trades, price changes, simple crosses)
- 2 chunks: ~0.9% (crosses with refills)
- 3+ chunks: ~0.1% (multi-level crosses with multiple refills)

## Edge Cases Handled

**Order at existing price level**: Update (not Insert). Generation checks if level exists.

**Partial fill vs full fill**: Both use Update. Full fill → qty/count reach 0 → implicit delete.

**Price modification**: Update(→0) old level + Insert new level. Two operations, order matters.

**Multi-level crossing**: Sequential Update(→0) at index 0. Each causes shift, next update also targets index 0.

**Refills after deletion**: InsertDelta with shift=0 at indices 17-19. No memmove, direct set.

**IOC/Market orders (no residual)**: Only Update deltas, no Insert for passive side.

**Post-crossing trade confirmation**: TickInfo only (num_deltas=0). Book already in final state.

**Snapshot bootstrapping**: Strategy joining mid-stream receives periodic full snapshot (Tick + 40×Insert). 1152 bytes total (18 chunks), acceptable for 1/sec frequency.

## Generation Flow

Deltas emitted inline during PriceLevels operations:

```cpp
// In PriceLevels::add()
if (is_new_level && idx < 20) {
    uint8_t side_index_shift = idx | (side << 5) | (1 << 6); // shift=1
    emit(InsertDelta{side_index_shift, price, qty, count});
} else if (idx < 20) {
    uint8_t side_index = idx | (side << 5);
    emit(UpdateDelta{side_index, +qty, +count});
}

// In PriceLevels::remove()
if (idx < 20) {
    uint8_t side_index = idx | (side << 5);
    emit(UpdateDelta{side_index, -qty, -count});
}
// Deletion implicit if qty/count → 0

// After deletion causes shift, refill from overflow:
for (auto& lvl : refill_levels) {
    uint8_t side_index_shift = idx | (side << 5); // shift=0
    emit(InsertDelta{side_index_shift, lvl.price, lvl.qty, lvl.count});
    idx++;
}
```

MBO::update_book() finalizes sequence:
- Prepend TickInfo from input event
- Pack deltas into 64-byte chunks
- Set `final` flag on last chunk
- No need for `last_affected_price` state tracking

## Open Questions (Deferred)

**Compression**: Could pack multiple UpdateDelta (16 bytes each) more tightly. Deferred until profiling shows bandwidth bottleneck.

**Batch operations**: Could add "UpdateRange" for N consecutive levels with same delta. Not common enough to justify complexity.

**Version field**: May need chunk versioning for protocol evolution. Reserved bits available in flags.

## Validation Against Reference

Reference implementation (CROSSING.md) generates multiple ticks per exchange event. Our deltas are orthogonal:
- Reference: TickInfo stream (newOrderCross → tradeMsg)
- Ours: Delta stream (minimal book updates)
- Both reconstruct to identical 20-level OutputRecord
- Validation: memcmp() reconstructed book against reference output

---

## Implementation Plan

### Phase 1: Delta Structures & Utilities

- [x] **1.1** Add delta struct definitions to mbo.cpp (after existing structs)
  - TickInfoDelta (20 bytes)
  - UpdateDelta (12 bytes)  
  - InsertDelta (24 bytes)
  - DeltaChunk (64 bytes)
  - static_assert all sizes

- [x] **1.2** Add << operators for delta types (single-line compact format)
  - `TickInfoDelta::operator<<` - show tick_type, side, price, qty
  - `UpdateDelta::operator<<` - decode side_index, show deltas
  - `InsertDelta::operator<<` - decode side_index_shift, show shift flag
  - `DeltaChunk::operator<<` - iterate payload, print all deltas + total bytes

- [x] **1.3** Add bitmask helper functions
  - `pack_side_index(side, index)` → uint8_t
  - `pack_side_index_shift(side, index, shift)` → uint8_t
  - `unpack_side(packed)` → bool
  - `unpack_index(packed)` → uint8_t
  - `unpack_shift(packed)` → bool

### Phase 2: DeltaEmitter Class

- [x] **2.1** Define DeltaEmitter class in mbo.cpp
  - Private: `boost::container::static_vector<DeltaChunk, MAX_CHUNKS>`
    - MAX_CHUNKS = 20 (worst case: snapshot with 40 levels = 18 chunks + buffer)
  - Private: current_offset_, token_, record_idx_
  - Public: `void set_event(token, record_idx)` - set metadata for new event
  - Public: `void emit_tick_info(tick_type, side, is_exch, price, qty)`
  - Public: `void emit_update(side, index, qty_delta, count_delta)`
  - Public: `void emit_insert(side, index, shift, price, qty, count)`
  - Public: `void finalize()` - set final flag on last chunk
  - Public: `std::span<const DeltaChunk> get_chunks()` - returns view of chunks
  - Public: `void clear()`

- [x] **2.2** Implement chunk packing logic
  - Track bytes used in current chunk payload (max 56)
  - When delta doesn't fit, flush current chunk (final=0), start new
  - On finalize(), set final=1 on last chunk
  - Increment num_deltas as deltas added

- [x] **2.3** Handle edge cases
  - First emit must be TickInfo (assert or auto-insert)
  - Empty sequence (TickInfo only) still valid
  - num_deltas validation in print

### Phase 3: PriceLevels Delta Emission

- [x] **3.1** Add DeltaEmitter* to PriceLevels
  - Constructor takes emitter pointer (can be nullptr initially)
  - `void set_emitter(DeltaEmitter* e)` for late binding

- [x] **3.2** Update PriceLevels::add() to emit deltas
  - Get index before/after modification: `idx = it - levels_.begin()`
  - If new level created at idx < 20: `emit_insert(side, idx, shift=1, price, qty, 1)`
  - Else if existing level at idx < 20: `emit_update(side, idx, +qty, +1)`

- [x] **3.3** Update PriceLevels::remove() to emit deltas
  - Get index before erase: `idx = it - levels_.begin()`
  - If idx < 20: `emit_update(side, idx, -qty, -count)`
  - After erase (if needed), check if level 20 now visible → emit refill

- [x] **3.4** Implement refill logic
  - After deletion at idx < 20, check `levels_.size()`
  - If size >= 20, emit `Insert(side, 19, shift=0, ...)` for new level 19
  - Single-level refill for single deletion (batching deferred for crossing)

### Phase 4: MBO Integration

- [x] **4.1** Add DeltaEmitter to MBO class
  - Member: `DeltaEmitter emitter_;`
  - Constructor: Pass emitter pointer to bids_/asks_ via set_emitter()

- [x] **4.2** Update MBO operations to emit TickInfo
  - new_order(): `emitter_.emit_tick_info('N', is_ask, ...)`
  - modify_order(): `emitter_.emit_tick_info('M', ...)`
  - cancel_order(): `emitter_.emit_tick_info('X', ...)`
  - trade(): `emitter_.emit_tick_info('T', ...)`

- [x] **4.3** Create process_event() to orchestrate delta emission
  - Restructured: process_event() calls clear(), set_event(), operation, update_book(), finalize()
  - Add `get_delta_chunks()` accessor
  - Runner now calls mbo.process_event() instead of individual operations

- [ ] **4.4** Remove last_affected_price tracking (deferred to post-validation)
  - Delete `last_affected_bid_price_`, `last_affected_ask_price_`
  - affected_lvl now computed from deltas (post-validation, for reference compat)

### Phase 5: Runner & Validation

- [x] **5.1** Update Runner::process_record()
  - After MBO operations, call `const auto& chunks = mbo.get_delta_chunks()`
  - Print all chunks with operator<< overload

- [x] **5.2** Implement delta reconstruction (for validation)
  - New function: `reconstruct_from_deltas(chunks) → OutputRecord`
  - Apply each delta sequentially to empty book
  - Track first delta index per side for affected_lvl (no lookups!)
  - Return final OutputRecord with all fields

- [x] **5.3** Compare reconstructed vs direct OutputRecord
  - Both methods should produce identical book state
  - Use OutputRecord::compare() method
  - Exit with detailed diff on mismatch

- [x] **5.4** Preserve OutputRecord generation initially
  - Keep update_book() logic for reference comparison
  - Remove only after delta validation passes on full test suite

### Phase 6: Testing & Refinement

- [ ] **6.1** Test with existing test data
  - Verify all chunks print correctly
  - Validate reconstruction matches reference
  - Check chunk count distribution (should be ~99% single-chunk)

- [ ] **6.2** Test edge cases
  - Empty book operations
  - Multi-level crossing with refills
  - Snapshot (40 levels)
  - Post-crossing trade (TickInfo only)

- [ ] **6.3** Verify affected_lvl compatibility
  - Extract from deltas: scan for Update/Insert, note index
  - Compare against reference OutputRecord.{bid,ask}_affected_lvl
  - Adjust extraction logic if needed for reference compat

- [ ] **6.4** Performance check
  - Measure delta generation overhead with PerfProfiler
  - Should be negligible vs book operations

### Phase 7: Documentation Updates

- [ ] **7.1** Update DESIGN.md with interface note
  - Note: static_vector approach has cache pollution (~500KB/s for 500 instruments)
  - Alternative for Phase 2: Runner provides pre-allocated DeltaChunk array per instrument
  - MBO writes directly to provided buffer (zero-copy)

- [ ] **7.2** Document delta validation results
  - Add to DELTAS.md or STATS.md: chunk count distribution from test run
  - Confirm 99%+ single-chunk hypothesis

### Deferred to Phase 2 (Post-Validation)

- [ ] Remove OutputRecord generation from MBO
- [ ] Switch to Runner-provided buffer interface (zero-copy)
- [ ] Optimize delta packing (compression, batching)
- [ ] Add delta versioning field for protocol evolution

---
*Design finalized: Jan 2026 - Sonnet 4.5 + o1 collaborative analysis*

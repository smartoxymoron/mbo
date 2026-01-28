# MBO Crossing Implementation Review

## Part 1: Publisher Side

### 1. STATE REDUCTION: PendingCross Fields

**Current**: 8 fields in `PendingCross`.

**Analysis**:
- `original_resting_price`, `aggressor_original_qty`, `original_affected_lvl`: Only needed for fully-consumed modify (X tick emission). Could be derived from order_map at crossing start, but order_map is mutated during modify. **Keep** — derivation would require a snapshot.
- `residual_tick_type`: Distinguishes 'N' vs 'M' origin. Could instead check `original_resting_price != 0`. **Eliminate**.
- `aggressor_on_level`: Tracks whether residual was added. Could derive from `order_map[aggressor_id].qty > passive.pending_cross_fill_qty()`. **Eliminate** — derive on demand.

**Proposal**: Reduce to 6 fields. Use `original_resting_price != 0` to detect modify-origin, derive `aggressor_on_level` when needed.

```cpp
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Price original_resting_price = 0;   // 0 for new_order, set for modify
    Qty aggressor_original_qty = 0;
    int8_t original_affected_lvl = 20;
    
    bool is_active() const { return aggressor_id != 0; }
    bool from_modify() const { return original_resting_price != 0; }
    void clear() { aggressor_id = 0; }
};
```

### 2. cancel_order() DECOMPOSITION

**Current**: 120 lines, three nested branches sharing common cleanup but diverging in tick emission and level operations.

**Observation**: All three paths share:
1. Determine cancel type (aggressor/passive-self-trade/regular)
2. Emit primary tick (C or X)
3. Modify levels (uncross/remove/re-cross)
4. Emit secondary tick if needed (S)
5. Maybe emit CrossingComplete
6. Erase from order_map

**Proposal**: Extract a `SelfTradeContext` helper that captures the decision and executes the variant:

```cpp
enum class CancelType { Regular, AggressorSelfTrade, PassiveSelfTrade };

CancelType classify_cancel(OrderId id, const OrderInfo& info) const {
    if (!pending_cross_.is_active()) return CancelType::Regular;
    if (id == pending_cross_.aggressor_id) return CancelType::AggressorSelfTrade;
    
    bool passive_side = (info.is_ask != pending_cross_.aggressor_is_ask);
    if (!passive_side) return CancelType::Regular;
    
    bool would_cross = pending_cross_.aggressor_is_ask 
        ? (pending_cross_.aggressor_price <= info.price)
        : (pending_cross_.aggressor_price >= info.price);
    return would_cross ? CancelType::PassiveSelfTrade : CancelType::Regular;
}
```

Then `cancel_order()` becomes:
```cpp
void MBO::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) {
        emitter_.emit_tick_info('X', false, true, 0, 0, id);
        return;
    }
    
    switch (classify_cancel(id, it->second)) {
        case CancelType::AggressorSelfTrade: cancel_aggressor(it); break;
        case CancelType::PassiveSelfTrade:   cancel_passive_self_trade(it); break;
        case CancelType::Regular:            cancel_regular(it); break;
    }
    order_map_.erase(it);
}
```

Each variant is 20-30 lines, single responsibility, testable in isolation.

### 3. CROSSING LIFECYCLE: cross_fills_ Management

**Current**: 7 methods on PriceLevels, "skip confirmed from front" logic duplicated in `pending_cross_vwap()` and `uncross()`.

**Proposal**: Extract the skip logic into a single iterator:

```cpp
// Returns range of unconfirmed fills (skipping confirmed from front)
std::span<const CrossFill> unconfirmed_fills() const {
    Qty total = 0;
    for (const auto& f : cross_fills_) total += f.qty;
    Qty skip = total - pending_cross_fill_qty_;
    
    size_t start = 0;
    for (; start < cross_fills_.size() && skip >= cross_fills_[start].qty; ++start)
        skip -= cross_fills_[start].qty;
    return {cross_fills_.data() + start, cross_fills_.size() - start};
}
```

Then `pending_cross_vwap()` and `uncross()` both iterate `unconfirmed_fills()` without duplicating skip logic.

**Deeper consolidation**: Consider tracking `confirmed_qty_` directly instead of deriving it. On `reconcile_cross_fill()`, increment `confirmed_qty_`. The skip calculation becomes `confirmed_qty_` directly.

### 4. CROSSING COMPLETION in trade()

**Current**: Lines 1177-1207 mix MBO semantics with reference-compat (synthetic X tick for fully-consumed modify).

**Proposal**: The synthetic X belongs in the receiver, not the publisher. The publisher should emit a richer `CrossingComplete` delta:

```cpp
struct CrossingCompleteDelta {
    uint8_t type;           // = 3
    uint8_t flags;          // bit 0: from_modify, bit 1: has_residual
    int8_t original_lvl;    // for X emission on receiver
    Price original_price;   // for X emission
    Qty original_qty;       // for X emission
};
```

Publisher emits this unconditionally when crossing completes. Receiver decides whether to synthesize X based on flags. This moves reference-compat logic entirely to receiver.

**Trade-off**: Grows CrossingComplete from 1B to ~20B. But crossing is 2% of events, acceptable.

### 5. PRICE COMPARISON: "would_cross" Check

**Current**: Three locations with slight variations:
- `new_order`: `is_ask ? (price <= passive_best) : (price >= passive_best)`
- `modify_order`: same pattern
- `cancel_order` (passive check): `aggressor_is_ask ? (...) : (...)`

**Proposal**: Free function or static method:

```cpp
inline bool would_cross(bool aggressor_is_ask, Price aggressor_price, Price passive_best) {
    if (passive_best == 0) return false;
    return aggressor_is_ask ? (aggressor_price <= passive_best) 
                            : (aggressor_price >= passive_best);
}
```

Single definition, used in all three places. Eliminates the subtle inversion risk.

---

## Part 2: Receiver Side

### 6. MULTI-OUTPUT EXPANSION

**Current**: "finalize, push, reset" pattern appears 3 times. `extra_records` vector, `multi_tick_secondary` flag, ordering logic in `process_deltas()`.

**Proposal**: Replace with a `DeltaExpander` that yields records in order:

```cpp
class DeltaExpander {
    OutputRecord current_;
    std::array<OutputRecord, 3> pending_;  // max 3 records (C+S+N)
    uint8_t pending_count_ = 0;
    uint8_t yield_idx_ = 0;
    
public:
    // Returns true while more records available
    bool next(OutputRecord& out);
    
    // Process deltas, populating pending_ in correct order
    void apply(std::span<const DeltaChunk> chunks, PendingAggressorState& agg);
};
```

Usage in `process_deltas()`:
```cpp
DeltaExpander expander;
expander.apply(shm_deltas_, agg_state);
OutputRecord rec;
while (expander.next(rec)) {
    if (!observer.on_book_update(rec)) return false;
}
```

The ordering logic moves inside `apply()`, eliminating the scattered finalization and the `multi_tick_secondary` flag.

### 7. PendingAggressorState Simplification

**Current**: 7 fields + `crossing_complete` flag.

**Observation**: `aggressor_remaining` is computed from `aggressor_original_qty` minus trades. The receiver tracks this because the publisher doesn't tell it.

**Proposal A** (Publisher emits more): Include `aggressor_remaining` in each trade's TickInfo during crossing. Receiver no longer needs to track it.

**Proposal B** (Eliminate redundancy): If we adopt the richer `CrossingComplete` (proposal 4), the receiver doesn't need to track original qty/price — they come from the delta. `PendingAggressorState` reduces to:

```cpp
struct PendingAggressorState {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Qty aggressor_remaining = 0;
    char original_tick_type = 0;
    
    bool is_active() const { return aggressor_id != 0; }
    void clear() { aggressor_id = 0; }
};
```

4 fields instead of 8. Price/qty come from CrossingComplete when needed.

### 8. CrossingComplete Delta

**Current**: 1-byte signal. Receiver must maintain state to know what to synthesize.

**Proposal**: Make it carry enough data that receiver needs no memory:

```cpp
struct CrossingCompleteDelta {
    uint8_t type;              // = 3
    uint8_t flags;             // bit 0: from_modify, bit 1: has_residual, bit 2: aggressor_is_ask
    Price aggressor_price;
    Qty aggressor_remaining;   // 0 if fully consumed
    OrderId aggressor_id;
    // For from_modify && !has_residual case:
    Price original_resting_price;
    Qty original_qty;
};
```

~40 bytes, but only emitted on crossing completion (rare). Receiver synthesizes N/M/X directly from this without needing `PendingAggressorState` at all for the completion path.

**Trade-off**: Receiver still needs `aggressor_id` during crossing for trade reconciliation. So `PendingAggressorState` can't be fully eliminated, but it becomes much simpler (just id + remaining).

### 9. PUBLISHER-RECEIVER BOUNDARY

**Current**: Publisher emits C and S as separate TickInfos. Receiver expands to C+S+N.

**Question**: Should publisher emit the full sequence?

**Analysis**: 
- The "full sequence" is reference-compat, not universal MBO semantics
- Different exchanges/strategies may want different output formats
- Self-trade handling is exchange-specific

**Recommendation**: Keep current split. Publisher emits universal crossing events (C, S as they occur), receiver handles reference-compat expansion. This keeps MBO exchange-agnostic.

However, the S tick could be enriched:
```cpp
// S tick carries aggressor info so C expansion doesn't need to look it up
emitter_.emit_tick_info('S', ..., cancelled_order_id, aggressor_id);
```

The `order_id2` field already exists — use it for aggressor_id in S ticks. Receiver can then expand C+S without needing to remember aggressor state.

---

## Summary: High-Impact Changes

| Change | Complexity Reduction | Risk | Effort |
|--------|---------------------|------|--------|
| Extract `would_cross()` | Minor | None | 10min |
| Reduce PendingCross to 6 fields | Minor | Low | 20min |
| Extract `classify_cancel()` + 3 methods | Major | Medium | 1hr |
| Extract `unconfirmed_fills()` iterator | Medium | Low | 30min |
| Richer CrossingComplete delta | Major | Medium | 2hr |
| `DeltaExpander` for receiver | Major | Medium | 2hr |

**Recommended sequence**:
1. `would_cross()` — quick win, reduces error surface
2. `unconfirmed_fills()` — cleans up PriceLevels, isolated change
3. `classify_cancel()` decomposition — biggest readability win
4. Richer CrossingComplete — simplifies receiver, but touches both sides

The receiver-side `DeltaExpander` can be done independently and would make the multi-output logic much cleaner.

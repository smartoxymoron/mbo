# MBO Crossing Implementation Review

## Part 1: Publisher Side

### 1. PendingCross State Reduction (8 → 6 fields)

**`residual_tick_type` is derivable.** It's `'M'` when `original_resting_price != 0`, `'N'` otherwise. Since price=0 is already used as a sentinel throughout (line 679: `if (best == 0) break`), this is safe. Eliminate the field and derive on use:

```cpp
bool is_from_modify() const { return original_resting_price != 0; }
```

**`original_resting_price`, `original_affected_lvl`, `aggressor_original_qty` — genuinely needed but are purely reference-compat cost.** These three exist solely for the fully-consumed-modify X tick in `trade()` (lines 1189-1199). They're never read elsewhere. Two options:

**Conservative:** Keep them, add a comment `// reference-compat: X tick for fully consumed modify`, derive `residual_tick_type` → 7 fields.

**Bold (recommended):** Move the X-tick-for-modify responsibility to the receiver. Emit `CrossingComplete` for ALL completion cases (including fully-consumed modify) and enrich it:

```cpp
struct CrossingCompleteDelta {
    uint8_t type;         // = 3
    uint8_t flags;        // bit 0: has_residual, bit 1: from_modify
};  // 2 bytes (was 1)
```

Emit the old resting info once at B-tick time as a small auxiliary delta (only for modify-that-crosses, very rare):

```cpp
struct ModifyOriginDelta {  // emitted after B TickInfo, before any level deltas
    uint8_t type;           // = 4
    int8_t  original_lvl;
    Price   original_price;
    Qty     original_qty;
};  // 14 bytes, emitted once per modify-cross
```

The receiver captures this alongside the B tick (same as it already captures S ticks). Now:
- PendingCross drops to **5 fields**: `{aggressor_id, aggressor_is_ask, aggressor_price, aggressor_on_level, is_from_modify}`
- `trade()` crossing completion becomes 3 lines:

```cpp
if (cross_passive.pending_cross_fill_qty() == 0) {
    cross_passive.clear_cross_fills();
    auto agg_it = order_map_.find(pending_cross_.aggressor_id);
    bool has_residual = (agg_it != order_map_.end() && agg_it->second.qty > 0);
    emitter_.emit_crossing_complete(has_residual, pending_cross_.is_from_modify());
    pending_cross_.clear();
}
```

**`aggressor_on_level` cannot be eliminated.** Traced carefully: it tracks whether a count of 1 was added to the aggressor's resting level. Since PriceLevels aggregates (no per-order tracking), this boolean is the only way to avoid double-counting during passive self-trade re-cross (line 1088). In the aggressor-cancel path, it appears redundant with `residual_on_level > 0` — the arithmetic works out to `residual_on_level == original_residual` which is 0 exactly when `aggressor_on_level` is false. But the passive cancel path genuinely needs it.

---

### 2. cancel_order() Decomposition

The 120-line method is three genuinely different operations sharing a lookup and an erase. Don't try to unify them with a clever abstraction — extract them as named methods:

```cpp
void MBO::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) {
        emitter_.emit_tick_info('X', false, true, 0, 0, id);
        return;
    }
    OrderInfo& info = it->second;

    if (pending_cross_.is_active() && id == pending_cross_.aggressor_id)
        cancel_aggressor(info);
    else if (is_passive_self_trade_cancel(info))
        cancel_passive_self_trade(id, info);
    else
        cancel_regular(id, info);

    order_map_.erase(it);
}
```

Where `is_passive_self_trade_cancel()` encapsulates the 8-line classification block (lines 1044-1055):

```cpp
bool MBO::is_passive_self_trade_cancel(const OrderInfo& info) const {
    if (!pending_cross_.is_active()) return false;
    if (info.is_ask == pending_cross_.aggressor_is_ask) return false;  // same side = not passive
    return prices_would_cross(pending_cross_.aggressor_is_ask,
                              pending_cross_.aggressor_price, info.price);
}
```

Each of the three handlers is 15-30 lines, trivially reviewable. No shared abstraction forced — the three paths are genuinely different algorithms.

---

### 3. Crossing Lifecycle: SpeculationLog Extraction

Seven methods on PriceLevels for crossing state, with the "skip confirmed from front" pattern duplicated between `pending_cross_vwap()` and `uncross()`. The root issue: `cross_fills_`, `pending_cross_fill_qty_`, and `pending_cross_fill_count_` form a cohesive sub-object — a FIFO speculation log with a confirmation cursor.

Extract it:

```cpp
class SpeculationLog {
    std::vector<CrossFill> fills_;
    Qty pending_qty_ = 0;
    Count pending_count_ = 0;

public:
    Qty pending_qty() const { return pending_qty_; }

    void record(Price price, Qty qty, Count count) {
        fills_.push_back({price, qty, count});
        pending_qty_ += qty;
        pending_count_ += count;
    }

    void confirm_qty(Qty qty) { pending_qty_ -= std::min(qty, pending_qty_); }
    void confirm_count(Count c) { pending_count_ -= c; }
    void unreserve(Qty qty) { confirm_qty(qty); if (pending_count_ > 0) pending_count_--; }

    // Iterate unconfirmed fills (skip confirmed FIFO prefix)
    template<typename F>
    void for_each_unconfirmed(F&& fn) const {
        Qty skip_qty = 0;
        for (const auto& f : fills_) skip_qty += f.qty;
        skip_qty -= pending_qty_;
        Count skip_count = 0;
        for (const auto& f : fills_) skip_count += f.count;
        skip_count -= pending_count_;

        for (const auto& f : fills_) {
            if (skip_qty >= f.qty) { skip_qty -= f.qty; skip_count -= f.count; continue; }
            fn(f.price, f.qty - skip_qty, f.count - skip_count);
            skip_qty = 0; skip_count = 0;
        }
    }

    std::pair<Price, Qty> pending_vwap() const {
        if (pending_qty_ == 0) return {0, 0};
        int64_t volume = 0; Qty counted = 0;
        for_each_unconfirmed([&](Price p, Qty q, Count) {
            Qty use = std::min(q, pending_qty_ - counted);
            volume += p * use;
            counted += use;
        });
        return {counted > 0 ? Price(volume / counted) : 0, pending_qty_};
    }

    void clear() { fills_.clear(); pending_qty_ = 0; pending_count_ = 0; }
    bool empty() const { return pending_qty_ == 0; }
};
```

PriceLevels becomes:

```cpp
class PriceLevels {
    // ...
    SpeculationLog speculation_;

    // Seven methods collapse to:
    Qty cross(Price aggressor_price, Qty aggressor_qty);      // uses speculation_.record()
    void uncross();                                            // uses speculation_.for_each_unconfirmed()
    // Five others forward directly to speculation_
};
```

`uncross()` stays on PriceLevels because it needs `add_liquidity()`, but it uses `for_each_unconfirmed()` instead of hand-rolled skip logic:

```cpp
void uncross() {
    speculation_.for_each_unconfirmed([&](Price p, Qty q, Count c) {
        Price canonical = p * side_multiplier_;
        auto it = levels_.find(canonical);
        add_liquidity(p, q, (it != levels_.end()) ? 0 : c);
    });
    speculation_.clear();
}
```

The "skip confirmed from front" logic now lives in exactly one place.

---

### 4. Crossing Completion in trade()

Lines 1177-1207 mix three concerns: checking completion, emitting reference-compat X for fully-consumed modifies, and emitting CrossingComplete for everything else.

With the enriched CrossingComplete from proposal 1, this collapses to:

```cpp
if (pending_cross_.is_active() && cross_passive.speculation_.empty()) {
    cross_passive.speculation_.clear();
    auto agg_it = order_map_.find(pending_cross_.aggressor_id);
    bool has_residual = (agg_it != order_map_.end() && agg_it->second.qty > 0);
    emitter_.emit_crossing_complete(has_residual, pending_cross_.is_from_modify());
    pending_cross_.clear();
}
```

All synthesis (N, M, or X) moves to the receiver where it belongs. The publisher just says "crossing done, here are the facts."

---

### 5. would_cross Unification

Four sites, one semantic: "do these two prices cross the spread?"

```cpp
// Crossing occurs when bid_price >= ask_price.
// Caller provides aggressor's side to determine which is which.
inline bool prices_would_cross(bool aggressor_is_ask, Price aggressor, Price passive) {
    return aggressor_is_ask ? (aggressor <= passive) : (aggressor >= passive);
}
```

Call sites become:

| Location | Before | After |
|----------|--------|-------|
| `new_order` | `is_ask ? (price <= passive_best) : (price >= passive_best)` | `prices_would_cross(is_ask, price, passive_best)` |
| `modify_order` | `info.is_ask ? (new_price <= passive_best) : (new_price >= passive_best)` | `prices_would_cross(info.is_ask, new_price, passive_best)` |
| `cancel_order` | `aggressor_is_ask ? (aggressor_price <= info.price) : (aggressor_price >= info.price)` | `prices_would_cross(aggressor_is_ask, aggressor_price, info.price)` |
| `cross()` | `is_ask_ ? (best <= aggressor_price) : (best >= aggressor_price)` | `prices_would_cross(!is_ask_, aggressor_price, best)` |

One function, zero conditional reasoning at call sites.

---

## Part 2: Receiver Side

### 6. Multi-Output: Finalize/Push/Reset Pattern

The 8-line finalization block (set affected_lvl, count filled levels, push to extras) is copy-pasted 3 times (lines 1282-1290, 1387-1395, 1439-1449). Extract:

```cpp
inline void finalize_record(OutputRecord& rec, const uint8_t (&affected)[2]) {
    rec.bid_affected_lvl = affected[0];
    rec.ask_affected_lvl = affected[1];
    rec.bid_filled_lvls = 0;
    rec.ask_filled_lvls = 0;
    for (int j = 0; j < 20; ++j) {
        if (rec.bids[j].price != 0) rec.bid_filled_lvls++;
        if (rec.asks[j].price != 0) rec.ask_filled_lvls++;
    }
}
```

The three sites differ only in whether `affected_lvl` is reset afterward (reset for secondary TickInfo, NOT reset for CrossingComplete-synthesized ticks). This is a 1-line difference after the call, not a reason to avoid extraction.

**Multi-output ordering.** The `multi_tick_secondary` heuristic in `process_deltas()` (lines 1641-1643) detects delivery order by peeking at the first extra record's tick type. This is fragile. Better: have `apply_deltas_to_book` return records in delivery order. Replace the `extra_records` out-parameter with a flat output buffer:

```cpp
// Returns all OutputRecords in delivery order (rec is always included)
int apply_deltas_to_book(OutputRecord& rec, std::span<const DeltaChunk> chunks,
                          PendingAggressorState& agg_state,
                          boost::container::static_vector<OutputRecord, 4>& output);
```

The function pushes records to `output` in the correct order internally. `process_deltas()` just delivers them in sequence — no ordering heuristic needed.

---

### 7. PendingAggressorState Reduction

Current: 7 fields + `crossing_complete`. With the enriched CrossingComplete delta from proposal 1:

**`original_tick_type` eliminated.** CrossingComplete now carries `from_modify` flag. The receiver no longer needs to remember whether the original tick was A or B to decide between N/M/X synthesis.

**`aggressor_original_qty` eliminated.** Only needed for X synthesis (fully-consumed modify). With the `ModifyOriginDelta` from proposal 1, the receiver captures old resting info at B-tick time, not from agg_state.

**`crossing_complete` flag eliminated.** This flag exists solely because CrossingComplete can arrive during a C tick's processing (aggressor self-trade cancel), and the C expansion code needs to know. If the enriched CrossingComplete carries `has_residual`, the C expansion can derive everything directly.

Reduced PendingAggressorState:

```cpp
struct PendingAggressorState {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Qty aggressor_remaining = 0;     // Updated on trades
    // Captured from ModifyOriginDelta (only if from modify-cross):
    Price original_resting_price = 0; // 0 → from new_order
    Qty original_resting_qty = 0;
    int8_t original_resting_lvl = 20;
};
```

The `original_resting_*` fields replace both `original_tick_type` and `aggressor_original_qty` — they are the data, not metadata about data.

---

### 8. CrossingComplete: Signal → Data

Expand from 1 byte to 2:

```cpp
struct CrossingCompleteDelta {
    uint8_t type;   // = 3
    uint8_t flags;  // bit 0: has_residual, bit 1: from_modify
};
```

This is still trivially small, fits in any chunk, and eliminates the receiver's need to:
- Compute `need_residual` from `aggressor_remaining > 0`
- Compute `need_cancel` from `!need_residual && original_tick_type == 'B'`
- Maintain the `crossing_complete` flag for self-trade timing

The publisher knows these facts; encoding them costs 1 extra byte per crossing event (~2% of events).

**Can CrossingComplete be implicit?** No. The receiver can't distinguish "last reconciling trade" from other trades without the publisher's knowledge of `pending_cross_fill_qty == 0`. The signal is necessary. But it should carry enough data to make the receiver's job mechanical.

---

### 9. Publisher-Receiver Boundary

The current boundary is correct in principle: publisher emits semantic deltas, receiver expands for reference-compat. Don't change it.

The S tick is already sideband metadata (captured at line 1268, consumed at 1475/1508, never delivered as a standalone record). Formalizing this is worthwhile — rename `TickInfoDelta` with `tick_type='S'` to something like a `SidebandInfo` flag, or simply document that S ticks are publisher→receiver metadata, not user-visible events.

The bigger improvement is structural: if `apply_deltas_to_book` returns records in delivery order (proposal 6), and CrossingComplete carries completion flags (proposal 8), the receiver's C expansion code shrinks from 90 lines to ~40. The `multi_tick_secondary` heuristic disappears entirely.

---

## Summary: Impact Matrix

| Proposal | Lines Saved | State Removed | Risk |
|----------|-------------|---------------|------|
| 1. Enriched CrossingComplete + ModifyOriginDelta | ~20 in trade() | 3 fields from PendingCross | Medium — new delta type |
| 2. cancel_order() decomposition | 0 net (reorganized) | — | Low — pure extract |
| 3. SpeculationLog extraction | ~15 (dedupe skip logic) | 3 raw fields → 1 object | Low — encapsulation |
| 4. trade() completion (follows from 1) | ~25 | — | Follows from 1 |
| 5. prices_would_cross() | ~8 | — | Trivial |
| 6. finalize_record() + ordered output | ~25 | multi_tick_secondary gone | Low |
| 7. PendingAggressorState (follows from 1, 8) | — | 3 fields + flag | Follows from 1, 8 |
| 8. CrossingComplete flags | — | Receiver derivation logic | Low |

**Recommended order:** 5 (trivial, immediate), 2 (pure extract), 6 (dedupe), 3 (encapsulate), then 1+4+8 together (the boundary change). Each step is independently testable against the reference.

**Total projected reduction:** ~90 lines of logic, 6 fields of state, elimination of the most fragile patterns (skip-confirmed duplication, multi_tick_secondary heuristic, scattered would_cross formulations). The code should feel like crossing is a natural extension — speculate, log, confirm or rollback — not a bolted-on state machine.

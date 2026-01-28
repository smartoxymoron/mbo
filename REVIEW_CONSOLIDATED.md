# Consolidated Design Review: MBO Crossing

Sources: OPUS45 (Opus 4.5), OPUS46 (Opus 4.6), MAX (Opus 4.6 MAX), SONNET45 (Sonnet 4.5), CODEX53 (Codex 5.3)

## Tier 1: Immediate Wins (trivial risk, do first)

### 1. `prices_would_cross()` Free Function
- [ ] Implement

**Source:** All five reviews independently propose this.

Four call sites with the same boolean expression, easy to flip a comparison and create a subtle bug. Extract once:

```cpp
inline bool prices_would_cross(bool aggressor_is_ask, Price aggressor, Price passive) {
    return aggressor_is_ask ? (aggressor <= passive) : (aggressor >= passive);
}
```

| Location | Before | After |
|----------|--------|-------|
| `new_order` | `is_ask ? (price <= passive_best) : (price >= passive_best)` | `prices_would_cross(is_ask, price, passive_best)` |
| `modify_order` | `info.is_ask ? (new_price <= passive_best) : (new_price >= passive_best)` | `prices_would_cross(info.is_ask, new_price, passive_best)` |
| `cancel_order` | inline 4-line block at lines 1050-1052 | `prices_would_cross(aggressor_is_ask, aggressor_price, info.price)` |
| `PriceLevels::cross()` | `is_ask_ ? (best <= aggressor_price) : ...` | `prices_would_cross(!is_ask_, aggressor_price, best)` |

**Recommendation:** Free function. Trivial, immediate, zero risk.

---

### 2. `finalize_record()` Extraction
- [ ] Implement

**Source:** All five reviews.

The 8-line block (set affected_lvl, count filled levels) is copy-pasted 3 times (lines 1282-1290, 1387-1395, 1439-1449):

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

The three sites differ only in whether `affected_lvl` is reset to 20 afterward (yes for secondary TickInfo, no for CrossingComplete-synthesized ticks). That's a 1-line difference after the call.

**Detail (SONNET45):** Can add a `reset_affected` parameter if preferred, but a 1-line `affected[0] = affected[1] = 20;` after the call is clearer — keeps the helper focused.

**Recommendation:** Extract. Trivial dedup, zero risk.

---

### 3. `residual_tick_type` Elimination
- [ ] Implement

**Source:** All five reviews.

`residual_tick_type` stores `'N'` or `'M'` but is perfectly derivable from `original_resting_price`:

```cpp
bool is_from_modify() const { return original_resting_price != 0; }
```

Price=0 is already a sentinel throughout the codebase. One field eliminated from `PendingCross` (8 -> 7).

**Recommendation:** Do it. Trivial, zero risk.

---

## Tier 2: High-Impact Structural Refactors (low-medium risk)

### 4. `cancel_order()` Decomposition
- [ ] Implement

**Source:** All five reviews propose essentially the same classify-then-dispatch pattern.

The 120-line method has three genuinely different algorithms sharing a lookup and an erase. Extract:

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

Classification predicate (MAX):
```cpp
bool MBO::is_passive_self_trade_cancel(const OrderInfo& info) const {
    if (!pending_cross_.is_active()) return false;
    if (info.is_ask == pending_cross_.aggressor_is_ask) return false;
    return prices_would_cross(pending_cross_.aggressor_is_ask,
                              pending_cross_.aggressor_price, info.price);
}
```

**Variation (OPUS46):** Uses an enum `CancelType { Regular, AggressorSelfTrade, PassiveSelfTrade }` + switch. Slightly more formal but equivalent.

**Variation (CODEX53):** Adds a fourth category `PassiveNotConsumed` for the `consumed_from_order == 0` case (line 1063). This makes the edge case explicit in the classification rather than buried as an early return inside `cancel_passive_self_trade()`. Worth considering — it avoids the misleading dispatch-then-fallback pattern.

**Variation (OPUS46):** Also proposes extracting shared C/S framing into `emit_self_trade_cancel()`. MAX argues against: the three paths are genuinely different algorithms, don't force shared abstractions. I agree with MAX here -- the paths diverge enough that shared framing would be fragile.

**Key edge case to preserve:** The `consumed_from_order == 0` guard (line 1063) that falls through to regular cancel must remain — either as an explicit classification category (CODEX53) or as an early return in `cancel_passive_self_trade()` (MAX).

**Recommendation:** Extract three methods + classification predicate. Pure reorganization, no logic changes. Low risk, biggest readability win.

---

### 5. SpeculationLog Extraction
- [ ] Implement

**Source:** MAX (standout idea). OPUS45/OPUS46 propose lighter variants (iterator-only). CODEX53 proposes a fundamentally different internal strategy (unconfirmed-only queue).

`cross_fills_`, `pending_cross_fill_qty_`, and `pending_cross_fill_count_` form a cohesive sub-object -- a FIFO speculation log with a confirmation cursor. Seven methods on PriceLevels collapse:

```cpp
class SpeculationLog {
    std::vector<CrossFill> fills_;
    Qty pending_qty_ = 0;
    Count pending_count_ = 0;

public:
    Qty pending_qty() const { return pending_qty_; }
    bool empty() const { return pending_qty_ == 0; }

    void record(Price price, Qty qty, Count count) {
        fills_.push_back({price, qty, count});
        pending_qty_ += qty;
        pending_count_ += count;
    }

    void confirm_qty(Qty qty) { pending_qty_ -= std::min(qty, pending_qty_); }
    void confirm_count(Count c) { pending_count_ -= c; }
    void unreserve(Qty qty) { confirm_qty(qty); if (pending_count_ > 0) pending_count_--; }

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

    std::pair<Price, Qty> pending_vwap() const { /* uses for_each_unconfirmed */ }
    void clear() { fills_.clear(); pending_qty_ = 0; pending_count_ = 0; }
};
```

PriceLevels gets `SpeculationLog speculation_` and `uncross()` becomes:

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

The "skip confirmed from front" logic (duplicated in `pending_cross_vwap()` and `uncross()`) now lives in exactly one place.

**Key alternative — Unconfirmed-only queue (CODEX53):** Instead of keeping all fills and skipping confirmed entries from the front, only keep unconfirmed fills. On trade reconciliation, consume from the front and pop fully confirmed entries:

```cpp
// invariant: fills_ contains only unconfirmed speculative consumption
void confirm_qty(Qty q) {
    while (q > 0 && !fills_.empty()) {
        Qty d = std::min(q, fills_.front().qty);
        fills_.front().qty -= d; q -= d; pending_qty_ -= d;
        if (fills_.front().qty == 0) fills_.pop_front();
    }
}
```

This eliminates the skip math entirely — `pending_cross_vwap()` becomes a direct fold over the container, `uncross()` restores everything that remains. Requires `std::deque<CrossFill>` (or a small ring buffer) for O(1) `pop_front()`. With at most ~4 levels consumed per crossing, a `boost::container::small_vector<CrossFill, 4>` with manual front erasure is also fine.

**Tradeoff:** The queue approach is simpler internally but mutates fill entries on partial confirmation (the `fills_.front().qty -= d` line). The skip approach preserves original fill records, which may be useful for debugging. Either can live inside the SpeculationLog abstraction.

**Variation (OPUS46):** Track `confirmed_qty_` directly instead of deriving skip amount. Avoids the sum-then-subtract calculation. A good complement if using the skip approach.

**Caution:** The `for_each_unconfirmed` callback handles the first partial entry with `qty - skip_qty` and `count - skip_count`. Need to verify this matches the existing skip logic in `uncross()` exactly, especially when a single CrossFill spans the confirmed/unconfirmed boundary.

**Recommendation:** Full SpeculationLog extraction. This is the single highest-impact refactor for making the publisher "feel minimal." The skip-logic dedup alone justifies it. Consider CODEX53's queue semantics as the internal implementation — it's the cleanest option if debugging traceability of original fills isn't needed.

---

### 6. Ordered Output from `apply_deltas_to_book`
- [ ] Implement

**Source:** MAX (ordered output buffer), OPUS45/CODEX53 (DeltaExpander/RecordBatchBuilder class).

The `multi_tick_secondary` heuristic in `process_deltas()` (lines 1641-1643) peeks at `extra_records[0].event.tick_type` to determine delivery order. This is fragile.

**MAX proposal:** Change `apply_deltas_to_book` to return records in delivery order via a flat output buffer:

```cpp
int apply_deltas_to_book(OutputRecord& rec, std::span<const DeltaChunk> chunks,
                         PendingAggressorState& agg_state,
                         boost::container::static_vector<OutputRecord, 4>& output);
```

`process_deltas()` just iterates `output` in sequence. No ordering heuristic.

**Recommendation:** Go with MAX's approach (output buffer, records in order). Eliminates `multi_tick_secondary` entirely.

---

## Tier 3: Boundary Redesign (medium risk, higher payoff, do together)

These proposals form a dependency chain: enriched CrossingComplete enables publisher simplification which enables receiver simplification.

### 7. CrossingComplete: 1 Byte -> 2 Bytes with Flags
- [ ] Implement

**Source:** MAX (2-byte flags), OPUS46 (8-14 bytes), OPUS45 (20-40 bytes), SONNET45 (6 bytes), CODEX53 (data-carrying `CrossResolvedDelta`).

The publisher already knows `has_residual` and `from_modify` at the moment it emits CrossingComplete. Encode them:

```cpp
struct CrossingCompleteDelta {
    uint8_t type;    // = 3
    uint8_t flags;   // bit 0: has_residual, bit 1: from_modify
};  // 2 bytes
```

This eliminates the receiver's need to:
- Compute `need_residual` from `aggressor_remaining > 0`
- Compute `need_cancel` from `!need_residual && original_tick_type == 'B'`
- Maintain the `crossing_complete` flag for self-trade timing

**Competing proposals:**
- OPUS46 proposes ~14 bytes (adds `residual_tick_type`, `aggressor_remaining`)
- OPUS45 proposes ~20-40 bytes (adds full aggressor info: price, remaining, id, original resting info)
- SONNET45 proposes 6 bytes (`{type, flags, residual_qty}`) — pragmatic middle ground
- CODEX53 proposes `CrossResolvedDelta` with `final_tick` field ('N'/'M'/'X'/0) — publisher tells receiver exactly what to synthesize. Most direct but couples publisher to reference format.

Both larger proposals aim to make the receiver fully stateless for completion handling. But the receiver still needs `aggressor_id` during crossing for trade reconciliation, so `PendingAggressorState` can't be fully eliminated anyway. The 2-byte version is the right tradeoff: minimal data, maximum signal.

**Recommendation:** 2-byte CrossingComplete (MAX). Keeps the delta tiny, provides enough signal to simplify the receiver substantially.

---

### 8. Move X-Tick-for-Modify to Receiver + PendingCross Reduction
- [ ] Implement (deferred — revisit after Tier 1+2)

**Source:** All five reviews. MAX proposes a `ModifyOriginDelta`; OPUS46 proposes carrying the data in CrossingComplete.

Lines 1189-1199 in `trade()` emit an X tick for fully-consumed modifies. Three fields in PendingCross (`original_resting_price`, `original_affected_lvl`, `aggressor_original_qty`) exist solely for this. Options:

**Option A -- ModifyOriginDelta (MAX):** Emit a new 14-byte delta alongside the B TickInfo carrying the old resting info. Receiver captures it at B-tick time. PendingCross drops to 5 fields. Publisher's `trade()` completion becomes 6 lines.

**Option B -- Carry in CrossingComplete (OPUS46):** Grow CrossingComplete to ~14-20 bytes to include `original_price`, `original_qty`, `original_lvl`. Receiver gets everything it needs from one delta. PendingCross drops similarly.

**Option C -- Keep in publisher (OPUS45/SONNET45 conservative):** Keep the three fields, just comment them as `// reference-compat`. Derive `residual_tick_type`. 7 fields.

**Critical constraint (OPUS45):** `order_map_` is updated *before* crossing in `modify_order` (lines 895, 982), so original state is lost by the time cancel/completion occurs. This means `original_resting_price` and `aggressor_original_qty` genuinely cannot be derived from `order_map_` later. Any aggressive PendingCross reduction (Options A/B) must either keep these fields or change `modify_order` to update `order_map_` *after* crossing — a non-trivial ordering change that needs careful validation.

**Decision factors:**
- Modify-that-fully-consumes is very rare
- Adding a new delta type (ModifyOriginDelta) has real complexity cost
- Growing CrossingComplete to 14-20 bytes for a rare case is wasteful
- The current publisher code is ugly but small and isolated
- The `order_map_` update ordering constraint makes aggressive reduction harder than it appears

**Recommendation:** Option C for now (conservative). Keep the three fields, derive `residual_tick_type`, document as reference-compat. PendingCross: 7 fields. Revisit after the Tier 1+2 refactors are done and validated -- the picture may look different. The X-tick-for-modify logic in `trade()` is contained (lines 1189-1199) and doesn't infect the rest of the code.

---

### 9. PendingAggressorState Reduction
- [ ] Implement

**Source:** All five reviews. Degree of reduction depends on CrossingComplete enrichment.

With the 2-byte CrossingComplete (proposal 7):
- `crossing_complete` flag can be eliminated -- the `from_modify` flag in CC replaces part of its role, and with ordered output (proposal 6), the timing issue that motivated it goes away
- `original_tick_type` might be eliminable if CC carries `from_modify` -- but receiver still needs it for C-expansion tick synthesis (`A` vs `B` -> `N` vs `M`)

**Conservative reduction (without Option A/B from proposal 8):**
```cpp
struct PendingAggressorState {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Qty aggressor_original_qty = 0;
    Qty aggressor_remaining = 0;
    char original_tick_type = 0;      // 'A' or 'B'
    // crossing_complete eliminated (derived from CC flags)
};
```

7 fields -> 6. Further reduction to 4-5 depends on moving X-tick-for-modify to receiver (proposal 8, deferred).

**Detail (SONNET45):** S tick emission (lines 1035, 1094) currently puts `aggressor_id` in `order_id2`, but the receiver never reads it from S ticks (lines 1508, 1526). This is dead data — can simplify S tick to `order_id2 = 0` and document that C+S pairing is by proximity. Low priority but removes a misleading linkage.

**Recommendation:** Eliminate `crossing_complete` flag once ordered output (proposal 6) and enriched CC (proposal 7) are in place. The rest stays for now.

---

## Non-Issues (Don't Change)

**Source:** SONNET45 (unique section among the five reviews — good engineering judgment to explicitly call out what's correct).

**Publisher-receiver boundary:** Current split is correct. All five reviews agree: publisher emits semantic crossing events, receiver handles reference-compat expansion. Don't push C+S+N into MBO core.

**Zero-delta updates for affected_lvl:** Lines 1140-1142 emit zero-delta updates to set `affected_lvl=0` on reconciled trades. This cleverly reuses delta machinery for metadata signaling. Keep it.

**Price negation for bids:** The canonical price approach (negative keys in `levels_` map, lines 499-547) is clean and correct. No change needed.

**CrossingComplete existence:** It's necessary. The receiver cannot infer crossing completion — only the publisher knows when `pending_cross_fill_qty` reaches zero. Make it richer (carry flags), don't remove it.

---

## Dropped Ideas

| Idea | Source | Why Dropped |
|------|--------|-------------|
| `DeltaExpander` class wrapping apply+next | OPUS45, CODEX53 | Over-abstraction; ordered output buffer (MAX) achieves the same goal with less ceremony |
| 40-byte CrossingComplete with full aggressor info | OPUS45 | Overkill; receiver still needs PendingAggressorState for trade tracking, so can't go fully stateless |
| `would_cross()` as PriceLevels method | OPUS46 | Less flexible than free function; cancel_order's check doesn't go through PriceLevels |
| Merge `reconcile_cross_count` into `reconcile_cross_fill` | OPUS46 | Superseded by SpeculationLog extraction which restructures these methods entirely |
| `confirmed_qty_` direct tracking instead of skip derivation | OPUS46 | Good idea but internal detail of SpeculationLog; not a separate proposal. Consider during implementation. |
| S tick as formal `SidebandInfo` delta type | MAX | Documentation-only change; not worth a code modification at this stage |
| Publisher emits full C+S+N sequence | OPUS46 raised, all five rejected | Pollutes universal MBO core with reference-specific concerns |
| Shared `emit_self_trade_cancel()` for aggressor/passive cancel | OPUS46 | Paths diverge too much (uncross-all vs unreserve+re-cross); forced shared framing would be fragile |
| Derive `aggressor_is_ask`/`aggressor_price` from `order_map_` | CODEX53 | Adds hash lookup on every access. Fields are 9 bytes total — cache-friendly struct beats hash lookup on a hot path |
| `CrossResolvedDelta` with `final_tick` ('N'/'M'/'X') | CODEX53 | Couples publisher to reference output format. Publisher should emit facts (has_residual, from_modify), not instructions |
| Negative `confirm(-qty)` for unreserve operations | OPUS45, SONNET45 | Semantic anti-pattern — "unreserve" and "confirm" are opposite operations, shouldn't share a sign convention |
| Emit N/M with qty=0 on CrossingComplete (skip tracking) | SONNET45 | Risky if book doesn't reflect residual at that point; receiver must know actual qty |

---

## Recommended Implementation Order

Each step is independently testable against the reference:

- [ ] 1. **`prices_would_cross()`** — trivial, 10 min
- [ ] 2. **`finalize_record()`** — trivial dedup, 10 min
- [ ] 3. **`residual_tick_type` elimination** — trivial state reduction, 10 min
- [ ] 4. **`cancel_order()` decomposition** — pure extract, 30-60 min
- [ ] 5. **SpeculationLog extraction** — encapsulate crossing state, 60 min
- [ ] 6. **Ordered output from `apply_deltas_to_book`** — kill `multi_tick_secondary`, 60 min
- [ ] 7. **CrossingComplete 1B -> 2B** — small delta change, enables receiver simplification, 30 min
- [ ] 8. **PendingAggressorState: eliminate `crossing_complete`** — follows from 6+7, 20 min
- [ ] 9. **S tick `order_id2` cleanup** — trivial, 5 min (do alongside 8)

**Total estimated: ~4-5 hours of careful work, ~120 lines removed, 2+ state fields eliminated, 3 fragile patterns removed (skip-logic duplication, multi_tick_secondary, scattered would_cross). All independently validated at each step.**

Steps 1-5 are publisher-only. Steps 6-9 touch the receiver. The X-tick-for-modify question (proposal 8) is deliberately deferred — revisit after the dust settles.

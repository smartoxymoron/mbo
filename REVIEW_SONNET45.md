# MBO Crossing Implementation Review

## Executive Summary

The implementation is correct but over-engineered. Key opportunities: reduce PendingCross from 8→5 fields, extract shared cancel logic into helper methods, collapse crossing lifecycle to 3 methods, make publisher emit richer deltas to eliminate receiver guesswork. Target: 30% less crossing state, ~100 fewer lines.

---

## Part 1: Publisher Side

### 1. STATE REDUCTION: PendingCross (8 fields → 5 fields)

**Can eliminate (3 fields):**

```cpp
// Current: 8 fields
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Price original_resting_price = 0;     // REMOVE: derive from order_map_
    Qty aggressor_original_qty = 0;       // REMOVE: derive from order_map_
    char residual_tick_type = 'N';
    int8_t original_affected_lvl = 20;    // REMOVE: recompute on-demand
    bool aggressor_on_level = false;
};

// Proposed: 5 fields + on-demand lookups
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;           // New price (post-modify)
    char residual_tick_type = 'N';
    bool aggressor_on_level = false;
    
    // Derive these when needed:
    // - Original price/qty: order_map_.find(aggressor_id) gives pre-modify state
    //   BUT order_map_ is updated *before* crossing, so can't retrieve.
    //   ALTERNATIVE: Pass old price/qty as arguments to cancel_order when needed.
    // - original_affected_lvl: recompute own_side.get_level_index(old_price)
    //   BUT level may be gone. Keep this field OR pass as arg to emit_update.
};
```

**Reality check:** You update `order_map_[id]` before crossing (lines 895, 982), so original state is lost. Two options:

**Option A (Conservative):** Keep `original_resting_price` + `original_affected_lvl` for modifies (used in lines 1192, 1199). Remove only `aggressor_original_qty` (can use qty from order_map at cancel time since cancels don't update order_map first). **Savings: 1 field.**

**Option B (Aggressive):** Change modify_order to update order_map *after* crossing. Then PendingCross needs only:
```cpp
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;        // New price for crossing check
    char residual_tick_type = 'N';
    bool aggressor_on_level = false;
};
```
When cancel arrives, lookup order_map for original state before erasing. **Savings: 3 fields, cleaner lifecycle.**

**Recommendation:** Option B. Update order_map *after* speculative operations, not before. Exchange view should lag MBO's strategy view, not lead it.

---

### 2. CANCEL_ORDER DECOMPOSITION (120 lines → ~80 lines)

**Problem:** Three nested branches (aggressor cancel 1005-1040, passive cancel 1043-1102, regular cancel 1104-1107) repeat similar patterns.

**Extract common operations:**

```cpp
// Helper: emit C+S pair for any self-trade cancel
void emit_self_trade_cancel(bool is_ask, Price vwap, Qty cross_qty, 
                            OrderId cancelled_id, OrderId aggressor_id,
                            Price cancel_price, Qty cancel_qty) {
    emitter_.emit_tick_info('C', is_ask, true, vwap, cross_qty, cancelled_id, aggressor_id);
    // ... passive restore/re-cross logic ...
    emitter_.emit_tick_info('S', is_ask, false, cancel_price, cancel_qty, cancelled_id, aggressor_id);
}

// Simplified cancel_order:
void MBO::cancel_order(OrderId id) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) {
        emitter_.emit_tick_info('X', false, true, 0, 0, id);
        return;
    }
    
    OrderInfo& info = it->second;
    PriceLevels& own_side = info.is_ask ? asks_ : bids_;
    
    if (!pending_cross_.is_active()) {
        // Fast path: no crossing
        emitter_.emit_tick_info('X', info.is_ask, false, info.price, info.qty, id);
        own_side.remove_liquidity(info.price, info.qty, 1);
        order_map_.erase(it);
        return;
    }
    
    // Crossing active: dispatch to specialized handlers
    if (id == pending_cross_.aggressor_id) {
        handle_aggressor_cancel(info);
    } else if (is_passive_crossing_order(info)) {
        handle_passive_cancel(info);
    } else {
        // Regular cancel during unrelated crossing
        emitter_.emit_tick_info('X', info.is_ask, false, info.price, info.qty, id);
        own_side.remove_liquidity(info.price, info.qty, 1);
    }
    order_map_.erase(it);
}
```

**Benefit:** Main method becomes 30 lines (structure visible), complexity isolated in handlers. The 40-line handlers (aggressor/passive) still have distinct logic but share the "emit C, restore, emit S" skeleton.

**Further collapse:** The C+S emission is identical (lines 1019-1021, 1070-1072). Extract to `emit_cross_summary(passive_side, id)`.

**Estimated reduction:** 120 lines → 80 lines (30 main + 2×25 handlers).

---

### 3. CROSSING LIFECYCLE (7 methods → 3 methods)

**Current crossing methods on PriceLevels:**
1. `cross()` - consume liquidity
2. `uncross()` - restore all pending
3. `unreserve_cross_fill()` - reduce pending by qty
4. `reconcile_cross_fill()` - confirm fill
5. `reconcile_cross_count()` - confirm count
6. `clear_cross_fills()` - reset state
7. `pending_cross_vwap()` - compute VWAP

**Observation:** The "skip confirmed from front" pattern (lines 575-587, 736-755) is duplicated. `reconcile_cross_fill` and `unreserve_cross_fill` manipulate `pending_cross_fill_qty_` but don't touch `cross_fills_`. Then `uncross`/`pending_cross_vwap` walk the vector skipping entries.

**Unified state model:**

```cpp
class PriceLevels {
    struct CrossState {
        std::vector<CrossFill> fills;      // All fills (confirmed + pending)
        Qty pending_qty = 0;               // Unconfirmed only
        Count pending_count = 0;
        
        // Iterator to first unconfirmed entry (confirmed fills are [begin, confirmed_end))
        auto unconfirmed_begin() const {
            Qty skip = total_qty() - pending_qty;
            Qty acc = 0;
            for (auto it = fills.begin(); it != fills.end(); ++it) {
                if (acc >= skip) return it;
                acc += it->qty;
            }
            return fills.end();
        }
        
        Qty total_qty() const {
            Qty total = 0;
            for (auto& f : fills) total += f.qty;
            return total;
        }
        
        Price vwap() const {
            int64_t vol = 0;
            Qty qty = 0;
            for (auto it = unconfirmed_begin(); it != fills.end(); ++it) {
                Qty use = std::min(it->qty, pending_qty - qty);
                vol += it->price * use;
                qty += use;
                if (qty >= pending_qty) break;
            }
            return qty > 0 ? (Price)(vol / qty) : 0;
        }
    };
    CrossState cross_state_;
    
    // Replace 7 methods with 3:
    Qty cross(Price aggressor_price, Qty aggressor_qty);      // Same logic
    void confirm(Qty fill_qty, Count count = 0);             // Decrement pending_qty/count
    void rollback();                                          // Restore unconfirmed, clear state
};
```

**Consolidation:**
- `reconcile_cross_fill` + `reconcile_cross_count` → `confirm(qty, count)`
- `unreserve_cross_fill` → `confirm(-qty, -count)` (negative = unreserve)
- `uncross` → `rollback()`
- `clear_cross_fills` → `cross_state_.clear()`
- `pending_cross_vwap` → `cross_state_.vwap()`

**Caller changes:**
- trade: `passive.confirm(fill_qty, fully_consumed ? 1 : 0)`
- passive cancel: `passive.confirm(-consumed_qty, -1)` then `passive.cross(...)`
- aggressor cancel: `passive.rollback()`

**Benefit:** Single mental model (confirmed vs pending split), no duplicated skip logic. ~40 lines saved.

---

### 4. CROSSING COMPLETION IN TRADE() (lines 1177-1207)

**Problem:** 30 lines mixing MBO semantics (tracking residual) with reference-compat (emit X for consumed modifies). The X emission (lines 1189-1199) feels bolted-on.

**Why X is needed:** Reference impl expects modify→cancel sequence to emit X at *original* price. When modify crosses and fully consumes, no subsequent cancel arrives, so trade() emits synthetic X.

**Cleaner split:**

```cpp
void MBO::trade(...) {
    // ... normal trade logic ...
    
    if (pending_cross_.is_active() && passive.pending_cross_fill_qty() == 0) {
        finalize_crossing();  // Extract to helper
    }
}

void MBO::finalize_crossing() {
    PriceLevels& passive = pending_cross_.aggressor_is_ask ? bids_ : asks_;
    passive.cross_state_.clear();
    
    auto agg_it = order_map_.find(pending_cross_.aggressor_id);
    bool has_residual = (agg_it != order_map_.end() && agg_it->second.qty > 0);
    
    if (!has_residual && pending_cross_.residual_tick_type == 'M') {
        emit_consumed_modify_as_cancel();  // Reference-compat quirk isolated
    } else {
        emitter_.emit_crossing_complete();
    }
    pending_cross_.clear();
}

void MBO::emit_consumed_modify_as_cancel() {
    // Lines 1192-1199 moved here with comment explaining reference quirk
    emitter_.emit_tick_info('X', pending_cross_.aggressor_is_ask, false, ...);
    emitter_.emit_update(pending_cross_.aggressor_is_ask, 
                        pending_cross_.original_affected_lvl, 0, 0);
}
```

**Benefit:** trade() body stays clean (MBO semantics), reference quirks confined to named helper. If you later decide publisher shouldn't care about this, delete `emit_consumed_modify_as_cancel()` and move logic to receiver.

---

### 5. PRICE COMPARISON DUPLICATION

**Three instances of would_cross check:**

```cpp
// new_order line 869:
bool would_cross = g_crossing_enabled && (passive_best != 0) &&
    (is_ask ? (price <= passive_best) : (price >= passive_best));

// modify_order line 949:
bool would_cross = (passive_best != 0) && 
    (info.is_ask ? (new_price <= passive_best) : (new_price >= passive_best));

// cancel_order line 1050:
bool would_cross = pending_cross_.aggressor_is_ask 
    ? (pending_cross_.aggressor_price <= info.price)
    : (pending_cross_.aggressor_price >= info.price);
```

**Extract helper:**

```cpp
inline bool would_cross(Price aggressor_price, bool aggressor_is_ask, 
                       Price passive_best) {
    return passive_best != 0 &&
           (aggressor_is_ask ? (aggressor_price <= passive_best) 
                             : (aggressor_price >= passive_best));
}

// Usage:
bool will_cross = g_crossing_enabled && 
                  would_cross(price, is_ask, passive.best_price());
```

**Trivial but removes visual clutter. Compiler will inline.**

---

## Part 2: Receiver Side

### 6. MULTI-OUTPUT EXPANSION (finalize+push+reset pattern)

**Problem:** Lines 1280-1295 (secondary TickInfo), 1386-1400 (CrossingComplete), 1479 + 1512 + 1536 (C expansion) all do:

```cpp
// Finalize affected_lvl and filled_lvls
rec.bid_affected_lvl = affected_lvl[0];
rec.ask_affected_lvl = affected_lvl[1];
rec.bid_filled_lvls = 0;
rec.ask_filled_lvls = 0;
for (int j = 0; j < 20; ++j) {
    if (rec.bids[j].price != 0) rec.bid_filled_lvls++;
    if (rec.asks[j].price != 0) rec.ask_filled_lvls++;
}
extra_records->push_back(rec);
affected_lvl[0] = 20; affected_lvl[1] = 20;  // Sometimes
```

**Extract helper:**

```cpp
void finalize_and_push(OutputRecord& rec, uint8_t* affected_lvl,
                      std::vector<OutputRecord>& extras, bool reset_affected) {
    rec.bid_affected_lvl = affected_lvl[0];
    rec.ask_affected_lvl = affected_lvl[1];
    rec.bid_filled_lvls = 0;
    rec.ask_filled_lvls = 0;
    for (int j = 0; j < 20; ++j) {
        if (rec.bids[j].price != 0) rec.bid_filled_lvls++;
        if (rec.asks[j].price != 0) rec.ask_filled_lvls++;
    }
    extras.push_back(rec);
    if (reset_affected) {
        affected_lvl[0] = affected_lvl[1] = 20;
    }
}

// Call sites:
finalize_and_push(rec, affected_lvl, *extra_records, /*reset=*/true);  // Line 1290
finalize_and_push(rec, affected_lvl, *extra_records, /*reset=*/false); // Line 1396 (CrossingComplete N/M inherits affected_lvl)
```

**Savings:** ~45 lines of duplication → single 10-line helper.

---

### 7. PENDINGAGGRESSORSTATE (7 fields → 5 fields + publisher help)

**Current:**

```cpp
struct PendingAggressorState {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Qty aggressor_original_qty = 0;      // For X tick on fully consumed modify
    Qty aggressor_remaining = 0;         // Updated on each trade
    char original_tick_type = 0;         // 'A' or 'B'
    bool crossing_complete = false;      // Seen CrossingComplete during self-trade
};
```

**Problem:** Receiver must track `aggressor_remaining` by intercepting every trade (line 1317). This is fragile (miss a trade = wrong qty) and duplicates work the publisher already does.

**Option 1 (no protocol change):** Eliminate `aggressor_remaining`. When CrossingComplete arrives, emit N/M with qty=0 (assume fully consumed) or let receiver lookup residual from book state. **Risky if book doesn't reflect residual.**

**Option 2 (publisher emits residual in CrossingComplete):**

```cpp
struct CrossingCompleteDelta {
    uint8_t type;              // = 3
    int32_t aggressor_residual_qty;  // 0 if fully consumed, >0 if residual remains
    // Optional: int8_t affected_lvl if residual added
};
```

Publisher knows exact residual (line 1187), no need for receiver to track trades. **Cost: 4 bytes per CrossingComplete (rare event).** Receiver PendingAggressorState drops `aggressor_remaining`.

**Option 3 (aggressive - make CrossingComplete richer):**

```cpp
struct CrossingCompleteDelta {
    uint8_t type = 3;
    char synthesized_tick_type;  // 'N', 'M', or 'X' (or 0 if none)
    int32_t qty;                 // Residual qty for N/M, original qty for X
    int64_t price;               // Aggressor price for N/M, original price for X
    int8_t affected_lvl;         // For X: original resting level
};
```

Now receiver doesn't synthesize anything—publisher tells it exactly what tick to emit. **Cost: 15 bytes per CrossingComplete.** But eliminates PendingAggressorState entirely (all info in delta).

**Recommendation:** Option 2 (emit residual qty in CrossingComplete). Saves receiver complexity, adds 4 bytes to rare delta. PendingAggressorState → 6 fields.

---

### 8. CROSSINGCOMPLETE DELTA DESIGN

**Current:** 1-byte signal, receiver synthesizes N/M/X.

**Is implicit completion possible?** No. Receiver can't know when `pending_cross_fill_qty` reaches zero (it doesn't track that—only publisher does). Without explicit signal, receiver wouldn't know when to emit residual tick.

**Should it carry more data?** Yes, per #7. At minimum, carry `aggressor_residual_qty` so receiver doesn't have to track trades. Ideally carry full tick info (type, price, qty, affected_lvl) to eliminate synthesis logic (lines 1401-1419).

**Proposed:**

```cpp
struct CrossingCompleteDelta {
    uint8_t type = 3;
    uint8_t flags;           // bit 0: has_residual, bit 1: is_modify_consumed
    int32_t residual_qty;    // 0 if fully consumed
    // Price/affected_lvl: receiver can derive from tracked agg_state
};
```

**Cost: 6 bytes.** Receiver logic (lines 1380-1422) simplifies:

```cpp
if (dtype == DeltaType::CrossingComplete) {
    const CrossingCompleteDelta* delta = ...;
    if (extra_records && agg_state.is_active() && !is_self_trade) {
        finalize_and_push(rec, affected_lvl, *extra_records, false);
        
        if (delta->residual_qty > 0) {
            rec.event.tick_type = (agg_state.original_tick_type == 'A') ? 'N' : 'M';
            rec.event.qty = delta->residual_qty;  // From delta, not tracked
            // ... other fields from agg_state ...
        } else if (delta->flags & 0x02) {  // is_modify_consumed
            rec.event.tick_type = 'X';
            // ... X logic ...
        }
    }
    agg_state.clear();
    offset += sizeof(CrossingCompleteDelta);
}
```

**Benefit:** Receiver doesn't track trade fills, publisher emits authoritative residual.

---

### 9. PUBLISHER-RECEIVER BOUNDARY

**Question:** Should publisher emit full C+S+N sequence, not just C+S deltas for receiver to expand?

**Current boundary:**
- Publisher emits: `TickInfo('C') + deltas + TickInfo('S') + deltas`
- Receiver expands to: `OutputRecord('C') + OutputRecord('S') + OutputRecord('N')`

**Why this split?** Publisher is universal (exchange-agnostic), C+S+N is reference-compat quirk. Keeps core MBO clean.

**Proposed boundary (no change):**
- Publisher emits minimal crossing deltas (C, S, CrossingComplete)
- Receiver/compatibility layer expands to match reference format

**Justification:** The receiver *already* contains reference-specific logic (e.g., `multi_tick_secondary` ordering, affected_lvl adjustments). C expansion (lines 1451-1547) belongs there, not in MBO core.

**Alternative:** Make MBO emit all three TickInfos directly. But this pollutes universal MBO with reference quirks. If you switch to a different reference (or run strategy without reference validation), you'd want minimal ticks.

**Recommendation:** Keep current boundary. Make receiver smarter (consolidate expansion logic) but don't push reference-compat into MBO.

**One improvement:** The S tick emission (lines 1035, 1094) uses `order_id2 = pending_cross_.aggressor_id` but receiver expects S to be standalone. This `order_id2` link is unused (lines 1508, 1526). **Simplify:** S tick should have `order_id2 = 0`, document that C+S pair is inferred by proximity, not by id linkage.

---

## Concrete Refactoring Roadmap

**Phase 1 (low-risk, immediate wins):**
1. Extract `would_cross()` helper (#5) — 5 min
2. Extract `finalize_and_push()` in receiver (#6) — 10 min
3. Extract `emit_cross_summary()` in cancel_order (#2 partial) — 15 min
4. Extract `finalize_crossing()` from trade() (#4) — 15 min

**Phase 2 (state reduction):**
5. Change modify_order to update order_map *after* crossing (#1) — 30 min
6. Reduce PendingCross to 5 fields (#1) — 10 min
7. Add `residual_qty` to CrossingCompleteDelta (#7, #8) — 30 min
8. Remove `aggressor_remaining` tracking in receiver (#7) — 15 min

**Phase 3 (deep refactor):**
9. Consolidate PriceLevels crossing methods to CrossState (#3) — 1 hour
10. Decompose cancel_order into handle_aggressor/passive_cancel (#2) — 45 min

**Impact:**
- State: PendingCross 8→5 fields, PendingAggressorState 7→6 fields
- Lines: ~100 fewer (120→80 cancel, ~40 from crossing lifecycle, ~30 from finalize duplication)
- Clarity: Crossing feels like natural extension, not bolted feature

---

## Non-Issues (Don't Change)

**Publisher-receiver boundary (#9):** Current split is correct. Don't push C+S+N expansion into MBO.

**Zero-delta updates for affected_lvl:** Lines 1140-1142 emit zero-delta updates to set `affected_lvl=0` on reconciled trades. This is clever (reuses delta machinery for metadata signaling). Keep it.

**Price negation for bids:** The canonical price approach (lines 499-547) is clean. No change needed.

**CrossingComplete existence:** It's necessary. Receiver can't infer completion. Make it richer (carry residual), don't remove it.

---

## Final Metrics

**Current:**
- PendingCross: 8 fields
- PendingAggressorState: 7 fields
- cancel_order: 120 lines
- PriceLevels crossing: 7 methods, ~150 lines
- Receiver expansion: ~100 lines with 3× duplication
- CrossingComplete: 1 byte

**After refactor:**
- PendingCross: 5 fields (−37%)
- PendingAggressorState: 6 fields (−14%)
- cancel_order: 80 lines (−33%)
- PriceLevels crossing: 3 methods, ~110 lines (−27%)
- Receiver expansion: ~70 lines (−30%)
- CrossingComplete: 6 bytes (+5 bytes, but much clearer contract)

**Code should feel:** Crossing is a natural mode (tracked state, confirm/rollback), not a parallel universe. Each structure earns its place.

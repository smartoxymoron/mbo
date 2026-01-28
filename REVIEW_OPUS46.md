# MBO Crossing Review — Opus 4.6

## Part 1: Publisher Side

### 1. PendingCross State Reduction (8 → 5 fields)

Three fields exist solely for the modify-fully-consumed X tick in `trade()`:
- `original_resting_price` — old price for the X tick
- `original_affected_lvl` — old level for the zero-delta update
- `aggressor_original_qty` — old qty for the X tick

**Proposal:** Move the X-tick emission for fully-consumed modifies to the receiver. Publisher always emits `CrossingComplete`; the receiver already has `aggressor_price` and `aggressor_original_qty` from the A/B tick and can synthesize X when `remaining == 0 && original_tick_type == 'B'`. This eliminates lines 1189–1199 from `trade()` and all three fields from `PendingCross`.

Remaining 5 fields all earn their place:
- `aggressor_id` — identity + active flag
- `aggressor_is_ask` — side for uncross/re-cross
- `aggressor_price` — re-cross price after self-trade
- `residual_tick_type` — 'N' vs 'M' (still needed for publisher to know whether to emit CrossingComplete vs let the receiver handle it... but see below)
- `aggressor_on_level` — whether residual was added (controls count_delta on re-cross)

**Further reduction (5 → 4):** If CrossingComplete is always emitted (proposal above), `residual_tick_type` becomes receiver-only state. Publisher doesn't need it.

```cpp
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    bool aggressor_on_level = false;
    
    bool is_active() const { return aggressor_id != 0; }
    void clear() { aggressor_id = 0; aggressor_on_level = false; }
};
```

### 2. cancel_order() Decomposition

The three branches have distinct semantics but share framing. The key insight: the regular cancel branch is clean already. The complexity is the two self-trade branches. Extract the shared C/S/cleanup framing:

```cpp
// In cancel_order, after detecting self-trade type:
void MBO::emit_self_trade_cancel(OrderId cancelled_id, const OrderInfo& info,
                                  bool is_aggressor_cancel) {
    PriceLevels& passive_side = pending_cross_.aggressor_is_ask ? bids_ : asks_;
    
    // C tick: VWAP of pending speculative fills
    auto [cross_vwap, cross_qty] = passive_side.pending_cross_vwap();
    emitter_.emit_tick_info('C', info.is_ask, true, cross_vwap, cross_qty,
                            cancelled_id, is_aggressor_cancel ? 0 : pending_cross_.aggressor_id);
    
    // --- Branch-specific liquidity adjustment (the only real difference) ---
    if (is_aggressor_cancel) {
        adjust_for_aggressor_cancel(info, passive_side);
    } else {
        adjust_for_passive_cancel(info, passive_side);
    }
    
    // S tick: actual cancelled order info
    emitter_.emit_tick_info('S', info.is_ask, false, info.price, info.qty,
                            cancelled_id, is_aggressor_cancel ? 0 : pending_cross_.aggressor_id);
    
    // Completion check
    if (is_aggressor_cancel || passive_side.pending_cross_fill_qty() == 0) {
        emitter_.emit_crossing_complete();
        passive_side.clear_cross_fills();
        pending_cross_.clear();
    }
}
```

This reduces `cancel_order()` to: classify → dispatch. The 120 lines become ~30 for classification + ~30 for the helper + two ~10-line adjustment methods.

The `consumed_from_order == 0` guard (line 1063) that falls through to regular cancel is the one subtle case — keep it as an early return before calling the helper.

### 3. Crossing Lifecycle: cross_fills_ Simplification

**Problem:** "Skip confirmed from front" logic duplicated in `pending_cross_vwap()` and `uncross()`.

**Proposal A — Track confirmed boundary explicitly:**

```cpp
// In PriceLevels:
Qty confirmed_fill_qty_ = 0;  // replaces the skip calculation

Qty reconcile_cross_fill(Qty fill_qty) {
    Qty reconciled = std::min(fill_qty, pending_cross_fill_qty_);
    pending_cross_fill_qty_ -= reconciled;
    confirmed_fill_qty_ += reconciled;  // advance the boundary
    return reconciled;
}
```

Then both `pending_cross_vwap()` and `uncross()` iterate starting from `confirmed_fill_qty_` without recomputing the skip. The "skip from front" loop becomes a simple qty offset.

**Proposal B — Merge reconcile_cross_count into reconcile_cross_fill:**

`reconcile_cross_count(1)` is called exactly once (trade.cpp line 1169), always with delta=1, always when `info.qty == 0`. Make `reconcile_cross_fill` accept an optional count parameter:

```cpp
Qty reconcile_cross_fill(Qty fill_qty, Count count_delta = 0) {
    Qty reconciled = std::min(fill_qty, pending_cross_fill_qty_);
    pending_cross_fill_qty_ -= reconciled;
    pending_cross_fill_count_ -= count_delta;
    return reconciled;
}
```

This eliminates one of the 7 methods. Similarly, `clear_cross_fills()` (2 lines) could be folded into the completion path rather than being a separate method.

**Net method count:** 7 → 4 (`cross`, `uncross`, `reconcile_cross_fill`, `pending_cross_vwap`). The remaining state clearing is inline at call sites since it's always paired with `pending_cross_.clear()`.

### 4. Crossing Completion in trade()

Lines 1177–1207 mix MBO semantics with reference-compat. With proposal #1 (always emit CrossingComplete, no X tick from publisher), this entire block collapses to:

```cpp
if (pending_cross_.is_active()) {
    PriceLevels& cross_passive = pending_cross_.aggressor_is_ask ? bids_ : asks_;
    if (cross_passive.pending_cross_fill_qty() == 0) {
        cross_passive.clear_cross_fills();
        emitter_.emit_crossing_complete();
        pending_cross_.clear();
    }
}
```

Six lines, no branching. The reference-compat X/N/M synthesis moves to the receiver where it belongs.

### 5. Price Comparison: would_cross

Three call sites with the same pattern. Add to `PriceLevels`:

```cpp
bool would_cross(Price aggressor_price) const {
    Price best = best_price();
    return best != 0 && (is_ask_ ? (aggressor_price >= best) : (aggressor_price <= best));
}
```

Note the inverted perspective: this is called on the *passive* side, asking "would an aggressor at this price cross my best?" The ask side is passive when a bid aggresses (bid price >= ask best), hence the inversion from the current code.

Call sites become:
```cpp
bool would_cross = g_crossing_enabled && passive.would_cross(price);
```

One clean predicate instead of three inline boolean expressions with easy-to-flip comparisons.

---

## Part 2: Receiver Side

### 6. Multi-Output Expansion

The "finalize, push, reset" pattern appears 3 times (lines 1281–1295, 1386–1396, and the final 1439–1449). Extract:

```cpp
void finalize_record(OutputRecord& rec, uint8_t (&affected_lvl)[2],
                     std::vector<OutputRecord>* extra_records) {
    rec.bid_affected_lvl = affected_lvl[0];
    rec.ask_affected_lvl = affected_lvl[1];
    rec.bid_filled_lvls = 0;
    rec.ask_filled_lvls = 0;
    for (int j = 0; j < 20; ++j) {
        if (rec.bids[j].price != 0) rec.bid_filled_lvls++;
        if (rec.asks[j].price != 0) rec.ask_filled_lvls++;
    }
    if (extra_records) extra_records->push_back(rec);
}
```

The third instance (lines 1439–1449) doesn't push to extras — it finalizes the "current" record. Distinguish by passing `nullptr` for extra_records in that case (it already writes directly into `rec`). Actually simpler: just always call `finalize_record` and let the caller decide what to do with `rec`.

This is a straightforward DRY cleanup — low risk, obvious win.

### 7. PendingAggressorState Reduction

Currently 7 fields. With richer `CrossingComplete` (proposal #8 below), several become unnecessary:

| Field | Currently needed for | With enriched CC |
|-------|---------------------|------------------|
| `aggressor_id` | N/M/X synthesis, C expansion | Still needed (identity) |
| `aggressor_is_ask` | Side for synthesized ticks | Still needed |
| `aggressor_price` | N/M tick price | Carried in CC → derivable |
| `aggressor_original_qty` | X tick qty | Carried in CC → derivable |
| `aggressor_remaining` | N tick qty | Carried in CC → derivable |
| `original_tick_type` | N vs M vs X decision | Carried in CC → derivable |
| `crossing_complete` | Self-trade ordering flag | Eliminated if CC carries data |

If `CrossingComplete` carries `{remaining_qty, original_tick_type}`, the receiver can fully synthesize N/M/X from `CrossingComplete` alone, without accumulating `on_trade()` calls. `PendingAggressorState` reduces to `{aggressor_id, aggressor_is_ask}` — just enough for C-tick expansion.

### 8. CrossingComplete: From Signal to Data

Currently 1 byte. Proposed expansion to ~14 bytes:

```cpp
struct CrossingCompleteDelta {
    uint8_t type;                // = 3
    char residual_tick_type;     // 'N' or 'M' (original order type)
    uint8_t padding[2];
    Qty aggressor_remaining;     // 0 = fully consumed, >0 = residual
    // Total: 8 bytes (fits easily in 58B payload)
};
```

This shifts work from receiver-side state accumulation to publisher-side emission. The publisher already knows all of this at the moment it emits `CrossingComplete`. The receiver no longer needs to track `aggressor_remaining` across multiple trades.

Cost: 7 extra bytes per crossing completion (~2% of events). Benefit: eliminates `on_trade()` tracking, most of `PendingAggressorState`, and the `multi_tick_secondary` ordering logic (because the receiver knows everything from the CC delta itself).

### 9. Publisher-Receiver Boundary

**Recommendation: Keep the current boundary.** The publisher should emit semantic primitives (C, S as separate TickInfos + CrossingComplete). Having the publisher emit reference-compat multi-output sequences (C+S+N as three full TickInfos with correct affected_lvl) would:
- Pollute the universal MBO core with reference-specific concerns
- Increase delta payload size for every crossing event
- Make it harder to adapt to different downstream formats

The right fix is #8 above: enrich `CrossingComplete` so the receiver has enough data to expand without maintaining complex state. The C+S+N expansion stays in the receiver (compatibility layer), but becomes stateless given the enriched CC.

The `multi_tick_secondary` ordering logic (lines 1641–1655) is a symptom of the receiver not knowing enough. With enriched CC, the receiver can emit records in the correct order without inspecting `extra_records[0].event.tick_type` to decide ordering.

---

## Summary: Impact Matrix

| Proposal | Lines removed | State removed | Risk |
|----------|--------------|---------------|------|
| 1. PendingCross → 4 fields | ~25 | 4 fields | Low (data moves to CC) |
| 2. cancel_order decomp | ~40 net | — | Low (extract, don't rewrite) |
| 3. cross_fills_ simplification | ~15 | 1 method | Low |
| 4. trade() completion collapse | ~25 | — | Low (follows from #1) |
| 5. would_cross predicate | ~10 | — | Trivial |
| 6. finalize_record helper | ~20 | — | Trivial |
| 7. PendingAggressorState → 2 fields | ~15 | 5 fields | Medium (follows from #8) |
| 8. Enriched CrossingComplete | +10, -30 | eliminates on_trade tracking | Medium |
| 9. Keep boundary | 0 | — | N/A |

**Recommended order:** 5 → 6 → 3 → 2 → 1 → 4 → 8 → 7. Each step is independently testable against the reference. Steps 5, 6, 3 are mechanical. Steps 1+4 and 8+7 are paired.

**Total estimated reduction:** ~120 lines, 9 state fields across publisher+receiver, with no functionality change. The crossing path should read as a natural extension of the non-crossing path rather than a parallel universe bolted on top.

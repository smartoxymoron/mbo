# MBO Crossing Complexity Review (Codex 5.3)

## Part 1: Publisher Side (`MBO`, `PriceLevels`)

### 1) `PendingCross` state reduction

**Proposal:** split state into:
- **Runtime crossing state** (needed for book correctness): `aggressor_id` only (derive side/price from `order_map_`), maybe `origin` (`new` vs `modify`) if still needed.
- **Compatibility metadata** (old resting price/level/original qty) moved out of core publisher path.

**Derivable on demand today:**
- `aggressor_is_ask`, `aggressor_price`: from `order_map_[aggressor_id]` while crossing is active.
- `aggressor_on_level`: derive as `visible = info.qty - passive.pending_cross_fill_qty(); visible > 0`.

**Likely removable from core:**
- `original_resting_price`, `original_affected_lvl`, `aggressor_original_qty`, `residual_tick_type` are only for ref-compat `X` synthesis from crossed modifies.

```cpp
struct ActiveCross {
    OrderId aggressor_id = 0;
    enum class Origin : uint8_t { New, Modify } origin = Origin::New; // only if still needed
    bool active() const { return aggressor_id != 0; }
};
```

---

### 2) `cancel_order()` decomposition

**Problem:** behavior is mostly "same phases, different reversal strategy" but encoded as nested branches.

**Proposal:** two-stage flow:
1. **Classify cancel role**: `Regular`, `AggressorCrossCancel`, `PassiveCrossCancel`, `PassiveNotConsumed`.
2. **Execute strategy** via small helpers with shared scaffolding (`emit C`, `emit S`, `maybe_finish_crossing`, `erase order`).

```cpp
enum class CancelRole { Regular, AggressorCross, PassiveCross, PassiveNoConsume };
CancelRole role = classify_cancel(id, info);

switch (role) {
  case CancelRole::Regular: cancel_regular(info); break;
  case CancelRole::AggressorCross: cancel_cross_aggressor(info); break;
  case CancelRole::PassiveCross: cancel_cross_passive(info); break;
  case CancelRole::PassiveNoConsume: cancel_regular(info); break;
}
finalize_cancel_erase(id);
```

This keeps rare-path complexity isolated and removes three deep nested scopes.

---

### 3) Crossing lifecycle (`cross_fills_`) simplification

**Current friction:** "skip confirmed from front" logic duplicated in `pending_cross_vwap()` and `uncross()`.

**Proposal:** keep only **unconfirmed** fills in a queue-like structure.
- On trade reconcile: consume from front (decrement qty / pop fully confirmed entries).
- `pending_cross_vwap()` becomes direct fold over current entries.
- `uncross()` restores directly from current entries.
- `clear_cross_fills()` and skip math become trivial.

```cpp
// invariant: fills_ contains only unconfirmed speculative consumption
void reconcile_qty(Qty q) {
  while (q > 0 && !fills_.empty()) {
    Qty d = std::min(q, fills_.front().qty);
    fills_.front().qty -= d; q -= d; pending_qty_ -= d;
    if (fills_.front().qty == 0) fills_.pop_front();
  }
}
```

`reconcile_cross_count()` can then be merged into the same ledger API instead of a separate side channel.

---

### 4) Crossing completion in `trade()`

**Proposal:** isolate completion into one helper and remove ref-compat branching from publisher:

```cpp
void maybe_complete_crossing() {
  if (!cross_.active()) return;
  auto& passive = cross_passive_side();
  if (passive.pending_cross_fill_qty() != 0) return;
  passive.clear_cross_state();
  emitter_.emit_crossing_complete();   // or richer CrossResolved
  cross_.clear();
}
```

Publisher then enforces MBO semantics only; receiver/compat layer decides whether to output synthetic `N/M/X`.

---

### 5) Price crossing comparison unification

**Proposal:** one canonical helper for all 3 call sites:

```cpp
inline bool crosses(bool aggressor_is_ask, Price aggressor_price, Price passive_price) {
  return aggressor_is_ask ? (aggressor_price <= passive_price)
                          : (aggressor_price >= passive_price);
}
```

Use in:
- `new_order` (best passive vs incoming price)
- `modify_order`
- passive-cancel-during-crossing predicate

This removes subtle drift between near-identical predicates.

## Part 2: Receiver Side (`apply_deltas_to_book`, `PendingAggressorState`, `Runner`)

### 6) Multi-output expansion cleanup (`C+S+N`, `T+N`)

**Problem:** repeated "finalize -> push -> reset" and implicit ordering via `multi_tick_secondary`.

**Proposal:** introduce a tiny `RecordBatchBuilder`:
- one `finalize_record(OutputRecord&)` function (single source of truth for affected/fill counts),
- explicit append APIs: `emit_primary`, `emit_secondary_before_primary`, `emit_secondary_after_primary`,
- return already ordered list from `apply_deltas_to_book`.

Then `Runner` just iterates returned records; no heuristic on first extra tick type.

---

### 7) `PendingAggressorState` reduction

Without publisher changes, state is justified but can be cleaner.

**If publisher emits minimal additional completion data**, state can shrink drastically:
- Add completion payload: `final_tick` (`none|N|M|X`) + `residual_qty`.
- Receiver no longer needs to track `aggressor_remaining` through every trade.

Then state can be ~`{aggressor_id, side, price}` for in-flight `C` expansion only, or eliminated entirely if `C` carries full aggressor context.

---

### 8) `CrossingComplete` delta (signal-only byte)

**Recommendation:** keep completion explicit (implicit completion is brittle with speculative removal), but make it data-carrying.

Example:

```cpp
struct CrossResolvedDelta {
  uint8_t type;        // new delta type
  char final_tick;     // 0, 'N', 'M', or 'X'
  bool is_ask;
  OrderId aggressor_id;
  Price aggressor_price;
  Qty residual_qty;
};
```

Crossing is rare, so extra bytes are acceptable; receiver complexity drops substantially.

---

### 9) Publisher-receiver boundary (`C`/`S` emission)

**Do not push full `C+S+N` expansion into universal MBO core.** That bakes reference formatting into core logic.

**Cleaner split:**
- Publisher emits **semantic crossing events** (start/self-trade/resolved) with enough data.
- Compatibility layer (receiver-side) performs reference-specific sequence expansion and ordering.

This preserves exchange-agnostic core behavior while still removing today’s receiver guesswork/state juggling.

## Suggested refactor order (low risk -> higher leverage)

1. Unify crossing predicate helper (`crosses(...)`).
2. Extract `cancel_order` classification + strategy helpers.
3. Replace `cross_fills_` lifecycle with unconfirmed-only queue semantics.
4. Move crossing completion decision into one helper.
5. Upgrade `CrossingComplete` to data-carrying `CrossResolved` and shrink/remove `PendingAggressorState`.

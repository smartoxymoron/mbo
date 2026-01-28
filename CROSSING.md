# MBO Crossing Mechanism

## Design Reference

### Core Principle

The strategy **never sees a crossed book**. When an aggressive order arrives that would cross with resting liquidity, we:
1. Consume passive liquidity immediately (update levels)
2. Add only the **residual** of the aggressive order to the book
3. Emit the uncrossed book state
4. Track consumed quantity for later reconciliation when exchange trade messages arrive

### Dual State Model

The system maintains **two views** of order state:

| Component | Tracks | Updated By |
|:----------|:-------|:-----------|
| `order_map_` | Exchange's authoritative view of each order | Trade messages from exchange |
| `PriceLevels` | Strategy's view (always uncrossed) | Crossing inference + reconciled trades |

**Example**: Aggressive ask order arrives with qty=225, matches 150 against passive bid:
- `order_map_[ask_id]` = {qty: **225**} — exchange's view, unchanged until trade confirms
- Ask level = {qty: **75**} — strategy's view, only residual visible
- Bid level = {qty: **0**} — consumed during crossing, removed from book

When trade message arrives confirming fill_qty=150:
- `order_map_[ask_id].qty` reduced: 225 → 75 (exchange authoritative)
- `order_map_[bid_id].qty` reduced: 150 → 0, order erased
- Levels: **unchanged** — already reflect post-cross state

### Order of Operations (Critical)

**Wrong** (add then cross):
```
1. Add aggressive order with full qty (225) to level
2. Detect crossing
3. Remove matched qty from passive ← correct
4. Problem: aggressive level still shows 225, should show 75
```

**Correct** (cross then add residual):
```
1. BEFORE adding: detect if order crosses opposite side
2. If crossing: consume passive levels, calculate residual
3. Add only RESIDUAL (75) to aggressive side level
4. Add to order_map with ORIGINAL qty (225) — exchange's view
```

### Reconciliation via Pending Quantity

Track `pending_cross_fill_qty_` on the **passive side only**. This represents quantity already removed from levels during crossing, awaiting trade confirmation.

When trade arrives:
```cpp
Qty reconciled = passive_side.reconcile_cross_fill(fill_qty);
Qty remaining = fill_qty - reconciled;
// 'remaining' is applied to BOTH orders' levels
// 'fill_qty' is applied to BOTH orders in order_map (exchange authoritative)
```

**Why passive side only?** The reconciled amount applies symmetrically:
- Passive side: we already removed this qty from level during crossing
- Aggressive side: we never added this qty to level (only added residual)

So `remaining = 0` means neither side needs level modification — both are already correct.

### Order Count Handling

During crossing, we don't know how many passive orders were consumed (we only track aggregated qty). Pass `count_delta=0` during crossing. When trade messages arrive, each trade for a fully-filled order decrements count by 1.

**Constraint**: Temporary count mismatches are acceptable. The final book after all trades in a crossing sequence have arrived must match reference.

### Worked Example

**Initial state**:
```
Bid[6220] = {qty: 150, count: 1}
Ask[6255] = {qty: 225, count: 1}
order_map: {bid_id: {qty:150, is_ask:false}, ...}
pending_cross_fill_qty_ (bids): 0
```

**Event: new_order(ask, price=6220, qty=225)**

Step 1 — Detect crossing:
- Aggressive ask at 6220 ≤ best bid at 6220 → crosses

Step 2 — Consume passive (bids.cross(6220, 225)):
- Bid[6220] has 150, consume all → match_qty = 150
- Bid[6220] removed from levels (qty → 0, triggers removal)
- `pending_cross_fill_qty_ = 150`
- Return consumed = 150

Step 3 — Calculate residual:
- residual = 225 - 150 = 75

Step 4 — Add aggressive order:
- `order_map_[ask_id] = {qty: 225, is_ask: true}` — exchange view
- Ask level: add 75 at price 6220 (residual only)

Step 5 — Emit tick:
- TickType: newOrderCross (A), price=6220, qty=225

**Post-crossing state**:
```
Bid[6200] = {qty: ..., count: ...}  ← 6220 level gone
Ask[6220] = {qty: 75, count: 1}     ← residual
order_map: {bid_id: {qty:150}, ask_id: {qty:225}}  ← unchanged
pending_cross_fill_qty_ (bids): 150
```

**Event: trade(bid_id, ask_id, price=6220, fill_qty=150)**

Step 1 — Reconcile:
- `reconciled = bids_.reconcile_cross_fill(150)` → 150
- `remaining = 150 - 150 = 0`

Step 2 — Update order_map (exchange authoritative):
- bid_id: 150 - 150 = 0 → erase
- ask_id: 225 - 150 = 75

Step 3 — Update levels (only if remaining > 0):
- remaining = 0, no level changes
- But bid_id fully filled: need count update → `remove_liquidity(price, 0, 1)`

Step 4 — Emit tick:
- TickType: tradeMsg (T), price=6220, qty=150

**Post-trade state**:
```
Bid[6200] = {qty: ..., count: ...}  ← unchanged
Ask[6220] = {qty: 75, count: 1}     ← unchanged
order_map: {ask_id: {qty:75}}       ← bid erased, ask reduced
pending_cross_fill_qty_ (bids): 0   ← reconciled
```

---

## Prediction and Rollback Model

### Philosophy: Universal MBO Implementation

This is a **universal** MBO implementation designed to handle any exchange's matching semantics. The core philosophy:

1. **We reconstruct market state** from exchange messages (trades, cancels, new/modify orders)
2. **We occasionally predict market state** when aggressive orders arrive (speculative crossing)
3. **We occasionally get predictions wrong** when the exchange behaves unexpectedly (self-trades, aggressor cancels)
4. **We must rollback and retry** when predictions are wrong

The goal is to maintain an accurate book that **never shows a crossed state** to the strategy, while handling all edge cases from the exchange's sequential matching engine.

### Sequential Matching Engine Model

Exchanges process orders **one at a time, to completion**. When an aggressive order arrives:

```
Exchange receives aggressive order
├─ Match against passive order 1 → emit Trade
├─ Match against passive order 2 → emit Trade  
├─ Detect self-trade with passive order 3 → emit Cancel (passive or aggressive)
├─ Match against passive order 4 → emit Trade
└─ Done: emit residual state or cancel if fully consumed
```

Key insight: **No other participant order is processed until this aggressive order completes**. This means:
- All messages for one crossing sequence arrive contiguously
- When we receive a cancel during crossing, it relates to THIS crossing
- We can track crossing state and know when it's complete

### Speculative Crossing: What We Predict

When an aggressive order arrives that would cross, we **speculatively update passive levels**:

```cpp
Qty consumed = passive_side.cross(aggressor_price, aggressor_qty);
```

This removes qty from passive levels immediately, predicting the exchange will match these quantities. We track what we consumed in `pending_cross_fill_qty_`.

**What we add to the book:**
- Aggressive side: only the **residual** (qty not consumed)
- order_map: the **full original qty** (exchange's authoritative view)

### When Predictions Are Wrong

Our speculation can be wrong in two ways:

#### 1. Passive Order Cancelled (Self-Trade Prevention)

The exchange encounters a passive order belonging to the same participant as the aggressor. Instead of trading, it cancels the passive order.

**What we predicted**: Passive order qty consumed, aggressor residual reduced
**What actually happened**: Passive order cancelled, aggressor still has that qty to trade

**Rollback actions:**
1. Determine how much was speculatively consumed from this passive order
2. Remove the passive order's remaining visible portion from its level
3. Unreserve the consumed portion (give it back to aggressor)
4. Re-cross: aggressor needs to find other liquidity for that qty

```cpp
// Simplified logic
Qty consumed_from_order = min(order.qty, pending_cross_fill_qty);
Qty remaining_on_level = order.qty - consumed_from_order;
passive_level.remove(remaining_on_level, count=1);
passive_side.unreserve(consumed_from_order);
passive_side.cross(aggressor_price, consumed_from_order);  // Re-cross
```

#### 2. Aggressor Cancelled

The exchange cancels the **aggressive order itself**. This can happen when:
- All matching passive orders are self-orders (nothing to trade with)
- Exchange risk/credit limits reject the aggressor mid-crossing
- Other exchange-specific rules

**What we predicted**: Aggressor consumed from multiple passive levels
**What actually happened**: Aggressor is cancelled, passive orders should not have been touched

**Rollback actions:**
1. Restore ALL speculative consumption to passive levels
2. Remove aggressor's residual from aggressor's level
3. Clear crossing state

This is more complex because we need to know **which levels** were consumed from, not just the total. See "Cross Fill Tracking" below.

### Cross Fill Tracking (Per-Level Consumption)

To enable proper rollback of aggressor cancels, we must track consumption per level during crossing:

```cpp
struct CrossFill {
    Price price;
    Qty qty;
};
std::vector<CrossFill> cross_fills_;  // Part of PriceLevels, sized to 4 initially
```

During `cross()`:
```cpp
Qty cross(Price limit, Qty qty) {
    cross_fills_.clear();
    Qty consumed = 0;
    while (remaining > 0 && can_cross(limit)) {
        Qty consume = min(remaining, level_qty);
        remove_liquidity(best_price, consume, 0);
        cross_fills_.push_back({best_price, consume});  // Track per-level
        consumed += consume;
        remaining -= consume;
    }
    pending_cross_fill_qty_ += consumed;
    return consumed;
}
```

For aggressor cancel, we can then restore:
```cpp
void uncross() {
    for (const auto& fill : cross_fills_) {
        add_liquidity(fill.price, fill.qty, 0);  // Restore qty, not count
    }
    pending_cross_fill_qty_ = 0;
    cross_fills_.clear();
}
```

**Why not rescan?** The levels themselves have been modified (reduced or removed). Scanning `order_map_` to reconstruct is prohibitively expensive for a hot path.

### Real Example: Aggressor Cancel

From observed exchange data:
```
351374: newOrderCross 1600000000031378 (ASK, qty=1800 at price 375)
351375: tradeMsg 1600000000031139 ← 1600000000031378 (75 @ 385)
351376: tradeMsg 1600000000023418 ← 1600000000031378 (300 @ 380)
351377: tradeMsg 1600000000027930 ← 1600000000031378 (300 @ 375)
        CANCEL: order 1600000000031378 (aggressor!) - still has qty on level
351378: cxlOrderCross 1600000000031378 ← 1600000000031378 (same ID = aggressor cancel)
351379: cxlOrderSelfTrade 1600000000031378
```

Timeline:
1. Aggressive ask order arrives, crosses bid book
2. Three trades execute successfully (total 675 qty)
3. Next match would be a self-trade
4. Exchange cancels the **aggressor** (not the passive)
5. Aggressor's remaining qty (1800 - 675 = 1125) is cancelled

In our system:
- Initial cross consumed qty from multiple bid levels
- Trades confirmed 675 qty → reconciled, pending reduced
- Cancel arrives for aggressor → remaining unconfirmed consumption must be restored

### Invariants

1. **pending_cross_fill_qty_ decreases as trades confirm** — reconcile on each trade
2. **Crossing is complete when pending_cross_fill_qty_ == 0** — all consumption confirmed or rolled back
3. **At most one crossing active per token** — exchange processes sequentially
4. **cross_fills_ only valid during active crossing** — cleared when crossing completes

### Edge Case: Cancel for Unconsumed Passive Order

If a cancel arrives for a passive order that **wasn't actually consumed** (e.g., aggressor qty exhausted before reaching it):

```cpp
Qty consumed_from_order = min(order.qty, pending_cross_fill_qty);
if (consumed_from_order == 0) {
    // Not a self-trade cancel, just a regular cancel during crossing
    emit_tick('X', ...);  // Not 'C'
    remove_full_order_from_level();
}
```

This ensures we emit the correct tick type.

### Performance Considerations

- **cross_fills_ vector**: Pre-sized to 4 elements (typical crossing touches ≤4 levels)
- **Aggressor cancels are rare**: Most crossings complete normally
- **Per-level tracking adds minimal overhead**: One push_back per level touched
- **No heap allocation in hot path**: Vector uses reserved capacity

---

## Implementation Guide

### New State

**PriceLevels** — add one member:
```cpp
Qty pending_cross_fill_qty_ = 0;  // Qty consumed during crossing, awaiting trade reconciliation
```

**MBO** — no new members needed. Existing `last_order_id_` suffices for aggressor detection in trade().

### Interface Changes

**PriceLevels::cross(Price aggressor_price, Qty aggressor_qty) → Qty**

Consume passive liquidity up to aggressor's price/qty. Called BEFORE adding aggressive order.
```cpp
Qty cross(Price aggressor_price, Qty aggressor_qty) {
    if (!g_crossing_enabled) return 0;
    
    Qty consumed = 0;
    Qty remaining = aggressor_qty;
    
    while (remaining > 0 && !levels_.empty()) {
        Price best = best_price();
        if (best == 0) break;
        
        // Check if best price crosses aggressor
        // For bids (passive when ask aggressive): best >= aggressor_price
        // For asks (passive when bid aggressive): best <= aggressor_price
        bool crosses = is_ask_ ? (best <= aggressor_price) : (best >= aggressor_price);
        if (!crosses) break;
        
        auto& [qty, count] = levels_.rbegin()->second;
        Qty consume = std::min(remaining, (Qty)qty);
        
        // Remove from level, count_delta=0 (will be fixed by trades)
        remove_liquidity(best, consume, 0);
        
        consumed += consume;
        remaining -= consume;
    }
    
    pending_cross_fill_qty_ += consumed;
    return consumed;
}
```

**PriceLevels::reconcile_cross_fill(Qty fill_qty) → Qty**

Called during trade processing. Returns how much of fill_qty was already applied to levels.
```cpp
Qty reconcile_cross_fill(Qty fill_qty) {
    Qty reconciled = std::min(fill_qty, pending_cross_fill_qty_);
    pending_cross_fill_qty_ -= reconciled;
    return reconciled;
}
```

### Updated MBO Operations

**MBO::new_order** — cross before add:
```cpp
void new_order(OrderId id, bool is_ask, Price price, Qty qty) {
    if (id == 0) return;
    
    // Cross BEFORE adding — consume passive, get residual
    PriceLevels& passive = is_ask ? bids_ : asks_;
    Qty consumed = passive.cross(price, qty);
    Qty residual = qty - consumed;
    
    // Tick type depends on whether crossing occurred
    char tick_type = (consumed > 0) ? 'A' : 'N';  // A=newOrderCross
    bool is_exch_tick = (consumed == 0);  // synthetic if crossed
    emitter_.emit_tick_info(tick_type, is_ask, is_exch_tick, price, qty);
    
    // order_map stores ORIGINAL qty (exchange view)
    order_map_[id] = {is_ask, price, qty};
    last_order_id_ = id;
    
    // Level gets only RESIDUAL
    if (residual > 0) {
        PriceLevels& aggressor = is_ask ? asks_ : bids_;
        aggressor.add_liquidity(price, residual, 1);
    }
}
```

**MBO::modify_order** — same pattern, but handle price change:
```cpp
void modify_order(OrderId id, Price new_price, Qty new_qty) {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return;
    OrderInfo& info = it->second;
    
    PriceLevels& own_side = info.is_ask ? asks_ : bids_;
    PriceLevels& passive = info.is_ask ? bids_ : asks_;
    
    // Remove old position first
    own_side.remove_liquidity(info.price, info.qty, 1);
    
    // Cross BEFORE adding new position
    Qty consumed = passive.cross(new_price, new_qty);
    Qty residual = new_qty - consumed;
    
    char tick_type = (consumed > 0) ? 'B' : 'M';  // B=modOrderCross
    bool is_exch_tick = (consumed == 0);
    emitter_.emit_tick_info(tick_type, info.is_ask, is_exch_tick, new_price, new_qty);
    
    // order_map stores new qty (exchange view)
    info.price = new_price;
    info.qty = new_qty;
    last_order_id_ = id;
    
    // Level gets only RESIDUAL
    if (residual > 0) {
        own_side.add_liquidity(new_price, residual, 1);
    }
}
```

**MBO::trade** — determine aggressor, tick type, reconcile:
```cpp
void trade(OrderId bid_id, OrderId ask_id, Price price, Qty fill_qty) {
    // Lookup both orders (0 means IOC/hidden - not in book)
    auto bid_it = bid_id ? order_map_.find(bid_id) : order_map_.end();
    auto ask_it = ask_id ? order_map_.find(ask_id) : order_map_.end();
    
    bool bid_in_book = (bid_it != order_map_.end());
    bool ask_in_book = (ask_it != order_map_.end());
    
    // Aggressor is the order NOT in book; if both/neither, use last_order_id_
    bool aggressor_is_ask = (bid_in_book != ask_in_book) ? bid_in_book 
                                                         : (ask_id == last_order_id_);
    auto aggressor_it = aggressor_is_ask ? ask_it : bid_it;
    OrderId aggressor_id = aggressor_is_ask ? ask_id : bid_id;

    // Tick type: 'D' = IOC (id=0), 'E' = market order (not in book), 'T' = normal
    char tick_type = (aggressor_id == 0) ? 'D' : (aggressor_it == order_map_.end() ? 'E' : 'T');
    
    emitter_.emit_tick_info(tick_type, aggressor_is_ask, true, price, fill_qty, bid_id, ask_id);
    
    // Reconcile against passive side
    PriceLevels& passive = aggressor_is_ask ? bids_ : asks_;
    Qty reconciled = passive.reconcile_cross_fill(fill_qty);
    Qty remaining = fill_qty - reconciled;
    
    // Apply to both orders in order_map
    for (auto it : {bid_it, ask_it}) {
        if (it == order_map_.end()) continue;
        OrderInfo& info = it->second;
        
        info.qty -= fill_qty;  // Exchange authoritative
        
        PriceLevels& half = info.is_ask ? asks_ : bids_;
        
        if (remaining > 0) {
            half.remove_liquidity(info.price, remaining, (info.qty == 0) ? 1 : 0);
        } else if (info.qty == 0) {
            half.remove_liquidity(info.price, 0, 1);
        }
        
        if (info.qty == 0) order_map_.erase(it);
    }
}
```

### Removed State

The following from Sonnet's scaffolding are **not needed**:
- `last_op_is_ask_` — aggressor side known from operation context
- `last_op_price_` — passed as argument to cross()
- `last_op_qty_` — passed as argument to cross()
- `has_crossed()` — crossing detection integrated into cross()
- `infer_and_apply_cross()` — crossing logic integrated into new_order/modify_order

### TODO: Future Refactoring

`PriceLevels::cross()` and `PriceLevels::remove_liquidity()` have overlapping logic (iterate levels, consume qty, emit deltas). After crossing is working, consider extracting common consume-from-level logic to reduce duplication.

---

## Implementation Plan

### Phase 1: Clean Up Scaffolding ✓
Remove Sonnet's incomplete crossing scaffolding from mbo.cpp:
- [x] Remove `last_op_is_ask_`, `last_op_price_`, `last_op_qty_` from MBO
- [x] Remove `has_crossed()` method
- [x] Remove `infer_and_apply_cross()` method and calls to it
- [x] Update `cross()`, `reconcile_cross_fill()` stubs, remove `has_pending_cross_fill()`
- [x] Keep `pending_cross_fill_qty_` member (will be used)
- [x] Keep `last_order_id_` (needed for aggressor detection in trade)
- [x] Verify non-crossing mode still works: `make && ./mbo 20k_nocrossing.input.bin --reference 20k_nocrossing.reference.bin`

### Phase 2: Implement PriceLevels::cross() ✓
- [x] Implement `cross(Price aggressor_price, Qty aggressor_qty) → Qty`
- [x] Loop: while remaining > 0 and best price crosses aggressor price
- [x] Each level: call `remove_liquidity(best, consume, 0)` — count_delta=0
- [x] Accumulate `pending_cross_fill_qty_`
- [x] Return total consumed
- [x] `reconcile_cross_fill(Qty) → Qty` already implemented
- [x] Fix remove_liquidity to allow qty=0 for count-only updates

### Phase 3: Update new_order() for Crossing ✓
- [x] Call `passive.cross(price, qty)` BEFORE adding to level
- [x] Calculate `residual = qty - consumed`
- [x] Set tick_type based on crossing: 'A' if consumed > 0, else 'N'
- [x] Add to order_map with original qty
- [x] Add only residual to aggressive level (if residual > 0)
- [x] Non-crossing mode still passes

### Phase 4: Update modify_order() for Crossing ✓
- [x] Peek at passive best to determine tick type BEFORE emitting deltas
- [x] Emit tick_info first (constraint: tick must precede all Update/Insert deltas)
- [x] Remove old position, call `passive.cross()`, add residual
- [x] Same fix applied to new_order() (tick_info before cross)
- [x] Non-crossing mode still passes

### Phase 5: Update trade() for Reconciliation ✓
- [x] After emitting TickInfo, call `passive.reconcile_cross_fill(fill_qty)`
- [x] Calculate `remaining = fill_qty - reconciled`
- [x] For each order: always update order_map qty (exchange authoritative)
- [x] For each order: only call `remove_liquidity` if remaining > 0, or if qty=0 for count update
- [x] Non-crossing mode still passes

### Phase 6: Validation ✓ COMPLETE
- [x] Test with crossing data: `./mbo 20k_crossing.input.bin --crossing --reference 20k_crossing.reference.bin`
- [x] Fixed affected_lvl mismatch by emitting synthetic zero-delta updates in trade() when reconciled > 0
- [x] Fixed residual 'N' record emission timing (emit only after final trade when pending_cross_fill_qty_ == 0)
- [x] **RESOLVED**: Tick types 'E', 'C', 'S', 'D' now handled
  - **E (mktOrderCross)**: Detected when aggressor not in order_map
  - **D (iocOrderCross)**: Detected when aggressor_id == 0
  - **C/S/N (self-trade)**: 'C' emitted with aggressor_id, receiver expands to C/S/N
- [x] **RESOLVED**: Multi-output encoding via receiver expansion
  - 'C' tick contains aggressor_id in order_id2
  - Receiver tracks aggressor state from 'A'/'B' ticks
  - Receiver expands 'C' to C/S/N using tracked state
- [x] All 20,000 records match for both crossing and non-crossing datasets

---

## Dual Callback Model (Reference Behavior)

### The Problem

When a crossing order leaves a residual, the reference emits **two callbacks at trade time** (T1), not one. This is deliberate - it presents two views of the same event.

### Timeline Example

**T0: Crossing order arrives** (order_id=8113, price=1655, qty=900, side=bid)
```
Input:  add_order(bid, 1655, 900)
Emit:   newOrderCross(bid, 1655, 900)     ← original order details
Book:   Already uncrossed - residual 825 visible at best bid
```

**T1: Trade confirms** (75 matched against ask at price 1600)
```
Input:  trade(bid=8113, ask=8090, price=1600, qty=75)
Emit:   tradeMsg(bid, 1600, 75)           ← trade confirmation
Emit:   newOrderMsg(bid, 1655, 825)       ← residual as "virtual new order"
Book:   Unchanged (already correct from T0)
```

### Two Views, One Book

The reference provides two interpretations:

| View | T0 Callback | T1 Callbacks | Use Case |
|:-----|:------------|:-------------|:---------|
| **Market view** | newOrderCross(900) | tradeMsg(75) | What actually happened on exchange |
| **Virtual uncrossed market** | (ignore) | tradeMsg(75) + newOrderMsg(825) | Simulates BSE-like markets where trade precedes residual |

Key insight: **Book state is always uncrossed at T0**. The residual tick at T1 is purely for tick_info consumers who want the "virtual market" view.

### Residual Callback Details

The residual callback uses:
- `tick_type`: newOrderMsg (N), NOT newOrderCross
- `is_exch_tick`: 0 (synthetic - not from exchange)
- `price`: Original order price (1655), not trade price
- `qty`: Residual quantity (825 = 900 - 75)
- `side`: Same as original order

### Implementation Implications

**Current problem**: Our fix queries `mbo.get_residual_info()` from Runner, but:
- Strategy process has no access to MBO
- All info must flow through DeltaChunks

**Solution needed**: Emit residual info in deltas so `apply_deltas_to_book()` can:
1. Produce the Trade OutputRecord
2. Produce the Residual OutputRecord (with tick_type='N', is_exch_tick=0)

Options:
- **ResidualDelta**: New delta type with {price, qty, is_ask}
- **TickInfo extension**: Embed residual info in trade's TickInfo
- **State tracking**: apply_deltas_to_book() tracks crossing state (complex)

---

## Tick Type Behavior Analysis (from 20k_crossing data)

### Tick Type Inventory

From `mds_objects.h` TickType enum (line 67-70 in mbo.cpp):
```
'N' = newOrderMsg      'M' = modOrderMsg      'T' = tradeMsg       'X' = cancelOrderMsg
'S' = cxlOrderSelfTrade 'A' = newOrderCross   'B' = modOrderCross  'C' = cxlOrderCross
'D' = iocOrderCross    'E' = mktOrderCross
```

### Data Coverage

| Input Type | Count | Reference Outputs | Our Outputs | Status |
|:-----------|------:|:------------------|:------------|:-------|
| N | 19817 | N(19704), A(128) | N(19704), A(128) | Working |
| T | 174 | T(160), E(4), D(10) | T(174) | E/D needed |
| M | 7 | M(7) | M(7) | Working |
| X | 2 | X(1), C+S+N(3 for 1 input) | X(2) | C/S/N needed |

**Not in test data**: B (modOrderCross) - assumed similar to A

---

### E: mktOrderCross (Market Order Cross)

**Definition**: Trade from a market order that was never visible on book (no resting possible).

**How to detect**: Trade message (`type:T`) where `id` was **never seen in a prior N record** for this instrument. The order crossed immediately on arrival with no residual (market orders have "infinitely good" price).

**Observed example** (tok:3880, order 8384):
```
Input sequence:
  [13313] id:8384 id2:8184 p:2480000 q:75  type:T side:BID  ← first appearance!
  [13314] id:8384 id2:8037 p:2482000 q:375 type:T side:BID  ← same order, more trades

Reference output:
  [13313] tick:E side:BID affected_bid:20 affected_ask:0 ltp:2480000 ltq:75
  [13314] tick:E side:BID affected_bid:20 affected_ask:0 ltp:2482000 ltq:375

Our output:
  [13313] tick:T side:BID affected_bid:20 affected_ask:0 ltp:2480000 ltq:75  ← should be E
  [13314] tick:T side:BID affected_bid:20 affected_ask:0 ltp:2482000 ltq:75  ← should be E
```

**Book behavior**: No book change from the aggressor's order (it was never added). Only passive side changes.

**Fix approach**: Track seen order IDs; if trade references unknown order, emit tick:E not tick:T.

---

### D: iocOrderCross (IOC Order Cross)

**Definition**: Trade confirmation for an IOC (Immediate Or Cancel) order. IOC orders cannot rest, so no residual.

**How to detect**: Trade message (`type:T`) with `id2 == 0` (no matching passive order visible). The order was previously added as a crossing order (tick:A), and now fully or partially fills.

**Observed example** (tok:197, order 2686):
```
Input sequence:
  [12554] id:2686 id2:0    p:10000 q:450 type:N side:BID ← New order (crosses)
  [12555] id:2686 id2:2434 p:9900  q:75  type:T side:BID ← Trade, partial fill
  [17489] id:2686 id2:0    p:10000 q:75  type:T side:BID ← IOC finish (id2=0!)

Reference output:
  [12554] tick:A side:BID affected_bid:0 affected_ask:0 ltp:10000 ltq:450
  [12555] tick:T side:BID affected_bid:0 affected_ask:0 ltp:9900 ltq:75
  [12555] tick:N side:BID affected_bid:0 affected_ask:0 ltp:10000 ltq:375  ← residual
  [17489] tick:D side:ASK affected_bid:0 affected_ask:20 ltp:10000 ltq:75  ← D, side FLIPPED!

Our output:
  [17489] tick:T side:BID affected_bid:0 affected_ask:20 ltp:10000 ltq:75  ← should be D, ASK
```

**Key insight**: Reference reports tick:D from the **passive side perspective** (ASK was hit), not the aggressor (BID). This is consistent with IOC semantics - the IOC order is gone, only the passive fill matters.

**Book behavior**: No residual added (IOC cannot rest). Passive side updated.

**Fix approach**: When trade has `id2 == 0`, emit tick:D with side flipped to passive perspective.

---

### C/S/N: Self-Trade Prevention (Cancel with Pending Crossing)

**Definition**: When an order is cancelled while it has pending crossing trades, AND the cancel is to prevent self-trade.

**Observed example** (tok:193, order 8646 vs 8687):
```
Order 8646 history:
  [16028] id:8646 p:7105 q:150 type:N side:BID  ← New BID at 7105
  [16029] id:8646 id2:8539 p:6900 q:75 type:T  ← Trade, qty now 75
  [16632] id:8646 p:0 q:0 type:X side:BID      ← Cancel

Order 8687 arrives between [16029] and [16632]:
  [16631] id:8687 p:6990 q:150 type:N side:ASK ← New ASK at 6990 (would cross 8646!)

Input sequence:
  [16631] id:8687 p:6990 q:150 type:N side:ASK ← Aggressive ASK crosses resting BID
  [16632] id:8646 p:0 q:0 type:X side:BID      ← Cancel BID to prevent self-trade

Reference output:
  [16631] tick:A side:ASK affected_bid:0 affected_ask:0 ltp:6990 ltq:150
  [16632] tick:C side:ASK affected_bid:0 affected_ask:0 ltp:7105 ltq:75    ← cancel cross
  [16632] tick:S side:BID affected_bid:20 affected_ask:20 ltp:7105 ltq:75  ← self-trade
  [16632] tick:N side:ASK affected_bid:20 affected_ask:0 ltp:6990 ltq:150  ← aggressor rests

Our output:
  [16631] tick:A side:ASK affected_bid:0 affected_ask:0 ltp:6990 ltq:150
  [16632] tick:X side:BID affected_bid:20 affected_ask:20 ltp:7105 ltq:75  ← just cancel
```

**Interpretation**:
1. Order 8687 (ASK@6990) arrives and would cross with 8646 (BID@7105)
2. But 8687 and 8646 are from the **same trader** (self-trade!)
3. Exchange cancels 8646 to prevent self-trade
4. Order 8687 now rests on the book (no crossing actually occurs)

**Reference emits THREE outputs for one input [16632]**:
- **tick:C (cxlOrderCross)**: Cancel notification, side=ASK (aggressor perspective), p/q from cancelled order
- **tick:S (cxlOrderSelfTrade)**: Self-trade event, side=BID (cancelled order's side)
- **tick:N (newOrderMsg)**: Aggressor now rests on book, side=ASK

**Book state evolution** (observed from reference):
```
STARTING STATE (before [16631]):
ASKS:                           BIDS:
[2] 7425  750  1                [0] 7105  75   1  ← order 8646 (will be cancelled)
[1] 7200  150  1                [1] 6775  300  1
[0] 7195  525  2                [2] 6600  75   1
                                [3] 6555  150  1

AFTER tick:A [16631] (aggressive ASK@6990 crosses BID@7105):
ASKS:                           BIDS:
[3] 7425  750  1                [0] 6775  300  1  ← BID@7105 consumed by cross
[2] 7200  150  1                [1] 6600  75   1
[1] 7195  525  2                [2] 6555  150  1
[0] 6990  75   1  ← residual (150-75 crossed = 75 remaining)

AFTER tick:C [16632] (cxlOrderCross) - ROLLBACK!:
ASKS:                           BIDS:
[3] 7425  750  1                [0] 6775  300  1
[2] 7200  150  1                [1] 6600  75   1
[1] 7195  525  2                [2] 6555  150  1
[0] 6990  150  1  ← RESTORED to original 150 (cross undone)

AFTER tick:S [16632] (cxlOrderSelfTrade):
(Book unchanged - same as tick:C)

AFTER tick:N [16632] (newOrderMsg):
(Book unchanged - ASK@6990 already at 150 from tick:C)
```

**Key insight: ROLLBACK of speculative cross**
- At tick:A, the cross was speculatively applied: ASK qty reduced from 150→75, BID@7105 removed
- At tick:C, the cross is **undone** because of self-trade: ASK qty restored from 75→150
- BID@7105 stays removed (it was being cancelled anyway)
- The aggressive ASK now rests with its **full original quantity**

This rollback behavior must be implemented or emulated. Either:
1. MBO tracks enough state to rollback speculative crosses on self-trade cancel
2. Reconstruction layer synthesizes the rollback from delta information

**affected_lvl values**:
- tick:C: `bid:0, ask:0` (rollback applied but reported as no change)
- tick:S: `bid:20, ask:20` (no additional change)
- tick:N: `bid:20, ask:0` (ASK now at best ask with full qty)

---

### Summary: Detection Rules

| Tick Type | Input Pattern | Detection Logic |
|:----------|:--------------|:----------------|
| E | type:T | Trade where order_id never seen in prior type:N |
| D | type:T with id2=0 | IOC finish - trade with no visible passive match |
| C | type:X | Cancel where order had pending crossing trades |
| S | type:X | Immediately follows C - same cancel, self-trade aspect |
| N (after X) | (none) | Synthetic - aggressor rests after self-trade cancel |

---

### Implementation Approach for Multi-Output Cases

For tick:C/S/N (one input → three outputs), options considered:

**Option 1: Multiple TickInfo deltas per chunk** - Each tick explicit in delta stream

**Option 2: SelfTradeCancel delta type** - Custom delta that expands to 3 records (rejected: adds complexity)

**Option 3: Receiver-side expansion** ✓ CHOSEN - Track aggressor state at receiver, expand 'C' tick

**Why Option 3**: Minimizes delta types; TickInfoDelta already extended with order_id/order_id2 for trade tracking; receiver needs aggressor state anyway for correct book reconstruction.

### Chosen Approach: C Tick with Aggressor Reference

**Decision**: Use the existing TickInfoDelta with tick_type='C' to encode self-trade cancels. The receiver tracks aggressor state from 'A'/'B' ticks and expands 'C' to C/S/N.

**Rationale**:
1. **No new delta types**: TickInfoDelta already has order_id fields (added for trade tracking)
2. **Receiver owns expansion**: MBO emits a 'C' tick, receiver synthesizes C/S/N sequence
3. **C implies S**: Every C is immediately followed by S (70:70 perfect 1:1 in data), so S adds no information

**Delta Encoding** (uses existing TickInfoDelta):
```cpp
// For 'C' tick:
TickInfoDelta {
    tick_type = 'C';
    is_ask = cancelled_order_is_ask;  // passive side
    price = cancelled_price;
    qty = cancelled_qty;
    order_id = cancelled_order_id;
    order_id2 = aggressor_id;         // Key: links to pending aggressor
}
```

**Receiver State** (tracks aggressor from 'A'/'B' ticks):
```cpp
struct PendingAggressorState {
    OrderId aggressor_id;
    bool aggressor_is_ask;
    Price aggressor_price;
    Qty aggressor_remaining;  // Updated on trades
};
```

**Receiver Expansion** (in `apply_deltas_to_book`):
1. On 'A'/'B' tick: `agg_state.set(order_id, is_ask, price, qty)`
2. On 'C' tick: Emit 3 OutputRecords using `agg_state`:
   - C: tick from delta (aggressor's POV)
   - S: tick_type='S', side=cancelled side
   - N: tick_type='N', side/price/qty from agg_state

### Implementation Status (2026-01-28)

**Implemented:**
- TickInfoDelta extended with order_id and order_id2 fields (32 bytes)
- PendingCross state tracking in MBO for self-trade detection
- Self-trade detection in cancel_order: emits 'C' tick with aggressor ID in order_id2
- Receiver-side aggressor tracking (PendingAggressorState)
- Receiver-side C/S/N expansion from 'C' tick
- Multi-output handling in Runner
- 'D' (iocOrderCross): Trade where aggressor ID is 0
- 'E' (mktOrderCross): Trade where aggressor has non-zero ID but not in order_map
- Trade aggressor detection: order NOT in book is aggressor (not based on side hint)

**Tested:**
- 20k_crossing dataset with --crossing: ✅ All 20,000 records match
- 20k_nocrossing dataset: ✅ All 20,000 records match

**All crossing tick types now implemented: A, B, C, D, E, S (via C expansion)**

---

## Empirical Behavior Analysis (from ~1M packet sample)

*Analysis of reference output from production data to understand tick type behavior and edge cases.*

### Tick Type Frequency (from ~1M packets)

```
426637 CANCEL_MSG (internal debug)
214008 cancelOrderMsg
     70 cxlOrderCross
     70 cxlOrderSelfTrade
  19381 iocOrderCross
   7868 mktOrderCross
    912 modOrderCross
 260126 modOrderMsg
  10507 newOrderCross
 401643 newOrderMsg
  17223 tradeMsg
```

### Confirmed Behaviors (100% Confident)

#### 1. mktOrderCross and iocOrderCross CANNOT cause self-trade cancels

Analysis of what follows mktOrderCross:
```
   3719 --
    763 cancelOrderMsg
     42 iocOrderCross
      8 modOrderCross
   1271 modOrderMsg
    110 newOrderCross
   1595 newOrderMsg
     ** NO cxlOrderCross or cxlOrderSelfTrade **
```

Analysis of what follows iocOrderCross:
```
  11143 --
   4066 cancelOrderMsg
     50 mktOrderCross
     21 modOrderCross
   3882 modOrderMsg
    174 newOrderCross
   4489 newOrderMsg
     ** NO cxlOrderCross or cxlOrderSelfTrade **
```

**Reason**: Market orders and IOC orders don't rest in the book. They either fill immediately or are gone. No opportunity for a later self-trade cancel situation.

#### 2. Extended self-trade sequences exist: A (C S N)* T

Example from seq 364615-364631 (one aggressive order hitting 5 self-orders then trading):
```
364615: newOrderCross 1400000000032153 (aggressive BID@370)
364616: cxlOrderCross 1400000000026753 (passive ASK cancelled)
364617: cxlOrderSelfTrade 1400000000026753
364618: newOrderCross 1400000000032153 (re-try crossing)
364619: cxlOrderCross 1400000000031805 (another passive ASK cancelled)
364620: cxlOrderSelfTrade 1400000000031805
... repeats 3 more times ...
364631: tradeMsg 1400000000032153 1400000000032108 (finally trades!)
```

**Pattern**: A → (C S N)* → T (or A → (C S N)* → N if no more crossing)

The aggressive order keeps re-crossing after each self-trade cancel until it either:
- Trades successfully with a non-self order (ends with T)
- Exhausts all crossing opportunities and rests (ends with N as newOrderMsg)

#### 3. Self-cancel case: aggressor ID == passive ID

When cxlOrderCross shows same ID twice, the aggressive order itself is being cancelled:
```
351377: cxlOrderCross 1600000000031378 1600000000031378 (same ID!)
351378: cxlOrderSelfTrade 1600000000031378
** NO newOrderMsg follows - aggressive is gone **
```

Context: Order traded partially, then hit a self-order, and the **aggressor was cancelled** (not the passive). No residue remains.

#### 4. Perfect match still leaves residue (if self-trade prevented)

Example (75 qty aggressive vs 75 qty passive - would be perfect match):
```
41043: newOrderCross 1400000000004868 (ASK@16705, qty 75)
41044: cxlOrderCross 1400000000004681 (passive BID@17005 qty 75 cancelled)
41045: cxlOrderSelfTrade 1400000000004681
41046: newOrderMsg 1400000000004868 (ASK rests!)
```

Even though quantities match exactly, the self-trade prevention means:
- Passive order is cancelled (C, S ticks)
- Aggressive order now **rests as a regular order** (N tick)
- 75 is the minimum order qty for this instrument

#### 5. cxlOrderCross and cxlOrderSelfTrade always come in pairs

In the data: 70 cxlOrderCross and 70 cxlOrderSelfTrade - perfect 1:1 match.
Every C is immediately followed by S (same input, multiple outputs).

### Edge Cases and Uncertainties

#### 1. What if aggressive has no residue when self-trade occurs?

**Observed case**: When aggressive == passive in cxlOrderCross:
- The aggressive is fully cancelled
- No N tick follows (no residue to rest)

**Unknown**: If a large aggressive fully matches, then a later cancel arrives - does this even produce a self-trade situation? The reference code looks up the aggressor in `m_generated_trades` queue, which suggests the aggressive must still exist (have pending qty) for self-trade handling to occur.

**Hypothesis**: Self-trade cancel can only happen when there IS a residue, because:
1. If aggressive fully matched, it's gone before any cancel could arrive
2. Market orders (E) never have residue, hence no self-trade (confirmed by data)
3. IOC orders (D) never rest, hence no self-trade (confirmed by data)

#### 2. What if aggressive matches multiple orders including self-orders interleaved?

Example observed:
```
Large aggressive → T T T (trades) → C S N (self-trade) → T T (more trades)
```

The reference re-runs crossing after each self-trade cancel. The aggressive continues matching with remaining passive orders.

**Open question**: Can we get nested patterns like A C S N C S N T C S N T? (self-trade, trade, self-trade, trade)

The data shows mostly clean sequences. More exotic patterns may exist but are rare.

#### 3. Reference code's `m_generated_trades` queue

From the reference code:
```cpp
if (!m_generated_trades.empty()) {
    apply_cancel_on_generated_trades(aggressor_id, generated_trds_reason);
    ...
}
```

The reference maintains a queue of speculative trades. When cancel arrives:
1. Check if cancelled order is in the queue
2. If so, drain queue and process self-trade
3. Re-cross the aggressor with remaining orders

**Our approach**: We don't track individual speculative trades. We track:
- `pending_cross_fill_qty_` on passive side
- When cancel arrives during active cross at a crossing price → self-trade

This is simpler but should handle the common cases.

### Input Sufficiency for Self-Trade Detection

**The cancel input contains sufficient information**:
- `order_id`: Which order is being cancelled
- From `order_map_`: We know the order's price, qty, side
- From `pending_cross_fill_qty_ > 0`: We know a cross is active
- The cancelled order's price allows us to determine if it was on the passive side of the cross

**Detection logic**:
1. Cancel arrives for order O
2. Check if `pending_cross_fill_qty_ > 0` on the opposite side from O
3. Check if O's price would have been crossed by the pending aggressive
4. If both true → self-trade situation

### Proposed Implementation: Multi-TickInfo Per Chunk

For C/S/N (one input → three outputs), emit multiple TickInfo in one chunk:

```
Chunk for [16632] (cancel causing self-trade):
  TickInfo{type=C, side=ASK, p=7105, q=75}   ← cxlOrderCross
  Update{...}                                 ← book changes for C
  TickInfo{type=S, side=BID, p=7105, q=75}   ← cxlOrderSelfTrade  
  TickInfo{type=N, side=ASK, p=6990, q=150}  ← newOrderMsg for resting aggressor
  Update{...}                                 ← book changes for N (qty restored)
```

`apply_deltas_to_book()` processes sequentially, yielding one OutputRecord per TickInfo.

**Why this approach**:
1. MBO stays clean - just emits natural deltas plus tick markers
2. Reconstruction layer handles the multi-output case
3. No "invented" information beyond the tick type markers
4. Each TickInfo maps 1:1 to an OutputRecord

### Summary: What We Can Implement Confidently

| Tick Type | Can Implement? | Notes |
|:----------|:---------------|:------|
| E (mktOrderCross) | YES | Detect: trade with unknown order_id. Simple. |
| D (iocOrderCross) | YES | Detect: trade with id2=0. Flip side to passive. |
| C (cxlOrderCross) | YES | Detect: cancel during active cross at crossed price. |
| S (cxlOrderSelfTrade) | YES | Always follows C. Emit immediately after C. |
| N (after self-trade) | YES | Emit with aggressor's restored qty after C/S. |
| B (modOrderCross) | ASSUMED | Same as A (newOrderCross), just for modify. Not in test data. |

### What May Need Refinement

1. **Rollback qty calculation**: When self-trade occurs, the cancelled order's qty must be added back to the aggressive order's residue. The exact qty to rollback = cancelled order's original qty at that price.

2. **Multiple self-trades in sequence**: The data shows this works (A C S N C S N ... T). Our re-crossing loop should handle this naturally.

3. **Very exotic edge cases**: Aggressive matching own orders at multiple price levels, etc. These are theoretically possible but not observed in the data. We'll handle them when validation catches them.

---

## Reference Implementation Details

*The following sections describe the reference implementation's internal mechanisms. Our implementation achieves equivalent behavior with simplified state.*

### Internal State Components (Reference)

#### 1. `m_generated_trades` (Queue)
- **Purpose**: Stores expected trades that haven't been confirmed by the exchange yet
- **Populated**: During `handle_crossing_on_add()` when an aggressive order matches passive levels
- **Cleared**: When matching `tradeMsg` arrives or when reconciled via cancellation

#### 2. `m_uncrossed_qty` (Per Level)
- **Purpose**: Tracks the "visible" quantity at each price level after accounting for matches
- **Updated**: Reduced immediately when crossing occurs (before exchange confirmation)
- **Effect**: Subsequent aggressive orders cannot match against already-consumed liquidity

#### 3. `m_crossed_buffer` (Per Half-Book)
- **Purpose**: Holds pointers to levels that were fully consumed during crossing
- **Populated**: During `try_crossing()` when a level's `m_uncrossed_qty` reaches zero
- **Cleared**: After processing (levels are removed from the book) or during cancel reconciliation

---

## Tick Types

### Basic Operations
| Type | Abbrev | Description |
|:-----|:-------|:------------|
| `newOrderMsg` | N | New order added to book (no crossing) |
| `modOrderMsg` | M | Order modified (no crossing) |
| `cancelOrderMsg` | X | Order cancelled |
| `tradeMsg` | T | Official exchange-confirmed trade |

### Crossing Events
| Type | Abbrev | Description |
|:-----|:-------|:------------|
| `newOrderCross` | A | New aggressive order crossed the book |
| `modOrderCross` | B | Modified order crossed the book |
| `cxlOrderCross` | C | Summary of virtual fills for a cancelled order with pending trades |
| `iocOrderCross` | D | IOC order crossed (exchange didn't generate new order) |
| `mktOrderCross` | E | Market order crossed (exchange didn't generate new order) |
| `cxlOrderSelfTrade` | S | Cancellation with pending trade reconciliation |

---

## Event Flows (Reference Implementation)

### Flow 1: Normal Crossing (Add Order)

**Input**: `add_order(px=6220, qty=225, side=ask)` when Best Bid is `[6220, 150]`

**Processing**:
```
1. Detect crossing: px (6220) <= Best Bid (6220)
2. Execute handle_crossing_on_add():
   - Match 150 @ 6220 against Best Bid
   - Reduce Best Bid's m_uncrossed_qty to 0
   - Add Best Bid level to m_crossed_buffer
   - Push GeneratedTrade{buy_oid, sell_oid, px=6220, qty=150} to m_generated_trades
   - Return qty_remaining = 75
3. Add residual order (75 shares) to ask side
4. Update m_published_book
```

**Emitted Tick #1** (Immediate):
```
TickType: newOrderCross
Side: ask
Price: 6220 (original order price)
Quantity: 225 (original order quantity)
is_exch_tick: 0 (synthetic)

OrderBook State:
  Best Bid: 6200 (the old 6220 level is gone)
  Best Ask: 6220 @ 75 (residual from aggressive order)
```

**Input**: `on_trade(buy_oid=..., sell_oid=..., px=6220, qty=150)`

**Processing**:
```
1. Match against m_generated_trades queue
2. validate_generated_trades(): pop from queue, reduce order quantities
3. No book structure change (already happened)
```

**Emitted Tick #2** (After trade confirmation):
```
TickType: tradeMsg
Side: ask
Price: 6220
Quantity: 150 (matched quantity)
is_exch_tick: 1 (official exchange message)

OrderBook State:
  IDENTICAL to Tick #1 (no change)
  Best Bid: 6200
  Best Ask: 6220 @ 75
```

---

### Flow 2: Crossing with Cancel Before Trade Confirmation (Reference)

**Input**: `amend_order(oid=4299, px=6220, qty=225, side=ask)` when Best Bid is `[6220, 75]`

**Processing**:
```
1. Detect crossing, execute handle_crossing_on_add()
2. Match 75 @ 6220, qty_remaining = 150
3. Push GeneratedTrade to m_generated_trades
4. Update book
```

**Emitted Tick #1**:
```
TickType: modOrderCross
Side: ask
Price: 6220
Quantity: 225
is_exch_tick: 0

OrderBook State:
  Best Bid: 6200 (6220 level removed)
  Best Ask: 6220 @ 150
```

**Input**: `cancel_order(oid=4299, side=ask)` **BEFORE** trade arrives

**Processing**:
```
1. Detect !m_generated_trades.empty()
2. apply_cancel_on_generated_trades():
   - Aggregate all pending trades: total_qty=75, weighted_px=6220
   - Pop all from m_generated_trades
   - Clear m_crossed_buffer on both sides
   - Set tick_info{type=cxlOrderCross, qty=75, px=6220}
3. Set cancel_reason = cxlOrderSelfTrade
```

**Emitted Tick #2**:
```
TickType: cxlOrderCross
Side: ask
Price: 6220 (weighted average of virtual fills)
Quantity: 75 (total filled quantity from pending trades)
is_exch_tick: 1

OrderBook State: UNCHANGED
  Best Bid: 6200
  Best Ask: 6220 @ 150
```

**Emitted Tick #3**:
```
TickType: cxlOrderSelfTrade
Side: ask
Price: 6220
Quantity: 150 (cancelled residual quantity)
is_exch_tick: 1
Info.m_cxl_info: {order_id=4299, aggressor_id=...}

OrderBook State:
  Best Bid: 6200
  Best Ask: 6225 (the 6220 level with 150 is now removed)
```

**Input**: `on_trade(...)` arrives later

**Processing**:
```
1. Match against m_generated_trades: QUEUE IS EMPTY
2. validate_generated_trades() already reconciled via cancel
3. Trade is processed normally (reduce quantities)
```

**Emitted Tick #4**:
```
TickType: tradeMsg
Side: ask
Price: 6220
Quantity: 75
is_exch_tick: 1

OrderBook State:
  Best Bid: 6200
  Best Ask: 6225
```

---

### Flow 3: Non-Crossing Operations (Reference)

**Input**: `add_order(px=6400, qty=225, side=ask)` when Best Bid is `[6220, 150]`

**Processing**:
```
1. No crossing detected (6400 > 6220)
2. Add order to book at level
3. No generated trades
```

**Emitted Tick**:
```
TickType: newOrderMsg
Side: ask
Price: 6400
Quantity: 225
is_exch_tick: 1

OrderBook State:
  Best Bid: 6220 @ 150
  Best Ask: 6255 @ 225 (6400 is deeper in the book)
```

---

## State Transition Matrix (Reference)

| Input Event | Crossing? | `m_generated_trades` | `m_uncrossed_qty` | Book Levels | Emitted Tick(s) |
|:------------|:----------|:---------------------|:------------------|:------------|:----------------|
| `add_order` (passive) | No | - | - | +1 level | 1× `newOrderMsg` |
| `add_order` (aggressive) | Yes | +N trades | Reduced at matched levels | Matched levels removed, residual added | 1× `newOrderCross`, later 1× `tradeMsg` per trade |
| `amend_order` (passive) | No | - | - | Updated | 1× `modOrderMsg` |
| `amend_order` (aggressive) | Yes | +N trades | Reduced at matched levels | Matched levels removed, residual updated | 1× `modOrderCross`, later 1× `tradeMsg` per trade |
| `cancel_order` (no pending) | - | - | - | -1 level | 1× `cancelOrderMsg` |
| `cancel_order` (with pending) | - | Cleared | - | -1 level | 1× `cxlOrderCross`, 1× `cxlOrderSelfTrade` |
| `on_trade` (generated match) | - | Pop validated trade | - | - | 1× `tradeMsg` |
| `on_trade` (unexpected) | - | - | Reduced | - | 1× `tradeMsg` (or `iocOrderCross`/`mktOrderCross`) |

---

## Critical Implementation Details (Reference)

### 1. Book State in Emitted Ticks
- **ALL** emitted ticks contain the `m_published_book` which reflects the **current** (uncrossed) state
- For `newOrderCross`/`modOrderCross`: Book is **already updated** before emission
- For `tradeMsg`: Book state is **identical** to the earlier cross tick (no structural change)
- For `cxlOrderCross`: Book state is **unchanged** (this tick only summarizes virtual fills)
- For `cxlOrderSelfTrade`: Book is updated to **remove** the cancelled order's residual

### 2. Quantity Semantics
- **Cross Ticks** (`newOrderCross`, `modOrderCross`): `m_qty` = original aggressive order quantity
- **Trade Ticks** (`tradeMsg`): `m_qty` = actual matched quantity
- **Cancel Cross** (`cxlOrderCross`): `m_qty` = sum of all pending virtual fills
- **Self-Trade Cancel** (`cxlOrderSelfTrade`): `m_qty` = cancelled residual quantity

### 3. Price Semantics
- **Cross Ticks**: `m_price` = aggressive order's limit price
- **Trade Ticks**: `m_price` = actual match price
- **Cancel Cross**: `m_price` = weighted average of pending fills
- **Self-Trade Cancel**: `m_price` = cancelled order's price

### 4. Sequence Guarantees
- A `newOrderCross` is **always** followed by matching `tradeMsg`(s) OR a `cxlOrderSelfTrade`
- A `cxlOrderCross` is **always** followed immediately by a `cxlOrderSelfTrade`
- `m_generated_trades` queue maintains FIFO order for validation

### 5. `is_exch_tick` Flag
- `0` (false): Synthetic tick generated by MBO crossing logic (`newOrderCross`, `modOrderCross`)
- `1` (true): All other ticks directly correspond to exchange messages

---

## Testing Scenarios

### Scenario 1: Simple Cross
```
Given: Bid[6220]=100
When:  add_order(ask, px=6220, qty=50)
Then:  Emit newOrderCross(ask, 6220, 50)
       Emit tradeMsg(6220, 50)
       Book: Bid[6220]=50
```

### Scenario 2: Multi-Level Cross
```
Given: Bid[6220]=50, Bid[6210]=100
When:  add_order(ask, px=6210, qty=120)
Then:  Emit newOrderCross(ask, 6210, 120)
       Emit tradeMsg(6220, 50)
       Emit tradeMsg(6210, 70)
       Book: Bid[6210]=30
```

### Scenario 3: Cancel Before Trade
```
Given: Bid[6220]=100
When:  add_order(ask, px=6220, qty=50)
       cancel_order(ask, oid=X)
Then:  Emit newOrderCross(ask, 6220, 50)
       Emit cxlOrderCross(ask, 6220, 50)
       Emit cxlOrderSelfTrade(ask, 6220, 0)
       Emit tradeMsg(6220, 50)
       Book: Bid[6220]=50
```

---

## Implementation Roadmap (Next Steps)

### Priority 1: E (mktOrderCross) and D (iocOrderCross) ✓ IMPLEMENTED

**These are simple and have many occurrences (7868 E, 19381 D).**

**Combined detection in trade()** - aggressor determined by which order is NOT in book:
```cpp
bool bid_in_book = (bid_it != order_map_.end());
bool ask_in_book = (ask_it != order_map_.end());

// Aggressor is the order NOT in book
bool aggressor_is_ask = (bid_in_book != ask_in_book) ? bid_in_book 
                                                     : (ask_id == last_order_id_);
OrderId aggressor_id = aggressor_is_ask ? ask_id : bid_id;
auto aggressor_it = aggressor_is_ask ? ask_it : bid_it;

// Tick type determination:
char tick_type = (aggressor_id == 0) ? 'D'                            // IOC (id=0)
               : (aggressor_it == order_map_.end()) ? 'E'             // Market order
               : 'T';                                                  // Normal trade
```

### Priority 2: C/S/N (Self-Trade Prevention) ✓ IMPLEMENTED

**Less common (70 occurrences) but architecturally significant.**

**MBO State**:
```cpp
struct PendingCross {
    OrderId aggressor_id = 0;
    bool aggressor_is_ask = false;
    Price aggressor_price = 0;
    Qty aggressor_original_qty = 0;
    
    bool is_active() const { return aggressor_id != 0; }
    void clear() { aggressor_id = 0; }
};
```

**Detection in cancel_order()** - emits 'C' tick with aggressor reference:
```cpp
if (pending_cross_.is_active()) {
    bool is_passive_side = (info.is_ask != pending_cross_.aggressor_is_ask);
    bool would_cross = /* check if cancelled order's price crosses aggressor */;
    
    if (is_passive_side && would_cross) {
        // Self-trade! Emit 'C' tick with aggressor_id in order_id2
        emitter_.emit_tick_info('C', info.is_ask, true, info.price, info.qty, 
                                id, pending_cross_.aggressor_id);
        // Book adjustments: remove cancelled order, restore aggressor qty
        // ...
    }
}
```

**Receiver Expansion** - tracks aggressor from 'A'/'B' and expands 'C':
```cpp
// On 'A' or 'B' tick:
agg_state.set(delta->order_id, is_ask, delta->price, delta->qty);

// On 'C' tick - expand to 3 outputs:
OutputRecord rec_c = rec;  // C tick as-is
extra_records->push_back(rec_s);  // S tick (cancelled order's side)
extra_records->push_back(rec_n);  // N tick (aggressor rests, from agg_state)
agg_state.clear();
```

### Priority 3: Multi-Output Delta Encoding ✓ IMPLEMENTED

**Target**: `apply_deltas_to_book()` produces multiple OutputRecords from one chunk.

**Approach**: 'C' tick expands to C/S/N using tracked aggressor state.

```cpp
int apply_deltas_to_book(OutputRecord& rec, std::span<const DeltaChunk> chunks,
                         PendingAggressorState& agg_state, 
                         std::vector<OutputRecord>* extra_records) {
    int num_records = 1;  // Primary record
    
    // ... process deltas ...
    
    if (delta->tick_type == 'A' || delta->tick_type == 'B') {
        // Track aggressor for potential C/S/N expansion
        agg_state.set(delta->order_id, is_ask, delta->price, delta->qty);
    }
    
    if (delta->tick_type == 'C' && agg_state.is_active()) {
        // Expand to 3 OutputRecords: C (primary), S, N (extra)
        
        // S tick: cancelled order's perspective (uses rec's side)
        OutputRecord rec_s = rec;
        rec_s.event.tick_type = 'S';
        extra_records->push_back(rec_s);
        
        // N tick: aggressor rests (uses tracked agg_state)
        OutputRecord rec_n = rec;
        rec_n.event.tick_type = 'N';
        rec_n.is_ask = agg_state.aggressor_is_ask;
        rec_n.event.price = agg_state.aggressor_price;
        rec_n.event.qty = agg_state.aggressor_remaining;
        extra_records->push_back(rec_n);
        
        agg_state.clear();
        num_records = 3;
    }
    
    return num_records;
}
```

**Runner Integration**:
```cpp
std::vector<OutputRecord> extra_records;
int num_records = apply_deltas_to_book(reconstructed, chunks, agg_state, &extra_records);
output_queue.push_back(reconstructed);
for (const auto& extra : extra_records) {
    output_queue.push_back(extra);
}
```

### Validation Strategy

1. **Run E/D fixes first** - many occurrences, easy to validate
2. **Run C/S/N fixes** - fewer occurrences but complex
3. **Multi-day validation** - catch remaining edge cases
4. **Performance check** - ensure multi-output handling doesn't add latency

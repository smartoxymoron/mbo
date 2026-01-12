# MBO Crossing Mechanism - Reference Documentation

## Overview

The MBO (Market-By-Order) system implements **early resolution** of crossing orders:
- When an aggressive order arrives, the book is **immediately updated** to reflect the match
- The updated (uncrossed) book is emitted to the strategy with a "Cross" tick type
- Generated trades are queued internally for validation when the exchange confirms them
- The exchange's official `tradeMsg` arrives later and is reconciled against the queue

**Key Principle**: The strategy **never sees a crossed book**. All emitted OrderBooks are already uncrossed.

---

## Internal State Components

### 1. `m_generated_trades` (Queue)
- **Purpose**: Stores expected trades that haven't been confirmed by the exchange yet
- **Populated**: During `handle_crossing_on_add()` when an aggressive order matches passive levels
- **Cleared**: When matching `tradeMsg` arrives or when reconciled via cancellation

### 2. `m_uncrossed_qty` (Per Level)
- **Purpose**: Tracks the "visible" quantity at each price level after accounting for matches
- **Updated**: Reduced immediately when crossing occurs (before exchange confirmation)
- **Effect**: Subsequent aggressive orders cannot match against already-consumed liquidity

### 3. `m_crossed_buffer` (Per Half-Book)
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

## Event Flows

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

### Flow 2: Crossing with Cancel Before Trade Confirmation

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

### Flow 3: Non-Crossing Operations

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

## State Transition Matrix

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

## Critical Implementation Details

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

## Example: Complete Trace

### Initial State
```
Best Bid: [6220, 150, 1 order]
Best Ask: [6255, 225, 1 order]
m_generated_trades: empty
```

### Event 1: Aggressive Sell Order
```
INPUT: add_order(oid=4299, px=6220, qty=225, side=ask)

INTERNAL:
  - Crossing detected: 6220 <= 6220
  - Match 150 @ 6220
  - m_generated_trades: [{buy=2391, sell=4299, px=6220, qty=150}]
  - m_uncrossed_qty[6220 bid]: 150 → 0
  - Book: Bid[6220] removed, Ask[6220]=75 added

OUTPUT TICK:
  type=newOrderCross, side=ask, px=6220, qty=225, is_exch=0
  Book: Bid[0]=6200, Ask[0]=6220@75
```

### Event 2: Exchange Trade Confirmation
```
INPUT: on_trade(buy=2391, sell=4299, px=6220, qty=150)

INTERNAL:
  - Match against m_generated_trades: FOUND
  - Pop trade from queue
  - m_generated_trades: empty
  - No book changes

OUTPUT TICK:
  type=tradeMsg, side=ask, px=6220, qty=150, is_exch=1
  Book: Bid[0]=6200, Ask[0]=6220@75 (UNCHANGED)
```

### Event 3: Cancel Residual
```
INPUT: cancel_order(oid=4299, side=ask)

INTERNAL:
  - m_generated_trades: empty (no reconciliation needed)
  - Remove order from book
  - Book: Ask[6220]=75 removed

OUTPUT TICK:
  type=cancelOrderMsg, side=ask, px=6220, qty=75, is_exch=1
  Book: Bid[0]=6200, Ask[0]=6225
```

---

## Binary Compatibility Requirements

To implement a binary-compatible MBO:

1. **Book Updates**: Must occur **before** emitting cross ticks
2. **Queue Management**: `m_generated_trades` must be FIFO and match exchange trade messages exactly
3. **Uncrossed Qty**: Must track separately from total qty to prevent double-matching
4. **Tick Sequences**: Must match the documented flows (cross → trade, or cross → cancel-cross → self-trade)
5. **Price/Qty Semantics**: Must follow the documented rules for each tick type
6. **Level Removal**: Levels with `m_uncrossed_qty == 0` must be invisible to subsequent crosses but remain for reconciliation

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


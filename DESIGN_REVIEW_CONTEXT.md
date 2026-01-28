# Design Review Context: MBO Crossing Implementation

## What This Is

A universal Market-By-Order (MBO) book builder. It ingests exchange events (new/modify/cancel/trade) and maintains a 20-level bid/ask order book. The implementation has two roles:

1. **Publisher (MBO)**: Processes exchange events, maintains authoritative order state, emits compact "deltas" describing book changes
2. **Receiver**: Applies deltas to reconstruct book snapshots, handles multi-output expansion for crossing events

These run in a single process currently but will be separated by shared memory in production (publisher writes deltas to SHM, strategy process reads them).

## Architecture

```
Exchange Events → MBO (publisher)
                    ├─ order_map_: {OrderId → {side, price, qty}}     (exchange-authoritative state)
                    ├─ bids_/asks_: PriceLevels (aggregate qty/count per price)
                    └─ DeltaEmitter → DeltaChunks (64-byte cache-aligned)
                                        │
                                        ▼  (simulated SHM)
                              apply_deltas_to_book() (receiver)
                                        │
                                        ▼
                              OutputRecord snapshots → Strategy/Validation
```

## Delta Primitives

Three types, packed into 64-byte chunks:
- **TickInfo** (36B): Event metadata — tick_type, side, price, qty, order_ids, record_idx
- **Update** (12B): Modify existing level — side, index, qty_delta, count_delta (implicit delete when qty→0)
- **Insert** (24B): Add level at index — side, index, shift flag, price, qty, count
- **CrossingComplete** (1B): Signals all speculative crossing confirmed by trades

A fourth type exists for crossing:
- **CrossingComplete** (1B): Signals crossing resolution

## Tick Types

| Type | Char | Meaning |
|------|------|---------|
| newOrderMsg | N | Passive new order (no crossing) |
| modOrderMsg | M | Passive modify (no crossing) |
| cancelOrderMsg | X | Cancel |
| tradeMsg | T | Exchange-confirmed trade |
| newOrderCross | A | Aggressive new order that crossed the book |
| modOrderCross | B | Aggressive modify that crossed the book |
| cxlOrderCross | C | Summary of reversed speculative fills (self-trade cancel) |
| iocOrderCross | D | IOC order trade (aggressor id=0) |
| mktOrderCross | E | Market order trade (aggressor not in book) |
| cxlOrderSelfTrade | S | Self-trade cancel notification |

## The Crossing Model

### Core Idea
When an aggressive order arrives that would cross the spread, we **speculatively consume** passive liquidity immediately so the strategy never sees a crossed book. We track what we consumed and reconcile against incoming trade confirmations.

### Dual State
- `order_map_`: Exchange's authoritative view (unchanged until trade confirms)
- `PriceLevels`: Strategy's view (speculatively uncrossed)

### Speculative Crossing Flow
```
Aggressive order arrives (price crosses best passive)
  1. Emit TickInfo with type 'A' or 'B' (crossing tick)
  2. passive_side.cross(price, qty):
     - Consume levels from best toward aggressor price
     - Track consumption in cross_fills_ vector [{price, qty, count}, ...]
     - Accumulate pending_cross_fill_qty_
  3. Add only RESIDUAL to aggressor's level
  4. Set pending_cross_ state (aggressor info for self-trade detection)
```

### Trade Reconciliation
```
Trade arrives confirming fill:
  1. reconciled = passive.reconcile_cross_fill(fill_qty)
     - Decrements pending_cross_fill_qty_
  2. remaining = fill_qty - reconciled
  3. If remaining > 0: apply to levels normally
  4. If remaining == 0: levels already correct, just update counts
  5. When pending_cross_fill_qty_ reaches 0: crossing complete
```

### Misprediction: Passive Self-Trade Cancel
Exchange cancels a passive order instead of trading (same participant as aggressor):
```
Cancel arrives for passive order during active crossing:
  1. Emit 'C' tick (VWAP of pending fills, total pending qty)
  2. Remove remaining visible portion of cancelled order from level
  3. unreserve_cross_fill(): give consumed qty back to aggressor
  4. Re-cross: passive_side.cross() with the unreserved quantity
  5. Emit 'S' tick (cancelled order's info)
  6. If pending_cross_fill_qty_ == 0: emit CrossingComplete
```

### Misprediction: Aggressor Self-Trade Cancel
Exchange cancels the aggressive order itself:
```
Cancel arrives for the aggressor during active crossing:
  1. Emit 'C' tick (VWAP of pending fills)
  2. passive_side.uncross(): restore ALL unconfirmed speculative consumption
  3. Remove aggressor's residual from its level
  4. Emit 'S' tick (aggressor's info)
  5. Emit CrossingComplete, clear all crossing state
```

### Receiver-Side Expansion
The receiver tracks aggressor state from A/B ticks and expands C ticks into multiple OutputRecords:
```
On 'A'/'B': save aggressor info in PendingAggressorState
On 'C': expand to C + S + N (or C + S for aggressor cancel)
On CrossingComplete: synthesize N/M/X for residual/consumed aggressor
On trades during crossing: update aggressor_remaining
```

## Current State Inventory (the complexity)

### Publisher-side state for crossing:
- `PendingCross` struct (8 fields): aggressor_id, is_ask, price, original_resting_price, original_qty, residual_tick_type, original_affected_lvl, aggressor_on_level
- `cross_fills_` vector: per-level consumption history [{price, qty, count}]
- `pending_cross_fill_qty_`: total unconfirmed speculative consumption
- `pending_cross_fill_count_`: order count across unconfirmed fills

### Receiver-side state:
- `PendingAggressorState` (7 fields): aggressor_id, is_ask, price, original_qty, remaining, original_tick_type, crossing_complete
- `self_trade_cancel_full_qty`, `self_trade_cancel_price`: captured from explicit S tick
- `extra_records` vector: overflow OutputRecords for multi-output cases
- `multi_tick_secondary` ordering logic

## Performance Context

- Target: 30ns/packet at 10M pkt/s peak (~150 CPU cycles)
- Crossing is rare (~2% of new/mod orders)
- Self-trade cancels are very rare (~70 in 1M records)
- Aggressor cancels are even rarer
- 99.9% of trades at level 0
- Single-threaded, isolated core, no preemption

## Validation Status

Both 1M-record datasets (crossing and non-crossing) pass full validation against reference implementation. All 20 bid/ask levels, tick types, order IDs, prices, quantities, and level counts are validated.

## What Feels Wrong (Author's Assessment)

1. **cancel_order() is a 120-line monster** with three deeply nested branches (aggressor cancel, passive self-trade cancel, regular cancel) that each do similar-but-different things
2. **Duplicated finalization patterns**: The "finalize current record, push to extras, reset affected_lvl" pattern appears 3 times in apply_deltas_to_book()
3. **Too much state**: PendingCross has 8 fields; some feel redundant or derivable
4. **Receiver-side complexity**: PendingAggressorState duplicates information that the publisher already knows; the multi_tick_secondary ordering logic is a patch
5. **cross_fills_ lifecycle**: The "skip confirmed from front" logic in pending_cross_vwap() and uncross() is repeated and subtle
6. **The CrossingComplete delta** feels like a band-aid — it exists because the receiver can't determine crossing completion on its own
7. **trade() has crossing-specific logic** (lines 1177-1207) that emits X ticks for fully-consumed modifies — mixing MBO semantics with reference-compatibility concerns
8. **The "would_cross" check** appears 3 times (new_order, modify_order, cancel_order passive check) with slightly different formulations

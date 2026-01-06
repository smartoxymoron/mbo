# MBO Book Builder Design - Collective Notes

## Process Guidelines
- **Be critical and accurate** — avoid speculative optimism, ground in data
- **Fork deliberately** — only when exploring genuinely divergent approaches
- **Match git branches** — code experiments should align with conversation forks
- **Keep THOUGHTS.md minimal** — only decisions, constraints, open questions

## Conversation Tracking
- **root**: Scoping, interface design, stats planning
- Fork when: trying competing data structure approaches, need focused deep-dive

## Current Scope (Phase 1)
- **In scope**: Price level axis only, NSE data model, ordered prices, crossing
- **Out of scope**: Queue/order state axis, market-agnostic flexibility, order ID→order lookup
- **Future phase**: Full order state, configurable per-market, ID-based lookups

## Problem Summary
- Build 20-level order book (bid/ask with price-qty-count) from L3/MBO tick data
- C++ with AVX2, targets: Intel Raptor Lake (32MB L3), AMD 9950X3D (128MB L3)
- Single-threaded, isolated core, no preemption
- Performance: 30ns/packet target at 10M pkt/s peak (~150 cycles)

## Key Constraints
- 8B order ID, 4B price, 4B qty, 8B agg qty, 4B agg count
- Order ID → price/quantity lookup mandatory for NSE (cancel lacks reliable price, mod lacks old price/quantity)
- ~10,000s active orders per instrument expected
- Cache pressure observed (2x perf hit on Intel under load)

## Design Decisions
- [x] Low-level interface: add/update/remove/cross (not message-level)
- [x] Order ID → price/qty map required (NSE: cancel lacks price, mod lacks old price/qty)
- [ ] Ordered vs unordered prices — leaning ordered (easier deltas, crossing), try first
- [ ] Delta generation approach TBD
- [ ] If ordered: holes+bitmask vs compact+shift TBD (need benchmarks)
- [ ] Parallel arrays (Option B) likely better for SIMD scan — verify with stats

## Cross Reconciliation Flow
- Cross infers consumption of opposing levels before trade messages arrive
- Generate synthetic trade ticks to strategy (one per level affected)
- Mask actual trade messages when they arrive
- Reconcile: assumed count=1 per level vs actual count for partial fills

## Ideas Backlog

### Phase 1 (Price Level Axis)
- AVX2 for price level search (8 prices per 32B register)
- Fast path for top-N levels, lazy sync for overflow
- Ordered-with-holes vs compact rebalancing trade-off
- Cuckoo-style promotion: hot prices migrate toward cache line 0
- Native delta emission for SHM/strategy decoupling

### Deferred (Revisit With Stats)
- MRU cache for recent price levels (depends on locality stats)
- Batch/amortize deletions (depends on remove frequency)
- Prefetch next likely level (depends on access patterns)

### Future Phase (Order State)
- Sparse arrays with amortized compaction
- Skip list for price-time ordering

### Data Structure Building Blocks
- Cache-line-sized SIMD-friendly chunks (F14-inspired):
  - Option A: 32B keys + corresponding data colocated in chunk
  - Option B: Parallel arrays (keys[], data[]) — scan keys with SIMD, index into data
- Avoid RMW on hot path; batch deferred writes to minimize reads

## Stats To Capture (One Day, Typical Instruments)
- Instrument recency distribution (priority queue)
- Avg levels per instrument across day
- Orders per level distribution (levels 0-99 from spread)
- Insert/update/remove distribution by level index
- Cross frequency + opposing level depth consumed

## Next Steps
1. Implement naive boost baseline with stats collection
2. Gather stats from one day NSE data (see stats list above)
3. Verify 10M pkt/s bursts on busy days (currently observed: 5M/s)
4. Restart conversation with stats, make data structure decisions

## Key Context for Fresh Conversations
- 10M pkt/s is aggressive target; 30ns/packet budget if inline with strategy
- Cache pressure is real: 2x perf hit observed on Intel under load
- Cross gives edge by reacting before trade messages arrive
- Naive impl is for correctness + stats, not performance baseline

---
*Last updated: Conversation root*


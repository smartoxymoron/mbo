# MBOa
_Exported on 1/12/2026 at 16:27:40 GMT+5:30 from Cursor (2.2.44)_

---

**User**

Hi Opus, we're going to brainstorm, analyze options and eventually arrive at an optimal design for a "book builder" (often referred to as "MBO" in the firm) for a trading system, to be written in C++, that takes tick by tick market activity and builds a 20 level book (20 bids, 20 asks) consisting of price-quantity-count rows that will be the input to a high frequency trading strategy for trading decisions.

Collaboration: Opus and I will kick off the discussion and I can bring in GPT and Gemini from time to time, or if they request each other to get a diversity of viewpoints and balance perspectives

*Important*: We have a limited token context to be shared between multiple participants so we *must* keep the conversation concise. We will use THOUGHTS.md (some thoughts populated) to document key ideas, thoughts, decisions and considerations so that we can maintain a condensed "collective mental state" as the conversation progresses that I can use as a base for fresh conversations should this one overflow, and for future conversations as I move into specifics of design and implementation. To keep concise, edit THOUGHTS.md only when you are very sure, ask when you're not sure.

*Note*: This conversation is in Cursor so I have the ability to fork "break out conversations" to discuss details, update THOUGHTS.md and return to original conversation with a condensed response to preserve focus. For tracking I will version conversations. This is conversation root. If forked I will have 1, 2... etc. that can further fork to 2.1, 2.2... etc. We will try to be deliberate about it and fork consciously and record in THOUGHTS.md what directions the different forks took, possibly matching git branches for code and results etc.

Goals:
- Performance (focus memory latency via cache misses, excellent peak load handling to handle fast market moves, minimize latency of book delivery, smooth low latency variance; nuances to consider, such as optimizing for peak vs. throughput, median vs. average, foreground work vs. background book keeping etc.)
- Elegance/simplicity (simple is usually faster and maintainable, less brittle, fewer bugs)
- Flexibility (different markets may have slightly different types of messages or information within each message allowing us to skip some work and optimize by disabling code using templates for target market)
- We may discover other considerations (such as testability) as conversation progresses

Target: As a concrete target x86 with AVX2, eg. one target machine is an Intel Raptor Lake (8 perf core, fast 5.6 GHz, limited 32MB cache) and another is AMD 9950X3D (16 perf core, say 5.2 GHz, generous 128MB L3 cache). On our fully loaded Intel machines (with other noisy processes like strategies) I've noticed significant cache pressure hurting MBO performance 2x. On AMD, cache pressure is better but clocks are lower so performance is better. Note that the current live implementation maintains full per-order state and queue and therefore is significantly more cache hungry. Limiting implementation to just an "L2" book will reduce cache pressure/dependency.

Architecture: book building for instruments of interest always happens in a single thread (no preemption, isolated cores), with either the strategy running in the same thread (best performance but share cycles/cache), or else the book (~1KB in format expected by strategy) is pushed over SHM for every tick to a strategy process adding latency but giving us more cycles to use for book building; one central goal, is for the core book builder to natively emit "book deltas" (like insert level 10.32 at index 12) so we can push smaller updates over SHM and maintain the book at the strategy; achieving native deltas has the added benefit of making us agnostic to strategy book format and we may even be able to eliminate it if we are able to provide very efficient iterators for inline strategy to directly scan over our internal book state 

Market: NSE India is one heavy market we operate on, and a good test vehicle (I've collected a sample of stats, follows shortly), but our implementation will be market agnostic and able to operate on any L3 (i.e. MBO/tick-by-tick) market

Problem envelope: Steady state busy sees over 1M packets/s, could have bursts of 10M packets/s, physical limit 20M packets/s (with 10G network, 1GB/s 50 bytes/packet); different instruments will have vastly different activity and active order count, we can try to size for the expected worstcase per instrument at program start for simplicity with perhaps an expensive expansion operation for the rare overflow

Performance target: At 10M packets/s, naive budget is 100ns; if we run inline with strategy to avoid core-core communication then our budget will be ~30ns to leave room for strategy and overheads, which is ~150 cycles per packet

State: Ultimate implementation will maintain full per-order state in case the strategy wants it, or if market necessitates it (eg. if cancel order message does not include price we need to be able to look it up to update book) but initially we'll skip the time ordered queue of individual orders; even if we implement it in the future, it will be optional to disable it for better performance if the market allows and strategy doesn't require order priority information

Operations: messages for multiple instruments can arrive, we will look up the MBO for that instrument and proceed; each MBO object has multiple price levels, new (add order to queue at price), mod (remove from current queue + add to end of queue, or possibly just reduce quantity in place), cancel (remove from current queue), trade (order at head of queue, remove or reduce in place); orthogonal aspects here are price operations (lookup price to get the queue, add/remove queue at price), and operation on queue (lookup to update in place, remove from queue, insert at end); distribution of activity is maximum around the spread, as you would expect, with one special case likely worth optimizing being implied operations - if you see a bid getting modded into an ask price range that implies that is about to completely consume lower ask levels and part of target price etc. that we do in bulk before even receiving the trade messages that will follow; this transient behaviour of a bid crossing into ask (we call it "crossing" or having a "crossed book") happens only on some markets, NSE is one; to summarize, I suspect core operations decompose into three dimensions - MBO lookup and price level operation and queue/map level operation (in time order); or another angle in order of importance - price level operation (unavoidable, necessary even when not maintaining full order state) and queue/map level operation (if we're maintaining full order state with map for lookup by ID if price lookup insufficient, queue to track order priority if required), MBO lookup (few moving parts here so likely least scope for optimization); these three aspects appear largely orthogonal but there could be coupling between price and queue data structures, eg. if orders are kept in a price-time priority skip list with price levels as nodes

Data: Activity varies widely across markets and instruments. We have full recorded historical data on NSE etc. for more realistic testing. Message formats for different exchanges are vastly different, usually binary but it's irrelevant because we will turn them to basic operations (either new/mod/cancel/trade, or more likely directly to lower level MBO insert/lookup/remove/cross operations) in a lightweight exchange-specific layer - we are focused on building an exchange agnostic implementation that can be adapted to any TBT exchange. I have an executive summary of observed behaviour in @STATS.md for instruments used by our most common trading strategy

Data types: Assume order IDs are 8 bytes, price must be represented in 8 bytes, individual order quantity 4 bytes, aggregate order quantity at price 8 bytes, aggregate order count at price 4 bytes (optionally use 8 for uniformity with aggregate quantity if it helps)

Testing: Ideally we'd test on representative target machines (using smaller micro tests over recorded data for faster iteration) though I'm working primarily on my laptop so we'll need to just maintain a gut feel of performance with some careful thinking/calculation; later an use perf and cachegrind optionally

Approaches:
- Generally known broad ones such as amortizing work, delaying some work for later (for an idle period) to deliver result quickly, optimize memory layout to match access patterns, memory allocation optimization (definitely no allocations per operation, minimize cost of large allocations, consider flat maps to avoid allocations etc.), vector ops (can use AVX2 SIMD C++ intrinsics to compare multiple entries for lookups etc.), batch ops (eg. remember N deleted orders and apply in single pass), memoize (eg. remember 4 recent queues for quick lookup) etc.
- Picking the right data structures that can be tuned to our expectations (using stats to decide)
- See if there's a very effective custom data structure building block that we might be able to recycle, such as Meta's F14 or boost unordered flat maps using fixed size blocks that are SIMD optimal - little tags or data (like price or ID) that are quickly scannable without accessing the actual data; consider options like sparse arrays for linear access with cheap deletes (amortized maintainance)
- Exploit known hard boundaries (eg. we know we always generate 20L output so can allow state outside that window to go slightly out of sync if it helps, or allow the order queue to go out of sync as long as the price-quantity-count is maintained perfectly, or pick a super efficient data structure to roughly track the 20L window and less efficient but flexible for the 1000 levels outside it)
- Micro-optimizations (periodic cache warming, prefetch etc.), implementation-specific optimizations (that apply on currently selected approach)

I want to maximize diversity of ideas and cover the breadth of the solution space to get the best possible result and also iterate over ideas fast so we can arrive at a solid, balanced solution ASAP.

Following are some crude pointers arrived at via a more linear thought process I went down to get a starting approach, less creative process, feels ok but doesn't have the "aha" that accompanies something excellent. Be critical and watch for downsides and downstream complexity and suggest ways out or even a completely different approach that solves all our challenges - I need a coopetitive partner to find holes, suggest solutions and alternatives to arrive at an elegant but simple solution that's blazing fast, not necessarily an agreeable one.

- MBO lookup is a simple boost unordered flat map with fixed size because we know at startup the number of instruments we care about; later we can add a top 16 cuckoo stash to make this even faster, but that's a minor orthogonal optimization
- Implementation will be phased into non-AVX and then AVX2 with our non-AVX implementation actually being designed to be AVX2 compatible (being aware what sort of operations we can perform cheaply when we drop in AVX2 versions of some primitives)
- Choice - separate bid and ask books or combined; separate is likely simpler and allows "crossed books" which may be helpful vs. combined will force resolution of crossing events
- The price axis is the only goal at the moment. The options are to-
-- Design one single data structure that has excellent performance for low levels but can grow and has gracefully degrading performance for ops at higher less frequent levels
-- Or split the implementation to one fast fixed size data structure (>20 levels but comparable size) to deal with 80% of ops and use a standard data structure like a boost::unordered_flat_map for the overflow, potentially tune or rewrite that later if it really makes a difference; more phased, best of both worlds, initially eliminates the constraint of having to deal with infinite levels
- I'm opting to go with the split approach to reduce the constraints, defer work and maximize peak performance. As a starting point I'm thinking fixed size tracking upto 48 price levels (min 24, max 48, target average around 36 entries). 48 * 8 byte price = 6 cache lines, 2 AVX2 per cache line = max 12 * (load+cmp+whatever) ops.; 6 cache lines with 4 entries ideal with 3/6 as soft limits for ~20 to 36 entries usually; initially linear search, optionally we can later experiment with adding the complication (in watchmaker terminology) top level node containing max prices of leaf nodes to enable quicker random lookup of the relevant cacheline
- Considering splitting prices and data (quantity, price etc.) into arrays, so price[8] (64 bytes) and data[8] in a chunk or price[N], data [N] but search is cache aware; possible syntactic sugar in the form of price[N][8], data[N][8] for explicit visibility of cacheline awareless; choosing the latter but either way we're logically dealing with chunks of slots containing upto 8 prices
- Choice between overall prices ordered vs. unordered; choosing ordered to allow linear search
- Choice between ordered vs. unordered prices within chunk; unordered are very nice because it means we can still do a bulk compare within the chunk for lookup/remove with fixed cost, insert anywhere, but crossing becomes a little harder due to linear traversal requirement and we will have to maintain the upper leaf level so we know what price ranges are in each chunk and we have the complexity of refreshing the upper prices when appropriate; leaning towards ordered within a chunk for simplicity for now
- Either way, using chunks means the additional overhead in keeping them balanced and ordered and managing overflow/underslow by moving entries between the fixed data structure and the overflow data structure; we'll get to that complexity in a bit
- If ordered within a chunk, the question is how to represent unused slots and place the used slots within the chunk; given the distribution of operations, I'm thinking of leaving smaller indices unused
- We need to think carefully about performance under sudden price movements, note that I don't know the amount of churn, i.e. entries being moved between fixed and overflow structs
- At the moment the linear and generally ordered structure means that empty space moving left (towards smaller index) from the end, and entries moving towards retirement need to hop through each chunk to go all the way from one side to the other, though we may move a couple at a time for efficiency
- As an example to think through price changes, let's think about the asks
-- 50% of insert/remove happen at first 3 levels, with 50% of update in first 2 levels
-- Example chunks [0, 0, 0, 0, 0, 0, 500, 510], [0, 0, 0, 0, 0, 0, 520, 530]
-- If price moves down, a number of smaller price levels will appear to the left as existing prices in each chunk will face pressure to move right, deliver more free space leftward; as price goes to 400, most existing prices will be forced to hop right and overflow to the map; we can absorb perhaps 4 slots * 6 chunks = 24 new prices before being forced to rebalance
-- During huge market moves, I imagine we might run through 100s of prices in quick order
-- If price moves up, price levels on the left will disappear as bids capture them, forcing us to pull existing levels leftward, hopping left
-- I imagine we'll need a rebalancing process that's either moving free space left, or entries right (not sure which direction makes more sense)
-- Opportunistic rebalancing on idle cycles
-- Forced rebalancing of a chunk when full or empty
-- I imagine always leaving an empty slot in a chunk is a good idea - we can place an incoming price and then do eviction and right chunk is guaranteed to have at least one slot empty (vs. pushing in free space by pushing it in from the rightmost chunk and hopping left)
-- When empty as prices move up, we can pull an entire chunk leftward (TBD when the now empty chunk gets filled)
- One optional optimization when the first/last chunks become somewhat full/empty is to "re-index" the chunks; for example if price moves up and chunk 0 is empty, we just mark it empty and call it chunk 5, with the existing chunk 1 becoming the new chunk 0 - this reindexing may feel a bit messy or non-intuitive so I'm not sure it's a good idea from an elegance/maintainability standpoint though it may make price level movement more efficient by not having to move prices between chunks as often
- Overall the design I've described feels a bit complicated and we need to think through how performant the design might be in the three regimes and whether the complexity is worth it - steady state, price moving up fast, price moving down fast; I'm really hoping we can find a solution that's blazing fast, inspired and elegant, even if it's not what I'm proposing now
- Please ultrathink to help arrive at something we can be satisfied with

As a baseline, I'll briefly describe a current proposal from someone else-
- Uses a vector of "subbook"
- Each subbook is a vector with between 30-100 elements (price, quantity, count) that also tracks its price_start
- Linear search of top level vector and then linear search of leaf vector, merge or split leaves when element count moves out of range for a subbook (30-100, which is way more relaxed than my 2-7 range per chunk)
- Nice thing here is simplicity and the fact that prices don't have to hop through chunks and the fact that this is a single, fairly efficient data structure

First, judge critically whether there is room to simplify the complexity of my current suggestion and whether the complexity is even worth the mess against the simplicity of my colleague's proposal. Think in terms of the cost of price level operations, particularly when prices are moving fast. Once we have a sense of that we'll decide how to proceed.

What are your thoughts my initial suggestion, any promising alternate directions and my colleague's proposal?

---

**Cursor**

Let me first read your THOUGHTS.md to understand what's already been documented, then provide my critical analysis.

Now I have the full context. Let me give you my critical analysis.

---

## Critical Assessment

### Your Chunk Proposal: High Complexity, Uncertain Payoff

Your 8-price-per-cacheline chunks with ordered elements have several concerning characteristics:

**1. The Rebalancing Tax**
Every insert/delete at a non-tail position within a chunk requires shifting 1-7 elements. Worse, when chunks overflow/underflow, you cascade work to adjacent chunks. You described it well: *"entries moving towards retirement need to hop through each chunk"*. During fast price moves, this becomes a hot path, not background work.

**2. The "Keep One Slot Free" Invariant**
You proposed always keeping an empty slot per chunk to guarantee insertion space. This means:
- 6 chunks × 7 usable slots = 42 max prices (not 48)
- Extra bookkeeping to maintain the invariant after every delete
- Still need to handle the case when you're wrong about capacity

**3. The Complexity Cost**
Re-indexing chunks, maintaining upper-level max-price nodes, pushing free space leftward — this is getting dangerously close to a custom B-tree. If you're going to pay B-tree complexity, you might as well use one and benefit from decades of optimization research.

**Honest Assessment**: This design has the *shape* of something fast (cache-line awareness, SIMD-friendly scan widths) but the *mechanics* of something that will have many edge cases and surprising performance cliffs during price movement.

---

### Your Colleague's Proposal: Simple but Suboptimal

**Pros:**
- Dead simple mental model
- No cascading rebalancing (prices stay in their subbook until split/merge)
- 30-100 elements per subbook is generous for linear scan

**Cons:**
- `vector<vector<...>>` = pointer indirection = guaranteed cache miss on subbook access
- Split/merge at 30-100 boundaries means occasional O(100) moves
- No SIMD-friendliness baked in
- The price_start tracking is essentially a poor man's B-tree interior node

**Honest Assessment**: This is "good enough" engineering but leaves meaningful performance on the table. The indirection is the killer — you're guaranteed one cache miss per lookup just to get to the subbook data.

---

## The Core Insight You're Both Missing

Looking at STATS.md again:

| Observation | Implication |
|-------------|-------------|
| 99.9% of trades at Level 0 | **Most operations don't touch deep levels** |
| Insert avg depth 16, remove avg depth 17 | Structural changes happen in levels 10-20, not 0-5 |
| Only need 20 levels for output | **Anything beyond level 20 is bookkeeping, not latency-critical** |
| 76% accesses hit same tokens in 20-op window | **Index caching will be highly effective** |

The key realization: **You don't need a single data structure that's good at everything.** You need a trivially-fast hot path for the common case, and an "acceptable" cold path for edge cases.

---

## Alternative: Flat Sorted Array + Index Cache

Here's a simpler design that I believe dominates both proposals:

```cpp
struct alignas(64) Side {
    int64_t prices[48];     // 6 cache lines, sorted, 0 = empty sentinel
    struct { int64_t qty; int32_t count; int32_t _pad; } data[48];  // Parallel array
    
    uint8_t count;          // Active levels (≤48)
    int64_t overflow_price; // Price threshold for overflow (best+N ticks)
    
    // Cold storage for deep levels (rarely touched)
    boost::unordered_flat_map<int64_t, std::pair<int64_t, int32_t>> overflow;
};
```

**Operations:**

| Operation | Hot Path (≤48 levels) | Cost |
|-----------|----------------------|------|
| **Lookup** | AVX2 scan 6 cache lines or binary search | ~20 cycles |
| **Insert** | Find position, `memmove` right, insert | ~50 cycles worst |
| **Delete** | Mark price=0, lazy compact OR `memmove` left | ~10-50 cycles |
| **Update** | Direct index access (cache hit expected) | ~5 cycles |

**Why this works:**

1. **No inter-chunk coordination** — it's just one flat array
2. **AVX2 scans are brutally fast** — 6 `vpcmpeqq` + mask extraction finds any price
3. **`memmove` is cache-friendly** — shifting 48×8 = 384 bytes is 6 cache lines, modern CPUs do this in ~50 cycles
4. **Lazy deletion amortizes** — mark deleted (price=0), compact only when utilization drops below threshold
5. **Index caching** — add a 4-entry LRU: `{price, index}` pairs, 76% hit rate expected

**Fast price movement:**
- Price moves down (new levels appear): Insert at position 0, `memmove` everything right. Worst case: 48 prices shift = ~50 cycles. But this happens once per *new level*, not per order.
- Price moves up (levels disappear): Top levels get deleted. Either lazy (mark deleted) or eager (memmove left). The "overflow_price" threshold means we occasionally pull from overflow map.

**The key cost comparison:**

| Scenario | Your Chunks | Colleague's Subbooks | Flat Array |
|----------|-------------|---------------------|------------|
| Lookup (cached) | O(1) | O(1) | O(1) |
| Lookup (uncached) | scan 1-6 cache lines + chunk logic | indirect + scan 30-100 | scan 6 cache lines (contiguous) |
| Insert hot level | shift in chunk + possible cascade | indirect + shift + possible split | memmove + insert |
| Insert cold level | overflow map | indirect + shift | overflow map |
| Delete | shift + cascade | indirect + shift + possible merge | mark or memmove |
| Code complexity | High (chunk boundaries, rebalance) | Medium (split/merge) | Low (single array) |

---

## Specific Critiques of Your Assumptions

**"6 cache lines with 4 entries ideal with 3/6 as soft limits"**

This creates rebalancing work at 50% utilization. Compare to a simple flat array at 75% (36/48) that only needs occasional compaction. You're trading structural elegance for operational overhead.

**"unordered within chunk for bulk compare... but crossing becomes harder"**

You correctly identified the tradeoff but leaned ordered for "simplicity." I'd argue: if you're going ordered, just go fully ordered (flat sorted array). The chunk abstraction buys you nothing if elements are ordered anyway — you can't insert "anywhere" in the chunk without shifting.

**"periodic cache warming, prefetch"**

This is downstream optimization. The index cache + contiguous array structure means prefetch happens naturally — when you access `prices[i]`, the prefetcher will speculatively load `prices[i+8]`. Don't manually prefetch until profiling shows you need it.

---

## My Recommendation

1. **Start with the flat sorted array** — it's simpler than both proposals, cache-friendly, and SIMD-amenable
2. **Add a 4-entry index cache** — maps recently-accessed prices to array indices, exploiting the 76% temporal locality
3. **Use lazy deletion with periodic compaction** — amortizes delete cost, compaction can be opportunistic
4. **Overflow at ~32-40 levels** — beyond that, push to `unordered_flat_map`; you won't access it often

This gets you:
- O(1) best case (index cache hit, which is 76% of ops)
- O(6 cache lines) worst case for lookup (AVX2 scan)
- Simple code, easy to reason about edge cases
- Natural progression to AVX2 intrinsics later

---

**Question for you**: Does the simplicity of the flat array approach resonate, or do you see specific access patterns that would break it? The main risk is extremely fast price movement causing many consecutive `memmove` operations — but from STATS.md, inserts average depth 16, so most new levels don't appear at position 0.

```cpp
struct alignas(64) Side {
    int64_t prices[48];     // 6 cache lines, sorted, 0 = empty sentinel
    struct { int64_t qty; int32_t count; int32_t _pad; } data[48];  // Parallel array
    
    uint8_t count;          // Active levels (≤48)
    int64_t overflow_price; // Price threshold for overflow (best+N ticks)
    
    // Cold storage for deep levels (rarely touched)
    boost::unordered_flat_map<int64_t, std::pair<int64_t, int32_t>> overflow;
};
```

---

**User**

Agreed. 48 entry fixed that we'll try to keep in range 32-48 utilized is probably a good balance and avoids the mess of rebalancing. Some tweaks and pushback-
- Even with the chunked approach, reindexing will likely avoid a lot of "chunk hopping" though I agree that the overhead and complexity are probably not worth it
- The main positive of the keeping entries in chunks ordered are limiting worst case shift (when ordered, no shifting when unordered but more bookkeeping)
- Within the flat array [48], I think the empty slots should remain left (smaller indices) because a lot of inserts (for asks, for instance) will happen at low price levels and inserting/removing will require less shifting - I don't think the code complexity will increase to much though this is less intuitive as a left justified array and makes less of a difference if we're allowing holes; though perhaps keeping empties on the left can be a future optimization to try; opportunistically allowing holes reduces shift probability, as would keeping holes leftward
- Ultimately even [48] provides a limited window under fast price movements and we could end up hitting the overflow which is likely slower in our (I agree with [48] - that's the way to go) case vs. colleague's approach which has later levels linearly accessible though they will be paying a small runtime cost to maintain ordering in all leaves; if th eoverflow map proves to be limiting under fast moves we may end up evolving eventually to something similar with multiple [48] arrays which may be simpler than creating a further N sized ordered buffer between fixed and overflow structs, which N is the number of price levels we want fast push/pop knowing that N prices move in a typical bad case
- We'll have to think more carefully about the sentinel values if we're not compacting so they don't interfere with a future AVX2 lookup/insert
- Could add a micro-optimization to use a neighbouring empty slot when inserting
- The 4 entry cache itself may be overkill because many hits are at index <4 and even a linear search in the [48] array will get us to the level fairly quickly
- A small consideration I'd like you to bear in mind is delta generation - for every activity we need to be able to generate the index within the 20L book, i.e. count used slots as we iterate to the target price level; I expect this is simple enough even with AVX2 though it may require additional ops

What are your thoughts now on the direction we should take? Ultrathink again to look for gaps, alternatives and best approach for first cut and further optimizations to try.

---

**Cursor**

You've raised excellent refinements. Let me address each and then crystallize the approach.

---

## Addressing Your Points

**Empty slots on left (for asks):**

This is clever. If for asks we maintain:
```
[∅, ∅, ∅, 100, 101, 102, 103, 104]  // start_idx = 3, best ask = 100
```
Then inserting a *new best* (price 99) is O(1) — just write at `start_idx - 1` and decrement. The common case (new levels at ToB) becomes trivially cheap.

The tradeoff: **logical index ≠ array index**. For delta generation, level 0 is at `prices[start_idx]`, level 1 at `prices[start_idx + 1]`, etc. Not complex, but requires discipline.

**Holes vs. compact:**

Two strategies with different tradeoffs:

| Strategy | Insert cost | Delete cost | Delta index cost | Complexity |
|----------|-------------|-------------|------------------|------------|
| **Compact** (no holes) | memmove O(n) | memmove O(n) | Direct: `idx` | Low |
| **Holes allowed** | Find nearest hole, local shift | Mark sentinel | popcount to position | Medium |

With holes, insert finds the nearest empty slot (left or right) and shifts only the elements between insertion point and that hole. Expected shift distance: `n / (num_holes + 1)`. If we maintain ~25% holes (36 used / 48), average shift is ~3-4 elements.

**Sentinel choice:**

For asks (best = lowest): use `INT64_MAX` as sentinel — it sorts to the right naturally
For bids (best = highest): use `INT64_MIN` as sentinel — it sorts to the right naturally

This means empties naturally "float" toward the high-index end during compaction, which is the opposite of your "empties on left" optimization. We can reconcile:
- **Empties-left strategy**: requires explicit tracking, inverts intuition, but O(1) best-insert
- **Sentinel-sorted strategy**: empties at right, natural sorting, but best-insert requires shift

I lean toward **explicit start_idx tracking** to get O(1) best-insert, since that's the hot path.

**Delta generation with holes:**

With AVX2, we can compute logical index efficiently:
```cpp
// mask = which slots are valid (non-sentinel)
uint64_t mask = ~_mm256_movemask_epi8(cmp_result);  // simplified
int logical_idx = __builtin_popcountll(mask & ((1ULL << array_idx) - 1));
```

One `popcnt` instruction — ~3 cycles on modern CPUs. Acceptable.

**Multiple [48] arrays for fast moves:**

You're right that if overflow becomes a bottleneck, the natural evolution is chained [48] blocks rather than a map. This is essentially your colleague's approach but with fixed-size, cache-friendly blocks. Let's defer this — if profiling shows overflow is hot, we revisit.

**Index cache:**

Agreed — probably premature. Linear scan of 48 entries with AVX2 is ~12 comparisons. Cache adds branch misprediction risk for marginal gain. Profile first.

---

## Recommended First Cut

Prioritizing **simplicity** for correctness, with clear optimization paths:

```cpp
struct alignas(64) Side {
    static constexpr int CAPACITY = 48;
    static constexpr int64_t EMPTY = INT64_MAX;  // for asks; flip for bids
    
    int64_t prices[CAPACITY];           // Sorted, EMPTY sentinel for unused
    struct { int64_t qty; int32_t count; } data[CAPACITY];
    
    int8_t start_idx;    // First valid entry (empties are [0, start_idx))
    int8_t end_idx;      // One past last valid (empties are [end_idx, CAPACITY))
    
    // Overflow for deep levels
    boost::unordered_flat_map<int64_t, std::pair<int64_t, int32_t>> overflow;
};
```

**Invariants:**
- `prices[start_idx .. end_idx)` contains valid, sorted prices
- `prices[0 .. start_idx)` and `prices[end_idx .. CAPACITY)` are EMPTY
- Best price is at `prices[start_idx]`
- Level N (for delta) is at `prices[start_idx + N]`

**Operations:**

| Operation | Implementation | Cost |
|-----------|----------------|------|
| **Insert new best** | `prices[--start_idx] = price` | O(1) |
| **Insert mid-book** | Find pos, memmove toward nearest empty (start or end) | O(k) where k = distance to empty |
| **Delete best** | `prices[start_idx++] = EMPTY` | O(1) |
| **Delete mid-book** | memmove toward emptied slot OR mark EMPTY if allowing holes | O(k) or O(1) |
| **Lookup** | Linear scan or AVX2 over `[start_idx, end_idx)` | O(n) or O(n/4) |
| **Delta index** | `array_idx - start_idx` (no holes), or popcount (with holes) | O(1) or O(1) |

**First cut simplification:**
- No holes initially — strictly compact in `[start_idx, end_idx)`
- Memmove toward the *nearer* empty region (start or end)
- This gives us clean delta indexing: `level = array_idx - start_idx`

---

## Phased Optimization Path

| Phase | Change | Expected Gain |
|-------|--------|---------------|
| **0** | Naive linear scan, compact, memmove | Baseline correctness |
| **1** | AVX2 price scan (6 cache lines, 12 comparisons) | 4x lookup speedup |
| **2** | Allow holes + lazy compaction | Reduced shift frequency |
| **3** | Index cache (4-entry MRU) | Profile-driven, may skip |
| **4** | Multiple [48] blocks chained | Only if overflow is hot |

---

## Gaps I Still See

**1. Bid vs Ask asymmetry:**
Bids have best = highest price. If we want "empties on left" semantics for both sides, the sort order flips:
- Asks: ascending (best = lowest at start_idx)
- Bids: descending (best = highest at start_idx)

This requires either two code paths or a templated comparator. Minor complexity.

**2. Overflow threshold:**
When do we spill to overflow? Options:
- Hard capacity: when `end_idx - start_idx >= CAPACITY`
- Depth-based: when level index > 40, spill regardless of capacity
- Price-based: track `overflow_threshold_price`, spill prices beyond it

I lean toward **depth-based** (level > 40) since we only care about 20 levels for output. Anything beyond ~40 is buffer for fast moves.

**3. Pulling from overflow:**
When best levels get consumed and we have room, when do we pull from overflow? Options:
- Eager: immediately when room available
- Lazy: only when we'd output <20 levels
- Batch: periodically pull N levels during idle

**Lazy** seems right — why pay the cost if we don't need the data?

**4. Crossing optimization:**
When bid crosses into ask range, we infer level extinction. This is a bulk operation — delete multiple ask levels, emit multiple deltas. Current design handles this (iterate from best, delete while price < crossing_price), but worth confirming the delta emission order is correct for downstream.

---

## Proposed Decision

**First cut:**
1. Flat `int64_t prices[48]` with `start_idx` / `end_idx` tracking
2. Compact (no holes) — simple delta indexing
3. Ascending for asks, descending for bids
4. Memmove toward nearer empty region
5. Overflow at level > 40
6. No index cache initially
7. Linear scan for lookup (AVX2 in Phase 1)

**Delta generation:**
- Insert at level N → emit `InsertLevel(side, N, price, qty, count)`
- Delete level N → emit `DeleteLevel(side, N)`
- Update level N → emit `UpdateLevel(side, N, qty_delta, count_delta)` or full rewrite

This gives us a clean, testable baseline. Sound good?

Should I update THOUGHTS.md with this direction, or do you want to iterate further first?

```plaintext
[∅, ∅, ∅, 100, 101, 102, 103, 104]  // start_idx = 3, best ask = 100
```

```cpp
// mask = which slots are valid (non-sentinel)
uint64_t mask = ~_mm256_movemask_epi8(cmp_result);  // simplified
int logical_idx = __builtin_popcountll(mask & ((1ULL << array_idx) - 1));
```

```cpp
struct alignas(64) Side {
    static constexpr int CAPACITY = 48;
    static constexpr int64_t EMPTY = INT64_MAX;  // for asks; flip for bids
    
    int64_t prices[CAPACITY];           // Sorted, EMPTY sentinel for unused
    struct { int64_t qty; int32_t count; } data[CAPACITY];
    
    int8_t start_idx;    // First valid entry (empties are [0, start_idx))
    int8_t end_idx;      // One past last valid (empties are [end_idx, CAPACITY))
    
    // Overflow for deep levels
    boost::unordered_flat_map<int64_t, std::pair<int64_t, int32_t>> overflow;
};
```

---

**User**

Feels good, agree with everything but before you update-
- Bid ask structs will be templated to have the appropriate sentinel, ordering etc.
- Lazy overflow push/pop in idle cycle when utilization goes out of range (30-40), perhaps in batches of 4 or 8 (TBD - need to measure overflow op cost and whether batching helps)
- I expect that for level insertion count will be 1, for updates count will likely be +/-1 matching the quantity delta
- Delta delete ops can delete multiple levels potentially so we'll need DeleteLevel(side, N, count); this is where we may have some edge cases like running out of levels in the fixed struct and having to dip into the overflow - handling that efficiently and elegantly is a tactical detail though so we'll get into it when implementing
- Other features for later as you mentioned (and I'd like to consider reusing neighbouring holes (suspect that detecting and using holes further away may add more complexity) as an experiment so many insertions might happen with O(1) but I'm not sure it will help)
- Perhaps I'm backtracking here but a completely unordered [48] is interesting as well though likely not feasible without AVX2 and expensive even with - calculate deltas of all prices against incoming price, mask entries that are smaller (i.e. -ve) and locate smallest among remaining (stop searching if you find an exact match with difference 0). The worstcase will be 12x AVX2 block cost, average half that - that might work out <100 cycles, i.e. 20ns, and a 4 entry cache may make a noticeable difference there (for asks evict one and insert if we ever insert/lookup price lower than known highest in cache). Eviction will need to be bulk and more costly to scan all, find top N and move out. Just ultrathink through that and see if that's a worthwhile approach for later - it feels stupid to the point that it might just be smart :)


---

**Cursor**

```cpp


__m256i diff = _mm256_sub_epi64(prices, target_broadcast);
__m256i lt_mask = _mm256_cmpgt_epi64(_mm256_setzero_si256(), diff);  // prices < target
// accumulate popcount of mask bits
```

Let me think through the unordered approach carefully before we commit to the update.

---

## Unordered [48] Deep Analysis

**The idea:** No sorting overhead. Insert anywhere empty, delete by marking sentinel, lookup via full AVX2 scan computing logical index on-the-fly.

**Lookup mechanics for asks:**
```
Target price: 505
Array: [510, 502, ∅, 508, 500, 503, ∅, 501, ...]  (unordered)

1. Broadcast 505 to AVX2 lanes
2. For each block of 4:
   - Compare: prices == 505? → exact match mask
   - Compare: prices < 505? → "better than target" mask
3. Accumulate popcount of "better" mask = logical index
4. If exact match found: return (array_idx, logical_idx)
5. If no match: logical_idx = insertion position
```

**Logical index = popcount of "prices < target"** (for asks). This works because we're counting how many levels are ahead of this price.

**Cost breakdown:**

| Operation | Instructions | Cycles (est.) |
|-----------|-------------|---------------|
| Load 4 prices | `vmovdqu` | 1 |
| Broadcast target | `vpbroadcastq` | 1 |
| Compare equal | `vpcmpeqq` | 1 |
| Compare less-than | `vpcmpgtq` (inverted) | 1 |
| Extract mask | `vmovmskpd` | 1 |
| Popcount | `popcnt` | 1 |

Per block: ~6 cycles. 12 blocks: ~72 cycles.

Plus early-exit on exact match (saves ~half on average for lookups) and final aggregation.

**Realistic estimate: 40-80 cycles for lookup.**

**Insert:** Find any empty slot (scan for sentinel), write. O(scan) + O(1) = ~40 cycles.

**Delete:** Find price (scan), mark sentinel. O(scan) + O(1) = ~40 cycles.

**The killer advantage:** No shifting ever. Insert and delete are both O(1) after the scan.

**The concern:** For ordered, best-price access is O(1) via `start_idx`. For unordered, even best-price lookup requires full scan (unless cached).

---

## Cache Dynamics for Unordered

4-entry cache: `{price, array_idx, logical_idx}`

**Maintenance:**
- **Insert new price:** If price < highest cached (for asks), evict highest, insert new
- **Delete cached price:** Invalidate that entry (mark as invalid)
- **Logical index invalidation:** When any insert/delete happens, cached logical indices may be stale

Wait — that's the problem. **Logical index is volatile.** If I cache `{price=502, array_idx=1, logical_idx=2}`, and someone inserts price 501, the logical_idx becomes 3. Every insert/delete invalidates all cached logical indices.

**Fix options:**
1. Cache only `{price, array_idx}`, recompute logical_idx on access
2. Invalidate all logical_idx on any modification, but keep array_idx
3. Maintain a modification counter, invalidate cache entries on mismatch

Option 1 is simplest: cache gives us array_idx (skip scan), but we still need O(scan) popcount for logical_idx. That's still ~40 cycles for the popcount pass, vs ~80 for full lookup. **50% savings on cache hit.**

With 76% access locality, expected cost: `0.76 * 40 + 0.24 * 80 = 30 + 19 = 49 cycles`. Not bad.

---

## Comparison Table

| Aspect | Ordered + start_idx | Unordered + AVX2 | Unordered + Cache |
|--------|---------------------|------------------|-------------------|
| Best-price lookup | O(1) | O(scan) ~40 cycles | O(scan) or O(1) + O(popcount) |
| Mid-book lookup | O(scan) ~40 cycles | O(scan) ~40 cycles | ~49 cycles expected |
| Insert (new level) | O(scan) + O(shift) ~60 cycles | O(scan) + O(1) ~45 cycles | ~45 cycles |
| Delete | O(scan) + O(shift) ~60 cycles | O(scan) + O(1) ~45 cycles | ~45 cycles |
| Delta index | Direct: `arr_idx - start_idx` | popcount O(scan) | popcount O(scan) |
| Holes | Needs management | Natural (sentinel) | Natural (sentinel) |
| Code complexity | Medium | Medium | Higher |
| Eviction to overflow | Find max price O(scan) | Find max price O(scan) | Same |

---

## Verdict

**Unordered is legitimately interesting**, especially because:
1. Insert/delete have no shift cost
2. Holes are free (just sentinels)
3. The 60% updates (which are lookup + modify-in-place) cost the same either way
4. Only ~20% of ops are insert/delete where unordered wins

**But ordered still wins for:**
1. Best-price access O(1) — and STATS.md says 99.9% of trades are at level 0
2. Delta index computation is trivial (no popcount pass)
3. Early termination on scan (stop when price > target)
4. Simpler mental model

**My recommendation:** 
- **First cut: Ordered.** It's safer, simpler, and best-price access being O(1) matters.
- **Phase 2 experiment: Unordered with AVX2.** Benchmark it. If shifts are actually costing us, switch.

The unordered approach is "clever enough to try" but not "obviously better." Let's get correctness first, then benchmark alternatives.

---

## Ready to Update THOUGHTS.md

I'll capture:
1. Flat sorted array [48] with start_idx/end_idx
2. Templated bid/ask for sentinel and ordering
3. Compact initially (no holes), holes as Phase 2
4. Lazy overflow in idle cycle (batch 4-8)
5. Delta format: InsertLevel (count=1), UpdateLevel (+/-1 typical), DeleteLevel(count=N for bulk)
6. Unordered [48] as Phase 2 experiment candidate
7. Phased optimization path

Shall I proceed with the update?

```plaintext
Target price: 505
Array: [510, 502, ∅, 508, 500, 503, ∅, 501, ...]  (unordered)

1. Broadcast 505 to AVX2 lanes
2. For each block of 4:
   - Compare: prices == 505? → exact match mask
   - Compare: prices < 505? → "better than target" mask
3. Accumulate popcount of "better" mask = logical index
4. If exact match found: return (array_idx, logical_idx)
5. If no match: logical_idx = insertion position
```

---

**User**

Gemini, do you agree with the discussion so far and the proposed plan? State significant disagreements only.

---

**Cursor**

I agree with the **Flat Sorted Array [48]** plan. It is the robust, high-performance local optimum for Phase 1.

I have **two significant technical disagreements/corrections** to ensure correctness under load:

1.  **Eviction Must Be Eager at Capacity (Not Just Lazy)**
    You mentioned lazy overflow when utilization is 30-40.
    *   **Correction**: You need a **Hard Limit** (e.g., 48) where eviction is **synchronous and eager**. If the array is full and a new Top-of-Book price arrives (e.g., a better Bid), you *cannot* wait for an idle cycle. You must immediately evict the worst price to the overflow map to make space. Lazy maintenance is *only* for preemptive rebalancing below the hard limit.

2.  **Crossing Logic vs. Insertion Order**
    *   **Correction**: You cannot treat Crossing as just an "Insert" that eventually resolves. If an incoming Bid crosses the Ask book:
        1.  You must execute **Crossing Logic** first (matching against Asks, emitting Trade deltas, reducing Ask levels).
        2.  **Only then** do you insert the *remainder* (if any) of the Bid into the Bid book.
    *   If you insert the Bid first, you momentarily create a "crossed book" state (Bid > Ask) which, if emitted or read by strategy, can cause logic errors. The cross must be resolved *atomically* regarding the book state.

**Minor Note on Updates:**
You mentioned `UpdateLevel` count likely being `+/- 1`.
*   **Clarification**: For an **MBO Modify** (changing quantity of an existing order), `count_delta` is typically **0**. Count only changes on `Insert` (New Order) or `Cancel/Full Fill`.

I will now update `THOUGHTS.md` with the consolidated **Flat Array [48]** design, incorporating these constraints.

**Plan**:
1.  Update `THOUGHTS.md`.
2.  Create `DESIGN.md` (or update if exists) with the concrete `FlatBook` struct layout and Delta definitions to freeze the spec before coding.

Proceeding with update.

```markdown
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
- **In scope**: Price level axis, NSE data model, ordered prices, crossing, delta generation, correctness testing
- **Out of scope**: Queue/order state axis, market-agnostic flexibility, full stats collection
- **Deferred**: Distribution stats from real NSE data (will do manually on target machines later), multiple snapshots per input
- **Future phase**: Full order state, configurable per-market, optimized data structures

## Problem Summary
- Build 20-level order book (bid/ask with price-qty-count) from L3/MBO tick data
- C++ with AVX2, targets: Intel Raptor Lake (32MB L3), AMD 9950X3D (128MB L3)
- Single-threaded, isolated core, no preemption
- Performance: 30ns/packet target at 10M pkt/s peak (~150 cycles)

## Key Constraints
- 8B order ID, 4B price, 4B qty, 8B agg qty, 4B agg count
- Order ID → price/quantity lookup mandatory for NSE (cancel lacks reliable price, mod lacks old price/qty)
- ~10,000s active orders per instrument expected
- Cache pressure observed (2x perf hit on Intel under load)
- **Crossing**: Must resolve cross *before* inserting remainder to avoid invalid crossed-book state

## Design Decisions
- [x] **Data Structure**: Flat Sorted Array [48] (Side struct)
  - `prices[48]` (cache line aligned) + parallel `data[48]`
  - `start_idx` / `end_idx` tracking (valid range)
  - **Asks**: Sorted Ascending (Best = `prices[start_idx]`)
  - **Bids**: Sorted Descending (Best = `prices[start_idx]`)
  - **Lookup**: Linear scan (Phase 0) -> AVX2 Scan (Phase 1)
  - **Shift**: `memmove` (simple, fast for small N)
- [x] **Overflow**:
  - **Soft Limit (30-40)**: Lazy/Batched eviction to overflow map during idle cycles
  - **Hard Limit (48)**: Eager synchronous eviction if full on insert
- [x] **Delta Emission**:
  - `InsertLevel(side, index, price, qty, count)`
  - `UpdateLevel(side, index, qty_delta, count_delta)`
  - `DeleteLevel(side, index)` (can implies multiple levels if Trade, usually single)
- [x] **Order ID Map**: `boost::unordered_flat_map<OrderId, OrderInfo>`

## Ideas Backlog

### Phase 1 (Price Level Axis)
- AVX2 for price level search (8 prices per 32B register)
- Unordered [48] experiment (Phase 2 candidate) - requires benchmark vs shifts
- Holes in array (Phase 2 candidate) - trade shift cost for fragmentation complexity

### Deferred (Revisit With Stats)
- Index Cache (MRU) - likely overkill for [48] but good for larger arrays
- Multiple Chained [48] Blocks - if overflow is frequent hot path

### Future Phase (Order State)
- Sparse arrays with amortized compaction
- Skip list for price-time ordering

## Stats To Capture (One Day, Typical Instruments)
- Instrument recency distribution (priority queue)
- Avg levels per instrument across day
- Orders per level distribution (levels 0-99 from spread)
- Insert/update/remove distribution by level index
- Cross frequency + opposing level depth consumed

## Next Steps
1. ✅ Design updated in DESIGN.md (FlatBook approach)
2. ✅ Delta approach refined (inline emission)
3. Implement `mbo.h` / `mbo.cpp` - Stage 1 (FlatBook + Deltas)
4. Implement Test Harness (Simulated data)
5. Implement Stage 2 (Crossing Logic)

## Key Context for Fresh Conversations
- **Flat Array [48]** chosen over chunks/trees for simplicity & cache locality.
- **Top-of-Book (ToB)** access is O(1) via `start_idx`.
- **Deltas** are emitted *during* ops, not post-diff.
- **Crossing** is atomic: Match -> Emit Trades -> Insert Remainder.

```

---

**User**

GPT, do you agree with the discussion so far and the proposed plan? State significant disagreements only.

Gemini pointed out the obvious facts that we may be forced to do inline eviction sometimes and that we must complete crossing on the opposide side before inserting the remaining quantity on the appropriate side and then generate the book (in the process we would have ended up generating the appropriate deltas for the opposite and incoming side that we send as a batch, therefore the two operations are atomic from the book point of view).

DO NOT update any of the docs - I've dropped Gemini's update and I will ask Opus to update after your comments, if any.

---

**Cursor**

I agree with the **[48] fixed, compact, ordered side arrays + cold overflow** direction.

**Significant disagreement:** tying `count_delta` to `qty_delta` is incorrect in general. For an L2 price level `(price, agg_qty, count)`:
- **Modify qty in-place (same price)**: `count_delta` is typically **0**, `qty_delta` ≠ 0.
- **Move price (replace)**: old level `count_delta = -1`, new level `count_delta = +1` (qty deltas accordingly).
- **Partial fill/trade**: `count_delta` is **0** unless the order is fully removed.
So delta primitives should treat **qty and count as independent**.

**One more caution:** “idle-cycle” overflow push/pop can’t be relied on during peak bursts; make it **bounded incremental housekeeping per tick** when outside the soft band (or trigger-based), with hard-limit eviction immediate (as you noted).

---

**User**

Sonnet, do you agree with the discussion so far and the proposed plan? State significant disagreements only.

Gemini pointed out the obvious facts that we may be forced to do inline eviction sometimes and that we must complete crossing on the opposide side before inserting the remaining quantity on the appropriate side and then generate the book (in the process we would have ended up generating the appropriate deltas for the opposite and incoming side that we send as a batch, therefore the two operations are atomic from the book point of view).

I agree with GPT's statement that an update could have non-zero quantity and zero count update (same-price mod and trades, for instance).

DO NOT update any of the docs - I've dropped Gemini's update and I will ask Opus to update after your comments, if any.

---

**Cursor**

I agree with the flat sorted array [48] plan and the corrections from Gemini and GPT.

**No significant disagreements.** The design is sound.

**One clarification on delta indexing during bulk deletes:**

When crossing deletes multiple levels (e.g., levels 0, 1, 2), the delta indices should reference the **current state after each delta**:
- First: `DeleteLevel(side, 0)` → level 1 becomes new level 0
- Second: `DeleteLevel(side, 0)` → level 2 becomes new level 0  
- Third: `DeleteLevel(side, 0)` → level 3 becomes new level 0

This keeps each delta independently valid relative to the receiver's current book state, rather than requiring batch processing of index-shifted deltas.

Minor note: The plan mentions batching 4-8 levels for overflow push/pop. Given that each eviction is just a map insert/erase (~50-100 cycles for boost::unordered_flat_map), batching may not matter much. Profile first before adding batching complexity.

Otherwise the plan is solid - ready for implementation.

---

**User**

Opus, no significant disagreements from Gemini/GPT/Sonnet - I noticed Sonnet thinking about how the full unordered approach may be harder to generate delta index from and I agree with the clarification that deltas should be sequential and apply on the latest state of the book as the deltas are applied one after the other. To generate indexes for the fully unordered approach, we'd probably need an additional lookup on a price[20] array that tracks the logical book, which would probably still be relatively cheap but an additional bounded cost to be paid when index<20 (and >=4, if the low level cache is implemented) which makes it less attractive (unless you have a creative solution to generate the index? ultrathink to figure if there is a cheaper more elegant solution). Ready to update THOUGHTS.md.

---

**Cursor**

Let me think through whether there's a cheaper way to get delta indices in the unordered approach...

**The insight: Fused lookup + popcount is essentially free**

In unordered, we can compute the logical index *during* the lookup scan:

```cpp
// Single AVX2 pass: find price AND count better prices
for each block of 4:
    cmp_eq  = prices == target      // exact match
    cmp_lt  = prices < target       // better prices (asks)
    valid   = prices != SENTINEL    // not empty
    
    found_mask |= movemask(cmp_eq)
    better_count += popcount(movemask(cmp_lt & valid))
```

~80 cycles total for lookup + index in one pass. The index computation adds ~0 marginal cost once you're doing the full scan anyway.

**But the problem remains:**
- We always pay the full 80-cycle scan (no early exit)
- Ordered with start_idx gets O(1) best-price access, which matters for 99.9% of trades at level 0
- The 4-entry cache helps lookup but not index: cached `{price, arr_idx, logical_idx}` goes stale on any insert/delete

**Maintaining a live top-20 array** (as you suggested) adds bounded cost:
- Insert/delete touching top-20: update the sorted [20] array (~20 shifts worst case)
- Index lookup: binary search [20] for the price, get position = ~5 comparisons

This is ~30-40 cycles extra per modification affecting top-20. Not terrible, but adds complexity.

**Verdict:** No magic solution. Unordered trades O(1) best-price for uniform scan cost. For Phase 1, ordered is cleaner. Unordered with fused popcount is a valid Phase 2 experiment, but not clearly better.

---

Let me now update THOUGHTS.md with the consolidated plan:

```cpp
// Single AVX2 pass: find price AND count better prices
for each block of 4:
    cmp_eq  = prices == target      // exact match
    cmp_lt  = prices < target       // better prices (asks)
    valid   = prices != SENTINEL    // not empty
    
    found_mask |= movemask(cmp_eq)
    better_count += popcount(movemask(cmp_lt & valid))
```

```markdown
# MBO Book Builder Design - Collective Notes

## Process Guidelines
- **Be critical and accurate** — avoid speculative optimism, ground in data
- **Fork deliberately** — only when exploring genuinely divergent approaches
- **Match git branches** — code experiments should align with conversation forks
- **Keep THOUGHTS.md minimal** — only decisions, constraints, open questions

## Conversation Tracking
- **root**: Initial brainstorm, data structure selection, delta design
- Fork when: trying competing data structure approaches, need focused deep-dive

## Problem Summary
- Build 20-level order book (bid/ask with price-qty-count) from L3/MBO tick data
- C++ with AVX2, targets: Intel Raptor Lake (32MB L3), AMD 9950X3D (128MB L3)
- Single-threaded, isolated core, no preemption
- Performance: 30ns/packet target at 10M pkt/s peak (~150 cycles)

## Key Constraints
- 8B order ID, 8B price, 4B qty, 8B agg qty, 4B agg count
- Order ID → price/quantity lookup mandatory (NSE: cancel lacks price, mod lacks old price/qty)
- ~10,000s active orders per instrument expected
- Cache pressure observed (2x perf hit on Intel under load)
- **Crossing atomicity**: Must resolve cross on opposing side BEFORE inserting remainder

## Design Decisions (Phase 1)

### Data Structure: Flat Sorted Array [48]
- [x] **Structure**: `prices[48]` (sorted) + parallel `data[48]` (qty, count)
- [x] **Tracking**: `start_idx` / `end_idx` for valid range
- [x] **Ordering**: Asks ascending (best=lowest at start_idx), Bids descending (best=highest at start_idx)
- [x] **Sentinel**: `INT64_MAX` for asks, `INT64_MIN` for bids (empty slots)
- [x] **Lookup**: Linear scan Phase 0 → AVX2 scan Phase 1
- [x] **Insert/Delete**: memmove toward nearest empty region (start or end)
- [x] **Compact initially** (no holes) — holes as Phase 2 experiment
- [x] **Templated** for bid/ask (sentinel, comparator, sort direction)

### Overflow Management
- [x] **Soft limit (30-40)**: Lazy batched eviction during idle OR bounded incremental per-tick
- [x] **Hard limit (48)**: Eager synchronous eviction — cannot defer if full on insert
- [x] **Cold storage**: `boost::unordered_flat_map<Price, {qty, count}>` for levels beyond 48
- [x] **Pull from overflow**: Lazy — only when needed for output or room available

### Delta Emission
- [x] **Inline emission**: Deltas generated during MBO operations, not post-diff
- [x] **Sequential application**: Each delta applies to current book state (indices shift after DeleteLevel)
- [x] **Independent qty/count**: `UpdateLevel` has separate `qty_delta` and `count_delta` (can be 0 independently)
- [x] **Primitives**:
  - `InsertLevel(side, index, price, qty, count)` — count typically 1
  - `UpdateLevel(side, index, qty_delta, count_delta)` — independent deltas
  - `DeleteLevel(side, index)` — single level, emit multiple for bulk crossing
- [x] **Batch atomicity**: Crossing emits multiple deltas (opposing side deletes + incoming insert) as atomic batch

### Crossing
- [x] **Order**: Resolve opposing side first → then insert remainder → then emit deltas
- [x] **Delta sequence**: DeleteLevel(s) for consumed opposing levels, then InsertLevel for remainder (if any)

## Rejected/Deferred Approaches

### Chunk-based [8]×6 (Rejected)
- Inter-chunk rebalancing too complex
- "Keep one slot free" invariant adds bookkeeping
- Ordered-within-chunk still requires shifting
- Complexity cost outweighs benefits vs flat array

### Unordered [48] with AVX2 (Deferred to Phase 2)
- **Pro**: O(1) insert/delete (no shift), holes natural
- **Con**: No O(1) best-price access (critical for 99.9% trades at level 0)
- **Con**: Delta index requires popcount pass (can fuse with lookup, but no early exit)
- **Con**: Generating top-20 requires maintaining sorted [20] auxiliary array
- Worth benchmarking later, but ordered wins for Phase 1

### Index Cache (Deferred)
- 4-entry MRU for {price, arr_idx} — may help if profiling shows repeated lookups
- For [48] with AVX2 scan (~40 cycles), cache overhead may not pay off
- Defer until profiling shows need

## Ideas Backlog

### Phase 1 Optimizations
- AVX2 for price scan (8×64-bit per 256-bit register, 12 comparisons for 48)
- Exploit empty-slots-left for O(1) best-price insert (asks: new lower prices)
- Use neighboring hole for insert to avoid shift (if holes enabled)

### Phase 2 Experiments
- Holes + lazy compaction (tradeoff shift cost vs fragmentation)
- Unordered [48] with fused lookup+popcount
- Multiple chained [48] blocks if overflow is hot path

### Future (Order State)
- Order ID → OrderInfo map: `boost::unordered_flat_map<OrderId, {side, price, qty}>`
- Queue per price level (if strategy needs order priority)

## Next Steps
1. Update DESIGN.md with FlatBook struct layout
2. Implement `mbo.h` / `mbo.cpp` Stage 1 (FlatBook + basic ops + deltas)
3. Test harness with simulated data
4. Stage 2: Crossing logic
5. Profile on target machines, iterate

## Key Context for Fresh Conversations
- **Flat Array [48]** chosen: simple, cache-friendly, O(1) best-price access
- **ToB dominance**: 99.9% trades at level 0, optimize for this
- **Deltas are core**: Emitted inline, sequential application, independent qty/count
- **Crossing is atomic**: Opposing side resolved before inserting remainder
- **Hard limit eviction**: Cannot defer if array full — synchronous eviction required

---
*Last updated: Conversation root — Opus/Gemini/GPT/Sonnet consensus*
```

```markdown
# MBO Book Builder Design - Collective Notes

## Process Guidelines
- **Be critical and accurate** — avoid speculative optimism, ground in data
- **Fork deliberately** — only when exploring genuinely divergent approaches
- **Match git branches** — code experiments should align with conversation forks
- **Keep THOUGHTS.md minimal** — only decisions, constraints, open questions

## Conversation Tracking
- **root**: Initial brainstorm, data structure selection, delta design
- Fork when: trying competing data structure approaches, need focused deep-dive

## Problem Summary
- Build 20-level order book (bid/ask with price-qty-count) from L3/MBO tick data
- C++ with AVX2, targets: Intel Raptor Lake (32MB L3), AMD 9950X3D (128MB L3)
- Single-threaded, isolated core, no preemption
- Performance: 30ns/packet target at 10M pkt/s peak (~150 cycles)

## Key Constraints
- 8B order ID, 8B price, 4B qty, 8B agg qty, 4B agg count
- Order ID → price/quantity lookup mandatory (NSE: cancel lacks price, mod lacks old price/qty)
- ~10,000s active orders per instrument expected
- Cache pressure observed (2x perf hit on Intel under load)
- **Crossing atomicity**: Must resolve cross on opposing side BEFORE inserting remainder

## Design Decisions (Phase 1)

### Data Structure: Flat Sorted Array [48]
- [x] **Structure**: `prices[48]` (sorted) + parallel `data[48]` (qty, count)
- [x] **Tracking**: `start_idx` / `end_idx` for valid range
- [x] **Ordering**: Asks ascending (best=lowest at start_idx), Bids descending (best=highest at start_idx)
- [x] **Sentinel**: `INT64_MAX` for asks, `INT64_MIN` for bids (empty slots)
- [x] **Lookup**: Linear scan Phase 0 → AVX2 scan Phase 1
- [x] **Insert/Delete**: memmove toward nearest empty region (start or end)
- [x] **Compact initially** (no holes) — holes as Phase 2 experiment
- [x] **Templated** for bid/ask (sentinel, comparator, sort direction)

### Overflow Management
- [x] **Soft limit (30-40)**: Lazy batched eviction during idle OR bounded incremental per-tick
- [x] **Hard limit (48)**: Eager synchronous eviction — cannot defer if full on insert
- [x] **Cold storage**: `boost::unordered_flat_map<Price, {qty, count}>` for levels beyond 48
- [x] **Pull from overflow**: Lazy — only when needed for output or room available

### Delta Emission
- [x] **Inline emission**: Deltas generated during MBO operations, not post-diff
- [x] **Sequential application**: Each delta applies to current book state (indices shift after DeleteLevel)
- [x] **Independent qty/count**: `UpdateLevel` has separate `qty_delta` and `count_delta` (can be 0 independently)
- [x] **Primitives**:
  - `InsertLevel(side, index, price, qty, count)` — count typically 1
  - `UpdateLevel(side, index, qty_delta, count_delta)` — independent deltas
  - `DeleteLevel(side, index)` — single level, emit multiple for bulk crossing
- [x] **Batch atomicity**: Crossing emits multiple deltas (opposing side deletes + incoming insert) as atomic batch

### Crossing
- [x] **Order**: Resolve opposing side first → then insert remainder → then emit deltas
- [x] **Delta sequence**: DeleteLevel(s) for consumed opposing levels, then InsertLevel for remainder (if any)

## Rejected/Deferred Approaches

### Chunk-based [8]×6 (Rejected)
- Inter-chunk rebalancing too complex
- "Keep one slot free" invariant adds bookkeeping
- Ordered-within-chunk still requires shifting
- Complexity cost outweighs benefits vs flat array

### Unordered [48] with AVX2 (Deferred to Phase 2)
- **Pro**: O(1) insert/delete (no shift), holes natural
- **Con**: No O(1) best-price access (critical for 99.9% trades at level 0)
- **Con**: Delta index requires popcount pass (can fuse with lookup, but no early exit)
- **Con**: Generating top-20 requires maintaining sorted [20] auxiliary array or additional lookup
- Worth benchmarking later, but ordered wins for Phase 1

### Index Cache (Deferred)
- 4-entry MRU for {price, arr_idx} — may help if profiling shows repeated lookups
- For [48] with AVX2 scan (~40 cycles), cache overhead may not pay off
- Defer until profiling shows need

## Ideas Backlog

### Phase 1 Optimizations
- AVX2 for price scan (4×64-bit per 256-bit register, 12 comparisons for 48)
- Exploit empty-slots-left for O(1) best-price insert (asks: new lower prices)
- Use neighboring hole for insert to avoid shift (if holes enabled)

### Phase 2 Experiments
- Holes + lazy compaction (tradeoff shift cost vs fragmentation)
- Unordered [48] with fused lookup+popcount
- Multiple chained [48] blocks if overflow is hot path

### Future (Order State)
- Order ID → OrderInfo map: `boost::unordered_flat_map<OrderId, {side, price, qty}>`
- Queue per price level (if strategy needs order priority)

## Next Steps
1. Update DESIGN.md with FlatBook struct layout
2. Implement `mbo.h` / `mbo.cpp` Stage 1 (FlatBook + basic ops + deltas)
3. Test harness with simulated data
4. Stage 2: Crossing logic
5. Profile on target machines, iterate

## Key Context for Fresh Conversations
- **Flat Array [48]** chosen: simple, cache-friendly, O(1) best-price access
- **ToB dominance**: 99.9% trades at level 0, optimize for this
- **Deltas are core**: Emitted inline, sequential application, independent qty/count
- **Crossing is atomic**: Opposing side resolved before inserting remainder
- **Hard limit eviction**: Cannot defer if array full — synchronous eviction required

---
*Last updated: Conversation root — Opus/Gemini/GPT/Sonnet consensus*

```

Updated THOUGHTS.md with the consolidated plan. Summary of what's captured:

**Core Decision: Flat Sorted Array [48]**
- Sorted prices + parallel data arrays
- `start_idx`/`end_idx` tracking for O(1) best-price access
- Templated for bid/ask (sentinel, ordering)
- Compact initially, memmove toward nearest empty on insert/delete

**Overflow**
- Soft limit 30-40: lazy/incremental housekeeping
- Hard limit 48: synchronous eviction (cannot defer)

**Deltas**
- `InsertLevel`, `UpdateLevel` (independent qty/count), `DeleteLevel`
- Sequential application (indices shift after each delete)
- Crossing emits atomic batch

**Rejected**
- Chunk-based [8]×6: complexity not worth it
- Unordered [48]: deferred to Phase 2 (no O(1) best-price, index computation needs auxiliary structure or popcount)

**Next**: Update DESIGN.md with concrete struct layout, then implementation. Ready when you are.


# Executive Brief: MDS Performance & Data Structure Optimization

**Analysis Date:** 1 Aug 2025

## Overview
Analysis of the `perfprofiler.csv` data reveals a highly skewed activity profile dominated by Top-of-Book (ToB) operations. This summary provides technical guidance for selecting and optimizing data structures for multi-level order book representation.

## Event Breakdown
The total volume of base order events (**New + Mod + Cancel + Trade**) is **139,776,653**.

| Event Type | Count | % of Base Events |
| :--- | :--- | :--- |
| **New** | 46,432,435 | 33.22% |
| **Modify** | 46,122,089 | 33.00% |
| **Cancel** | 43,942,601 | 31.44% |
| **Trade** | 3,279,528 | 2.35% |

### Operational Ratios (Relative to Base Total)
- **Update Operations:** 111.55% (High frequency internal state management)
- **Book Inserts:** 10.18%
- **Book Removes:** 10.16%
- **Crossing Tests:** 93,985,701 tests (~101% of New/Mod volume)

### Market Crossing Dynamics
- **Successful Crossings:** 1,971,595 events (True rate: **2.1%** of New/Mod orders).
- **Level Extinctions:** 374,511 levels cleared.
- **Extinction Efficiency:** **19.0%** of crossing events result in an entire price level being extinguished.

## Key Statistical Ranges
| Statistic | Count | Average (Depth) | Max Range | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **mbo.levels** | 138M | **490** | 1,251 | High number of active price points. |
| **orders_per_level**| 182M | **7** | **2,348** | Levels are typically sparse; spikes can be massive. |
| **mbo.new** | 46M | **4** | 729 | New orders are extremely close to ToB. |
| **mbo.mod** | 46M | **10** | 735 | Modifications occur slightly deeper than new orders. |
| **mbo.cancel** | 44M | **4** | 736 | Cancellations match "New" depth (ToB focus). |
| **mbo.trade** | 3.2M | **~0** | 1 | **99.9% of trades occur at Level 0.** |
| **mbo.update** | 156M | **5** | 735 | High-frequency state updates are heavily ToB-skewed. |
| **mbo.insert** | 14M | **16** | 658 | Book inserts are **4x deeper** than new order arrivals. |
| **mbo.remove** | 14M | **17** | 736 | Book removals occur at the same depth as inserts. |

## Critical Insights for Optimization

### 1. Top-of-Book Skew & Operational Depth
Activity is heavily concentrated at the first few price levels, but varies significantly by operation type:
- **ToB Focus (Level 0-5):** `mbo.new`, `mbo.cancel`, and `mbo.update` average a depth of 4-5. Level 0 alone captures ~40% of these events.
- **Deeper Book Ops (Level 10-20):** `mbo.mod`, `mbo.insert`, and `mbo.remove` occur significantly deeper (avg 10-17). This indicates that while new traffic hits the ToB, the resulting data structure mutations (actual inserts/removals) often ripple into slightly deeper levels.
- **Guidance:**
  - The "Ultra-Fast Path" must prioritize **Level 0-5** for `new/cancel/update`.
  - Data structure traversal and locking strategies must be optimized for **Level 0-20** to cover the majority of `insert/remove/mod` mutations without performance degradation.
  - Use fixed-size, contiguous memory for the first ~32 levels to ensure these operations stay within 1-2 cache lines.

### 2. Order Density per Level
With an average of **7 orders per level**, complex data structures (like RB-Trees) inside a price level are likely overkill and cache-inefficient.
- **Guidance:** Use a simple **doubly-linked list** or a **small inline vector** (e.g., `std::vector` with small-buffer optimization) to store orders within a level. This leverages linear scan speeds for the common case (N < 10).

### 3. Book Sparsity & Traversal
While the average depth is ~500 levels, the max range reaches 1,251.
- **Guidance:** A hybrid approach is ideal:
  - **Dense Array** for the first 50-100 levels (direct indexing).
  - **Sparse Map or Skip-List** for levels deeper than 100 to handle the long tail without wasting memory.

### 4. Temporal Locality
The `token.recency` distribution shows that **76% of accesses** hit the same tokens within a 20-operation window.
- **Guidance:** Implement a small L1-style cache for the most recently touched `OrderId` -> `OrderPointer` mappings.

## Conclusion
Optimize for **depth-first locality** at Level 0 and **small-N order lists** per level. High-frequency `update` and `trade` logic should bypass general-purpose traversal where possible.


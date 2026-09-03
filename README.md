# Lock-Free Order Book in C++

A high-performance limit order book implementation built to learn systems C++, cache optimization, and lock-free data structures. Targets the design patterns used in HFT matching engines.

## What it does

- Matches limit and market orders with price-time (FIFO) priority
- Supports add, cancel, and market order types
- Logs all trades with price, quantity, aggressor ID, and resting ID
- Ingests orders via a lock-free SPSC queue from a separate producer thread

## Architecture

### Order types and request dispatch

Uses `enum class Side` instead of strings for bid/ask — saves heap allocation and allows compiler-enforced exhaustiveness. Uses `std::variant<NewOrderRequest, CancelOrderRequest, MarketOrderRequest>` for request dispatch with `if constexpr` branching — no virtual dispatch, no vtable, no pointer indirection.

### Matching engine

Single-threaded. Implements price-time priority: orders at the same price level are matched FIFO. The matching loop runs before insertion — if an incoming order crosses the opposite side, it matches first and only the remainder (if any) rests in the book.

Market orders use a sentinel price (infinity for bids, zero for asks) to consume all available liquidity. Unmatched remainder is dropped.

### Price level storage — three implementations

**Map-based (`main_map.cpp`) — `std::map<double, vector<Order>>`:**
O(log n) price level lookup. Each tree node is a separate heap allocation — pointer chasing on every traversal causes cache misses. Competitive only when active price levels are few and stable.

**Flat vector (`main.cpp`) — `vector<vector<Order>>` indexed by tick:**
Prices are converted to integer indices using `(price - min_price) / tick_size`. O(1) price level lookup with no pointer chasing — the entire price ladder is contiguous in memory. Uses `best_ask_idx` and `best_bid_idx` pointers to avoid scanning from index 0 on every order.

**Flat vector + sorted active levels (`main_sorted_vec.cpp`):**
Extends the flat vector approach with a sorted `vector<int>` of active price indices. The matching loop iterates only over occupied price levels, skipping empty levels entirely. Dramatically outperforms both other approaches under realistic market conditions with high price level churn.

Lazy deletion is used for matched and cancelled orders — a `cancelled` flag on each `Order` lets the matching loop skip stale entries without restructuring the vector. A per-level active count tracks when a level becomes empty so it can be removed from the active index.

Cancel lookup uses `Order_info` (price and side) and `Order_index` (position within the price level vector) — cancel is not on the hot matching path so O(log n) lookup is acceptable here.

### SPSC queue

A fixed-size circular buffer connecting the producer (network/order generation) thread to the consumer (matching engine) thread. Lock-free — no mutexes.

Key design decisions:

- `head` and `tail` are `std::atomic<size_t>` — prevents stale reads across cores
- `alignas(64)` on both — places them on separate cache lines to prevent false sharing (two cores writing to variables on the same cache line force constant cache invalidation between them even when writing to different variables)
- `memory_order_release` on writes, `memory_order_acquire` on reads — sufficient for SPSC, avoids the full memory fence that `memory_order_seq_cst` would insert. On ARM this is a meaningful performance difference; on x86 it is equivalent but documents intent clearly
- Raw head/tail values for full/empty arithmetic, modulo only for array indexing — allows unbounded increment without overflow issues using `size_t`

## Performance

All benchmarks run with 100k orders, 80% limit / 20% market, two threads (producer + consumer via SPSC queue). Throughput in orders/second.

### Narrow integer prices — 95 to 105, tick size 1.0 (11 active levels)

| Implementation | Throughput |
|---|---|
| Map | 362,000 |
| Flat vector | 166,000 |
| Flat vector + sorted active | 174,000 |

Map wins — 11 tree nodes fit entirely in cache, traversal is essentially free. Vector overhead from sorted active list insertion hurts at this scale.

### Wide integer prices — 1 to 10000, tick size 1.0 (up to 10,000 active levels)

| Implementation | Throughput |
|---|---|
| Map | 43,000 |
| Flat vector | 55,000 |
| Flat vector + sorted active | 94,000 |

Vector wins — tree traversal across 10,000 nodes causes cache misses on every lookup. Vector's O(1) index access dominates. Sorted active levels provide an additional 70% speedup by skipping empty levels.

### Realistic tick prices — 95.0 to 105.0 doubles, tick size 0.01 (1,000 levels, high churn)

| Implementation | Throughput |
|---|---|
| Map | 14,000 |
| Flat vector | 247,000 |
| Flat vector + sorted active | 331,000 |

Vector wins by 20x. High order churn means constant tree insertions and deletions, scattering nodes across the heap. Map also pays floating point key comparison overhead. Vector is completely unaffected by either factor.

### Key insight

Map is competitive only when active price levels are few and stable. As levels increase or churn increases, the map's cache miss penalty compounds. The flat vector with O(1) index lookup scales independently of both. The sorted active index layer removes the remaining bottleneck of iterating empty slots between occupied price levels.

## Known limitations

- Cancel orders are excluded from the multithreaded benchmark — the producer cannot know order IDs assigned by the matching engine without a separate feedback queue
- Tick size and price range are set at the top of each file — a production engine would take these from an instrument definition at startup
- Sorted active level maintenance is O(n) per insert/cancel — fine for typical books, degrades with very large number of active levels.

## How to run

**Requirements:** GCC 16+ with C++17 support. On Windows, install via MSYS2:
```bash
pacman -S mingw-w64-x86_64-gcc
```

**Compile and run each implementation:**

```bash
# Map-based implementation
g++ -std=c++17 -O2 -o main_map main_map.cpp
./main_map

# Flat vector implementation
g++ -std=c++17 -O2 -o main main.cpp
./main

# Flat vector + sorted active levels (best performance)
g++ -std=c++17 -O2 -o main_sorted_vec main_sorted_vec.cpp
./main_sorted_vec
```

Each binary runs the benchmark automatically and prints total time, average latency, and throughput.

To change the benchmark scenario, adjust `price_dist`, `max_price`, and `tick_size` near the top of the respective file.

## What I'd build next

- Output SPSC queue from matching engine to a market data publisher thread
- CPU core pinning for producer and consumer threads to eliminate scheduling jitter
- Replace `Order_info` map with a flat array indexed by order ID for O(1) cancel lookup

# Low-Latency Limit Order Book in C++

A single-instrument limit order book and matching engine, built to study cache-friendly data layout, lock-free queues, and how to measure latency properly. The repo holds four versions of the same engine, each storing price levels a different way. All four run the same order stream through the same benchmark, and all four produce byte-identical trade logs.

## What it does

- Matches limit and market orders with price-time (FIFO) priority, and supports cancel by order ID
- Stores prices as integer tick indices (tick size 0.01), so price comparisons are exact
- Logs every trade as aggressor ID, resting order ID, price and quantity
- Feeds orders from a producer thread to the engine thread through a lock-free SPSC ring buffer
- Times every order individually with the CPU cycle counter and reports p50 / p95 / p99 / p99.9 / max

## Versions

| File | How price levels are stored | Cancel |
|---|---|---|
| `main_bare.cpp` | Flat `vector<vector<Order>>` indexed by tick, scanned level by level | Lazy: order is flagged, stays in the vector |
| `main_map.cpp` | `std::map<double, list<Order*>>` per side, keyed by price | Eager: erased through a stored list iterator |
| `main_sorted_vec.cpp` | Flat vector by tick, plus a sorted vector of the non-empty levels so matching skips empty ones | Lazy |
| `main_pool.cpp` | Preallocated order pool, each level an index-linked FIFO list | O(1) unlink through an order-ID lookup |

### The pool version

This is the fastest one, and the one the rest of this README is about.

- **Order pool.** All orders live in one preallocated `vector<Slots>`. A slot is 16 bytes (`id, quantity, prev, next`), so four fit in a 64-byte cache line. Adding an order never calls `new`.
- **Index-linked levels.** Each price level stores only `head` and `tail` slot numbers, and slots link to each other by index (`-1` means none). Matching walks from `head`, which keeps time priority.
- **Free list.** Empty slots are chained through their `next` field. A new order takes the slot at `free_head`; a filled or cancelled order's slot is pushed back onto it, so slots are reused.
- **O(1) cancel.** `loc[order_id]` gives the slot, tick index and side. Cancel unlinks the slot from its level (four cases: alone, head, tail, middle) and frees it. There are no dead orders left behind, unlike the lazy-deletion versions.
- **Best price at the back.** `activ_bid_idx` is sorted ascending and `activ_ask_idx` descending, so the best level on both sides is `back()`. When the best level empties during matching it is removed with `pop_back()`.

### Request dispatch

Requests are a `std::variant<NewOrderRequest, CancelOrderRequest, MarketOrderRequest>` handled with `std::visit` and `if constexpr`, so there is no virtual dispatch on the hot path. Market orders are run as limit orders at a sentinel price (`max_price + tick_size` for buys, `0` for sells) that crosses every level; any unfilled remainder is dropped.

### SPSC queue

A fixed 1,000-slot ring buffer between the producer and the engine thread, with no locks.

- `head` and `tail` are `std::atomic<size_t>` with `alignas(64)`, so they sit on separate cache lines and the two threads don't invalidate each other's line on every update (false sharing).
- The writer publishes with `memory_order_release` and the reader uses `memory_order_acquire`. That is enough to make the slot's contents visible before the index update, without the full fence of `seq_cst`.
- `head` and `tail` only ever increase; the modulo is applied only when indexing the array.

## Performance

**Setup.** 100,000 pre-generated orders: 60% limit, 10% market, 30% cancel of a random live order. Limit prices are uniform in 95-105 and rounded to the 0.01 tick (1,000 price levels), quantities 1-100, seed 42. The producer pushes the pre-generated orders into the SPSC queue, and the engine thread times each `process_order` call on its own with `__rdtsc`, converted to ns using the run's wall-clock time.

**All four versions, same harness** (median of 9 runs, `g++ -std=c++20 -O3`, Ubuntu VM with 2 vCPUs on a Ryzen 7 7840HS laptop):

| Version | Throughput | p50 | p95 | p99 |
|---|---|---|---|---|
| Flat vector (`main_bare`) | 0.95M orders/s | 280 ns | 7,440 ns | 10,570 ns |
| Sorted vector (`main_sorted_vec`) | 1.37M orders/s | 340 ns | 1,980 ns | 5,400 ns |
| Map (`main_map`) | 2.40M orders/s | 260 ns | 1,120 ns | 1,640 ns |
| **Pool (`main_pool`)** | **9.39M orders/s** | **60 ns** | **190 ns** | **310 ns** |

**Pool on Windows** (median of 13 runs, GCC 16.1 MSYS2, `-O3`, same laptop):

| Setup | Throughput | p50 | p95 | p99 | p99.9 |
|---|---|---|---|---|---|
| Through the SPSC queue | 9.2M orders/s | 70 ns | 160 ns | 260 ns | 551 ns |
| Engine alone, single thread (earlier harness, no queue) | 16.3M orders/s | 50 ns | 90 ns | 180 ns | 380 ns |

### What the numbers show

- **The pool cuts p95 about 10x against the sorted vector** (1,980 ns to 190 ns), and the gap is bigger in the tail than in the median. The slow orders in the vector versions are the ones that hit levels full of cancelled orders.
- **`std::map` beats both vector versions.** With 30% cancels, the lazy-deletion vectors fill up with dead orders that matching has to skip over, while the map erases cancelled orders immediately. The pool keeps the vector's direct indexing and also removes orders immediately, which is why it wins.
- **The flat vector is slowest** because matching scans tick by tick through empty levels and dead orders.
- **End-to-end latency** (push into the queue to processed) is about 100 µs at p50 for the pool. That is queueing, not engine time: the producer pushes as fast as it can, so the 1,000-slot queue is always full and each order waits behind ~1,000 others.

### Measurement notes

- `std::chrono` on Windows ticks every 100 ns, too coarse for a 50 ns operation, so each order is timed with `__rdtsc` instead. On this laptop the cycle counter moves in roughly 10 ns steps, so treat p50 values as ±10 ns.
- Orders are generated before timing starts, so the random number generator is never in the measured path.
- p99.9 and max vary a lot from run to run (OS scheduling, core placement), which is why the tables report medians.

## Correctness

The four versions are tested against each other (differential testing). Each writes its trades to a text file (`main_trades.txt`, `map_trades.txt`, `vec_trades.txt`, `pool_trades.txt`), one line per trade. On seeds 1, 7 and 42, all four files are byte-identical (61,616 trades on seed 42).

Bugs found and fixed along the way:

- **Floating-point price comparison.** `conv_idx_to_price(9999)` returns `99.99000000000001`, so a buy at 99.99 failed `price <= 99.99` and never matched, leaving the book locked (best bid equal to best ask). Matching now compares integer tick indices.
- **Cancel after fill.** Cancelling an order that had already been filled decremented the level's count a second time. Cancel now rejects orders that are already filled or cancelled.
- **Market order sentinel.** The old buy sentinel `999999999` overflowed when converted to a tick index; it is now `max_price + tick_size`.
- **Sell-side matching order.** The sell branch walked bids from the lowest price, hit a non-crossing level and stopped early, leaving the book crossed. It now matches from the best bid.

## How to run

Requires GCC with C++20 (on Windows: MSYS2, `pacman -S mingw-w64-x86_64-gcc`).

```bash
cd main
g++ -std=c++20 -O3 -pthread main_pool.cpp -o pool
./pool
```

The same command works for `main_bare.cpp`, `main_map.cpp` and `main_sorted_vec.cpp`. Each prints throughput, engine latency and end-to-end latency percentiles, and writes its trade file. To check that two versions agree:

```bash
cmp vec_trades.txt pool_trades.txt        # Linux / macOS
fc.exe vec_trades.txt pool_trades.txt     # Windows PowerShell
```

The seed is `mt19937 rng(42)` near `random_request()` in each file.

## Known limitations

- Single instrument, single engine thread; price range and tick size are hard-coded at the top of each file.
- The pool is sized to the number of orders in the run and does not grow.
- Adding a new price level inserts into a sorted vector, which is O(number of active levels).
- The producer runs closed-loop (always full queue), so end-to-end latency measures queueing, not a realistic arrival rate.
- No modify/replace order.

## What I'd build next

- A network front end: TCP gateway thread → SPSC → engine → SPSC → publisher thread, and measure wire-to-wire latency
- An open-loop producer that sends at a fixed rate, for realistic end-to-end latency
- Pin the producer and engine threads to separate cores
- Turn the seed comparison into an automatic fuzz loop over hundreds of seeds
- Modify order



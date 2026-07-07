# lob-engine

A limit order book / matching engine in C++17. Price-time priority, limit and
market orders, O(1) cancels. Single header, no dependencies.

```
make test        # unit + property tests, -O3
make test-asan   # same tests under ASan + UBSan
make bench       # latency percentiles and throughput
```

## Numbers

On an Apple M-series laptop, `clang++ -O3 -march=native`, 2M mixed operations
(70% adds / 20% cancels / 10% aggressive) against a book pre-warmed with 50k
resting orders:

```
throughput        : 6.33 M ops/s
latency p50       : 42 ns
latency p90       : 500 ns
latency p99       : 875 ns
latency p99.9     : 1542 ns
```

The max sample is ~1.15 ms — a rare multi-level sweep or a map rebalance. Percentiles
are the honest way to report this; the mean says nothing useful about a heavy-tailed
distribution.

## How it works

- Price levels live in `std::map<Price, Level>` (bids descending, asks ascending),
  so best price is `begin()`. Level count is small; the log factor doesn't matter
  here. Order-id lookup is the hot large-N path, so that's an `unordered_map`.
- Each level holds an intrusive doubly-linked FIFO of orders. Time priority is the
  physical list order, and cancelling from the middle of a queue is just pointer
  surgery — no search, no allocation.
- Orders come from a fixed pool (`std::vector<Order>` + free list) sized at
  construction. Nothing on the add/cancel/match path touches the allocator.
- Prices are `int64_t` ticks. No floating point anywhere in matching.
- An incoming order sweeps opposite levels while it crosses (fills happen at the
  resting order's price), then rests any remainder. Market orders are
  fill-what-you-can, drop the rest.
- Trades are reported through a `std::function` callback so the engine doesn't own
  any I/O.

## Tests

`tests/test_order_book.cpp` uses a small hand-rolled CHECK macro rather than a
framework (46 checks). Directed cases cover priority order at a level, multi-level
sweeps, partial fills, IOC market orders, and cancels at the head/middle/tail of a
queue. The one that has caught actual bugs is the property test: 200k random
add/cancel operations, then assert conservation — every submitted unit of quantity
is accounted for as traded (twice: maker + taker), still resting, or cancelled.

Both the -O3 and sanitizer builds run the full suite.

## TODO

- cancel/replace (keeping the id, losing time priority)
- flat array of levels keyed by tick offset from mid, to kill the map entirely
- top-of-book change callback

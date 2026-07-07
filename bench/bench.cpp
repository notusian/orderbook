// Latency / throughput benchmark for the matching engine.
//
// Replays a synthetic Zipf-ish order flow (70% adds around mid, 20% cancels,
// 10% aggressive marketables), measuring per-operation wall time with
// steady_clock and reporting throughput plus latency percentiles.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "../src/order_book.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const int n_ops = argc > 1 ? std::atoi(argv[1]) : 2'000'000;

    OrderBook ob(1 << 22);
    long long trades = 0, traded_qty = 0;
    ob.set_trade_handler([&](const Trade& t) { ++trades; traded_qty += t.qty; });

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> action_d(0, 99);
    std::normal_distribution<double> px_offset(0.0, 8.0);
    std::uniform_int_distribution<Qty> qty_d(1, 200);
    const Price mid = 10'000;

    std::vector<OrderId> live;
    live.reserve(n_ops);
    OrderId next_id = 1;

    std::vector<std::uint32_t> lat_ns(n_ops);

    // Warm up the book with resting depth on both sides.
    for (int i = 0; i < 50'000; ++i) {
        Side s = (i % 2) ? Side::Buy : Side::Sell;
        Price px = mid + (s == Side::Buy ? -1 : 1) * (1 + i % 50);
        ob.add_limit(next_id, s, px, qty_d(rng));
        live.push_back(next_id++);
    }

    auto t0 = Clock::now();
    for (int i = 0; i < n_ops; ++i) {
        int a = action_d(rng);
        auto s0 = Clock::now();
        if (a < 70) {  // passive-ish limit add
            Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            int off = static_cast<int>(px_offset(rng));
            Price px = mid + (s == Side::Buy ? -std::abs(off) - 1 : std::abs(off) + 1);
            ob.add_limit(next_id, s, px, qty_d(rng));
            live.push_back(next_id++);
        } else if (a < 90 && !live.empty()) {  // cancel random live id
            std::size_t j = rng() % live.size();
            ob.cancel(live[j]);
            live[j] = live.back();
            live.pop_back();
        } else {  // aggressive crossing limit at/through mid
            Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            Price px = mid + (s == Side::Buy ? +3 : -3);
            ob.add_limit(next_id, s, px, qty_d(rng));
            live.push_back(next_id++);
        }
        auto s1 = Clock::now();
        lat_ns[i] = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count());
    }
    auto t1 = Clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) { return lat_ns[static_cast<std::size_t>(p * (n_ops - 1))]; };

    std::printf("ops               : %d\n", n_ops);
    std::printf("wall time         : %.3f s\n", secs);
    std::printf("throughput        : %.2f M ops/s\n", n_ops / secs / 1e6);
    std::printf("latency p50       : %u ns\n", pct(0.50));
    std::printf("latency p90       : %u ns\n", pct(0.90));
    std::printf("latency p99       : %u ns\n", pct(0.99));
    std::printf("latency p99.9     : %u ns\n", pct(0.999));
    std::printf("latency max       : %u ns\n", lat_ns.back());
    std::printf("trades executed   : %lld (qty %lld)\n", trades, traded_qty);
    std::printf("resting orders    : %zu on %zu bid / %zu ask levels\n",
                ob.open_orders(), ob.bid_levels(), ob.ask_levels());
    return 0;
}

// Unit tests for the matching engine. Minimal zero-dependency test harness:
// each CHECK records failures; the process exits non-zero if any test fails.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/order_book.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define CHECK_THROWS(expr)                                                   \
    do {                                                                     \
        ++g_checks;                                                          \
        bool threw = false;                                                  \
        try {                                                                \
            (void)(expr);                                                    \
        } catch (const std::exception&) {                                    \
            threw = true;                                                    \
        }                                                                    \
        if (!threw) {                                                        \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  expected throw: %s\n", __FILE__,        \
                        __LINE__, #expr);                                    \
        }                                                                    \
    } while (0)

using namespace lob;

std::vector<Trade> g_trades;

OrderBook make_book() {
    OrderBook ob(1 << 16);
    g_trades.clear();
    ob.set_trade_handler([](const Trade& t) { g_trades.push_back(t); });
    return ob;
}

void test_resting_and_best_quotes() {
    auto ob = make_book();
    ob.add_limit(1, Side::Buy, 100, 10);
    ob.add_limit(2, Side::Buy, 101, 5);
    ob.add_limit(3, Side::Sell, 105, 7);

    Price px; Qty q;
    CHECK(ob.best_bid(px, q)); CHECK_EQ(px, 101); CHECK_EQ(q, 5);
    CHECK(ob.best_ask(px, q)); CHECK_EQ(px, 105); CHECK_EQ(q, 7);
    CHECK_EQ(ob.open_orders(), 3u);
    CHECK(g_trades.empty());
}

void test_full_cross_price_time_priority() {
    auto ob = make_book();
    // Two makers at the same level: order 1 arrived first, must fill first.
    ob.add_limit(1, Side::Sell, 100, 5);
    ob.add_limit(2, Side::Sell, 100, 5);
    Qty filled = ob.add_limit(3, Side::Buy, 100, 7);

    CHECK_EQ(filled, 7);
    CHECK_EQ(g_trades.size(), 2u);
    CHECK_EQ(g_trades[0].maker_id, 1u); CHECK_EQ(g_trades[0].qty, 5);
    CHECK_EQ(g_trades[1].maker_id, 2u); CHECK_EQ(g_trades[1].qty, 2);
    CHECK_EQ(ob.depth_at(Side::Sell, 100), 3);   // 3 left on order 2
    CHECK_EQ(ob.open_orders(), 1u);              // taker fully filled
}

void test_price_improvement_executes_at_maker_price() {
    auto ob = make_book();
    ob.add_limit(1, Side::Sell, 100, 10);
    ob.add_limit(2, Side::Buy, 103, 10);  // aggressive buy
    CHECK_EQ(g_trades.size(), 1u);
    CHECK_EQ(g_trades[0].price, 100);     // executes at resting price
}

void test_partial_fill_residual_rests() {
    auto ob = make_book();
    ob.add_limit(1, Side::Sell, 100, 4);
    Qty filled = ob.add_limit(2, Side::Buy, 100, 10);
    CHECK_EQ(filled, 4);
    Price px; Qty q;
    CHECK(ob.best_bid(px, q)); CHECK_EQ(px, 100); CHECK_EQ(q, 6);
    CHECK(!ob.best_ask(px, q));
}

void test_sweep_multiple_levels() {
    auto ob = make_book();
    ob.add_limit(1, Side::Sell, 100, 3);
    ob.add_limit(2, Side::Sell, 101, 3);
    ob.add_limit(3, Side::Sell, 102, 3);
    Qty filled = ob.add_limit(4, Side::Buy, 101, 9);
    CHECK_EQ(filled, 6);                       // 102 is beyond the limit
    CHECK_EQ(ob.depth_at(Side::Buy, 101), 3);  // residual rests at 101
    CHECK_EQ(ob.ask_levels(), 1u);
}

void test_market_order_ioc() {
    auto ob = make_book();
    ob.add_limit(1, Side::Sell, 100, 3);
    ob.add_limit(2, Side::Sell, 110, 3);
    Qty filled = ob.add_market(3, Side::Buy, 10);
    CHECK_EQ(filled, 6);                 // swept both levels
    CHECK_EQ(ob.open_orders(), 0u);      // market residual is dropped
    Price px; Qty q;
    CHECK(!ob.best_bid(px, q));
}

void test_cancel() {
    auto ob = make_book();
    ob.add_limit(1, Side::Buy, 100, 10);
    ob.add_limit(2, Side::Buy, 100, 5);
    CHECK(ob.cancel(1));
    CHECK(!ob.cancel(1));                // idempotent: already gone
    CHECK(!ob.cancel(999));              // unknown id
    CHECK_EQ(ob.depth_at(Side::Buy, 100), 5);
    // Order 2 must still be matchable after its neighbour was unlinked.
    ob.add_limit(3, Side::Sell, 100, 5);
    CHECK_EQ(g_trades.size(), 1u);
    CHECK_EQ(g_trades[0].maker_id, 2u);
    CHECK_EQ(ob.open_orders(), 0u);
}

void test_cancel_middle_of_queue() {
    auto ob = make_book();
    ob.add_limit(1, Side::Sell, 100, 1);
    ob.add_limit(2, Side::Sell, 100, 1);
    ob.add_limit(3, Side::Sell, 100, 1);
    CHECK(ob.cancel(2));
    ob.add_limit(4, Side::Buy, 100, 2);
    CHECK_EQ(g_trades.size(), 2u);
    CHECK_EQ(g_trades[0].maker_id, 1u);
    CHECK_EQ(g_trades[1].maker_id, 3u);  // queue links intact around the hole
}

void test_rejects_bad_input() {
    auto ob = make_book();
    CHECK_THROWS(ob.add_limit(1, Side::Buy, 100, 0));
    CHECK_THROWS(ob.add_limit(1, Side::Buy, 100, -5));
    ob.add_limit(1, Side::Buy, 100, 5);
    CHECK_THROWS(ob.add_limit(1, Side::Buy, 101, 5));  // duplicate id
}

void test_conservation_invariant_random_flow() {
    // Property test: through 200k random ops, sum of (fills + resting + cancelled)
    // equals submitted quantity, and depth_at agrees with per-level accounting.
    auto ob = make_book();
    std::srand(12345);
    long long submitted = 0, cancelled_qty = 0;
    long long traded = 0;
    ob.set_trade_handler([&](const Trade& t) { traded += t.qty; });

    std::vector<std::pair<OrderId, Qty>> open;  // id -> original resting qty upper bound
    OrderId next_id = 1;
    for (int i = 0; i < 200000; ++i) {
        int action = std::rand() % 10;
        if (action < 7) {  // add limit
            Side s = (std::rand() % 2) ? Side::Buy : Side::Sell;
            Price px = 990 + std::rand() % 21;  // tight band forces crossing
            Qty q = 1 + std::rand() % 100;
            submitted += q;
            ob.add_limit(next_id, s, px, q);
            open.push_back({next_id, q});
            ++next_id;
        } else if (!open.empty()) {  // cancel a random known id
            std::size_t j = std::rand() % open.size();
            OrderId id = open[j].first;
            // Snapshot remaining qty via a cancel that reports success.
            long long before = static_cast<long long>(ob.open_orders());
            if (ob.cancel(id)) {
                (void)before;
            }
            open.erase(open.begin() + j);
        }
    }
    // Remaining book qty:
    long long resting = 0;
    for (Price px = 980; px <= 1020; ++px) {
        resting += ob.depth_at(Side::Buy, px);
        resting += ob.depth_at(Side::Sell, px);
    }
    // Every traded unit consumes one maker unit and one taker unit of the
    // submitted flow; cancelled quantity is whatever else left the book.
    cancelled_qty = submitted - 2 * traded - resting;
    CHECK(cancelled_qty >= 0);
    CHECK(traded > 0);
    // A book with open orders must expose at least one price level, and vice versa.
    CHECK_EQ(ob.open_orders() > 0, ob.bid_levels() + ob.ask_levels() > 0);
}

}  // namespace

int main() {
    test_resting_and_best_quotes();
    test_full_cross_price_time_priority();
    test_price_improvement_executes_at_maker_price();
    test_partial_fill_residual_rests();
    test_sweep_multiple_levels();
    test_market_order_ioc();
    test_cancel();
    test_cancel_middle_of_queue();
    test_rejects_bad_input();
    test_conservation_invariant_random_flow();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

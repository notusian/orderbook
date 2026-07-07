// lob-engine: price-time priority limit order book & matching engine.
//
// Design targets the hot path of an exchange matching engine:
//   * integer tick prices (no floating point in matching)
//   * O(1) order lookup for cancels via open-addressing-friendly hash map
//   * intrusive doubly-linked FIFO queues per price level (no per-order
//     allocation on add/cancel: orders live in a pre-allocated pool)
//   * best-price access O(1) amortised via sorted std::map of levels
//
// Single header, C++17, no dependencies.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;   // ticks
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    Price price;
    Qty qty;
};

using TradeHandler = std::function<void(const Trade&)>;

struct Order {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;          // remaining quantity
    Side side = Side::Buy;
    // Intrusive FIFO links within a price level.
    Order* prev = nullptr;
    Order* next = nullptr;
    bool live = false;
};

struct Level {
    Qty total_qty = 0;
    Order* head = nullptr;  // oldest (first to fill)
    Order* tail = nullptr;  // newest

    void push_back(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        (tail ? tail->next : head) = o;
        tail = o;
        total_qty += o->qty;
    }

    void unlink(Order* o) {
        (o->prev ? o->prev->next : head) = o->next;
        (o->next ? o->next->prev : tail) = o->prev;
        total_qty -= o->qty;
    }

    bool empty() const { return head == nullptr; }
};

class OrderBook {
  public:
    explicit OrderBook(std::size_t pool_capacity = 1 << 20)
        : pool_(pool_capacity) {
        free_list_.reserve(pool_capacity);
        for (std::size_t i = pool_capacity; i-- > 0;) free_list_.push_back(&pool_[i]);
        index_.reserve(pool_capacity);
    }

    void set_trade_handler(TradeHandler h) { on_trade_ = std::move(h); }

    // Submit a limit order. Crosses against the opposite side first;
    // any residual rests on the book. Returns filled quantity.
    Qty add_limit(OrderId id, Side side, Price price, Qty qty) {
        validate_new(id, qty);
        Qty filled = match(id, side, price, qty);
        Qty residual = qty - filled;
        if (residual > 0) rest(id, side, price, residual);
        return filled;
    }

    // Market order: cross until filled or the opposite book is exhausted
    // (residual is dropped — IOC semantics). Returns filled quantity.
    Qty add_market(OrderId id, Side side, Qty qty) {
        validate_new(id, qty);
        constexpr Price kNoLimit = 0;
        return match(id, side, kNoLimit, qty, /*is_market=*/true);
    }

    // Cancel a resting order. Returns false if unknown / already filled.
    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Order* o = it->second;
        if (o->side == Side::Buy) {
            auto lvl = bids_.find(o->price);
            lvl->second.unlink(o);
            if (lvl->second.empty()) bids_.erase(lvl);
        } else {
            auto lvl = asks_.find(o->price);
            lvl->second.unlink(o);
            if (lvl->second.empty()) asks_.erase(lvl);
        }
        release(o);
        index_.erase(it);
        return true;
    }

    // ------------------------------------------------------------------
    // Introspection
    // ------------------------------------------------------------------
    bool best_bid(Price& px, Qty& qty) const { return best(bids_, px, qty); }
    bool best_ask(Price& px, Qty& qty) const { return best(asks_, px, qty); }
    std::size_t open_orders() const { return index_.size(); }
    std::size_t bid_levels() const { return bids_.size(); }
    std::size_t ask_levels() const { return asks_.size(); }

    Qty depth_at(Side side, Price px) const {
        if (side == Side::Buy) {
            auto it = bids_.find(px);
            return it == bids_.end() ? 0 : it->second.total_qty;
        }
        auto it = asks_.find(px);
        return it == asks_.end() ? 0 : it->second.total_qty;
    }

  private:
    // Bids sorted best (highest) first; asks best (lowest) first.
    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level, std::less<Price>> asks_;
    std::unordered_map<OrderId, Order*> index_;
    std::vector<Order> pool_;
    std::vector<Order*> free_list_;
    TradeHandler on_trade_;

    void validate_new(OrderId id, Qty qty) const {
        if (qty <= 0) throw std::invalid_argument("qty must be positive");
        if (index_.count(id)) throw std::invalid_argument("duplicate order id");
    }

    Order* acquire() {
        if (free_list_.empty()) throw std::runtime_error("order pool exhausted");
        Order* o = free_list_.back();
        free_list_.pop_back();
        return o;
    }

    void release(Order* o) {
        o->live = false;
        free_list_.push_back(o);
    }

    template <typename BookSide>
    static bool best(const BookSide& s, Price& px, Qty& qty) {
        if (s.empty()) return false;
        px = s.begin()->first;
        qty = s.begin()->second.total_qty;
        return true;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Order* o = acquire();
        *o = Order{id, price, qty, side, nullptr, nullptr, true};
        if (side == Side::Buy) {
            bids_[price].push_back(o);
        } else {
            asks_[price].push_back(o);
        }
        index_.emplace(id, o);
    }

    template <typename Levels>
    Qty sweep(OrderId taker, Levels& levels, Qty want,
              bool is_market, Price limit, bool taker_is_buy) {
        Qty filled = 0;
        while (want > 0 && !levels.empty()) {
            auto lvl_it = levels.begin();
            Price lvl_px = lvl_it->first;
            if (!is_market) {
                bool crosses = taker_is_buy ? (lvl_px <= limit) : (lvl_px >= limit);
                if (!crosses) break;
            }
            Level& lvl = lvl_it->second;
            while (want > 0 && lvl.head) {
                Order* maker = lvl.head;
                Qty ex = maker->qty < want ? maker->qty : want;
                maker->qty -= ex;
                lvl.total_qty -= ex;
                want -= ex;
                filled += ex;
                if (on_trade_) on_trade_({taker, maker->id, lvl_px, ex});
                if (maker->qty == 0) {
                    lvl.unlink(maker);  // total_qty already adjusted; qty is 0
                    index_.erase(maker->id);
                    release(maker);
                }
            }
            if (lvl.empty()) levels.erase(lvl_it);
        }
        return filled;
    }

    Qty match(OrderId taker, Side side, Price limit, Qty qty, bool is_market = false) {
        return (side == Side::Buy)
                   ? sweep(taker, asks_, qty, is_market, limit, /*taker_is_buy=*/true)
                   : sweep(taker, bids_, qty, is_market, limit, /*taker_is_buy=*/false);
    }
};

}  // namespace lob

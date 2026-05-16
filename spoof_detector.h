// ============================================================
//  SPOOF DETECTOR — plugs into the existing L3 OrderBook
//  Include this in main.cpp alongside market_microstructure.cpp
//
//  Usage:
//    SpoofDetector detector;
//    // On every ADD:
//    detector.on_add(order, mid_price_at_add_time);
//    // On every CANCEL:
//    detector.on_cancel(order, mid_price_at_cancel_time);
//    // On every FILL:
//    detector.on_fill(order);
//    // Periodically:
//    detector.print_alerts();
// ============================================================

#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <deque>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <chrono>

// ── Tuning knobs ─────────────────────────────────────────────
// Adjust these for your instrument. NQ futures defaults shown.

static constexpr int64_t LARGE_ORDER_THRESHOLD = 2;
// Orders with orig_size >= this are "large" and watched closely.
// For NQ: 10 contracts is notable. For ES: maybe 20.

static constexpr int64_t MIN_LIFETIME_NS = 500'000'000LL;
// Orders cancelled in under 500ms score high on the speed signal.

static constexpr double  PRICE_IMPACT_TICKS = 2.0;
// Mid-price must move at least this many ticks away from the
// order's side during its life to score the impact signal.
// NQ tick = 0.25, so 2 ticks = 0.50 price movement.

static constexpr double  TICK_SIZE = 0.25;
// Instrument tick size. Change for different products.

static constexpr int     REPEAT_WINDOW = 10;
// If same source cancels N+ large zero-fill orders in a rolling
// window of REPEAT_WINDOW events, repeat signal fires.

static constexpr int     ALERT_THRESHOLD = 40;
// Spoof score >= this triggers a printed alert. 0-100 scale.

// ─────────────────────────────────────────────────────────────

struct SpoofAlert
{
    uint64_t    order_id;
    std::string source;
    Side        side;
    double      price;
    int64_t     orig_size;
    int64_t     lifetime_ms;
    double      price_impact;   // ticks mid moved during order's life
    int         score;          // 0-100
    std::string reasons;        // human-readable breakdown
    std::string time_str;
};

// Tracks an order from Add until Cancel/Fill
struct WatchedOrder
{
    uint64_t order_id;
    Side     side;
    double   price;
    int64_t  orig_size;
    double   mid_at_add;        // mid-price when order entered book
    int64_t  add_time_ns;
    std::string source;
    std::string time_str;
};

class SpoofDetector
{
public:

    // ── Public interface ─────────────────────────────────────

    // Call this immediately after book.add_order()
    void on_add(const Order& o, double mid_price)
    {
        // Only watch orders above the large-order threshold
        if (o.orig_size < LARGE_ORDER_THRESHOLD) return;

        WatchedOrder w;
        w.order_id   = o.order_id;
        w.side       = o.side;
        w.price      = o.price;
        w.orig_size  = o.orig_size;
        w.mid_at_add = mid_price;
        w.add_time_ns = o.timestamp_ns;
        w.source     = o.source;
        w.time_str   = o.source;   // you can pass time_str separately if needed

        watched_[o.order_id] = w;
    }

    // Call this immediately after book.cancel_order()
    void on_cancel(const Order& o, double mid_now, int64_t cancel_time_ns)
    {
        auto it = watched_.find(o.order_id);
        if (it == watched_.end()) return;   // wasn't a large order
        WatchedOrder& w = it->second;

        // Run scoring
        SpoofAlert alert = score(w, o, mid_now, cancel_time_ns);

        if (alert.score >= ALERT_THRESHOLD)
        {
            alerts_.push_front(alert);
            if ((int)alerts_.size() > 100) alerts_.pop_back();
        }

        // Track repeat pattern per source
        repeat_tracker_[w.source].push_back(cancel_time_ns);
        // Keep only recent events in the window
        auto& v = repeat_tracker_[w.source];
        while ((int)v.size() > REPEAT_WINDOW) v.erase(v.begin());

        watched_.erase(it);
    }

    // Call on every Fill — reduces suspicion (real fills = genuine order)
    void on_fill(const Order& o)
    {
        // If it gets filled, it's less suspicious — remove from watch
        // (partial fills are handled by fill_count on the order itself)
        auto it = watched_.find(o.order_id);
        if (it == watched_.end()) return;

        // Only remove from watch if fully filled
        if (o.size <= 0) watched_.erase(it);
    }

    // Print the most recent alerts to terminal (call from your render loop)
   void print_alerts(int max_rows = 5) const
{
    if (alerts_.empty()) return;

    std::cout << "\033[31m\033[1m";
    std::cout << "\n  +------ SPOOF ALERTS (" << alerts_.size() << " total) ------+\n";
    std::cout << "\033[0m";

    int shown = 0;
    for (auto& a : alerts_)
    {
        if (shown++ >= max_rows) break;

        std::string side_str = (a.side == Side::BUY) ? "BID" : "ASK";
        std::string col      = (a.side == Side::BUY)
                               ? "\033[32m" : "\033[31m";

        std::cout << "  | " << col
                  << std::left << std::setw(8) << side_str
                  << " px=" << std::fixed << std::setprecision(2) << a.price
                  << " sz=" << a.orig_size
                  << " life=" << a.lifetime_ms << "ms"
                  << " score=" << a.score
                  << "\033[0m\n";
        std::cout << "  |   \033[2m" << a.reasons << "\033[0m\n";
    }

    std::cout << "\033[31m\033[1m";
    std::cout << "  +----------------------------------------------+\n";
    std::cout << "\033[0m";
}

    // Access alert list for custom rendering
    const std::deque<SpoofAlert>& alerts() const { return alerts_; }
    int alert_count() const { return (int)alerts_.size(); }

private:

    std::unordered_map<uint64_t, WatchedOrder> watched_;
    std::deque<SpoofAlert>                     alerts_;
    std::unordered_map<std::string, std::vector<int64_t>> repeat_tracker_;

    // ── Scoring engine ───────────────────────────────────────
    // Each signal contributes independently. Scores are additive.
    // Total max = 100.
    //
    //   Signal 1 — Size (large order)          : 0–25 pts
    //   Signal 2 — Zero fills on cancel        : 0–25 pts
    //   Signal 3 — Short lifetime              : 0–25 pts
    //   Signal 4 — Mid-price moved away        : 0–15 pts
    //   Signal 5 — Repeat pattern same source  : 0–10 pts
    //
    SpoofAlert score(const WatchedOrder& w,
                     const Order&        o,
                     double              mid_now,
                     int64_t             cancel_time_ns)
    {
        SpoofAlert a;
        a.order_id  = w.order_id;
        a.source    = w.source;
        a.side      = w.side;
        a.price     = w.price;
        a.orig_size = w.orig_size;
        a.score     = 0;

        std::ostringstream reasons;

        // ── Signal 1: Size ───────────────────────────────────
        // Larger orders score more. Scales up to 5x threshold.
        {
            double ratio = (double)w.orig_size / LARGE_ORDER_THRESHOLD;
            int    pts   = (int)std::min(25.0, ratio / 5.0 * 25.0);
            a.score += pts;
            reasons << "[size=" << w.orig_size << " +" << pts << "pts] ";
        }

        // ── Signal 2: Zero fills ─────────────────────────────
        // Cancelled with zero fills = almost certainly fake
        {
            int pts = 0;
            if (o.fill_count == 0)
            {
                pts = 25;   // full 25 — no fill at all is very suspicious
                reasons << "[0fills +" << pts << "pts] ";
            }
            else if (o.filled_size < w.orig_size / 4)
            {
                pts = 12;   // partial fill < 25% of original — still suspicious
                reasons << "[<25%filled +" << pts << "pts] ";
            }
            a.score += pts;
        }

        // ── Signal 3: Lifetime ───────────────────────────────
        // Very short-lived large orders are classic spoofing
        {
            int64_t lifetime_ns = cancel_time_ns - w.add_time_ns;
            a.lifetime_ms = lifetime_ns / 1'000'000LL;

            int pts = 0;
            if      (lifetime_ns < 100'000'000LL)   pts = 25;  // < 100ms
            else if (lifetime_ns < MIN_LIFETIME_NS)  pts = 15;  // 100-500ms
            else if (lifetime_ns < 2'000'000'000LL)  pts = 5;   // 500ms-2s
            a.score += pts;
            reasons << "[life=" << a.lifetime_ms << "ms +" << pts << "pts] ";
        }

        // ── Signal 4: Price impact ───────────────────────────
        // Did mid-price move away from the order's side while it sat?
        // A bid spoof pushes price down; ask spoof pushes price up.
        {
            double impact_ticks = 0.0;
            if (w.side == Side::BUY)
                impact_ticks = (w.mid_at_add - mid_now) / TICK_SIZE;  // positive = price fell
            else
                impact_ticks = (mid_now - w.mid_at_add) / TICK_SIZE;  // positive = price rose

            a.price_impact = impact_ticks;
            int pts = 0;
            if (impact_ticks >= PRICE_IMPACT_TICKS * 2) pts = 15;
            else if (impact_ticks >= PRICE_IMPACT_TICKS) pts = 8;
            a.score += pts;
            if (pts > 0)
                reasons << "[impact=" << std::fixed << std::setprecision(1)
                        << impact_ticks << "tks +" << pts << "pts] ";
        }

        // ── Signal 5: Repeat pattern ─────────────────────────
        // Same source cancelling large zero-fill orders repeatedly
        {
            auto it = repeat_tracker_.find(w.source);
            int pts = 0;
            if (it != repeat_tracker_.end() && (int)it->second.size() >= 3)
            {
                pts = std::min(10, (int)it->second.size() * 2);
                reasons << "[repeat=" << it->second.size() << " +" << pts << "pts] ";
            }
            a.score += pts;
        }

        a.score   = std::min(100, a.score);
        a.reasons = reasons.str();
        return a;
    }
};

// ============================================================
//  SPOOF DETECTOR v2 — plugs into the existing L3 OrderBook
//  Include this in main.cpp AFTER L3_orderflow.cpp.
//
//  Usage (see main.cpp for the exact call order):
//    SpoofDetector detector;
//    detector.tick(ts_ns, time_str);              // every message, first
//    detector.on_add(book, order);                // AFTER  book.process(add)
//    detector.on_cancel(book, order, ts_ns);      // BEFORE book.process(cancel)
//    detector.on_fill(order, fill_size, ts_ns);   // BEFORE book.process(fill)
//    detector.on_reset();                         // on a Reset message
//    detector.print_alerts();                     // from the render loop
//    detector.flush();                            // at end of feed
//
//  FILTERS (v2)
//  ------------
//   #1 Opposite-side execution — the same source gets filled on the
//      other side of the book while its large order is resting (or
//      within OPP_POST_CANCEL_NS after it is pulled).
//      NEEDS a `source` column with >= 2 distinct values.
//   #2 Distance from touch     — orders far behind the touch cannot
//      move anyone's decision: they are not watched beyond
//      MAX_WATCH_DISTANCE_TICKS, and closer orders score higher.
//   #3 Cancel-on-approach      — the order is pulled as the opposite
//      touch moves toward it (about to be hit), not simply expired.
//   #4 Relative size           — size as a share of visible depth AND
//      versus the source's own recent order sizes, instead of a fixed
//      contract count.
//
//  SCORE BUDGET (max 100)
//    relative size 15 | zero fills 15 | lifetime 10 | price impact 10
//    repeat 5 | opposite-side exec 25 | approach 10 | proximity 10
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
#include <cstdint>
#include <algorithm>

// ── Tuning knobs ─────────────────────────────────────────────
// NQ futures defaults. Adjust for your instrument.

static constexpr double  TICK_SIZE = 0.25;
// Instrument tick size.

static constexpr int     ALERT_THRESHOLD = 60;
// Spoof score >= this triggers a printed alert. 0-100 scale.

// ── Watch gate: which resting orders are tracked at all ──────
static constexpr int64_t LARGE_ORDER_THRESHOLD = 4;
// Absolute floor. Orders smaller than this are never watched.
// For NQ real data: raise this (10+ is notable). It is only a floor now;
// the order must ALSO be large relative to depth or to its source's norm.

static constexpr int     DEPTH_LEVELS = 10;
// Number of price levels per side used to measure "visible depth".

static constexpr double  MIN_DEPTH_SHARE = 0.10;
// #4: watch an order if it is >= this share of visible same-side depth...

static constexpr double  MIN_SIZE_RATIO = 2.0;
// #4: ...or >= this multiple of the source's median recent order size.

static constexpr size_t  SIZE_HISTORY = 50;
// #4: how many recent adds per source feed that median.

static constexpr size_t  MIN_SIZE_HISTORY = 8;
// #4: need at least this many prior adds before the ratio is trusted.

static constexpr double  MAX_WATCH_DISTANCE_TICKS = 20.0;
// #2: orders placed deeper than this behind the same-side touch are ignored.

// ── Lifetime & price impact ──────────────────────────────────
static constexpr int64_t MIN_LIFETIME_NS = 500'000'000LL;
// Orders cancelled in under 500ms score high on the speed signal.

static constexpr double  PRICE_IMPACT_TICKS = 2.0;
// Mid must move at least this many ticks IN THE SPOOFER'S FAVOUR
// (bid spoof -> mid up, ask spoof -> mid down) while the order rests.

// ── #1 Opposite-side execution ───────────────────────────────
static constexpr int64_t OPP_LOOKBACK_NS    = 5'000'000'000LL;
// Look this far back from the cancel (never earlier than the order's add).

static constexpr int64_t OPP_POST_CANCEL_NS = 1'000'000'000LL;
// Wait this long after the cancel for follow-up opposite-side fills.
// Alerts are therefore reported this much feed-time after the cancel.

static constexpr int64_t FILL_HISTORY_NS    = 15'000'000'000LL;
static constexpr size_t  MAX_FILLS_PER_SOURCE = 256;

// ── #3 Cancel-on-approach ────────────────────────────────────
static constexpr double  APPROACH_MIN_TICKS  = 2.0;
// Gap to the opposite touch must have shrunk by at least this much...

static constexpr double  APPROACH_NEAR_TICKS = 3.0;
// ...and full points need the gap now within this many ticks.

// ── Repeat pattern ───────────────────────────────────────────
static constexpr int64_t REPEAT_WINDOW_NS = 60'000'000'000LL;
// Prior zero-fill large cancels by the same source within this window.

// ─────────────────────────────────────────────────────────────

struct SpoofAlert
{
    uint64_t    order_id      = 0;
    std::string source;
    Side        side          = Side::BUY;
    double      price         = 0.0;
    int64_t     orig_size     = 0;
    int64_t     lifetime_ms   = 0;
    double      price_impact  = 0.0;   // ticks mid moved in spoofer's favour
    double      dist_ticks    = 0.0;   // #2 distance behind same-side touch at add
    double      depth_share   = 0.0;   // #4 size / visible same-side depth
    double      size_ratio    = 0.0;   // #4 size / source median (0 = n/a)
    int64_t     opp_fill_qty  = 0;     // #1 same-source opposite-side fills
    double      approach_ticks = 0.0;  // #3 how much the gap shrank
    int         score         = 0;     // 0-100
    std::string reasons;               // human-readable breakdown
    std::string time_str;              // feed time of the cancel
};

// Tracks a large order from Add until Cancel/Fill
struct WatchedOrder
{
    uint64_t    order_id    = 0;
    Side        side        = Side::BUY;
    double      price       = 0.0;
    int64_t     orig_size   = 0;
    int64_t     add_time_ns = 0;
    std::string source;
    double      mid_at_add  = 0.0;      // mid just AFTER the order joined (0 = n/a)
    double      dist_ticks  = 0.0;      // #2
    double      depth_share = 0.0;      // #4
    double      size_ratio  = 0.0;      // #4
    bool        ratio_valid = false;    // #4
    double      gap_at_add  = 0.0;      // #3 ticks to opposite touch
    bool        has_gap_add = false;
};

class SpoofDetector
{
public:

    // ── Public interface ─────────────────────────────────────

    // Call for EVERY feed message, before the event handler.
    // Advances the detector's clock and finalises cancels whose
    // post-cancel observation window (#1) has closed.
    void tick(int64_t ts_ns, const std::string& time_str)
    {
        now_str_ = time_str;
        while (!pending_.empty() && pending_.front().deadline_ns < ts_ns)
        {
            finalize(pending_.front());
            pending_.pop_front();
        }
    }

    // Call immediately AFTER book.process(add).
    void on_add(const OrderBook& book, const Order& o)
    {
        // ── Source size history (every add, watched or not) ──
        auto& hist = size_hist_[o.source];
        const bool ratio_valid = hist.size() >= MIN_SIZE_HISTORY;
        const double median    = ratio_valid ? median_of(hist) : 0.0;
        hist.push_back(o.orig_size);
        if (hist.size() > SIZE_HISTORY) hist.pop_front();

        // ── Gate 1: absolute floor ───────────────────────────
        if (o.orig_size < LARGE_ORDER_THRESHOLD) return;

        const BookSnap s = snap(book, true);

        // ── Gate 2 (#2): too far behind the touch to matter ──
        const double dist = dist_behind_touch(o.side, o.price, s);
        if (dist > MAX_WATCH_DISTANCE_TICKS) return;

        // ── Gate 3 (#4): large vs depth OR vs this source's norm
        const int64_t side_depth = (o.side == Side::BUY) ? s.bid_depth : s.ask_depth;
        const double  denom      = (double)std::max<int64_t>(side_depth, o.orig_size);
        const double  share      = (double)o.orig_size / denom;
        const double  ratio      = (ratio_valid && median > 0.0)
                                   ? (double)o.orig_size / median : 0.0;
        const bool big = share >= MIN_DEPTH_SHARE
                      || (ratio_valid && ratio >= MIN_SIZE_RATIO);
        if (!big) return;

        WatchedOrder w;
        w.order_id    = o.order_id;
        w.side        = o.side;
        w.price       = o.price;
        w.orig_size   = o.orig_size;
        w.add_time_ns = o.timestamp_ns;
        w.source      = o.source;
        w.mid_at_add  = s.mid;
        w.dist_ticks  = dist;
        w.depth_share = share;
        w.size_ratio  = ratio;
        w.ratio_valid = ratio_valid && median > 0.0;
        w.has_gap_add = gap_to_opposite(o.side, o.price, s, w.gap_at_add);

        watched_[o.order_id] = std::move(w);
    }

    // Call immediately BEFORE book.process(cancel).
    // Scoring is deferred by OPP_POST_CANCEL_NS so fills that arrive
    // just after the cancel can still count (#1).
    void on_cancel(const OrderBook& book, const Order& o, int64_t cancel_time_ns)
    {
        auto it = watched_.find(o.order_id);
        if (it == watched_.end()) return;   // wasn't a watched order

        const BookSnap s = snap(book, false);

        PendingCancel p;
        p.w            = std::move(it->second);
        p.fill_count   = o.fill_count;
        p.filled_size  = o.filled_size;
        p.cancel_ns    = cancel_time_ns;
        p.mid_now      = s.mid;
        p.has_gap_now  = gap_to_opposite(p.w.side, p.w.price, s, p.gap_now);
        p.deadline_ns  = cancel_time_ns + OPP_POST_CANCEL_NS;
        p.time_str     = now_str_;
        watched_.erase(it);

        pending_.push_back(std::move(p));
    }

    // Call immediately BEFORE book.process(fill).
    // `o` is the RESTING order that was hit; fill_size is the executed qty.
    void on_fill(const Order& o, int64_t fill_size, int64_t ts_ns)
    {
        // Record the execution against its source (#1)
        if (!o.source.empty())
        {
            auto& v = fills_[o.source];
            v.push_back({ts_ns, o.side, fill_size});
            while (!v.empty() && ts_ns - v.front().ts > FILL_HISTORY_NS) v.pop_front();
            while (v.size() > MAX_FILLS_PER_SOURCE) v.pop_front();
        }

        // A fully filled watched order was genuine — stop watching it.
        // (o.size has NOT been reduced yet: the book processes the fill after.)
        auto it = watched_.find(o.order_id);
        if (it != watched_.end() && (o.size - fill_size) <= 0)
            watched_.erase(it);
    }

    // Book was wiped: resting orders no longer exist.
    void on_reset() { watched_.clear(); }

    // End of feed: score everything still waiting.
    void flush()
    {
        while (!pending_.empty())
        {
            finalize(pending_.front());
            pending_.pop_front();
        }
    }

    // Print the most recent alerts to terminal (call from your render loop)
    void print_alerts(int max_rows = 5) const
    {
        if (alerts_.empty()) return;

        std::cout << "\033[31m\033[1m";
        std::cout << "\n  +------ SPOOF ALERTS (" << alerts_.size() << " total) ------+\n";
        std::cout << "\033[0m";

        int shown = 0;
        for (const auto& a : alerts_)
        {
            if (shown++ >= max_rows) break;

            const std::string side_str = (a.side == Side::BUY) ? "BID" : "ASK";
            const std::string col      = (a.side == Side::BUY) ? "\033[32m" : "\033[31m";

            std::cout << "  | " << col
                      << a.time_str << " "
                      << std::left << std::setw(4) << side_str
                      << " src=" << (a.source.empty() ? "?" : a.source)
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
    int    alert_count()      const { return (int)alerts_.size(); }
    size_t watched_count()    const { return watched_.size(); }
    size_t pending_count()    const { return pending_.size(); }
    size_t distinct_sources() const { return size_hist_.size(); }

private:

    // ── Internal types ───────────────────────────────────────

    struct BookSnap
    {
        double  best_bid  = 0.0;
        double  best_ask  = 0.0;
        double  mid       = 0.0;      // 0 when the book is one-sided/empty
        int64_t bid_depth = 0;        // total size, top DEPTH_LEVELS
        int64_t ask_depth = 0;
    };

    struct FillRec { int64_t ts; Side side; int64_t qty; };

    struct PendingCancel
    {
        WatchedOrder w;
        int          fill_count  = 0;
        int64_t      filled_size = 0;
        int64_t      cancel_ns   = 0;
        double       mid_now     = 0.0;
        double       gap_now     = 0.0;
        bool         has_gap_now = false;
        int64_t      deadline_ns = 0;
        std::string  time_str;
    };

    std::unordered_map<uint64_t, WatchedOrder>            watched_;
    std::deque<PendingCancel>                             pending_;
    std::deque<SpoofAlert>                                alerts_;
    std::unordered_map<std::string, std::deque<int64_t>>  size_hist_;   // #4
    std::unordered_map<std::string, std::deque<FillRec>>  fills_;       // #1
    std::unordered_map<std::string, std::deque<int64_t>>  repeat_;      // repeat signal
    std::string                                           now_str_;

    // ── Helpers ──────────────────────────────────────────────

    static double median_of(const std::deque<int64_t>& h)
    {
        std::vector<int64_t> v(h.begin(), h.end());
        auto mid = v.begin() + v.size() / 2;
        std::nth_element(v.begin(), mid, v.end());
        return (double)*mid;
    }

    static BookSnap snap(const OrderBook& book, bool with_depth)
    {
        BookSnap s;
        s.best_bid = book.best_bid();
        s.best_ask = book.best_ask();
        s.mid      = (s.best_bid > 0.0 && s.best_ask > 0.0)
                     ? (s.best_bid + s.best_ask) * 0.5 : 0.0;
        if (with_depth)
        {
            int n = 0;
            for (const auto& kv : book.bids)
            { s.bid_depth += kv.second.total_size; if (++n >= DEPTH_LEVELS) break; }
            n = 0;
            for (const auto& kv : book.asks)
            { s.ask_depth += kv.second.total_size; if (++n >= DEPTH_LEVELS) break; }
        }
        return s;
    }

    // Price difference -> ticks, rounded to 1/1000 tick so floating-point
    // noise can't push an exact "3.0 ticks" over a threshold.
    static double to_ticks(double price_diff)
    {
        return std::round(price_diff / TICK_SIZE * 1000.0) / 1000.0;
    }

    // #2: how many ticks BEHIND the same-side touch the order sits.
    // 0 = at (or improving) the touch.
    static double dist_behind_touch(Side side, double price, const BookSnap& s)
    {
        if (side == Side::BUY)
        {
            if (s.best_bid <= 0.0) return 0.0;
            return std::max(0.0, to_ticks(s.best_bid - price));
        }
        if (s.best_ask <= 0.0) return 0.0;
        return std::max(0.0, to_ticks(price - s.best_ask));
    }

    // #3: ticks between the order and the OPPOSITE touch (the price at
    // which an aggressor could hit it). Returns false if that side is empty.
    static bool gap_to_opposite(Side side, double price, const BookSnap& s, double& out)
    {
        if (side == Side::BUY)
        {
            if (s.best_ask <= 0.0) return false;
            out = to_ticks(s.best_ask - price);
        }
        else
        {
            if (s.best_bid <= 0.0) return false;
            out = to_ticks(price - s.best_bid);
        }
        return true;
    }

    // Opposite-side execution and repeat tracking only make sense when
    // the source field actually identifies different participants.
    bool source_usable(const std::string& src) const
    {
        return !src.empty() && size_hist_.size() >= 2;
    }

    // #1: qty the same source executed on the OTHER side in the window
    // [order add (bounded by lookback) .. cancel + post-cancel wait].
    int64_t opposite_fill_qty(const PendingCancel& p) const
    {
        auto it = fills_.find(p.w.source);
        if (it == fills_.end()) return 0;

        const int64_t lo = std::max(p.w.add_time_ns, p.cancel_ns - OPP_LOOKBACK_NS);
        const int64_t hi = p.deadline_ns;
        int64_t q = 0;
        for (const FillRec& f : it->second)
            if (f.side != p.w.side && f.ts >= lo && f.ts <= hi) q += f.qty;
        return q;
    }

    // ── Finalise one cancelled order: score, maybe alert ─────
    void finalize(const PendingCancel& p)
    {
        SpoofAlert a = score(p);
        if (a.score >= ALERT_THRESHOLD)
        {
            alerts_.push_front(a);
            if ((int)alerts_.size() > 100) alerts_.pop_back();
        }

        // Repeat tracker is updated AFTER scoring, and only with
        // zero-fill large cancels (what the signal is meant to count).
        if (p.fill_count == 0 && source_usable(p.w.source))
        {
            auto& v = repeat_[p.w.source];
            v.push_back(p.cancel_ns);
            while (!v.empty() && p.cancel_ns - v.front() > REPEAT_WINDOW_NS) v.pop_front();
            while (v.size() > 64) v.pop_front();
        }
    }

    // ── Scoring engine ───────────────────────────────────────
    // Each signal contributes independently. Scores are additive.
    //
    //   Relative size (#4)           : 0-15 pts
    //   Zero fills on cancel         : 0-15 pts
    //   Short lifetime               : 0-10 pts
    //   Mid-price moved in favour    : 0-10 pts
    //   Repeat pattern, same source  : 0-5  pts
    //   Opposite-side execution (#1) : 0-25 pts
    //   Cancel-on-approach (#3)      : 0-10 pts
    //   Proximity to touch (#2)      : 0-10 pts
    //
    SpoofAlert score(const PendingCancel& p) const
    {
        const WatchedOrder& w = p.w;

        SpoofAlert a;
        a.order_id    = w.order_id;
        a.source      = w.source;
        a.side        = w.side;
        a.price       = w.price;
        a.orig_size   = w.orig_size;
        a.dist_ticks  = w.dist_ticks;
        a.depth_share = w.depth_share;
        a.size_ratio  = w.ratio_valid ? w.size_ratio : 0.0;
        a.time_str    = p.time_str;
        a.score       = 0;

        std::ostringstream r;
        r << std::fixed << std::setprecision(1);

        // ── Relative size (#4) ───────────────────────────────
        {
            double pts_f = std::min(8.0, w.depth_share / 0.30 * 8.0);
            if (w.ratio_valid && w.size_ratio > 1.0)
                pts_f += std::min(7.0, (w.size_ratio - 1.0) / 9.0 * 7.0);
            const int pts = (int)std::lround(pts_f);
            a.score += pts;
            r << "[size=" << w.orig_size << " " << (w.depth_share * 100.0) << "%depth";
            if (w.ratio_valid) r << " x" << w.size_ratio << "med";
            r << " +" << pts << "] ";
        }

        // ── Zero fills ───────────────────────────────────────
        {
            int pts = 0;
            if (p.fill_count == 0)
            {
                pts = 15;
                r << "[0fills +" << pts << "] ";
            }
            else if (p.filled_size * 4 < w.orig_size)
            {
                pts = 8;
                r << "[<25%filled +" << pts << "] ";
            }
            a.score += pts;
        }

        // ── Lifetime ─────────────────────────────────────────
        {
            const int64_t life_ns = p.cancel_ns - w.add_time_ns;
            a.lifetime_ms = life_ns / 1'000'000LL;

            int pts = 0;
            if      (life_ns < 100'000'000LL)    pts = 10;   // < 100ms
            else if (life_ns < MIN_LIFETIME_NS)  pts = 6;    // 100-500ms
            else if (life_ns < 2'000'000'000LL)  pts = 2;    // 500ms-2s
            a.score += pts;
            r << "[life=" << a.lifetime_ms << "ms +" << pts << "] ";
        }

        // ── Price impact (in the spoofer's favour) ───────────
        // A BID spoof wants price UP (so the spoofer can sell higher);
        // an ASK spoof wants price DOWN. Baseline is the mid just after
        // the order joined, so the order's own quote isn't counted.
        {
            double impact = 0.0;
            if (w.mid_at_add > 0.0 && p.mid_now > 0.0)
                impact = (w.side == Side::BUY)
                         ? (p.mid_now - w.mid_at_add) / TICK_SIZE
                         : (w.mid_at_add - p.mid_now) / TICK_SIZE;

            a.price_impact = impact;
            int pts = 0;
            if      (impact >= PRICE_IMPACT_TICKS * 2) pts = 10;
            else if (impact >= PRICE_IMPACT_TICKS)     pts = 5;
            a.score += pts;
            if (pts > 0) r << "[impact=" << impact << "tks +" << pts << "] ";
        }

        // ── Repeat pattern ───────────────────────────────────
        {
            int n = 0;
            if (source_usable(w.source))
            {
                auto it = repeat_.find(w.source);
                if (it != repeat_.end())
                    for (int64_t t : it->second)
                        if (p.cancel_ns - t <= REPEAT_WINDOW_NS) n++;
            }
            const int pts = (n >= 2) ? std::min(5, n) : 0;
            a.score += pts;
            if (pts > 0) r << "[repeat=" << n << " +" << pts << "] ";
        }

        // ── Opposite-side execution (#1) ─────────────────────
        {
            int pts = 0;
            if (source_usable(w.source))
            {
                const int64_t opp = opposite_fill_qty(p);
                a.opp_fill_qty = opp;
                if (opp > 0)
                {
                    const double need = std::max(1.0, 0.25 * (double)w.orig_size);
                    const double frac = std::min(1.0, (double)opp / need);
                    pts = 15 + (int)std::lround(10.0 * frac);
                    r << "[opp-side fills=" << opp << " +" << pts << "] ";
                }
            }
            a.score += pts;
        }

        // ── Cancel-on-approach (#3) ──────────────────────────
        {
            int pts = 0;
            if (w.has_gap_add && p.has_gap_now)
            {
                const double shrink = w.gap_at_add - p.gap_now;
                a.approach_ticks = shrink;
                if (shrink >= APPROACH_MIN_TICKS)
                {
                    pts = (p.gap_now <= APPROACH_NEAR_TICKS) ? 10 : 5;
                    r << "[approach=" << shrink << "tks gap=" << p.gap_now
                      << " +" << pts << "] ";
                }
            }
            a.score += pts;
        }

        // ── Proximity to touch (#2) ──────────────────────────
        {
            int pts;
            if      (w.dist_ticks <= 1.0) pts = 10;
            else if (w.dist_ticks <= 3.0) pts = 7;
            else if (w.dist_ticks <= 6.0) pts = 4;
            else                          pts = 1;
            a.score += pts;
            r << "[dist=" << w.dist_ticks << "tks +" << pts << "] ";
        }

        a.score   = std::min(100, a.score);
        a.reasons = r.str();
        return a;
    }
};

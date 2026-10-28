// ============================================================
//  main.cpp  —  Entry point with Session Fast-Forward
//  Wires L3_orderflow.cpp + spoof_detector.h together
//
//  FAST-FORWARD FEATURE:
//  Set SKIP_TO_TIME to jump directly to any session.
//  All messages before the target time are still processed
//  (book state stays correct) — just no rendering happens.
//
//  Session times (UTC):
//    Asia   :  00:00 – 08:00
//    London :  08:00 – 16:30
//    NY     :  13:30 – 20:00
//
//  ★ FIX (this version) — REAL-TIME-ACCURATE PLAYBACK
//  -----------------------------------------------------
//  OLD BEHAVIOUR:
//    Every render frame slept a FIXED REFRESH_MS (50ms) regardless
//    of how much feed-time had actually elapsed. Since message
//    processing itself is near-instant, every frame ended up paced
//    at roughly the same constant wall-clock interval — bursts of
//    real market activity got artificially slowed down, and quiet
//    periods got artificially sped up. Replay speed had nothing to
//    do with the data's actual timestamps.
//
//  NEW BEHAVIOUR:
//    The sleep between frames is now proportional to the *feed-time*
//    delta between renders (scaled by PLAYBACK_SPEED), so a 20ms
//    burst plays back in ~20ms and a 5s lull plays back in ~5s
//    (at 1.0x speed). A MAX_GAP_NS cap prevents genuinely long gaps
//    (e.g. session boundaries) from stalling playback for real
//    minutes/hours.
//
//  ★ FIX 2 — SKIP THE WARM-UP, DON'T WATCH IT
//  -------------------------------------------
//  Fast-forward used to stop purely on TIME. If the book was still
//  thin right at the target time (few resting orders per side, huge
//  spread left over from a recent Reset), playback would start anyway
//  and you'd sit through the real warm-up period as it (correctly,
//  per FIX 1) ticks along at real-time speed — which can genuinely
//  take 15-20+ minutes of feed-time. Now fast-forward waits for BOTH
//  the target time AND a sane book (MIN_DEPTH_LEVELS per side, spread
//  under MAX_WARMUP_SPREAD) before it stops — and that waiting happens
//  at full CPU speed with no sleeping, so it costs milliseconds of
//  real time no matter how long the underlying warm-up actually took.
// ============================================================
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

#include "L3_orderflow.cpp"
#include "spoof_detector.h"

// ─────────────────────────────────────────────────────────────
//  SESSION JUMP SETTINGS
//  Set SKIP_TO_TIME to the session you want to start from.
//  Format: "HH:MM:SS" in UTC
//  Set to "" or "00:00:00" to play from the very beginning.
// ─────────────────────────────────────────────────────────────

//                        ↓ change this
const std::string SKIP_TO_TIME = "14:00:00";   // NY session open

// Common session start times:
// const std::string SKIP_TO_TIME = "00:00:00"; // Asia open (or play from start)
// const std::string SKIP_TO_TIME = "08:00:00"; // London open
// const std::string SKIP_TO_TIME = "13:30:00"; // NY open
// const std::string SKIP_TO_TIME = "14:30:00"; // NY + 1 hour (after open volatility)
// const std::string SKIP_TO_TIME = "18:00:00"; // NY afternoon
// const std::string SKIP_TO_TIME = "20:00:00"; // NY close

// ─────────────────────────────────────────────────────────────
//  PLAYBACK PACING SETTINGS
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────
//  BOOK WARM-UP / READINESS SETTINGS
//  ★ FIX — the old fast-forward only checked TIME. If the book was
//  still thin at the target time (few orders per side, huge spread
//  from a recent Reset/session-open), you'd start rendering anyway
//  and have to sit through the real "aggregating orders" warm-up —
//  which we now know can genuinely take 15-20+ minutes of feed-time.
//
//  Now, fast-forward waits for BOTH conditions before it starts
//  rendering: target time reached AND the book looks "real" (enough
//  resting levels per side, spread back down to a sane value). The
//  warm-up itself still gets processed — just silently, at full CPU
//  speed (no sleeping), so it costs milliseconds of real time instead
//  of minutes, no matter how long it took in the actual feed.
// ─────────────────────────────────────────────────────────────

static constexpr int    MIN_DEPTH_LEVELS   = 5;     // need at least this many price levels per side
static constexpr double MAX_WARMUP_SPREAD  = 5.0;   // don't start until spread is back under this (NQ ticks)

// 1.0 = real-time (matches actual elapsed market time)
// 2.0 = 2x speed, 0.5 = half speed, etc.
static constexpr double PLAYBACK_SPEED = 1.0;

// Hard cap on how long any single real-time wait can be, in nanoseconds.
// Prevents long genuine market gaps (lunch lulls, session boundaries)
// from freezing playback for real minutes. Lower this for snappier
// replay through quiet periods, raise it for stricter time-accuracy.
static constexpr int64_t MAX_GAP_NS = 1'000'000'000LL;   // 1 second

int main(int argc, char* argv[])
{
    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    #endif

    // ── CSV path ─────────────────────────────────────────────
    const std::string FALLBACK_PATH =
        "D:\\C++_PROJECTS\\L3 OF VISUALISER\\L3 Visualizer\\tmpar_yjsl7.csv";

    std::string csv_path;
    if (argc >= 2)
        csv_path = argv[1];
    else
        csv_path = FALLBACK_PATH;

    // ── Optional: override skip time from command line ────────
    // Usage: program.exe data.csv 13:30:00
    std::string skip_to = SKIP_TO_TIME;
    if (argc >= 3)
        skip_to = argv[2];

    // ── Create objects ────────────────────────────────────────
    OrderBook     book;
    CSVFeed       feed;
    Visualizer    viz;
    SpoofDetector detector;

    // ── Open CSV ──────────────────────────────────────────────
    if (!feed.open(csv_path))
    {
        std::cerr << "Cannot open: " << csv_path << "\n";
        return 1;
    }

    // ── Print session info ────────────────────────────────────
    std::cout << "Opened : " << csv_path << "\n";
    if (skip_to.empty() || skip_to == "00:00:00")
        std::cout << "Mode   : Play from beginning\n";
    else
        std::cout << "Mode   : Fast-forward to " << skip_to << " UTC\n";

    std::cout << "Speed  : " << PLAYBACK_SPEED << "x real-time"
               << " (max gap cap " << (MAX_GAP_NS / 1'000'000LL) << "ms)\n";

    std::cout << "\nSessions (UTC):\n";
    std::cout << "  Asia    00:00 - 08:00   " << (skip_to == "00:00:00" ? "<--" : "") << "\n";
    std::cout << "  London  08:00 - 16:30   " << (skip_to == "08:00:00" ? "<--" : "") << "\n";
    std::cout << "  NY      13:30 - 20:00   " << (skip_to == "13:30:00" ? "<--" : "") << "\n";
    std::cout << "\nStarting in 2 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ── Feed loop ─────────────────────────────────────────────
    FeedMessage msg;
    int64_t prev_ts        = 0;
    int64_t skip_count     = 0;
    bool    reached_time   = (skip_to.empty() || skip_to == "00:00:00");
    bool    reached_target = false;

    // ★ FIX: book is only "ready" once it has real depth on both sides
    // AND the spread has come back down to a sane value. This is what
    // lets us skip the thin/warm-up period entirely instead of watching
    // it crawl in (now correctly) real-time.
    auto book_ready = [&]() {
        return (int)book.bids.size() >= MIN_DEPTH_LEVELS
            && (int)book.asks.size() >= MIN_DEPTH_LEVELS
            && book.spread() > 0.0
            && book.spread() <= MAX_WARMUP_SPREAD;
    };

    while (feed.next(msg))
    {
        // ── FAST-FORWARD PHASE ────────────────────────────────
        // Always process every message so the book state is correct.
        // Just skip rendering until target time is reached AND the
        // book is no longer thin (no sleeping here — full speed).
        if (!reached_target)
        {
            // Process silently — book must be accurate when we start rendering
            book.process(msg);
            skip_count++;

            // Compare time strings directly — "HH:MM:SS" is lexicographically safe
            if (!reached_time && !msg.time_str.empty() && msg.time_str >= skip_to)
                reached_time = true;

            if (reached_time && book_ready())
            {
                reached_target = true;

                std::cout << "\033[2J\033[H";
                std::cout << "═══════════════════════════════════════\n";
                std::cout << "  JUMPED TO  : " << msg.time_str << " UTC\n";
                std::cout << "  MSGS SKIPPED: " << skip_count << "\n";
                std::cout << "  BID LEVELS  : " << book.bids.size() << "\n";
                std::cout << "  ASK LEVELS  : " << book.asks.size() << "\n";
                std::cout << "  BEST BID    : " << book.best_bid() << "\n";
                std::cout << "  BEST ASK    : " << book.best_ask() << "\n";
                std::cout << "  SPREAD      : " << book.spread() << "\n";
                std::cout << "  MID         : " << book.mid_price() << "\n";
                std::cout << "  ORDERS LIVE : " << book.order_map.size() << "\n";
                std::cout << "  (book skipped straight past warm-up — spread/depth now sane)\n";
                std::cout << "═══════════════════════════════════════\n";
                std::cout << "Press ENTER to start display...\n";
                std::cin.get();   // pause so you can read it

                // Clear screen and show what we skipped
                std::cout << "\033[2J\033[H";   // clear terminal
                std::cout << "\n  Fast-forwarded " << skip_count
                          << " messages\n";
                std::cout << "  Arrived at: " << msg.time_str << " UTC\n";
                std::cout << "  Book state: "
                          << book.bids.size() << " bid levels, "
                          << book.asks.size() << " ask levels\n";
                std::cout << "\n  Starting display...\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));

                viz.initialized = false;    // force fresh screen draw
                prev_ts = msg.timestamp_ns; // reset render timer
                // NOTE: this also resets the pacing reference point —
                // playback resumes counting real-time gaps from here,
                // so the jump itself never causes a giant catch-up sleep.
            }
            else
            {
                // Show progress every 50,000 messages during skip
                if (skip_count % 50000 == 0)
                {
                    std::cout << "\r  Fast-forwarding...  "
                              << skip_count << " msgs  "
                              << "time=" << msg.time_str
                              << (reached_time ? "  [time OK, waiting on book depth/spread]"
                                                : ("  target=" + skip_to))
                              << "  bid_lvls=" << book.bids.size()
                              << "  ask_lvls=" << book.asks.size()
                              << "  spread=" << book.spread()
                              << "     " << std::flush;
                }
                continue;   // skip the render section below
            }
        }

        // ── NORMAL PROCESSING (after reaching target time) ────
        double mid = book.mid_price();

        if (msg.event_type == EventType::RESET)
        {
            viz.initialized = false;
            book.process(msg);
        }
        else if (msg.event_type == EventType::ADD)
        {
            book.process(msg);
            auto it = book.order_map.find(msg.order_id);
            if (it != book.order_map.end())
                detector.on_add(it->second, mid);
        }
        else if (msg.event_type == EventType::CANCEL)
        {
            auto it = book.order_map.find(msg.order_id);
            if (it != book.order_map.end())
                detector.on_cancel(it->second, mid, msg.timestamp_ns);
            book.process(msg);
        }
        else if (msg.event_type == EventType::FILL)
        {
            auto it = book.order_map.find(msg.order_id);
            if (it != book.order_map.end())
                detector.on_fill(it->second);
            book.process(msg);
        }
        else
        {
            book.process(msg);
        }

        // ── Render every ~100ms of FEED time, paced at REAL time ──
        if (msg.timestamp_ns - prev_ts >= 100'000'000LL
            && !book.bids.empty()
            && !book.asks.empty())
        {
            // ★ FIX: sleep proportional to the feed-time gap, not a
            // fixed constant. This is what makes playback track the
            // market's actual pace instead of a flat frame rate.
            int64_t feed_delta_ns = msg.timestamp_ns - prev_ts;
            int64_t sleep_ns      = (int64_t)((double)feed_delta_ns / PLAYBACK_SPEED);
            sleep_ns              = std::min(sleep_ns, MAX_GAP_NS);

            if (sleep_ns > 0)
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));

            viz.render(book, msg.time_str);   // ★ pass current feed time for display
            detector.print_alerts(3);
            prev_ts = msg.timestamp_ns;
        }
    }

    // ── End of feed ───────────────────────────────────────────
    viz.render(book, msg.time_str);
    detector.print_alerts(10);
    std::cout << "\n[END OF FEED]\n";
    std::cout << "Total messages skipped : " << skip_count << "\n";
    std::cout << "Total spoof alerts     : " << detector.alert_count() << "\n";
    return 0;
}

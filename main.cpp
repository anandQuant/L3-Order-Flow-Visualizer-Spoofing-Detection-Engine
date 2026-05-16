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
// ============================================================

#include "L3_orderflow.cpp"
#include "spoof_detector.h"

// ─────────────────────────────────────────────────────────────
//  SESSION JUMP SETTINGS
//  Set SKIP_TO_TIME to the session you want to start from.
//  Format: "HH:MM:SS" in UTC
//  Set to "" or "00:00:00" to play from the very beginning.
// ─────────────────────────────────────────────────────────────

//                        ↓ change this
const std::string SKIP_TO_TIME = "13:30:00";   // NY session open

// Common session start times:
// const std::string SKIP_TO_TIME = "00:00:00"; // Asia open (or play from start)
// const std::string SKIP_TO_TIME = "08:00:00"; // London open
// const std::string SKIP_TO_TIME = "13:30:00"; // NY open
// const std::string SKIP_TO_TIME = "14:30:00"; // NY + 1 hour (after open volatility)
// const std::string SKIP_TO_TIME = "18:00:00"; // NY afternoon
// const std::string SKIP_TO_TIME = "20:00:00"; // NY close

int main(int argc, char* argv[])
{
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

    std::cout << "\nSessions (UTC):\n";
    std::cout << "  Asia    00:00 - 08:00   " << (skip_to == "00:00:00" ? "<--" : "") << "\n";
    std::cout << "  London  08:00 - 16:30   " << (skip_to == "08:00:00" ? "<--" : "") << "\n";
    std::cout << "  NY      13:30 - 20:00   " << (skip_to == "13:30:00" ? "<--" : "") << "\n";
    std::cout << "\nStarting in 2 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ── Feed loop ─────────────────────────────────────────────
    FeedMessage msg;
    int64_t prev_ts       = 0;
    int64_t skip_count    = 0;
    bool    reached_target = (skip_to.empty() || skip_to == "00:00:00");

    while (feed.next(msg))
    {
        // ── FAST-FORWARD PHASE ────────────────────────────────
        // Always process every message so the book state is correct.
        // Just skip rendering until we reach the target time.
        if (!reached_target)
        {
            // Process silently — book must be accurate when we start rendering
            if (msg.event_type == EventType::RESET)
                book.process(msg);
            else
                book.process(msg);

            skip_count++;

            // Compare time strings directly — "HH:MM:SS" is lexicographically safe
            if (!msg.time_str.empty() && msg.time_str >= skip_to)
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
std::cout << "═══════════════════════════════════════\n";
std::cout << "Press ENTER to start display...\n";
std::cin.get();   // pause so you can read it

viz.initialized = false;
prev_ts = msg.timestamp_ns;

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

                viz.initialized = false;   // force fresh screen draw
                prev_ts = msg.timestamp_ns; // reset render timer
            }
            else
            {
                // Show progress every 50,000 messages during skip
                if (skip_count % 50000 == 0)
                {
                    std::cout << "\r  Fast-forwarding...  "
                              << skip_count << " msgs  "
                              << "time=" << msg.time_str
                              << "  target=" << skip_to
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

        // ── Render every ~100ms of feed time ─────────────────
        if (msg.timestamp_ns - prev_ts >= 100'000'000LL
            && !book.bids.empty()
            && !book.asks.empty())
        {
            viz.render(book);
            detector.print_alerts(3);
            std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_MS));
            prev_ts = msg.timestamp_ns;
        }
    }

    // ── End of feed ───────────────────────────────────────────
    viz.render(book);
    detector.print_alerts(10);
    std::cout << "\n[END OF FEED]\n";
    std::cout << "Total messages skipped : " << skip_count << "\n";
    std::cout << "Total spoof alerts     : " << detector.alert_count() << "\n";
    return 0;
}

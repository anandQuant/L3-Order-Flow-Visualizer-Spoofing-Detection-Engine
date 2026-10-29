// ============================================================
//  main.cpp  —  Entry point with Session Fast-Forward
//  Wires L3_orderflow.cpp + spoof_detector.h together
//
//  FAST-FORWARD FEATURE:
//  Set SKIP_TO_TIME to jump directly to any session.
//  All messages before the target time are still processed
//  (book state stays correct) — just no rendering happens.
//
//  HOW TO RUN
//    - Just run it. It asks which CSV file (type the path or drag the file
//      into the window) and which start time, and remembers both for next time.
//    - Or pass them:  program.exe "data.csv" 13:30:00   (both optional)
//    - If a launcher mangles the arguments (glued or split), they are repaired.
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

#include <fstream>
#include <iostream>
#include <string>

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

// ─────────────────────────────────────────────────────────────
//  STARTUP HELPERS
//  Choose the CSV file and the start time without depending on how
//  the program was launched (IDE, double-click, terminal). Whatever is
//  missing or unreadable is simply asked for, so any CSV works every time.
// ─────────────────────────────────────────────────────────────

// Remembers the last file and start time, so the next run is just ENTER, ENTER.
static const char* LAST_RUN_FILE = "l3_last_run.txt";

// Removes spaces, tabs, line ends and surrounding quotes
// (dragging a file into a console window adds quotes around the path).
static std::string trim_text(std::string s)
{
    auto junk = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\''; };
    while (!s.empty() && junk(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && junk(s[i])) i++;
    return s.substr(i);
}

// True for "HH:MM:SS"
static bool is_clock(const std::string& s)
{
    if (s.size() != 8) return false;
    for (size_t i = 0; i < 8; i++)
    {
        if (i == 2 || i == 5) { if (s[i] != ':') return false; }
        else if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static bool file_readable(const std::string& p)
{
    if (p.empty()) return false;
    std::ifstream f(p);
    return static_cast<bool>(f);
}

static void load_last_run(std::string& path, std::string& time)
{
    std::ifstream f(LAST_RUN_FILE);
    std::string a, b;
    if (f && std::getline(f, a))
    {
        path = trim_text(a);
        if (std::getline(f, b)) time = trim_text(b);
    }
}

static void save_last_run(const std::string& path, const std::string& time)
{
    std::ofstream f(LAST_RUN_FILE);
    if (f) f << path << "\n" << time << "\n";
}

// Some launchers (Code::Blocks' console runner, for one) drop the quotes or the space
// between the arguments. This repairs both mistakes:
//    "D:\data\x.csv"18:28:01      -> path + time glued together
//    D:\my data\x.csv 18:28:01    -> a path with spaces split into several arguments
// Returns true and fills path / time (time stays empty if none was found) when the
// repaired path can be opened.
static bool repair_arguments(int argc, char* argv[], std::string& path, std::string& time)
{
    std::string joined;
    for (int i = 1; i < argc; i++)
    {
        if (i > 1) joined += ' ';
        joined += argv[i];
    }
    joined = trim_text(joined);

    const std::string first = argc >= 2 ? trim_text(argv[1]) : std::string();
    const std::string candidates[2] = { first, joined };
    for (const std::string& c : candidates)
    {
        if (file_readable(c)) { path = c; time.clear(); return true; }
        if (c.size() > 8 && is_clock(c.substr(c.size() - 8)))
        {
            const std::string head = trim_text(c.substr(0, c.size() - 8));
            if (file_readable(head)) { path = head; time = c.substr(c.size() - 8); return true; }
        }
    }
    return false;
}

int main(int argc, char* argv[])
{
    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    #endif

    // ── CSV path and start time ──────────────────────────────
    // Command line (both optional):   program.exe data.csv 13:30:00
    // Anything missing or unreadable is asked for below, so it behaves the
    // same from an IDE, a double-click or a terminal, and for any CSV.
    std::string csv_path = (argc >= 2) ? trim_text(argv[1]) : std::string();
    std::string skip_to  = SKIP_TO_TIME;
    bool time_given = false;
    std::string bad_time;                       // an argument that was not a usable time

    if (argc >= 3)
    {
        const std::string t = trim_text(argv[2]);
        if (t.empty() || is_clock(t)) { skip_to = t; time_given = true; }
        else bad_time = t;
    }

    if (argc >= 2 && !file_readable(csv_path))
    {
        std::string fixed_path, fixed_time;
        if (repair_arguments(argc, argv, fixed_path, fixed_time))
        {
            csv_path = fixed_path;
            if (is_clock(fixed_time)) { skip_to = fixed_time; time_given = true; }
        }
    }
    if (!time_given && !bad_time.empty() && file_readable(csv_path))
        std::cout << "Ignoring start time [" << bad_time << "] (it must look like 14:30:00).\n";

    std::string last_path, last_time;
    load_last_run(last_path, last_time);
    if (!file_readable(last_path)) last_path.clear();

    bool asked = false;
    while (!file_readable(csv_path))
    {
        if (!csv_path.empty())
            std::cerr << "Cannot open: [" << csv_path << "]\n";
        std::cout << "\nWhich CSV file? Type the path, or drag the file into this window, then press ENTER.\n";
        if (!last_path.empty())
            std::cout << "  (ENTER alone = last file: " << last_path << ")\n";
        std::cout << "> " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) return 1;       // nobody at the keyboard
        line = trim_text(line);
        if (line.empty())
        {
            if (last_path.empty()) { std::cout << "No file given. Quitting.\n"; return 1; }
            line = last_path;
        }
        csv_path = line;
        asked = true;
    }

    if (asked && !time_given)
    {
        const std::string def = is_clock(last_time) ? last_time : SKIP_TO_TIME;
        for (;;)
        {
            std::cout << "\nStart time, UTC, as HH:MM:SS.  ENTER = " << def
                      << "   (00:00:00 = play from the start of the file)\n> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) { skip_to = def; break; }
            line = trim_text(line);
            if (line.empty()) { skip_to = def; break; }
            if (is_clock(line)) { skip_to = line; break; }
            std::cout << "  That is not HH:MM:SS (example: 14:30:00). Try again.\n";
        }
    }

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
    save_last_run(csv_path, skip_to);

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

                // This message was already applied to the book above (book.process).
                // Do NOT fall through to the normal path, or it is processed a second
                // time: an Add would be inserted twice and only ever removed once,
                // leaving a phantom order in the book for the rest of the run.
                continue;
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
        // Detector clock first: finalises cancels whose post-cancel
        // observation window has closed (opposite-side check).
        detector.tick(msg.timestamp_ns, msg.time_str);

        if (msg.event_type == EventType::RESET)
        {
            viz.initialized = false;
            detector.on_reset();
            book.process(msg);
        }
        else if (msg.event_type == EventType::ADD)
        {
            // AFTER the add, so the detector sees the book with the new order in it
            book.process(msg);
            auto it = book.order_map.find(msg.order_id);
            if (it != book.order_map.end())
                detector.on_add(book, it->second);
        }
        else if (msg.event_type == EventType::CANCEL)
        {
            // BEFORE the cancel: the order (and its fill history) is still in the book
            auto it = book.order_map.find(msg.order_id);
            if (it != book.order_map.end())
                detector.on_cancel(book, it->second, msg.timestamp_ns);
            book.process(msg);
        }
        else if (msg.event_type == EventType::FILL)
        {
            // BEFORE the fill: detector computes the remaining size itself
            auto it = book.order_map.find(msg.order_id);
            if (it != book.order_map.end())
                detector.on_fill(it->second, msg.size, msg.timestamp_ns);
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
    detector.flush();   // score cancels still waiting on their post-cancel window
    viz.render(book, msg.time_str);
    detector.print_alerts(10);
    std::cout << "\n[END OF FEED]\n";
    std::cout << "Total messages skipped : " << skip_count << "\n";
    std::cout << "Total spoof alerts     : " << detector.alert_count() << "\n";
    if (detector.distinct_sources() < 2)
        std::cout << "NOTE: the 'source' column has fewer than 2 distinct values in the part of the\n"
                  << "      feed that was played. Opposite-side execution and repeat signals need\n"
                  << "      per-participant IDs, so they were inactive.\n";
    return 0;
}

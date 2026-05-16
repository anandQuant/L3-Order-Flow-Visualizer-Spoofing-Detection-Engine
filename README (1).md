# L3 Order Flow Visualizer + Spoofing Detection Engine

A professional-grade **Level 3 order book visualizer** and **real-time market manipulation detector** built entirely in C++17. Reads raw L3 market data and renders a live Depth of Market terminal display while simultaneously scoring every order for spoofing behaviour.

> Built from scratch as a deep-dive into market microstructure, order flow analysis, and manipulation detection — the same concepts used by proprietary trading firms and regulators.

---

## What Is L3 Data?

| Level | What You See | Use Case |
|---|---|---|
| L1 | Best bid + best ask only | Price display |
| L2 | Total size per price level | Depth of market |
| **L3** | **Every individual order ID at every level** | **Order flow analysis, manipulation detection** |

L3 is the most granular market data available. It lets you see not just *how much* liquidity exists at a price, but *which specific orders* are there and *how they behave over time*. Without L3, spoofing detection is impossible.

---

## System Architecture

```
CSV Feed (L3 Data)
        │
        ▼
┌───────────────────┐
│   L3_orderflow.cpp│  ← Core engine
│                   │
│  ┌─────────────┐  │
│  │  OrderBook  │  │  Processes: Add, Cancel, Modify,
│  │  (L3 engine)│  │            Trade, Fill, Reset
│  └──────┬──────┘  │
│         │         │
│  ┌──────┴──────┐  │
│  │  Visualizer │  │  Two-panel terminal DOM
│  └─────────────┘  │
└────────┬──────────┘
         │
    ┌────┴────────────────┐
    │                     │
    ▼                     ▼
┌──────────┐     ┌─────────────────┐
│  DOM     │     │ spoof_detector.h│
│ Display  │     │                 │
└──────────┘     │ 1. Single spoof │
                 │ 2. Layering     │
                 │ 3. OTR analysis │
                 └────────┬────────┘
                          │
                          ▼
                   ┌─────────────┐
                   │ Alert Output│
                   └─────────────┘
```

---

## Features

### L3 Order Book Engine
- Full L3 event processing — Add, Cancel, Modify, Trade, Fill, Reset
- Internal sequential ID system — handles scientific notation order ID collisions in CSV feeds
- Correct ask/bid display ordering (best ask at bottom of ask section, closest to spread)
- Auto-detects tab or comma CSV delimiter

### Terminal Visualizer
- **Live DOM** — 10 price levels per side (configurable)
- **Time and Sales tape** — every execution with direction (^ buy / v sell)
- **Order imbalance bar** — visual bid vs ask pressure
- **Cumulative delta** — running buy volume minus sell volume
- **Session fast-forward** — jump directly to NY, London, or Asia session

### Spoofing Detection Engine
Three independent detection modules running simultaneously:

#### 1. Single Order Spoof Detector
Watches every large order from placement to removal. On cancel, scores across 5 signals:

| Signal | Points | Logic |
|---|---|---|
| Order size | 0-25 | Larger orders = higher suspicion |
| Zero fills | 0-25 | Cancelled with no fills = never intended to trade |
| Lifetime | 0-25 | Under 500ms = placed and pulled before market reacts |
| Price impact | 0-15 | Did mid-price move in spoofer's favour during order's life |
| Repeat pattern | 0-10 | Same source doing this repeatedly |

#### 2. Layering Detector
Groups orders by source and side within a 2-second window. Fires when:
- 3 or more orders placed at different price levels
- All cancelled within 1 second
- Zero fills across the entire group

#### 3. OTR (Order-to-Trade Ratio)
Tracks per-source rolling ratio of orders placed vs orders that resulted in fills.
```
OTR = orders_placed / orders_traded
Alert threshold: OTR > 50 with minimum 20 orders placed
```

#### Alert Scoring
| Score | Interpretation |
|---|---|
| 0 - 40 | Normal order behaviour |
| 40 - 60 | Suspicious, monitor this source |
| 60 - 80 | Probable manipulation |
| 80 - 100 | Near-certain spoof signature |

---

## Display Layout

```
 DOM  Last: 25301.00 [+]  Spread: 0.25  Mid: 25301.13
 Vol:45231  Buy:28441  Sell:16790  Delta:+11651

 TIME       SIZE    PRICE  |ORD [BID DEPTH  ] BID SZ   PRICE  ASK SZ [ASK DEPTH] ORD
                           |                           25303.00   500 [######   ]  3
 13:29:44    50  25301.00 ^|                           25302.75   200 [###      ]  1
 13:29:44   200  25300.50 v|                           25302.50   800 [#########]  5
 13:29:43   100  25301.00 v| --- SPREAD 0.25  MID 25301.13 ---
 13:29:42    50  25301.25 ^|  2 [##       ]  300  25301.00
 13:29:41   300  25300.75 v|  5 [#########] 1000  25300.75
                           | IMB[----#####|--------] D:+11651  imb:+0.234

  +------ MANIPULATION ALERTS (2 total) ------+
  | SPOOF ASK src=FirmA px=25340.00 sz=50 score=82
  |   [sz=50 +22] [0fills +25] [life=94ms +25] [impact=3.2tks +15]
  | LAYER BID src=FirmB px=25290.00 sz=4  score=71
  |   [4 orders +24] [0fills +30] [cancel_span=380ms +15] [3 levels +9]
  +--------------------------------------------+
```

---

## File Structure

```
project/
├── main.cpp              <- Entry point, wires everything together
├── L3_orderflow.cpp      <- OrderBook engine + Visualizer + CSVFeed
└── spoof_detector.h      <- All three detection modules
```

Build only `main.cpp` — the other two are included automatically.

---

## Build and Run

**Requirements:** C++17 compiler, ANSI-capable terminal

```bash
# Linux / macOS
g++ -std=c++17 -O2 -o market main.cpp
./market data.csv

# Windows MinGW
g++ -std=c++17 -O2 -o market.exe main.cpp
market.exe data.csv
```

**Code::Blocks:**
1. File -> New -> Project -> Console Application -> C++
2. Add only `main.cpp` to the build
3. Right-click `L3_orderflow.cpp` -> Properties -> uncheck all build targets
4. Ctrl+F9 to build, Ctrl+F10 to run

---

## Configuration

### CSV Path
Edit `main.cpp`:
```cpp
const std::string FALLBACK_PATH = "D:\\path\\to\\your\\data.csv";
```

### Session Fast-Forward
Jump directly to any trading session:
```cpp
const std::string SKIP_TO_TIME = "00:00:00";  // Asia open / play from start
const std::string SKIP_TO_TIME = "08:00:00";  // London open
const std::string SKIP_TO_TIME = "13:30:00";  // NY open
const std::string SKIP_TO_TIME = "14:30:00";  // NY + 1 hour
```
All messages before the target time are still processed so book state stays accurate.

### Book Depth and Speed
```cpp
// In L3_orderflow.cpp:
static constexpr int BOOK_DEPTH = 10;    // price levels per side
static constexpr int REFRESH_MS = 80;    // redraw interval in ms
```

### Detection Thresholds
```cpp
// In spoof_detector.h:
static constexpr int64_t LARGE_ORDER_THRESHOLD = 10;   // min size to watch
static constexpr int     ALERT_THRESHOLD        = 60;   // min score to alert
static constexpr double  TICK_SIZE              = 0.25; // NQ futures
static constexpr int     LAYER_MIN_ORDERS       = 3;    // orders per layer
static constexpr double  OTR_ALERT_THRESHOLD    = 50.0; // OTR ratio limit
```

---

## CSV Format

Tab or comma separated. Delimiter auto-detected from header row.

```
timestamp                          action  side  price    size  order_id      source
2026-04-05T13:29:43.776471934+0000 Add     Buy   25300.75 5     687509000001  TradingFirm_A
2026-04-05T13:29:43.776891234+0000 Add     Sell  25301.00 3     687509000002  TradingFirm_B
2026-04-05T13:29:43.812340000+0000 Trade   Buy   25301.00 2     0
2026-04-05T13:29:43.812340000+0000 Fill    Buy   25301.00 2     687509000002
2026-04-05T13:29:44.001230000+0000 Cancel  Buy   25300.75 5     687509000001
```

| Column | Format | Notes |
|---|---|---|
| timestamp | ISO 8601 with nanoseconds | UTC timezone |
| action | Add / Cancel / Modify / Trade / Fill / Reset | Case-insensitive |
| side | Buy / Sell | Case-insensitive |
| price | Decimal | Any precision |
| size | Integer or float | Contracts |
| order_id | Integer or scientific notation | 6.87509E+12 handled correctly |
| source | String | Market participant identifier |

**Order ID collision handling:** Scientific notation in CSV loses precision, causing multiple orders to share the same parsed ID. This system assigns internal sequential IDs and maintains an external-to-internal mapping, ensuring correct cancel and fill routing even with imprecise source data.

---

## How the L3 Engine Works

```
FeedMessage
     │
     ├── ADD    -> assign internal_id, insert into bids/asks + order_map
     ├── CANCEL -> resolve external->internal ID, remove from level
     ├── MODIFY -> adjust size; if price changed, move to new level
     ├── TRADE  -> record execution, update delta and tape
     ├── FILL   -> reduce resting order size, remove if fully filled
     └── RESET  -> clear entire book, reset ID mappings
```

**Key data structures:**
- `bids` — `std::map` sorted high to low
- `asks` — `std::map` sorted low to high
- `order_map` — `unordered_map` keyed by internal ID, O(1) lookup
- `ext_to_int` — external to internal ID mapping for collision handling

---

## Why This Project Exists

Most retail trading tools show L2 data — price levels with aggregated sizes. What you cannot see from L2:

- Whether a large order is genuine or a decoy
- Whether the same participant places and cancels repeatedly
- Whether multiple orders at different levels come from the same actor
- Whether a fill actually happened or the order simply disappeared

L3 data exposes all of this. This project processes L3 at the order level and applies scoring logic similar to what exchange surveillance systems use to flag suspicious behaviour before it goes to a human reviewer.

---

## How Regulators Actually Catch Spoofing

Exchange surveillance systems flag orders automatically using similar signals. When a flag fires:

1. Exchange sends a suspicious activity report to the regulator
2. Regulator reconstructs the full book at that moment using stored L3 data
3. Account identity behind the order ID is revealed (we only see order IDs — they see the trader)
4. Chat logs, emails, and phone records are subpoenaed
5. Civil or criminal referral follows if intent is proven

Famous cases: Navinder Sarao (Flash Crash 2010, $38M fine), JPMorgan precious metals desk ($920M fine), Tower Research Capital ($67M fine).

Our detector performs step 1 — the automated flagging. Everything after requires regulatory access to identity data.

---

## Known Limitations

- **Source field identity**: Works best when source = unique participant ID. If all orders share one source string (e.g. data provider name), source-based layering groups all orders together.
- **Scientific notation IDs**: Collision resolution is LIFO — cancel events pick the most recently added order with that external ID when multiple collide.
- **CSV replay only**: No live feed support yet. Swap CSVFeed for a WebSocket reader to go live — OrderBook, Visualizer, and SpoofDetector require no changes.
- **Terminal rendering**: Requires ANSI-capable terminal, minimum 35 rows height.

---

## Roadmap

- [ ] Footprint chart — bid vs ask volume at each price level
- [ ] Volume profile — total volume per price across session
- [ ] Iceberg order detector — refill pattern at same level
- [ ] Absorption detection — large flow absorbed without price moving
- [ ] VWAP and standard deviation bands
- [ ] Live WebSocket feed — Binance free API as first target
- [ ] Web dashboard replacing terminal display
- [ ] ML classification model trained on confirmed spoof patterns

---

## Learning Resources

**Books**
- *Trading and Exchanges* — Larry Harris (market microstructure)
- *Market Microstructure Theory* — Maureen O'Hara
- *Flash Boys* — Michael Lewis (accessible introduction)

**Academic Papers**
- Cont, Kukanov, Stoikov (2014) — *The Price Impact of Order Book Events*
- Cao, Chen, Griffin (2005) — *Informational Content of Order Flow*

**Regulations**
- Dodd-Frank Act Section 747 — US spoofing prohibition
- MiFID II Article 12 — European market manipulation rules

---

## License

MIT License — free to use, modify, and distribute with attribution.

---

## Author

Built as an independent research project into market microstructure and L3 order flow analysis. Order book engine, visualizer, and detection logic written from scratch in C++17.

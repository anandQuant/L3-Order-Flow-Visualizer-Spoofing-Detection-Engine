# L3 Order Flow Visualizer + Spoofing Detector

A terminal program written in **C++17** that replays raw **Level 3 (L3)** market data, draws a live **Depth of Market (DOM)** screen, and scores every large order for **spoofing** behaviour while it runs.

> Built as a learning project in market microstructure: how order books work, and how surveillance systems spot manipulation.

**Jump to:**
[Quick Start](#quick-start) ·
[Your Data](#your-data-the-csv-file) ·
[Which Path Are You On?](#which-path-are-you-on) ·
[Reading the Screen](#reading-the-screen) ·
[How the Detector Thinks](#how-the-detector-thinks) ·
[Settings](#settings) ·
[Tune It](#tune-it-on-your-own-data) ·
[Troubleshooting](#troubleshooting) ·
[Limits](#limits-read-this)

---

## What Is This?

| Data level | What you see | Good for |
|---|---|---|
| L1 | Best bid and best ask only | Showing a price |
| L2 | Total size at each price | Depth of market |
| **L3** | **Every single order, at every price** | **Order flow and spotting manipulation** |

**Spoofing** means placing big orders you never plan to trade, to trick other traders into moving the price, then cancelling them. L3 data is the only kind detailed enough to see this, because you can watch each order from the moment it appears until it disappears.

**What this program does, in one line:** it reads a CSV of L3 events, rebuilds the order book, shows it live, and prints an alert whenever an order looks like a spoof.

---

## Quick Start

Tick the boxes as you go.

### Step 1: Get the files

- [ ] Download or clone this repo. You need these files in **one folder**: `main.cpp`, `L3_orderflow.cpp`, `spoof_detector.h`.

### Step 2: Check you have a C++ compiler

- [ ] Open a terminal and type `g++ --version`.

<details>
<summary><b>It printed a version</b> (click to see what's next)</summary>

Great, go to Step 3.
</details>

<details>
<summary><b>"g++ is not recognized" on Windows</b> (click to fix)</summary>

If you installed **Code::Blocks**, the compiler is already on your PC. Add it to your PATH for the current PowerShell window:

```powershell
$env:Path += ";C:\Program Files\CodeBlocks\MinGW\bin"
g++ --version
```

If that folder doesn't exist, search for it:

```powershell
Get-ChildItem "C:\Program Files\CodeBlocks","C:\Program Files (x86)\CodeBlocks" -Recurse -Filter g++.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
```

Use the folder it prints (without `g++.exe`). You must repeat the `$env:Path` line in every new PowerShell window.
</details>

<details>
<summary><b>"g++: command not found" on Linux / macOS</b> (click to fix)</summary>

- Ubuntu / Debian: `sudo apt install g++`
- macOS: `xcode-select --install`
</details>

### Step 3: Build

- [ ] Go to the project folder and compile **only `main.cpp`**. The other two files are included automatically.

```bash
# Linux / macOS
g++ -std=c++17 -O2 -o market main.cpp

# Windows (MinGW), in PowerShell
g++ -std=c++17 -O2 -o market.exe main.cpp
```

No output means success.

### Step 4: Run

- [ ] **Easiest way:** run it with nothing after the name. It asks which CSV file (type the path, or drag the file into the window) and which start time (UTC). It remembers both, so the next run is just ENTER, ENTER.

```bash
./market
```

On Windows: `.\market.exe`. The same works from Code::Blocks or a double-click.

- [ ] **Or give them on the command line.** Both are optional:

```bash
./market data.csv                 # starts at the time set inside main.cpp (default 14:00:00)
./market data.csv 00:00:00        # play from the very beginning  <-- good for a first test
./market data.csv 13:30:00        # jump to the New York open
```

On Windows use `.\market.exe "data.csv" 00:00:00`.

### Step 5: Use it

- [ ] The program prints the session info and waits 2 seconds.
- [ ] It **fast-forwards** silently to your start time. It also waits until the book looks healthy (at least 5 price levels per side and a spread of 5.0 or less).
- [ ] You see a **"JUMPED TO"** summary. **Press ENTER** to start the display.
- [ ] The live screen plays at real market speed. Alerts appear underneath.
- [ ] Press **Ctrl+C** to quit.

> **Tip:** use **Windows Terminal** or PowerShell 7 on Windows, and make the window at least **35 rows tall**.

---

## Your Data: The CSV File

You bring your own data. The repo does not include any.

### Required layout

Columns are read **by position**, not by name. The first line (header) is skipped. Tabs or commas both work.

| # | Column | Example | Notes |
|---|---|---|---|
| 1 | timestamp | `2026-04-05T13:29:43.776471934+0000` | ISO format with fractions of a second. Treated as UTC. |
| 2 | action | `Add` | `Add`, `Cancel`, `Modify`, `Trade`, `Fill`, `Reset` (also all-lowercase). Other spellings are skipped. |
| 3 | side | `Buy` | `Buy`, `buy` or `BUY` = buy. **Anything else is treated as sell.** |
| 4 | price | `25300.75` | Decimal |
| 5 | size | `5` | Contracts |
| 6 | order_id | `687509000001` | See the warning below |
| 7 | source | `TradingFirm_A` | Optional, but needed for one detector filter. See [Which Path Are You On?](#which-path-are-you-on) |

```
timestamp                           action  side  price     size  order_id      source
2026-04-05T13:29:43.776471934+0000  Add     Buy   25300.75  5     687509000001  TradingFirm_A
2026-04-05T13:29:43.776891234+0000  Add     Sell  25301.00  3     687509000002  TradingFirm_B
2026-04-05T13:29:43.812340000+0000  Trade   Buy   25301.00  2     0
2026-04-05T13:29:43.812340000+0000  Fill    Buy   25301.00  2     687509000002
2026-04-05T13:29:44.001230000+0000  Cancel  Buy   25300.75  5     687509000001
```

### What each action does

| Action | Meaning |
|---|---|
| `Add` | A new order joins the book |
| `Cancel` | An order is removed (matched by order_id) |
| `Modify` | An order gets a new price and size |
| `Trade` | An anonymous trade print. Updates the tape, volume and delta. order_id can be `0`. |
| `Fill` | A **resting order** was hit. order_id is that resting order. Its size shrinks. |
| `Reset` | Wipe the book and start fresh |

> **Warning: order IDs.** The program uses `order_id` exactly as it is read. If your file shows IDs in scientific notation (like `6.87509E+12`), different orders can end up with the same ID and overwrite each other. Use plain whole numbers.

<details>
<summary><b>Click to check your file with PowerShell</b></summary>

Change the column names if your header is different.

```powershell
$f = "data.csv"
$d = if ((Get-Content $f -TotalCount 1) -match "`t") { "`t" } else { "," }
$rows = Get-Content $f -TotalCount 200000 | ConvertFrom-Csv -Delimiter $d

"First 3 rows:"; $rows | Select-Object -First 3
"Distinct sources: " + ($rows | Group-Object source | Measure-Object).Count
"IDs in scientific notation: " + @($rows | Where-Object { $_.order_id -match "E" }).Count
```
</details>

---

## Which Path Are You On?

One detector filter (opposite-side execution) needs to know **who** placed each order. Check your `source` column and pick your row:

| Your `source` column | Path | What works |
|---|---|---|
| Missing, empty, or the **same value on every row** | **Path B: no identity** | All filters except opposite-side execution and the repeat signal |
| **Different values** for different participants | **Path A: identity available** | Everything |

The program tells you at the end of every run: if it prints a **NOTE** about the `source` column, you are on Path B.

<details>
<summary><b>Why can't the order ID tell who the trader is?</b></summary>

Every order gets its own unique ID, so two orders from the same trader have unrelated IDs. In public market data, the ID is just a number the exchange hands out. The firm, trader and account behind it are hidden. Only the exchange, brokers and regulators can see them.

So the detector can only link a trader's orders together if your data has a `source` column that says so.
</details>

<details>
<summary><b>How do investigators find a real spoofer?</b></summary>

1. The exchange's surveillance system flags a pattern (this program does this step).
2. The exchange pulls its full order log. Every order carries a firm / session / trader ID that the public feed hides. It shows that the big fake orders and the real trades came from the **same login**.
3. The broker links that login to a real customer account.
4. The regulator lines up orders, trades and profits, and checks whether it happened repeatedly.
5. They look for proof of **intent**: chats, emails, phone calls, trading-program code.
6. Penalties follow: fine, ban, or criminal referral.

This program covers step 1 only.
</details>

---

## Reading the Screen

```
 TIME       SIZE    PRICE  |ORD [BID DEPTH  ] BID SZ   PRICE  ASK SZ [ASK DEPTH] ORD
                           |                           25303.00   500 [######   ]  3
 13:29:44    50  25301.00 ^|                           25302.75   200 [###      ]  1
 13:29:44   200  25300.50 v| --- SPREAD 0.25  MID 25301.13 ---
 13:29:43   100  25301.00 v|  2 [##       ]  300  25301.00
                           |  5 [#########] 1000  25300.75
                           | IMB[----#####|--------] D:+11651  imb:+0.234
```

| Part | Meaning |
|---|---|
| **Left panel** | Time and Sales tape: every trade. `^` = buyer hit the offer, `v` = seller hit the bid. |
| **Right panel** | The DOM: asks on top (best ask at the bottom of that block), bids below. Each row shows order count, size and a depth bar. |
| **Title rows** | Feed time, last price, spread, mid price, total volume, buy volume, sell volume, and **delta** (buy volume minus sell volume). |
| **IMB bar** | Which side has more resting size in the top levels. `imb` runs from -1 (all asks) to +1 (all bids). |
| **Alerts box** | Appears under the screen once the detector finds something. |

---

## How the Detector Thinks

### The idea in four steps

1. **Watch** every big order from the moment it is placed.
2. **Wait.** When one is cancelled, hold on for 1 second of feed time. A fill might still arrive.
3. **Score** it from 0 to 100 using the eight questions below.
4. **Alert** if the score reaches `ALERT_THRESHOLD` (default **40**).

### Which orders get watched?

An order must pass all three gates:

| Gate | Rule | Default |
|---|---|---|
| **1. Not tiny** | Size at least `LARGE_ORDER_THRESHOLD` (a sanity floor only) | 10 |
| **2. Close to the market** (filter #2) | No more than `MAX_WATCH_DISTANCE_TICKS` behind the best price on its side | 20 ticks |
| **3. Big for the book** (filter #4) | At least `MIN_DEPTH_SHARE` of the **recent average** visible depth on its side. The average is smoothed over `AVG_DEPTH_WINDOW_S`, so it adapts to the instrument and the time of day, and a sudden thin moment does not make ordinary orders look big. | 3% of a 300 s average |

Optional: set `MIN_SIZE_RATIO` above 0 to also watch orders that are that many times their source's own median size. It is **off** by default, because it flags a fixed slice of every trader's orders and caused most of the false alerts in testing.

### The eight questions (max 100 points)

| Signal | Plain-English question | Max |
|---|---|---|
| **Opposite-side execution** (filter #1) | Did the same source get a real fill on the *other* side while this order sat there (or within 1s after)? | **25** |
| **Relative size** (filter #4) | How big is it versus the visible book and versus this trader's normal orders? | 15 |
| **Zero fills** | Was it cancelled without trading? (15 for none, 8 if less than 25% filled) | 15 |
| **Lifetime** | How fast was it pulled? (under 100ms = 10, under 500ms = 6, under 2s = 2) | 10 |
| **Price impact** | Did the mid price move the way a spoofer wants? Bid spoof wants **up**, ask spoof wants **down**. (2 ticks = 5, 4 ticks = 10) | 10 |
| **Closeness to the touch** (filter #2) | How near the best price was it placed? (1 tick = 10, up to 3 = 7, up to 6 = 4, further = 1) | 10 |
| **Cancel-on-approach** (filter #3) | Was it pulled just as the market ran toward it? (price moved 2+ ticks closer; full marks if now within 3 ticks) | 10 |
| **Repeat** | Did this source already cancel 2+ big zero-fill orders in the last 60 seconds? | 5 |

Opposite-side execution and Repeat only work on **Path A** (see above).

### What an alert looks like

```
  +------ SPOOF ALERTS (1 total) ------+
  | 13:29:41 BID  src=FirmA px=24999.50 sz=40 life=100ms score=61
  |   [size=40 71.4%avgdepth x40.0med +15] [0fills +15] [life=100ms +6] [opp-side fills=3 +18] [dist=2.0tks +7]
  +----------------------------------------------+
```

Line 1: when, which side, who, price, size, how long it lived, total score.
Line 2: **why**, one tag per signal with the points it added.

| Tag | Means |
|---|---|
| `size=40 71.4%avgdepth x40.0med` | 40 lots, 71.4% of the recent average visible depth, 40 times this source's median order |
| `0fills` or `<25%filled` | Cancelled with no (or little) fill |
| `life=100ms` | How long it lived |
| `impact=2.0tks` | Mid moved 2 ticks in the spoofer's favour |
| `repeat=2` | Two earlier zero-fill cancels by this source |
| `opp-side fills=3` | 3 lots filled on the other side for the same source |
| `approach=3.0tks gap=1.0` | Market moved 3 ticks closer; it was cancelled 1 tick from the opposite touch |
| `dist=2.0tks` | Placed 2 ticks behind the best price on its side |

### How to read the score

Rules of thumb, not statistics:

| Score | Meaning |
|---|---|
| Below 40 | Not shown |
| 40 to 60 | Worth a look |
| 60 to 80 | Probably suspicious |
| 80 and up | Very strong spoof pattern |

> **A high score is a lead, not proof.** Genuine market makers also cancel fast. See [Limits](#limits-read-this).

<details>
<summary><b>For developers: how the pieces connect (click to expand)</b></summary>

```
CSV file ──> CSVFeed ──> main.cpp loop ──> OrderBook (L3 book) ──> Visualizer (screen)
                              │
                              └──> SpoofDetector (spoof_detector.h) ──> alerts
```

Call order inside the loop (see `main.cpp`):

```cpp
detector.tick(ts, time_str);          // every message, first
detector.on_add(book, order);         // AFTER  book.process(add)
detector.on_cancel(book, order, ts);  // BEFORE book.process(cancel)
detector.on_fill(order, qty, ts);     // BEFORE book.process(fill)
detector.on_reset();                  // on a Reset message
detector.flush();                     // at end of feed
```

The order matters. `on_cancel` and `on_fill` must run **before** the book changes, so the order is still there to read.

Data structures: `bids` is a `std::map` sorted high to low, `asks` sorted low to high, and `order_map` is an `unordered_map` from order ID to order.
</details>

---

## Settings

Everything is a constant at the top of a file. Change it, then rebuild (Step 3).

### I want to...

| I want to... | Change this | In |
|---|---|---|
| See **fewer** alerts | Raise `ALERT_THRESHOLD` (try 55 to 60) | `spoof_detector.h` |
| See **more** alerts | Lower `ALERT_THRESHOLD` | `spoof_detector.h` |
| Watch only **bigger** orders (less noise) | Raise `MIN_DEPTH_SHARE` (0.03 to 0.05) | `spoof_detector.h` |
| Watch **smaller** orders (more noise) | Lower `MIN_DEPTH_SHARE` (0.02) | `spoof_detector.h` |
| Data has **no source/participant column** | Lower `ALERT_THRESHOLD` to 30 to 35 (two signals are off) | `spoof_detector.h` |
| Use a different **instrument** | Set `TICK_SIZE` to its tick (NQ = 0.25) | `spoof_detector.h` |
| Start at a different **time** | Type it when asked, pass it as the 2nd argument, or edit `SKIP_TO_TIME` (the default) | `main.cpp` |
| Play **faster or slower** | `PLAYBACK_SPEED` (2.0 = twice as fast) | `main.cpp` |
| Stop long quiet gaps from freezing playback | Lower `MAX_GAP_NS` | `main.cpp` |
| Change the book-ready check | `MIN_DEPTH_LEVELS`, `MAX_WARMUP_SPREAD` | `main.cpp` |
| Show more or fewer price levels | `BOOK_DEPTH` | `L3_orderflow.cpp` |

<details>
<summary><b>All detector settings (click to expand)</b></summary>

| Constant | Default | What it does |
|---|---|---|
| `TICK_SIZE` | 0.25 | Smallest price step of the instrument |
| `ALERT_THRESHOLD` | 40 | Minimum score to show an alert |
| `LARGE_ORDER_THRESHOLD` | 10 | Sanity floor: smallest order size ever watched |
| `DEPTH_LEVELS` | 10 | Price levels used to measure visible depth |
| `MIN_DEPTH_SHARE` | 0.03 | Watch if size is at least this share of the recent average side depth |
| `AVG_DEPTH_WINDOW_S` | 300 | Smoothing time (feed seconds) of that average depth. 60 to 300 behaved the same in tests |
| `MIN_SIZE_RATIO` | 0 (off) | Optional: ...or at least this many times the source's median size |
| `SIZE_HISTORY` / `MIN_SIZE_HISTORY` | 50 / 8 | How many past orders feed that median / how many are needed |
| `MAX_WATCH_DISTANCE_TICKS` | 20 | Ignore orders further behind the best price than this |
| `MIN_LIFETIME_NS` | 500 ms | Limit for the "fast cancel" signal |
| `PRICE_IMPACT_TICKS` | 2.0 | Mid move needed for price-impact points |
| `OPP_LOOKBACK_NS` | 5 s | How far back to look for opposite-side fills |
| `OPP_POST_CANCEL_NS` | 1 s | How long to wait after a cancel for opposite-side fills |
| `APPROACH_MIN_TICKS` | 2.0 | How much closer the market must come |
| `APPROACH_NEAR_TICKS` | 3.0 | Distance for full approach points |
| `REPEAT_WINDOW_NS` | 60 s | Window for the repeat signal |
</details>

---

## Tune It On Your Own Data

Do this once per dataset. It takes about ten minutes.

1. **Check your file** with the PowerShell snippet in [Your Data](#your-data-the-csv-file). Note the number of distinct sources.
2. **Set `TICK_SIZE`** for your instrument.
3. **Check the size floor.** `MIN_DEPTH_SHARE` (3% of the recent average depth) adapts to your market by itself, so you normally leave it alone. `LARGE_ORDER_THRESHOLD` (10) is only a floor. Look at how big your orders are, and change the floor only if 10 is far above or far below a normal order:

   ```powershell
   $f = "data.csv"
   $d = if ((Get-Content $f -TotalCount 1) -match "`t") { "`t" } else { "," }
   $rows = Get-Content $f -TotalCount 200000 | ConvertFrom-Csv -Delimiter $d
   $sizes = $rows | Where-Object { $_.action -eq "Add" } | ForEach-Object { [double]$_.size } | Sort-Object
   foreach ($p in 50,90,99,99.9) { "{0}th percentile size: {1}" -f $p, $sizes[[math]::Min($sizes.Count-1,[int]($sizes.Count*$p/100))] }
   ```
4. **Run once with `ALERT_THRESHOLD = 30`** to see what scores real flagged orders get. If your file has **no usable `source` column**, 30 to 35 is the normal setting, not just a test.
5. **Raise `MIN_DEPTH_SHARE` (0.05) or the threshold** until you only get a handful of alerts per session that you would genuinely want to review. Lowering `MIN_DEPTH_SHARE` to 0.02 catches smaller spoof orders but adds noise; 0.01 was far too noisy in tests.
6. **Check the top alerts by hand.** Find the time in the CSV and read the order flow around it. Does it really look like a spoof?

---

## Troubleshooting

<details>
<summary><b>"market.exe is not recognized"</b></summary>

You are in the wrong folder, or you have not built it yet. Go to the project folder first:

```powershell
cd "D:\path\to\your\project"
g++ -std=c++17 -O2 -o market.exe main.cpp
.\market.exe "data.csv" 00:00:00
```

If you built with Code::Blocks, the program is in a `bin\Debug` or `bin\Release` folder instead, and has your project's name.
</details>

<details>
<summary><b>"Cannot open: D:\..."</b></summary>

The path is wrong or the file is not there. The message shows what the program tried, inside `[ ]`, and then asks again: type the correct path or drag the file into the window. If a launcher glued the path and the time together, or split a path with spaces into pieces, the program repairs that by itself.

Make sure the CSV is where you say it is. A path with spaces is fine, typed or dragged in.
</details>

<details>
<summary><b>The screen is full of <code>←[2J</code> and odd characters</b></summary>

Your console is not drawing ANSI colour codes. Open **Windows Terminal** from the Start menu and run it there. The program already turns on ANSI support on Windows, but the old console window (and some IDE run windows) can ignore it.
</details>

<details>
<summary><b>It stays on "Fast-forwarding..." or jumps straight to [END OF FEED]</b></summary>

The start time is after the end of your data, or the book never became "ready" (5 levels per side, spread 5.0 or less). Try:

```powershell
.\market.exe "data.csv" 00:00:00
```

Or loosen `MIN_DEPTH_LEVELS` and `MAX_WARMUP_SPREAD` in `main.cpp`.
</details>

<details>
<summary><b>The book looks wrong, or everything is on the sell side</b></summary>

Check your CSV spelling. Actions must be written like `Add` or `add`, and side must be `Buy`, `buy` or `BUY`. Any other side text is treated as **sell**. Rows with unknown actions are skipped silently.
</details>

<details>
<summary><b>The screen works but there are no alerts</b></summary>

That can simply mean there was no spoofing. If you want to be sure the detector is looking:

- Is `LARGE_ORDER_THRESHOLD` higher than almost every order in your file, or `MIN_DEPTH_SHARE` too high? Lower it.
- Is your data very quiet (volume 0, tiny orders)? Try a busier time window.
- Try `ALERT_THRESHOLD = 30` once.
- Look for the NOTE at the end about the `source` column. Without identity, the two strongest signals are off.
</details>

<details>
<summary><b>Far too many alerts</b></summary>

Raise `MIN_DEPTH_SHARE` (try 0.05) and `ALERT_THRESHOLD`. Fast-cancelling market makers can look like spoofers on their own. See [Limits](#limits-read-this).
</details>

<details>
<summary><b>Linker error about threads</b></summary>

Build with `-pthread`:

```bash
g++ -std=c++17 -O2 -pthread -o market main.cpp
```
</details>

<details>
<summary><b>The display is cut off or garbled</b></summary>

Make the terminal window at least 35 rows tall, then restart the program.
</details>

---

## Limits (Read This)

- **A score is not proof.** Real cases need intent, account identity and records that only exchanges, brokers and regulators have. Treat alerts as a list to review.
- **Identity depends on your data.** Without a useful `source` column, opposite-side execution and the repeat signal cannot work.
- **Market makers can look like spoofers.** They also place and cancel fast. A per-source baseline would help, and is not built yet.
- **CSV replay only.** There is no live feed yet.
- **Order IDs are used as they are.** Duplicate or scientific-notation IDs will confuse the book.
- **The fast-forward part is not scanned.** Orders added before the jump are not watched.
- **Modify events are not scored**, so "shrink instead of cancel" tricks are missed. An alert also shows the order's price **when it was added**, and its price impact is measured from that moment, so an order that was moved with Modify shows a stale price.
- **The average-depth yardstick needs a minute or two to warm up.** Start the detector on a fully built book (the fast-forward does this). If a file's opening snapshot is still loading when the detector starts, early orders can look large against a half-built book.
- **Smaller spoof orders can slip under the 3% gate.** Orders that are only 2% to 3% of the average depth are not watched unless you lower `MIN_DEPTH_SHARE`.
- **One instrument, one tick size.** `TICK_SIZE` is a single constant.
- **Alerts arrive about 1 second (of feed time) after the cancel**, because the detector waits for late fills.
- **Terminal needs ANSI support** and at least 35 rows.
- **Not built yet:** layering-group detection and order-to-trade-ratio tracking.

---

## Project Files

| File | What it is |
|---|---|
| `main.cpp` | Entry point. Reads the CSV, runs the loop, handles fast-forward and playback speed. **Build this one.** |
| `L3_orderflow.cpp` | The order book engine, the CSV reader and the terminal display. Included by `main.cpp`. |
| `spoof_detector.h` | The spoof detector and all its settings. Included by `main.cpp`. |
| `L3 Visualizer.cbp` | Code::Blocks project file. Add only `main.cpp` to the build. |
| `LICENSE` | MIT license |

<details>
<summary><b>Using Code::Blocks</b></summary>

1. Open `L3 Visualizer.cbp`.
2. Make sure **only `main.cpp`** is built. Right-click `L3_orderflow.cpp`, choose **Properties**, and untick all build targets.
3. Press **Ctrl+F9** to build. The program ends up in a `bin` folder inside the project.
4. Press **Ctrl+F11** to build and run. Leave **Project → Set programs' arguments** empty. The program asks which CSV file and which start time, so the same project works for any CSV.
5. If the run window does not draw colours, run it from a terminal instead (Step 4).
</details>

<details>
<summary><b>What changed in this version</b></summary>

**New filters:** opposite-side execution, closeness to the touch, cancel-on-approach, and relative size.

**Changes to existing behaviour:**
- The **price-impact signal was backwards** (it rewarded price moving *toward* the order). It now rewards price moving in the spoofer's favour.
- The score budget was rebalanced to make room (still 100 maximum).
- `LARGE_ORDER_THRESHOLD` is now only a minimum size (a sanity floor).
- **v2.1:** the size gate is now "at least 3% of the recent average visible depth" (`MIN_DEPTH_SHARE`, `AVG_DEPTH_WINDOW_S`), so there is no instrument-specific contract count to tune. The old "or 4 times the source's median" branch is off by default (`MIN_SIZE_RATIO = 0`).
- Alerts now show the time and the source.
- Fully filled orders are now released from the watch list.
- The repeat signal now counts only earlier zero-fill cancels within 60 seconds.
- Alerts are delayed about 1 second so late fills can be counted.
</details>

---

## How Regulators Catch Spoofing

Exchange surveillance systems flag orders with signals like the ones above. After a flag, the exchange hands the full order log to the regulator, the broker identifies the account behind the login, and investigators gather communications to prove intent. Well-known cases include Navinder Sarao (Flash Crash 2010, $38M), the JPMorgan precious metals desk ($920M), and Tower Research Capital ($67M).

---

## Roadmap

- [x] Opposite-side execution, distance from touch, cancel-on-approach, relative size
- [ ] Handle Modify events (shrink or reprice instead of cancel)
- [ ] Layering-group detector (several same-side orders cancelled together)
- [ ] Order-to-trade ratio per source
- [ ] Per-source baseline to reduce market-maker false positives
- [ ] Pseudo-identity for anonymous data (group orders that act like one trader)
- [ ] Footprint chart and volume profile
- [ ] Iceberg and absorption detection
- [ ] Live WebSocket feed (Binance free API as a first target)
- [ ] Web dashboard instead of the terminal
- [ ] ML model trained on confirmed spoof patterns

---

## Learning Resources

**Books**
- *Trading and Exchanges* by Larry Harris (market microstructure)
- *Market Microstructure Theory* by Maureen O'Hara
- *Flash Boys* by Michael Lewis (easy introduction)

**Papers**
- Cont, Kukanov, Stoikov (2014): *The Price Impact of Order Book Events*
- Cao, Chen, Griffin (2005): *Informational Content of Order Flow*

**Rules**
- Dodd-Frank Act Section 747 (US spoofing ban)
- EU Market Abuse Regulation, Article 12 (market manipulation)

---

## License

MIT License. Free to use, modify and distribute with attribution.

## Author

Built as an independent research project into market microstructure and L3 order flow analysis. The order book engine, visualizer and detection logic are written from scratch in C++17.

all rights reserves to anand (builder of this repo)

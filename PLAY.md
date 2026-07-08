# How to Play — 2-Player Stock Face-Off

FIFA-style trading duel: you and a friend each pick a stock or crypto,
get the same starting cash, and trade its **real live price** for a
timed match. Whoever grows their P&L more wins.

This guide walks through everything from a blank computer to playing
your first match. No prior experience needed — just follow the steps
in order. For deeper technical detail see [RunMe.md](RunMe.md); for the
full design history see [changes.md](changes.md).

---

## Part 1 — Install the tools you need (one-time)

You need two programs installed on the computer that will **host** the
game (the other player doesn't need to install anything if you're
playing over Wi-Fi — see Part 4).

### 1a. Install uv (runs the game's backend)

1. Go to **https://docs.astral.sh/uv/getting-started/installation/**
2. Follow the install instructions for your operating system (on
   Windows, this is usually one command you paste into PowerShell).
3. Close and reopen your terminal/PowerShell window after installing.
4. Check it worked by typing:
   ```powershell
   uv --version
   ```
   You should see something like `uv 0.11.8`. If you see an error, uv
   didn't install correctly — retry step 2.

### 1b. Install Node.js (runs the game's screen/interface)

1. Go to **https://nodejs.org/** and download the **LTS** version (the
   left, recommended button).
2. Run the installer, clicking "Next" through the defaults.
3. Close and reopen your terminal.
4. Check it worked:
   ```powershell
   node --version
   npm --version
   ```
   Both should print a version number.

That's it for installs. You do **not** need to install a compiler,
Postgres, or anything else to play — see Part 2 for what's optional.

---

## Part 2 — Set up the project (one-time)

Open a terminal/PowerShell window and navigate into the project folder
(replace the path with wherever you put it):

```powershell
cd "C:\0.Coding_pt2\ProfitPlayground antigrav"
```

Everything below is run from this folder.

### 2a. Install the backend's Python packages

```powershell
uv sync --project api
```

This downloads everything the game server needs. It only needs to be
done once (or again later if the project's dependencies change).

### 2b. Install the frontend's packages

```powershell
cd frontend
npm install
cd ..
```

(The `cd ..` at the end brings you back to the project's main folder —
you'll need to be there for the next steps.)

### 2c. Set up the configuration file

Copy the example settings file:

```powershell
copy api\.env.example api\.env
```

Now open the new file `api\.env` in any text editor (Notepad works
fine) and check it looks like this:

```
API_TOKEN=dev-token
DATABASE_URL=
HOST=127.0.0.1
PORT=8080
DEV=1
```

**About `DATABASE_URL` — do you need a database?**

No, you don't. A database (Postgres) is only used to permanently save
game history to disk — order-by-order records of every trade ever made.
The game itself (rooms, live matches, scores) works 100% without it,
because that state already lives in the server's memory while it's
running.

- **Leave `DATABASE_URL` blank** (as shown above) — simplest option,
  nothing extra to install, the game works exactly the same for
  playing. This has been tested and works correctly (the server reports
  `"db":"disabled"` and everything still functions).
- **If you already have Postgres installed and running** on your
  computer, you *can* point `DATABASE_URL` at it instead, and every
  order/trade will be saved permanently. This was also tested — with a
  local Postgres running, the format is:
  ```
  DATABASE_URL=postgresql+psycopg://<username>:<password>@localhost:5432/<database_name>
  ```
  You'd need to have already created that database in Postgres first
  (e.g. `CREATE DATABASE profitplayground;`). If this sounds unfamiliar,
  just leave `DATABASE_URL` blank — you're not missing any gameplay by
  skipping it.

**Optional — unlock real stocks, not just crypto:**

By default the game only lets you pick crypto (Bitcoin, Ethereum,
Solana) — these need no signup and their prices are always live, day or
night. If you also want real stocks (NVIDIA, Tesla, Meta, JPMorgan,
etc.) on the picker:

1. Go to **https://finnhub.io/** and click "Get free API key" (just
   needs an email).
2. Copy the key it gives you.
3. Add this line to `api\.env`:
   ```
   FINNHUB_API_KEY=paste_your_key_here
   ```

If you skip this, the stock instruments simply won't show up on the
pick screen — crypto still works fine, and it's the easier option since
it moves at any time of day.

### 2d. Build the trading engine (one-time)

The actual matching engine (the thing that processes buy/sell orders)
is written in C for speed, and needs to be compiled once:

```powershell
mingw32-make dll
```

If that command isn't found, try:
```powershell
make dll
```

You should see it print a build command and finish with no errors. If
it fails, see the Troubleshooting table at the bottom.

---

## Part 3 — Start the game (every time you want to play)

You need **two terminal windows open at the same time**, both running
something. Leave both open for the whole game session.

### Terminal window 1 — the game server (backend)

```powershell
cd "C:\0.Coding_pt2\ProfitPlayground antigrav"
uv run --project api python -m api.run
```

Wait until you see a line like:
```
INFO:     Uvicorn running on http://127.0.0.1:8080
```
That means the backend is up. **Leave this window open** — closing it
ends the game for everyone.

### Terminal window 2 — the game screen (frontend)

Open a **second, separate** terminal window (don't close the first
one!) and run:

```powershell
cd "C:\0.Coding_pt2\ProfitPlayground antigrav\frontend"
npm run dev
```

Wait until you see:
```
Local:   http://localhost:5173/
```
**Leave this window open too.**

You now have both pieces running. Move on to Part 4 to actually connect
and play.

---

## Part 4 — Connect two players

Pick whichever situation matches you and your friend:

### Option A — Same computer (easiest, for testing solo)

1. Open your web browser.
2. Go to `http://localhost:5173` — this is Player 1's window.
3. Open a **second browser tab** (or a second browser entirely) and go
   to the same address `http://localhost:5173` — this is Player 2's
   window.
4. Keep both tabs open side by side and play the match between them.

### Option B — Two computers on the same Wi-Fi (most common for playing with a friend in person)

1. On the **host computer** (the one running both terminal windows from
   Part 3), stop the frontend if it's running (click into that terminal
   and press `Ctrl+C`), then restart it with an extra flag:
   ```powershell
   npm run dev -- --host
   ```
2. Find the host computer's local network address. In a new terminal:
   ```powershell
   ipconfig
   ```
   Look for a line under your active network adapter that says
   `IPv4 Address` — it looks like `192.168.1.42`. Write this down.
3. On the **host computer's browser**, go to `http://localhost:5173`
   (Player 1).
4. On the **friend's computer or phone** (must be on the **same Wi-Fi
   network** as the host), open a browser and go to
   `http://192.168.1.42:5173` — but use the actual IP address you found
   in step 2, not this example one. This is Player 2.

### Option C — Playing over the internet (friend is somewhere else)

This needs one extra piece of software because your home computer
usually isn't directly reachable from the internet. The easiest option
is a "tunnel" tool such as **ngrok** or a **Cloudflare Tunnel**, pointed
at port `5173` on the host computer. This is a more advanced step —
if you want to do this, ask and step-by-step instructions can be added
here for whichever tool you'd like to use.

---

## Part 5 — Play a match

1. **Player 1** picks a match length — 5, 10, or 15 minutes — and
   clicks **Create match**. A 5-character room code appears at the top
   of the screen (something like `J9SE8`). Click it to copy it.
2. **Player 1** sends that code to **Player 2** however you'd like
   (text message, Discord, shout across the room).
3. **Player 2** types the code into the **Join with a code** box on
   their screen and clicks **Join**.
4. Both players now land on the **pick screen**, showing a list of
   instruments (BTC, ETH, and stocks if you added the Finnhub key) with
   their current live prices and today's % change. Each player clicks
   whichever one they want to trade. You can both pick the same one, or
   different ones — it doesn't matter, it's not shared.
5. The instant **both** players have picked, the match starts
   automatically:
   - Both players get the exact same starting cash.
   - A countdown clock starts at the top.
   - Each player gets their **own private order book** that
     continuously quotes the real live market price.
6. **To trade:** use the order ticket on the right side of the screen.
   - Choose **Buy** or **Sell**.
   - Choose an order type: **LIMIT** (set your own price), **MARKET**
     (trade instantly at the current price), **IOC**, or **FOK**.
   - Set a price (if using LIMIT) and a quantity.
   - Click the big Buy/Sell button at the bottom.
7. Watch the **scoreline** at the top of the screen the whole match —
   it shows both players' current profit/loss ("net P&L") side by side,
   updating live, with whoever's currently ahead highlighted.
8. When the countdown clock hits `0:00`, trading locks automatically and
   a **winner screen** pops up showing both players' final P&L and who
   won (or if it was a draw).
9. Click **Back to lobby** to return to the start screen and set up a
   new match — you can immediately create or join another one.

---

## Troubleshooting

| Symptom | What's happening | Fix |
|---|---|---|
| `uv: command not found` (or similar) | uv isn't installed, or your terminal needs restarting | Reinstall uv (Part 1a), then fully close and reopen your terminal |
| `npm: command not found` | Node.js isn't installed correctly | Reinstall Node.js (Part 1b), then reopen your terminal |
| "Room not found" when joining | The code is wrong, or the server was restarted since the room was created | Double check the code with whoever created the room; if the backend terminal was closed/restarted, all rooms are gone — create a new one |
| Backend terminal was accidentally closed mid-game | Rooms and matches only exist while that terminal is running (nothing is saved unless you set up a database) | Reopen Terminal window 1 from Part 3 and start a new match |
| No stocks show up in the picker, only BTC/ETH/SOL | You haven't added a `FINNHUB_API_KEY` | Either add one (Part 2c) or just play with crypto — it works the same |
| "Match not live" message when trying to trade | Not both players have picked an instrument yet | Check the pick screen — both players need a green checkmark before trading starts |
| Player 2's browser can't load the page at all (Option B) | Not on the same Wi-Fi, wrong IP address, or frontend wasn't restarted with `--host` | Re-check `ipconfig` for the correct IP, confirm both devices show the same Wi-Fi network name, and make sure you used `npm run dev -- --host` (not just `npm run dev`) |
| `mingw32-make dll` (or `make dll`) fails with a compiler error | Windows compiler issue — see the notes in RunMe.md | Read the "Build the C engine DLL" section in [RunMe.md](RunMe.md) — there's a known workaround already documented there |
| Server shows `"db":"disabled"` when checking `/ready` | This is expected and fine if you left `DATABASE_URL` blank | No action needed — this just means game history isn't being saved to disk, which doesn't affect gameplay |

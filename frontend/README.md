# ProfitPlayground Frontend

A 2-player trading dashboard: dark theme, live order book ladder, canvas
price chart, order ticket, trade tape, and positions — all driven by the
FastAPI layer's REST + WebSocket surface (`api/`).

No heavy chart library — the price chart is a small dependency-free
canvas component fed by the live trade tape, kept deliberately tiny so the
whole app stays fast for two players trading against each other in real
time.

## Run

Requires the API server running first — see the repo root
[README.md](../README.md) Quick Start (build `bridge/libpipeline.dll`,
then `uv run --project api python -m api.run` on port 8080).

```powershell
cd frontend
npm install
npm run dev
```

Open http://localhost:5173. Vite's dev server proxies `/api/*` to
`http://127.0.0.1:8080` and proxies `/ws` to the same host as a
WebSocket — no CORS configuration needed, and no separate `.env` required
to get started (defaults to the `dev-token` bearer token baked into the
API's own `.env.example`). Copy `.env.example` to `.env` only if you
changed `API_TOKEN` on the server.

## How two players use it

Each player opens the app in their own browser (or tab) and picks
**Player 1** or **Player 2** from the account switcher in the top bar —
this choice persists per-browser via `localStorage`. Both players share
the same live order book: an order placed by Player 1 can be filled by
Player 2's crossing order, and both sides see the resulting trade, book
update, and portfolio change immediately over the shared WebSocket feed.

## Layout

- **Left rail** — minimal icon nav (visual only for now).
- **Top bar** — symbol tabs (AAPL/MSFT/GOOG), live cash/position/P&L
  stats for the active player, account switcher, connection indicator.
- **Order book ladder** — top-20 bids/asks with depth bars; click a row
  to prefill that price into the order ticket.
- **Price chart** — canvas line chart built from the live trade tape.
- **Trade tape** — recent fills, colored by aggressor side.
- **Order ticket** — Buy/Sell, order type (LIMIT/MARKET/IOC/FOK), price
  and quantity steppers, submit.
- **Positions strip** — cash, position, avg price, and P&L per symbol for
  the active player.

Responsive: below ~980px the grid collapses to a single column
(ticket → chart → ladder → tape → positions) so it works on a phone or a
split-screen laptop window for two players sitting side by side.

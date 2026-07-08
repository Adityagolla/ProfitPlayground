import { useEffect, useRef, useState, useCallback } from "react";
import { currentAuthToken, getBookSnapshot, getPortfolio, getTrades } from "../api";
import type { BookSnapshot, OrderView, PortfolioView, Trade, WsEnvelope } from "../types";

const MAX_TRADES = 200;
const RECONNECT_BASE_MS = 500;
const RECONNECT_MAX_MS = 8000;

export type ConnState = "connecting" | "open" | "closed";

/** Trade + client-side receive time — server ts_ns is monotonic, not
 * wall-clock, so it's useless for a human-readable timestamp. */
export interface TradeWithRecv extends Trade {
  _recvMs: number;
}

interface EngineState {
  connState: ConnState;
  book: BookSnapshot | null;
  trades: TradeWithRecv[];
  portfolio: PortfolioView | null;
  orders: Map<number, OrderView>;
  lastTradePrice: number | null;
}

const initialState: EngineState = {
  connState: "connecting",
  book: null,
  trades: [],
  portfolio: null,
  orders: new Map(),
  lastTradePrice: null,
};

/**
 * Owns one WebSocket connection for the given (symbol, accountId) pair.
 * Re-subscribes automatically on symbol/account change or reconnect.
 * Order-book/trades are public; portfolio/orders are private and filtered
 * client-side to the active accountId (the server's "orders" channel is
 * not itself scoped to one account).
 */
export function useEngineSocket(symbol: string, accountId: number) {
  const [state, setState] = useState<EngineState>(initialState);
  const wsRef = useRef<WebSocket | null>(null);
  const attemptRef = useRef(0);
  const closedByUsRef = useRef(false);

  const reset = useCallback(() => {
    setState({ ...initialState, connState: "connecting" });
  }, []);

  useEffect(() => {
    // Lobby / pick screens pass an empty symbol — nothing to stream yet.
    if (!symbol) {
      setState({ ...initialState, connState: "closed" });
      return;
    }
    closedByUsRef.current = false;
    attemptRef.current = 0;
    reset();

    // The "portfolio"/"orders" WS channels only push on new events (no
    // subscribe-time snapshot), so hydrate once via REST; the book gets a
    // WS snapshot almost immediately but fetching it too avoids a blank
    // flash on slow connections.
    let cancelled = false;
    Promise.all([
      getBookSnapshot(symbol).catch(() => null),
      getTrades(symbol).catch(() => null),
      getPortfolio(accountId).catch(() => null),
    ]).then(([book, tradesPage, portfolio]) => {
      if (cancelled) return;
      const now = Date.now();
      setState((s) => ({
        ...s,
        book: book ?? s.book,
        trades: tradesPage?.items.map((t) => ({ ...t, _recvMs: now })) ?? s.trades,
        portfolio: portfolio ?? s.portfolio,
      }));
    });

    let ws: WebSocket;
    let reconnectTimer: ReturnType<typeof setTimeout> | undefined;

    function connect() {
      const proto = location.protocol === "https:" ? "wss" : "ws";
      ws = new WebSocket(`${proto}://${location.host}/ws`);
      wsRef.current = ws;
      setState((s) => ({ ...s, connState: "connecting" }));

      ws.onopen = () => {
        attemptRef.current = 0;
        // Player room token when in a match (server derives identity from
        // it), admin dev token otherwise.
        ws.send(JSON.stringify({ type: "auth", token: currentAuthToken() }));
        ws.send(JSON.stringify({ type: "subscribe", channel: "orderbook", symbol, depth: 20 }));
        ws.send(JSON.stringify({ type: "subscribe", channel: "trades", symbol }));
        ws.send(JSON.stringify({ type: "subscribe", channel: "portfolio", user_id: accountId }));
        ws.send(JSON.stringify({ type: "subscribe", channel: "orders" }));
        setState((s) => ({ ...s, connState: "open" }));
      };

      ws.onmessage = (ev) => {
        let msg: WsEnvelope;
        try {
          msg = JSON.parse(ev.data);
        } catch {
          return;
        }

        if (msg.event === "ping") {
          ws.send(JSON.stringify({ type: "pong" }));
          return;
        }

        if (msg.channel === "orderbook" && (msg.event === "snapshot" || msg.event === "delta")) {
          const data = msg.data as { bids: BookSnapshot["bids"]; asks: BookSnapshot["asks"] };
          setState((s) => ({
            ...s,
            book: { symbol, bids: data.bids, asks: data.asks, seq: msg.seq ?? 0, ts_ns: msg.ts_ns ?? 0 },
          }));
          return;
        }

        if (msg.event === "trade" && msg.channel === "trades") {
          const trade: TradeWithRecv = { ...(msg.data as Trade), _recvMs: Date.now() };
          setState((s) => ({
            ...s,
            lastTradePrice: trade.price,
            trades: [trade, ...s.trades].slice(0, MAX_TRADES),
          }));
          return;
        }

        if (msg.event === "order" && msg.channel === "orders") {
          const order = msg.data as OrderView;
          if (order.account_id !== accountId) return; // private — filter to active player
          setState((s) => {
            const next = new Map(s.orders);
            next.set(order.server_order_id, order);
            return { ...s, orders: next };
          });
          // The C engine doesn't push portfolio events — any state change on
          // one of our orders (fill/cancel) can move cash/positions, so
          // re-hydrate the portfolio via REST.
          getPortfolio(accountId)
            .then((p) => setState((s) => ({ ...s, portfolio: p })))
            .catch(() => undefined);
          return;
        }

        if (msg.event === "portfolio" && msg.channel === "portfolio") {
          setState((s) => ({ ...s, portfolio: msg.data as PortfolioView }));
          return;
        }
      };

      ws.onclose = () => {
        setState((s) => ({ ...s, connState: "closed" }));
        if (closedByUsRef.current) return;
        const delay = Math.min(RECONNECT_BASE_MS * 2 ** attemptRef.current, RECONNECT_MAX_MS);
        attemptRef.current += 1;
        reconnectTimer = setTimeout(connect, delay);
      };

      ws.onerror = () => ws.close();
    }

    connect();

    return () => {
      cancelled = true;
      closedByUsRef.current = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      wsRef.current?.close();
    };
  }, [symbol, accountId, reset]);

  return state;
}

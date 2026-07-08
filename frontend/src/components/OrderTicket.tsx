import { useEffect, useState } from "react";
import { ApiError, newClientOrderId, submitOrder } from "../api";
import { PRICE_SCALE } from "../format";
import type { OrderSide, OrderType } from "../types";

interface Props {
  symbol: string;              // engine symbol (room-scoped)
  displaySymbol?: string;      // human label, e.g. "BTCUSDT"
  accountId: number;
  prefillPrice: number | null;
  disabled?: boolean;          // match not live (full time / lobby)
}

const TYPES: OrderType[] = ["LIMIT", "MARKET", "IOC", "FOK"];

export function OrderTicket({ symbol, displaySymbol, accountId, prefillPrice,
                              disabled = false }: Props) {
  const [side, setSide] = useState<OrderSide>("BID");
  const [type, setType] = useState<OrderType>("LIMIT");
  const [price, setPrice] = useState("100.00");
  const [qty, setQty] = useState(10);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<{ kind: "ok" | "err"; text: string } | null>(null);

  // Clicking a ladder row fills the price field (fast workflow, LIMIT only).
  useEffect(() => {
    if (prefillPrice != null) setPrice((prefillPrice / PRICE_SCALE).toFixed(2));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [prefillPrice]);

  const priceInt = Math.round(parseFloat(price || "0") * PRICE_SCALE);
  const canSubmit = qty > 0 && (type === "MARKET" || priceInt > 0) && !busy && !disabled;

  async function handleSubmit() {
    setBusy(true);
    setNote(null);
    try {
      const ack = await submitOrder({
        client_order_id: newClientOrderId(),
        account_id: accountId,
        symbol,
        type,
        side,
        price: type === "MARKET" ? 0 : priceInt,
        qty,
      });
      if (ack.status === "ACCEPTED") {
        setNote({ kind: "ok", text: `Order #${ack.server_order_id} accepted` });
      } else {
        setNote({ kind: "err", text: ack.reject_reason ?? ack.status });
      }
    } catch (e) {
      setNote({ kind: "err", text: e instanceof ApiError ? e.message : "network error" });
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="panel ticket">
      <div className="panel-header">Order Ticket</div>
      <div className="ticket-body">
        <div className="side-toggle">
          <button
            className={`side-btn buy${side === "BID" ? " active" : ""}`}
            onClick={() => setSide("BID")}
          >
            Buy
          </button>
          <button
            className={`side-btn sell${side === "ASK" ? " active" : ""}`}
            onClick={() => setSide("ASK")}
          >
            Sell
          </button>
        </div>

        <div className="field">
          <span className="field-label">Order Type</span>
          <div className="type-select">
            {TYPES.map((t) => (
              <button
                key={t}
                className={`type-opt${t === type ? " active" : ""}`}
                onClick={() => setType(t)}
              >
                {t}
              </button>
            ))}
          </div>
        </div>

        <div className="field">
          <span className="field-label">Price</span>
          <div className="stepper">
            <button onClick={() => setPrice((p) => (parseFloat(p || "0") - 0.01).toFixed(2))} disabled={type === "MARKET"}>
              −
            </button>
            <input
              value={type === "MARKET" ? "Market" : price}
              onChange={(e) => setPrice(e.target.value)}
              disabled={type === "MARKET"}
              inputMode="decimal"
            />
            <button onClick={() => setPrice((p) => (parseFloat(p || "0") + 0.01).toFixed(2))} disabled={type === "MARKET"}>
              +
            </button>
          </div>
        </div>

        <div className="field">
          <span className="field-label">Quantity</span>
          <div className="stepper">
            <button onClick={() => setQty((q) => Math.max(1, q - 1))}>−</button>
            <input
              value={qty}
              onChange={(e) => setQty(Math.max(0, parseInt(e.target.value, 10) || 0))}
              inputMode="numeric"
            />
            <button onClick={() => setQty((q) => q + 1)}>+</button>
          </div>
        </div>

        <button
          className={`submit-btn ${side === "BID" ? "buy" : "sell"}`}
          disabled={!canSubmit}
          onClick={handleSubmit}
        >
          {disabled
            ? "Match not live"
            : busy
              ? "Submitting…"
              : `${side === "BID" ? "Buy" : "Sell"} ${displaySymbol ?? symbol}`}
        </button>

        <div className={`ticket-note ${note?.kind === "err" ? "err" : note?.kind === "ok" ? "ok" : ""}`}>
          {note?.text ?? ""}
        </div>
      </div>
    </section>
  );
}

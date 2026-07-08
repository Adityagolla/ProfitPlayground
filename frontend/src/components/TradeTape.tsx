import type { TradeWithRecv } from "../hooks/useEngineSocket";
import { fmtPrice, fmtQty, fmtTime } from "../format";

interface Props {
  trades: TradeWithRecv[];
}

export function TradeTape({ trades }: Props) {
  return (
    <section className="panel tape">
      <div className="panel-header">Recent Trades</div>
      <div className="tape-body">
        {trades.length === 0 && (
          <div className="chart-empty" style={{ position: "static", padding: 20 }}>
            No trades yet
          </div>
        )}
        {trades.map((t) => (
          <div key={t.trade_id} className="tape-row">
            <span className={`price ${t.aggressor_side === "BID" ? "buy" : "sell"}`}>
              {fmtPrice(t.price)}
            </span>
            <span className="qty">{fmtQty(t.qty)}</span>
            <span className="time">{fmtTime(t._recvMs)}</span>
          </div>
        ))}
      </div>
    </section>
  );
}

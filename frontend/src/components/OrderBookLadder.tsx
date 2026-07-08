import type { BookSnapshot } from "../types";
import { fmtPrice, fmtQty } from "../format";

interface Props {
  book: BookSnapshot | null;
  lastTradePrice: number | null;
  prevTradePrice: number | null;
  onSelectPrice: (price: number) => void;
}

const ROWS = 12;

export function OrderBookLadder({ book, lastTradePrice, prevTradePrice, onSelectPrice }: Props) {
  const asks = (book?.asks ?? []).slice(0, ROWS);
  const bids = (book?.bids ?? []).slice(0, ROWS);
  const maxQty = Math.max(1, ...asks.map((l) => l.qty), ...bids.map((l) => l.qty));

  const midDir =
    lastTradePrice == null || prevTradePrice == null
      ? ""
      : lastTradePrice > prevTradePrice
      ? "up"
      : lastTradePrice < prevTradePrice
      ? "down"
      : "";

  return (
    <section className="panel ladder">
      <div className="panel-header">Order Book</div>
      <div className="ladder-body">
        <div className="ladder-side ladder-asks">
          {asks.map((lvl) => (
            <div key={lvl.price} className="ladder-row" onClick={() => onSelectPrice(lvl.price)}>
              <div className="ladder-row-bar" style={{ width: `${(lvl.qty / maxQty) * 100}%` }} />
              <span className="price ask">{fmtPrice(lvl.price)}</span>
              <span className="qty">{fmtQty(lvl.qty)}</span>
            </div>
          ))}
        </div>

        <div className={`ladder-mid ${midDir}`}>
          {lastTradePrice != null ? fmtPrice(lastTradePrice) : "—"}
        </div>

        <div className="ladder-side ladder-bids">
          {bids.map((lvl) => (
            <div key={lvl.price} className="ladder-row" onClick={() => onSelectPrice(lvl.price)}>
              <div className="ladder-row-bar" style={{ width: `${(lvl.qty / maxQty) * 100}%` }} />
              <span className="price bid">{fmtPrice(lvl.price)}</span>
              <span className="qty">{fmtQty(lvl.qty)}</span>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

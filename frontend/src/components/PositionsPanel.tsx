import type { PortfolioView } from "../types";
import { fmtMoney, fmtPrice, fmtQty } from "../format";

interface Props {
  portfolio: PortfolioView | null;
  accountId: number;
}

/** Room-scoped engine symbols look like "8ACSZ1:BTCUSDT" — show the part
 * the player actually picked. */
function displaySymbol(engineSymbol: string): string {
  const i = engineSymbol.indexOf(":");
  return i >= 0 ? engineSymbol.slice(i + 1) : engineSymbol;
}

export function PositionsPanel({ portfolio, accountId }: Props) {
  const positions = portfolio?.positions ?? [];

  return (
    <section className="panel positions">
      <div className="panel-header">Player {accountId} · Positions</div>
      <div className="positions-body">
        <table className="pos-table">
          <thead>
            <tr>
              <th>Symbol</th>
              <th>Net Qty</th>
              <th>Avg Price</th>
              <th>Unrealised P&amp;L</th>
            </tr>
          </thead>
          <tbody>
            {positions.length === 0 && (
              <tr>
                <td colSpan={4} style={{ color: "var(--text-dim)", textAlign: "center" }}>
                  No open positions
                </td>
              </tr>
            )}
            {positions.map((p) => (
              <tr key={p.symbol}>
                <td>{displaySymbol(p.symbol)}</td>
                <td className={p.net_qty > 0 ? "pos" : p.net_qty < 0 ? "neg" : ""}>
                  {fmtQty(p.net_qty)}
                </td>
                <td>{fmtPrice(p.avg_price)}</td>
                <td className={p.unrealised_pnl > 0 ? "pos" : p.unrealised_pnl < 0 ? "neg" : ""}>
                  {fmtMoney(p.unrealised_pnl)}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

import { useEffect, useState } from "react";
import type { ConnState } from "../hooks/useEngineSocket";
import type { PortfolioView, ScoreResponse, SessionInfo } from "../types";
import { fmtMoney } from "../format";

interface Props {
  session: SessionInfo;
  score: ScoreResponse | null;
  portfolio: PortfolioView | null;
  connState: ConnState;
}

function fmtClock(s: number): string {
  const m = Math.floor(s / 60);
  const r = s % 60;
  return `${m}:${String(r).padStart(2, "0")}`;
}

function usePartialCountdown(endsAt: number | null): number | null {
  const [now, setNow] = useState(() => Date.now() / 1000);
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now() / 1000), 1000);
    return () => clearInterval(t);
  }, []);
  if (endsAt == null) return null;
  return Math.max(0, Math.floor(endsAt - now));
}

/** Match header: FIFA-style score line — you vs rival net P&L, the match
 * clock between them, plus your cash/position and connection health. */
export function TopBar({ session, score, portfolio, connState }: Props) {
  const remaining = usePartialCountdown(score?.ends_at ?? null);

  const meKey = String(session.player_id);
  const rivalKey = session.player_id === 1 ? "2" : "1";
  const my = score?.players?.[meKey];
  const rival = score?.players?.[rivalKey];

  const pos = portfolio?.positions.find((p) => p.symbol === session.symbol);
  const myPnl = my?.net_pnl ?? 0;
  const rivalPnl = rival?.net_pnl ?? 0;

  const [copied, setCopied] = useState(false);
  function copyCode() {
    navigator.clipboard?.writeText(session.code).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    });
  }

  return (
    <header className="topbar">
      <div className="match-id">
        <span className="match-instrument">{session.instrument_id}</span>
        <button className="room-code small" onClick={copyCode} title="Click to copy">
          {copied ? "Copied!" : session.code}
        </button>
      </div>

      <div className="scoreline">
        <div className={`score-side me${myPnl > rivalPnl ? " leading" : ""}`}>
          <span className="score-who">You · {my?.instrument_id ?? "—"}</span>
          <span className={`score-pnl ${myPnl > 0 ? "pos" : myPnl < 0 ? "neg" : ""}`}>
            {fmtMoney(myPnl)}
          </span>
        </div>
        <div className="score-clock">
          {remaining != null ? fmtClock(remaining) : "--:--"}
        </div>
        <div className={`score-side rival${rivalPnl > myPnl ? " leading" : ""}`}>
          <span className="score-who">Rival · {rival?.instrument_id ?? "—"}</span>
          <span className={`score-pnl ${rivalPnl > 0 ? "pos" : rivalPnl < 0 ? "neg" : ""}`}>
            {fmtMoney(rivalPnl)}
          </span>
        </div>
      </div>

      <div className="topbar-stats">
        <div className="stat">
          <span className="stat-label">Cash</span>
          <span className="stat-value">{portfolio ? fmtMoney(portfolio.cash) : "—"}</span>
        </div>
        <div className="stat">
          <span className="stat-label">Position</span>
          <span className="stat-value">{pos ? pos.net_qty.toLocaleString() : "0"}</span>
        </div>
        <div className="stat">
          <span className="stat-label">Live px</span>
          <span className="stat-value">
            {my?.mark_price != null ? fmtMoney(my.mark_price) : "—"}
          </span>
        </div>
      </div>

      <div className={`conn-dot ${connState}`} title={`WebSocket: ${connState}`} />
    </header>
  );
}

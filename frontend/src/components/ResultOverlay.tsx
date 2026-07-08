import type { ScoreResponse, SessionInfo } from "../types";
import { fmtMoney } from "../format";

interface Props {
  session: SessionInfo;
  score: ScoreResponse;
  onLeave: () => void;
}

/** Full-time screen: winner, both P&Ls, and the way out. */
export function ResultOverlay({ session, score, onLeave }: Props) {
  const meKey = String(session.player_id);
  const rivalKey = session.player_id === 1 ? "2" : "1";
  const my = score.players?.[meKey];
  const rival = score.players?.[rivalKey];

  const iWon = score.winner === session.player_id;
  const draw = score.winner === 0;
  const headline = draw ? "Draw!" : iWon ? "You win! 🏆" : "Rival wins";

  return (
    <div className="result-backdrop">
      <div className="result-card">
        <h2 className={`result-headline ${draw ? "" : iWon ? "won" : "lost"}`}>
          {headline}
        </h2>
        <div className="result-rows">
          <div className={`result-row${iWon ? " win" : ""}`}>
            <span>You · {my?.instrument_id ?? "—"}</span>
            <span className={(my?.net_pnl ?? 0) >= 0 ? "pos" : "neg"}>
              {my ? fmtMoney(my.net_pnl) : "—"}
            </span>
          </div>
          <div className={`result-row${!iWon && !draw ? " win" : ""}`}>
            <span>Rival · {rival?.instrument_id ?? "—"}</span>
            <span className={(rival?.net_pnl ?? 0) >= 0 ? "pos" : "neg"}>
              {rival ? fmtMoney(rival.net_pnl) : "—"}
            </span>
          </div>
        </div>
        <p className="lobby-sub">
          Positions marked at the final live price. Spread and timing decide
          matches — rematch and prove it wasn&rsquo;t luck.
        </p>
        <button className="lobby-primary" onClick={onLeave}>
          Back to lobby
        </button>
      </div>
    </div>
  );
}

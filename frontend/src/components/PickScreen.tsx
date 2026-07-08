import { useEffect, useState } from "react";
import { ApiError, listInstruments, pickInstrument } from "../api";
import type { InstrumentInfo, RoomPublic, SessionInfo } from "../types";

interface Props {
  session: SessionInfo;
  room: RoomPublic;
  onPicked: (s: SessionInfo) => void;
  onLeave: () => void;
}

const ROSTER_POLL_MS = 15_000;

function fmtQuote(q: InstrumentInfo["quote"]): string {
  if (!q) return "—";
  return q.price >= 1000
    ? q.price.toLocaleString(undefined, { maximumFractionDigits: 0 })
    : q.price.toLocaleString(undefined, { maximumFractionDigits: 2 });
}

/** Team-select: both players pick their instrument; match starts when
 * both have locked in. */
export function PickScreen({ session, room, onPicked, onLeave }: Props) {
  const [roster, setRoster] = useState<InstrumentInfo[]>([]);
  const [busyId, setBusyId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    let alive = true;
    const load = () =>
      listInstruments()
        .then((r) => alive && setRoster(r.items))
        .catch(() => undefined);
    load();
    const t = setInterval(load, ROSTER_POLL_MS);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, []);

  const me = room.players[String(session.player_id)];
  const rivalId = session.player_id === 1 ? "2" : "1";
  const rival = room.players[rivalId];
  const myPick = session.instrument_id ?? me?.instrument_id ?? null;

  async function handlePick(id: string) {
    setBusyId(id);
    setError(null);
    try {
      onPicked(await pickInstrument(session.code, id));
    } catch (e) {
      setError(e instanceof ApiError ? e.message : "network error");
    } finally {
      setBusyId(null);
    }
  }

  function copyCode() {
    navigator.clipboard?.writeText(session.code).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    });
  }

  return (
    <div className="lobby">
      <div className="pick-card">
        <div className="pick-head">
          <div>
            <h2 className="pick-title">Pick your instrument</h2>
            <p className="lobby-sub">
              Same cash, same clock — whoever grows their P&amp;L more wins.
            </p>
          </div>
          <button className="room-code" onClick={copyCode}
                  title="Click to copy">
            {copied ? "Copied!" : `Room ${session.code}`}
          </button>
        </div>

        <div className="pick-status">
          <span className={`pick-badge${myPick ? " ok" : ""}`}>
            You{myPick ? ` — ${myPick} ✓` : ": pick below"}
          </span>
          <span className={`pick-badge${rival?.picked ? " ok" : ""}`}>
            {!rival?.joined
              ? "Waiting for rival to join…"
              : rival.picked
                ? `Rival — ${rival.instrument_id} ✓`
                : "Rival: picking…"}
          </span>
        </div>

        <div className="roster">
          {roster.length === 0 && (
            <div className="chart-empty">Loading roster…</div>
          )}
          {roster.map((i) => {
            const chg = i.quote?.change_pct;
            return (
              <button
                key={i.id}
                className={`roster-card${myPick === i.id ? " picked" : ""}`}
                disabled={busyId !== null}
                onClick={() => handlePick(i.id)}
              >
                <span className="roster-sym">{i.display}</span>
                <span className="roster-name">{i.name}</span>
                <span className="roster-price">{fmtQuote(i.quote)}</span>
                <span
                  className={`roster-chg ${chg == null ? "" : chg >= 0 ? "pos" : "neg"}`}
                >
                  {chg == null ? "" : `${chg >= 0 ? "+" : ""}${chg.toFixed(2)}%`}
                </span>
                <span className="roster-lot">per {i.lot_label}</span>
                {busyId === i.id && <span className="roster-busy">…</span>}
              </button>
            );
          })}
        </div>

        <div className="pick-foot">
          <span className="lobby-error">{error ?? ""}</span>
          <button className="lobby-leave" onClick={onLeave}>Leave room</button>
        </div>
      </div>
    </div>
  );
}

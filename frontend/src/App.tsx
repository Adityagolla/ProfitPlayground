import { useCallback, useEffect, useRef, useState } from "react";
import "./styles.css";
import { Rail } from "./components/Rail";
import { TopBar } from "./components/TopBar";
import { OrderBookLadder } from "./components/OrderBookLadder";
import { PriceChart } from "./components/PriceChart";
import { TradeTape } from "./components/TradeTape";
import { OrderTicket } from "./components/OrderTicket";
import { PositionsPanel } from "./components/PositionsPanel";
import { Home } from "./components/Home";
import { PickScreen } from "./components/PickScreen";
import { ResultOverlay } from "./components/ResultOverlay";
import { useEngineSocket } from "./hooks/useEngineSocket";
import { getMySession, getScore, setAuthToken } from "./api";
import type { RoomPublic, ScoreResponse, SessionInfo } from "./types";

const SESSION_KEY = "pp.session";
const ROOM_POLL_MS = 2000;
const SCORE_POLL_MS = 2000;

function loadSession(): SessionInfo | null {
  try {
    const raw = localStorage.getItem(SESSION_KEY);
    return raw ? (JSON.parse(raw) as SessionInfo) : null;
  } catch {
    return null;
  }
}

export default function App() {
  const [session, setSession] = useState<SessionInfo | null>(() => {
    const s = loadSession();
    if (s) setAuthToken(s.token);
    return s;
  });
  const [room, setRoom] = useState<RoomPublic | null>(session?.room ?? null);
  const [score, setScore] = useState<ScoreResponse | null>(null);

  const adoptSession = useCallback((s: SessionInfo) => {
    setAuthToken(s.token);
    localStorage.setItem(SESSION_KEY, JSON.stringify(s));
    setSession(s);
    setRoom(s.room);
  }, []);

  const leave = useCallback(() => {
    localStorage.removeItem(SESSION_KEY);
    setAuthToken(null);
    setSession(null);
    setRoom(null);
    setScore(null);
  }, []);

  // Restore/refresh the session on mount (handles page reloads and rooms
  // that vanished with a server restart).
  useEffect(() => {
    if (!session) return;
    let alive = true;
    getMySession(session.code)
      .then((s) => alive && adoptSession(s))
      .catch(() => alive && leave());
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []); // once

  const status = room?.status ?? null;
  const inMatch = session?.symbol != null && (status === "live" || status === "finished");

  // Pre-match: poll our session until both players have picked.
  useEffect(() => {
    if (!session || inMatch) return;
    const t = setInterval(() => {
      getMySession(session.code)
        .then(adoptSession)
        .catch(() => leave());
    }, ROOM_POLL_MS);
    return () => clearInterval(t);
  }, [session, inMatch, adoptSession, leave]);

  // In-match: poll the scoreboard (stops once we have a final board).
  useEffect(() => {
    if (!session || !inMatch || status === "finished") return;
    let alive = true;
    const tick = () =>
      getScore(session.code)
        .then((s) => {
          if (!alive) return;
          setScore(s);
          setRoom((r) => (r ? { ...r, status: s.status, ends_at: s.ends_at } : r));
        })
        .catch(() => undefined);
    tick();
    const t = setInterval(tick, SCORE_POLL_MS);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, [session, inMatch, status]);

  // Fetch the final board once when the match ends.
  useEffect(() => {
    if (!session || status !== "finished") return;
    getScore(session.code).then(setScore).catch(() => undefined);
  }, [session, status]);

  const [prefillPrice, setPrefillPrice] = useState<number | null>(null);
  const prevPriceRef = useRef<number | null>(null);

  const symbol = session?.symbol ?? "";
  const accountId = session?.account_id ?? 0;
  const { connState, book, trades, portfolio, lastTradePrice } =
    useEngineSocket(inMatch ? symbol : "", accountId);

  const prevTradePrice = prevPriceRef.current;
  useEffect(() => {
    prevPriceRef.current = lastTradePrice;
  }, [lastTradePrice]);

  // ── Screens ────────────────────────────────────────────────────────────
  if (!session) {
    return <Home onSession={adoptSession} />;
  }

  if (!inMatch) {
    return (
      <PickScreen
        session={session}
        room={room ?? session.room}
        onPicked={adoptSession}
        onLeave={leave}
      />
    );
  }

  return (
    <div className="app-shell">
      <Rail />
      <TopBar
        session={session}
        score={score}
        portfolio={portfolio}
        connState={connState}
      />
      <OrderBookLadder
        book={book}
        lastTradePrice={lastTradePrice}
        prevTradePrice={prevTradePrice}
        onSelectPrice={setPrefillPrice}
      />
      <PriceChart trades={trades} symbol={session.instrument_id ?? symbol} />
      <TradeTape trades={trades} />
      <OrderTicket
        symbol={symbol}
        displaySymbol={session.instrument_id ?? symbol}
        accountId={accountId}
        prefillPrice={prefillPrice}
        disabled={status !== "live"}
      />
      <PositionsPanel portfolio={portfolio} accountId={accountId} />
      {status === "finished" && score && (
        <ResultOverlay session={session} score={score} onLeave={leave} />
      )}
    </div>
  );
}

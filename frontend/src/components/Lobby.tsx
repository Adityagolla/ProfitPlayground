import { useState } from "react";
import { ApiError, createRoom, joinRoom } from "../api";
import type { SessionInfo } from "../types";

interface Props {
  onSession: (s: SessionInfo) => void;
}

const DURATIONS = [
  { s: 300, label: "5 min" },
  { s: 600, label: "10 min" },
  { s: 900, label: "15 min" },
];

/** Landing screen: host a match or join with a friend's code. */
export function Lobby({ onSession }: Props) {
  const [durationS, setDurationS] = useState(600);
  const [code, setCode] = useState("");
  const [busy, setBusy] = useState<"create" | "join" | null>(null);
  const [error, setError] = useState<string | null>(null);

  async function handleCreate() {
    setBusy("create");
    setError(null);
    try {
      onSession(await createRoom(durationS));
    } catch (e) {
      setError(e instanceof ApiError ? e.message : "network error");
    } finally {
      setBusy(null);
    }
  }

  async function handleJoin() {
    if (code.trim().length < 4) return;
    setBusy("join");
    setError(null);
    try {
      onSession(await joinRoom(code.trim().toUpperCase()));
    } catch (e) {
      setError(e instanceof ApiError ? e.message : "network error");
    } finally {
      setBusy(null);
    }
  }

  return (
    <div className="lobby">
      <div className="lobby-card">
        <div className="lobby-logo" />
        <h1 className="lobby-title">Profit Playground</h1>
        <p className="lobby-sub">
          Pick a stock. Trade its live price. Beat your rival&rsquo;s P&amp;L
          before the clock runs out.
        </p>

        <div className="lobby-section">
          <span className="field-label">Match length</span>
          <div className="type-select">
            {DURATIONS.map((d) => (
              <button
                key={d.s}
                className={`type-opt${d.s === durationS ? " active" : ""}`}
                onClick={() => setDurationS(d.s)}
              >
                {d.label}
              </button>
            ))}
          </div>
          <button
            className="lobby-primary"
            disabled={busy !== null}
            onClick={handleCreate}
          >
            {busy === "create" ? "Creating…" : "Create match"}
          </button>
        </div>

        <div className="lobby-divider"><span>or</span></div>

        <div className="lobby-section">
          <span className="field-label">Join with a code</span>
          <div className="lobby-join-row">
            <input
              className="lobby-code-input"
              placeholder="e.g. J9SE8"
              value={code}
              maxLength={5}
              onChange={(e) => setCode(e.target.value.toUpperCase())}
              onKeyDown={(e) => e.key === "Enter" && handleJoin()}
            />
            <button
              className="lobby-secondary"
              disabled={busy !== null || code.trim().length < 4}
              onClick={handleJoin}
            >
              {busy === "join" ? "Joining…" : "Join"}
            </button>
          </div>
        </div>

        <div className="lobby-error">{error ?? ""}</div>
      </div>
    </div>
  );
}

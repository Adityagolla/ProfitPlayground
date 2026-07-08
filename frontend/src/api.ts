import type {
  BookSnapshot, InstrumentInfo, NewOrderRequest, OrderAck, OrderView,
  PortfolioView, RoomPublic, ScoreResponse, SessionInfo, TopOfBook, Trade,
} from "./types";

const FALLBACK_TOKEN = import.meta.env.VITE_API_TOKEN ?? "dev-token";

// The active bearer: the player's room token once they create/join a match,
// the admin dev token otherwise (lets the raw API remain scriptable).
let authToken: string = FALLBACK_TOKEN;

export function setAuthToken(token: string | null): void {
  authToken = token ?? FALLBACK_TOKEN;
}

export function currentAuthToken(): string {
  return authToken;
}

class ApiError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`/api${path}`, {
    ...init,
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${authToken}`,
      ...init?.headers,
    },
  });
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new ApiError(res.status, body?.error?.message ?? res.statusText);
  }
  return res.json() as Promise<T>;
}

export { ApiError };

export function submitOrder(body: NewOrderRequest): Promise<OrderAck> {
  return request<OrderAck>("/orders", { method: "POST", body: JSON.stringify(body) });
}

export function cancelOrder(orderId: number): Promise<{ status: string; order_id: number; cancelled_qty: number }> {
  return request(`/orders/${orderId}`, { method: "DELETE" });
}

export function getOrder(orderId: number): Promise<OrderView> {
  return request<OrderView>(`/orders/${orderId}`);
}

export function getTopOfBook(symbol: string): Promise<TopOfBook> {
  return request<TopOfBook>(`/orderbook/${symbol}/top`);
}

export function getBookSnapshot(symbol: string, depth = 20): Promise<BookSnapshot> {
  return request<BookSnapshot>(`/orderbook/${symbol}?depth=${depth}`);
}

export function getTrades(symbol: string, limit = 50): Promise<{ items: Trade[]; next_cursor: string | null }> {
  return request(`/trades?symbol=${symbol}&limit=${limit}`);
}

export function getPortfolio(userId: number): Promise<PortfolioView> {
  return request<PortfolioView>(`/portfolio/${userId}`);
}

let nextClientOrderId = Date.now();
/** Monotonic per-tab id; combined with account_id it's a stable idempotency key. */
export function newClientOrderId(): number {
  return nextClientOrderId++;
}

// ─── Rooms / face-off mode ───────────────────────────────────────────────────

export function listInstruments(): Promise<{ items: InstrumentInfo[] }> {
  return request("/instruments");
}

export function createRoom(durationS: number): Promise<SessionInfo> {
  return request("/rooms", {
    method: "POST",
    body: JSON.stringify({ duration_s: durationS }),
  });
}

export function joinRoom(code: string): Promise<SessionInfo> {
  return request(`/rooms/${encodeURIComponent(code)}/join`, { method: "POST" });
}

export function pickInstrument(code: string, instrumentId: string): Promise<SessionInfo> {
  return request(`/rooms/${encodeURIComponent(code)}/pick`, {
    method: "POST",
    body: JSON.stringify({ instrument_id: instrumentId }),
  });
}

export function getRoomState(code: string): Promise<RoomPublic> {
  return request(`/rooms/${encodeURIComponent(code)}`);
}

export function getMySession(code: string): Promise<SessionInfo> {
  return request(`/rooms/${encodeURIComponent(code)}/me`);
}

export function getScore(code: string): Promise<ScoreResponse> {
  return request(`/rooms/${encodeURIComponent(code)}/score`);
}

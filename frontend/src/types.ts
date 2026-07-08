// Mirrors api/schemas.py. snake_case kept as-is to match the wire format
// exactly — no mapping layer needed between fetch/WS payloads and state.

export type OrderType = "LIMIT" | "MARKET" | "IOC" | "FOK" | "STOP" | "STOP_LIMIT";
export type OrderSide = "BID" | "ASK";
export type AckStatus = "ACCEPTED" | "REJECTED" | "DUPLICATE" | "THROTTLED" | "KILLED";
export type OrderStatus = "OPEN" | "PARTIAL" | "FILLED" | "CANCELLED" | "REJECTED";

export interface NewOrderRequest {
  client_order_id: number;
  account_id: number;
  symbol: string;
  type: OrderType;
  side: OrderSide;
  price: number;
  trigger_price?: number;
  qty: number;
  ttl_ns?: number;
}

export interface OrderAck {
  status: AckStatus;
  server_order_id: number;
  client_order_id: number;
  ingress_ts_ns: number;
  ack_ts_ns: number;
  reject_reason: string | null;
}

export interface OrderView {
  server_order_id: number;
  client_order_id: number;
  account_id: number;
  symbol: string;
  type: OrderType;
  side: OrderSide;
  price: number;
  trigger_price: number;
  qty_original: number;
  qty_remain: number;
  qty_filled: number;
  status: OrderStatus;
  created_ts_ns: number;
  updated_ts_ns: number;
}

export interface Level {
  price: number;
  qty: number;
}

export interface TopOfBook {
  symbol: string;
  bid_price: number;
  bid_qty: number;
  ask_price: number;
  ask_qty: number;
  spread: number;
  mid: number;
  ts_ns: number;
}

export interface BookSnapshot {
  symbol: string;
  bids: Level[];
  asks: Level[];
  seq: number;
  ts_ns: number;
}

export interface Trade {
  trade_id: number;
  symbol: string;
  price: number;
  qty: number;
  aggressor_side: OrderSide;
  aggressor_id: number;
  resting_id: number;
  ts_ns: number;
}

export interface Position {
  symbol: string;
  net_qty: number;
  avg_price: number;
  unrealised_pnl: number;
}

export interface PortfolioView {
  user_id: number;
  cash: number;
  positions: Position[];
  realised_pnl: number;
  open_buy_notional: number;
  open_sell_qty: number;
}

// ─── Rooms / face-off mode (api/rooms.py) ──────────────────────────────────

export type RoomStatus = "waiting" | "picking" | "live" | "finished";

export interface InstrumentInfo {
  id: string;
  display: string;
  name: string;
  feed: "binance" | "finnhub";
  lot_label: string;
  quote: { price: number; change_pct: number | null } | null;
}

export interface RoomPlayerPublic {
  joined: boolean;
  picked: boolean;
  instrument_id: string | null;
}

export interface RoomPublic {
  code: string;
  status: RoomStatus;
  duration_s: number;
  ends_at: number | null;       // epoch seconds
  remaining_s: number | null;
  players: Record<string, RoomPlayerPublic>;
}

export interface SessionInfo {
  code: string;
  player_id: number;
  token: string;
  account_id: number;
  instrument_id: string | null;
  symbol: string | null;        // room-scoped engine symbol
  room: RoomPublic;
}

export interface PlayerScore {
  player_id: number;
  instrument_id: string | null;
  cash: number;
  position_qty: number;
  mark_price: number | null;
  live_price: number | null;
  net_pnl: number;
}

/** GET /rooms/{code}/score — room fields with `players` replaced by
 * per-player score entries (empty object before the match is live). */
export interface ScoreResponse extends Omit<RoomPublic, "players"> {
  players: Record<string, PlayerScore>;
  winner: number | null;        // 1 | 2 | 0 (draw) | null
}

// ─── WebSocket envelope (api/routes_ws.py) ─────────────────────────────────

export type WsEventKind =
  | "snapshot" | "delta" | "trade" | "order" | "portfolio"
  | "ping" | "error" | "ack";

export interface WsEnvelope<T = unknown> {
  event: WsEventKind;
  channel?: string;
  symbol?: string;
  user_id?: number;
  seq?: number;
  ts_ns?: number;
  data?: T;
}

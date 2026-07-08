// price_scale is 100 (cents) per api/models.py Symbol default — every
// integer price/qty over the wire gets divided down for display here.
export const PRICE_SCALE = 100;

export function fmtPrice(p: number): string {
  return (p / PRICE_SCALE).toFixed(2);
}

export function fmtMoney(p: number): string {
  const v = p / PRICE_SCALE;
  const sign = v < 0 ? "-" : "";
  return `${sign}$${Math.abs(v).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`;
}

export function fmtQty(q: number): string {
  return q.toLocaleString();
}

// NB: server ts_ns is time.monotonic_ns() (process-relative), not wall
// clock — never format it as a Date. Callers should stamp their own
// Date.now() on receipt and format that instead.
export function fmtTime(epochMs: number): string {
  const d = new Date(epochMs);
  if (Number.isNaN(d.getTime())) return "--:--:--";
  return d.toLocaleTimeString(undefined, { hour12: false });
}

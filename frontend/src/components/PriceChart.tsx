import { useEffect, useRef } from "react";
import type { TradeWithRecv } from "../hooks/useEngineSocket";
import { PRICE_SCALE } from "../format";

interface Props {
  trades: TradeWithRecv[]; // newest-first
  symbol: string;
}

/**
 * Dependency-free canvas line chart. Trades arrive newest-first (matches
 * the trade tape ordering) so it reverses to chronological once, then
 * draws a scaled polyline + gradient fill. No external chart library —
 * keeps the bundle tiny and updates cheap for a live 2-player feed.
 */
export function PriceChart({ trades, symbol }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const wrapRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    const wrap = wrapRef.current;
    if (!canvas || !wrap) return;

    const draw = () => {
      const dpr = window.devicePixelRatio || 1;
      const w = wrap.clientWidth;
      const h = wrap.clientHeight;
      canvas.width = w * dpr;
      canvas.height = h * dpr;
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      ctx.scale(dpr, dpr);
      ctx.clearRect(0, 0, w, h);

      const chrono = [...trades].reverse();
      if (chrono.length < 2) return;

      const prices = chrono.map((t) => t.price / PRICE_SCALE);
      const min = Math.min(...prices);
      const max = Math.max(...prices);
      const span = max - min || 1;
      const padTop = 16;
      const padBottom = 16;
      const plotH = h - padTop - padBottom;

      const x = (i: number) => (i / (prices.length - 1)) * w;
      const y = (p: number) => padTop + plotH - ((p - min) / span) * plotH;

      const rising = prices[prices.length - 1] >= prices[0];
      const lineColor = rising ? "#1ec98b" : "#f0424d";
      const fillColor = rising ? "rgba(30,201,139,0.16)" : "rgba(240,66,77,0.16)";

      ctx.beginPath();
      prices.forEach((p, i) => {
        const px = x(i);
        const py = y(p);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      });
      ctx.strokeStyle = lineColor;
      ctx.lineWidth = 1.75;
      ctx.lineJoin = "round";
      ctx.stroke();

      ctx.lineTo(w, h);
      ctx.lineTo(0, h);
      ctx.closePath();
      ctx.fillStyle = fillColor;
      ctx.fill();

      // last-price marker
      const lastY = y(prices[prices.length - 1]);
      ctx.beginPath();
      ctx.arc(w - 3, lastY, 3, 0, Math.PI * 2);
      ctx.fillStyle = lineColor;
      ctx.fill();

      ctx.strokeStyle = "rgba(255,255,255,0.06)";
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
      ctx.beginPath();
      ctx.moveTo(0, lastY);
      ctx.lineTo(w, lastY);
      ctx.stroke();
      ctx.setLineDash([]);
    };

    draw();
    const ro = new ResizeObserver(draw);
    ro.observe(wrap);
    return () => ro.disconnect();
  }, [trades]);

  return (
    <section className="panel chart-panel">
      <div className="panel-header">{symbol} · Price</div>
      <div className="chart-canvas-wrap" ref={wrapRef}>
        <canvas ref={canvasRef} />
        {trades.length < 2 && <div className="chart-empty">Waiting for trades…</div>}
      </div>
    </section>
  );
}

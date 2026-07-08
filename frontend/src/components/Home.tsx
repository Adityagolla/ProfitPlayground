import { useEffect, useRef, useState } from "react";
import { motion, useScroll, useTransform } from "framer-motion";
import { listInstruments } from "../api";
import { fmtMoney } from "../format";
import type { InstrumentInfo, SessionInfo } from "../types";
import { Lobby } from "./Lobby";

interface Props {
  onSession: (s: SessionInfo) => void;
}

const STEPS = [
  {
    n: "01",
    title: "Pick your asset",
    body: "Choose a stock or crypto from the roster — live price included. Same choice available to your rival, but you don't have to pick the same one.",
  },
  {
    n: "02",
    title: "Get funded",
    body: "Both players start with identical cash the moment you both lock in a pick. No edge, no head start — just the tape.",
  },
  {
    n: "03",
    title: "Trade the tape",
    body: "A live market maker quotes your book off the real price. Buy, sell, scalp the spread — whatever grows your P&L before the clock hits zero.",
  },
];

/** Scroll-driven landing page. One continuous page: hero -> how it works
 * -> live roster preview -> the actual create/join form (reused Lobby). */
export function Home({ onSession }: Props) {
  const heroRef = useRef<HTMLDivElement>(null);
  const { scrollYProgress } = useScroll({
    target: heroRef,
    offset: ["start start", "end start"],
  });
  const heroOpacity = useTransform(scrollYProgress, [0, 1], [1, 0]);
  const heroScale = useTransform(scrollYProgress, [0, 1], [1, 1.08]);
  const heroY = useTransform(scrollYProgress, [0, 1], [0, 80]);

  const [instruments, setInstruments] = useState<InstrumentInfo[]>([]);
  useEffect(() => {
    listInstruments()
      .then((r) => setInstruments(r.items))
      .catch(() => setInstruments([]));
  }, []);

  function scrollToLobby() {
    document.getElementById("home-lobby")?.scrollIntoView({ behavior: "smooth" });
  }
  function scrollToHow() {
    document.getElementById("home-how")?.scrollIntoView({ behavior: "smooth" });
  }

  return (
    <div className="home">
      <section className="home-hero" ref={heroRef}>
        <motion.div
          className="home-hero-media"
          style={{ opacity: heroOpacity, scale: heroScale, y: heroY }}
        >
          <video autoPlay loop muted playsInline src="/hero-duel.mp4" />
          <div className="home-hero-overlay" />
        </motion.div>

        <motion.div
          className="home-hero-content"
          initial={{ opacity: 0, y: 16 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.6, ease: "easeOut" }}
        >
          <span className="home-eyebrow">Two traders. One clock.</span>
          <h1 className="home-title">
            Settle the tape.
            <br />
            <span className="home-title-accent">1v1 trading duels.</span>
          </h1>
          <p className="home-sub">
            Pick a stock or crypto, trade its real live price, and see who
            grows their P&amp;L more before time runs out. Same cash, same
            clock, different tickers.
          </p>
          <div className="home-hero-cta">
            <button className="home-cta-btn" onClick={scrollToLobby}>
              Start a duel
            </button>
            <button className="home-ghost-btn" onClick={scrollToHow}>
              How it works
            </button>
          </div>
        </motion.div>

        <div className="home-scroll-hint">
          <span>Scroll</span>
          <div className="home-scroll-line" />
        </div>
      </section>

      <section className="home-how" id="home-how">
        <motion.h2
          className="home-section-title"
          initial={{ opacity: 0, y: 20 }}
          whileInView={{ opacity: 1, y: 0 }}
          viewport={{ once: true, margin: "-80px" }}
          transition={{ duration: 0.5 }}
        >
          The combat cycle
        </motion.h2>

        <div className="home-steps">
          {STEPS.map((s, i) => (
            <motion.div
              key={s.n}
              className="home-step"
              initial={{ opacity: 0, y: 28 }}
              whileInView={{ opacity: 1, y: 0 }}
              viewport={{ once: true, margin: "-80px" }}
              transition={{ duration: 0.5, delay: i * 0.12 }}
            >
              <span className="home-step-n">{s.n}</span>
              <h3>{s.title}</h3>
              <p>{s.body}</p>
            </motion.div>
          ))}
        </div>
      </section>

      {instruments.length > 0 && (
        <section className="home-roster">
          <motion.h2
            className="home-section-title"
            initial={{ opacity: 0, y: 20 }}
            whileInView={{ opacity: 1, y: 0 }}
            viewport={{ once: true, margin: "-80px" }}
            transition={{ duration: 0.5 }}
          >
            Live right now
          </motion.h2>
          <motion.div
            className="home-ticker-row"
            initial={{ opacity: 0 }}
            whileInView={{ opacity: 1 }}
            viewport={{ once: true, margin: "-80px" }}
            transition={{ duration: 0.6 }}
          >
            {instruments.map((i) => {
              const chg = i.quote?.change_pct ?? null;
              const up = chg != null && chg >= 0;
              return (
                <div key={i.id} className="home-ticker-item">
                  <span className="home-ticker-sym">{i.display}</span>
                  <span className="home-ticker-px">
                    {i.quote ? fmtMoney(Math.round(i.quote.price * 100)) : "—"}
                  </span>
                  {chg != null && (
                    <span className={`home-ticker-chg ${up ? "pos" : "neg"}`}>
                      {up ? "+" : ""}
                      {chg.toFixed(2)}%
                    </span>
                  )}
                </div>
              );
            })}
          </motion.div>
        </section>
      )}

      <section className="home-lobby-section" id="home-lobby">
        <motion.div
          initial={{ opacity: 0, y: 24 }}
          whileInView={{ opacity: 1, y: 0 }}
          viewport={{ once: true, margin: "-60px" }}
          transition={{ duration: 0.5 }}
        >
          <Lobby onSession={onSession} />
        </motion.div>
      </section>
    </div>
  );
}

import { useEffect, useState } from "react";
import { fetchBookEvents } from "../api";
import type { BookEvent } from "../types";
import "./FixMessageFeed.css";

const POLL_INTERVAL_MS = 2000;
const FEED_LIMIT = 50;

function formatTime(ns: number): string {
  return new Date(ns / 1e6).toLocaleTimeString(undefined, { hour12: false });
}

// GET /api/orderbook/<id>/events, backed by LiveBookRegistry's real
// per-instrument ring buffer (cpp/api/live_book_registry.hpp) -- every row
// here is a real tse::fix::Order/Execution that actually flowed through
// the live pipeline, not a synthesized message.
export default function FixMessageFeed({ instrumentId }: { instrumentId: string }) {
  const [events, setEvents] = useState<BookEvent[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!instrumentId) {
      setEvents([]);
      return;
    }
    let cancelled = false;
    async function poll() {
      try {
        const data = await fetchBookEvents(instrumentId, FEED_LIMIT);
        if (!cancelled) {
          setEvents(data);
          setError(null);
        }
      } catch (e) {
        if (!cancelled) setError(e instanceof Error ? e.message : String(e));
      }
    }
    poll();
    const id = setInterval(poll, POLL_INTERVAL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [instrumentId]);

  if (!instrumentId) return <p className="empty-state">Select an instrument to see its FIX message feed.</p>;
  if (error) return <p className="ticker-error">{error}</p>;
  if (events.length === 0) return <p className="empty-state">No messages for {instrumentId} yet.</p>;

  // Most recent first -- the feed arrives oldest-first from the ring
  // buffer (arrival order), reversed here for a "latest at the top" read,
  // the natural way to watch a live feed.
  const mostRecentFirst = [...events].reverse();

  return (
    <div className="fix-feed">
      <div className="fix-feed-row fix-feed-header">
        <span className="col-fix-time">TIME</span>
        <span className="col-fix-type">TYPE</span>
        <span className="col-fix-side">SIDE</span>
        <span className="col-fix-price">PRICE</span>
        <span className="col-fix-qty">QTY</span>
        <span className="col-fix-order">ORDER ID</span>
        <span className="col-fix-account">ACCOUNT</span>
      </div>
      {mostRecentFirst.map((event, i) => {
        // FIX 4.2's OrderCancelRequest has no Price field at all -- never
        // transmitted on a real cancel (see message_translator.cpp's own
        // comment). Showing "0.0000" there would read as "cancelled at
        // price zero," not "this message type doesn't carry a price."
        const priceApplies = event.msg_type !== "CANCEL";
        return (
          <div className="fix-feed-row" key={`${event.order_id}-${event.timestamp_ns}-${i}`}>
            <span className="col-fix-time tabular">{formatTime(event.timestamp_ns)}</span>
            <span className={`col-fix-type fix-type-${event.msg_type.toLowerCase()}`}>{event.msg_type}</span>
            <span className={`col-fix-side ${event.side === "BUY" ? "fix-side-buy" : "fix-side-sell"}`}>{event.side}</span>
            <span className="col-fix-price tabular">{priceApplies ? event.price.toFixed(4) : "—"}</span>
            <span className="col-fix-qty tabular">{event.qty}</span>
            <span className="col-fix-order">{event.order_id}</span>
            <span className="col-fix-account">{event.account_id}</span>
          </div>
        );
      })}
    </div>
  );
}

"""
observability.py — structured logging, request-ids, and Prometheus metrics.

Closes the first batch of items in production.md §1:
  - /metrics endpoint
  - /ready readiness probe (DB ping)
  - JSON structured logging
  - X-Request-Id correlation middleware
  - Custom histograms/counters/gauges for the order pipeline

Usage (wired in main.py):
    from .observability import (
        setup_logging, RequestIdMiddleware, instrument_app,
        ORDER_ACK_LATENCY, ORDER_REJECTED, WS_CLIENTS, PUMP_LAG,
    )
"""

from __future__ import annotations
import contextvars
import logging
import sys
import time
import uuid
from typing import Awaitable, Callable

import structlog
from fastapi import FastAPI, Request, Response
from prometheus_client import Counter, Gauge, Histogram
from prometheus_fastapi_instrumentator import Instrumentator
from starlette.middleware.base import BaseHTTPMiddleware


# ─── Request-id context ──────────────────────────────────────────────────────
# A contextvar lets us tag log lines emitted *anywhere* in a request handler
# with the same request id, without threading the id through every function.
_REQUEST_ID: contextvars.ContextVar[str] = contextvars.ContextVar(
    "request_id", default="-"
)


def current_request_id() -> str:
    """Read the request id of the currently-executing request, or '-' outside one."""
    return _REQUEST_ID.get()


# ─── structlog setup ─────────────────────────────────────────────────────────
def setup_logging(level: str = "INFO", json_output: bool = True) -> None:
    """Configure stdlib logging + structlog so every log line is JSON.

    Should be called exactly once at process start (before any logger is used).
    """
    # 1. Funnel stdlib logging through stderr at the requested level. This also
    #    captures uvicorn / sqlalchemy / our own modules.
    logging.basicConfig(
        format="%(message)s",
        stream=sys.stderr,
        level=level.upper(),
    )

    # 2. Configure structlog. Each log call goes through this processor chain.
    processors = [
        structlog.contextvars.merge_contextvars,           # picks up bound context
        structlog.processors.add_log_level,
        structlog.processors.TimeStamper(fmt="iso", utc=True),
        _attach_request_id,
        structlog.processors.StackInfoRenderer(),
        structlog.processors.format_exc_info,
    ]
    if json_output:
        processors.append(structlog.processors.JSONRenderer())
    else:
        processors.append(structlog.dev.ConsoleRenderer(colors=True))

    structlog.configure(
        processors=processors,
        wrapper_class=structlog.make_filtering_bound_logger(
            getattr(logging, level.upper(), logging.INFO)
        ),
        logger_factory=structlog.PrintLoggerFactory(file=sys.stderr),
        cache_logger_on_first_use=True,
    )


def _attach_request_id(_logger, _method_name, event_dict):
    """structlog processor: stamp every log line with the current request id."""
    rid = _REQUEST_ID.get()
    if rid and rid != "-":
        event_dict.setdefault("request_id", rid)
    return event_dict


# Convenience: a structlog logger every module can import.
log = structlog.get_logger("api")


# ─── Request-id middleware ───────────────────────────────────────────────────
class RequestIdMiddleware(BaseHTTPMiddleware):
    """Read or mint X-Request-Id, store it in a contextvar, echo it back.

    The request id is then automatically attached to every structlog line
    emitted while this request is in scope.
    """

    HEADER = "x-request-id"

    async def dispatch(
        self,
        request: Request,
        call_next: Callable[[Request], Awaitable[Response]],
    ) -> Response:
        rid = request.headers.get(self.HEADER) or uuid.uuid4().hex[:12]
        token = _REQUEST_ID.set(rid)
        t0 = time.perf_counter()
        try:
            response = await call_next(request)
        finally:
            _REQUEST_ID.reset(token)
        response.headers[self.HEADER] = rid
        # One structured access line per request — uvicorn's default is plain text.
        log.info(
            "http_request",
            method=request.method,
            path=request.url.path,
            status=response.status_code,
            duration_ms=round((time.perf_counter() - t0) * 1000, 3),
            request_id=rid,
        )
        return response


# ─── Custom Prometheus metrics ───────────────────────────────────────────────
# Latency histograms — buckets chosen for our SLO budget (p99 ≤ 5 ms for ack).
_LAT_BUCKETS = (
    0.0001, 0.0005, 0.001, 0.002, 0.005,
    0.01,   0.025,  0.05,  0.1,   0.25,
    0.5,    1.0,    2.5,
)

ORDER_ACK_LATENCY = Histogram(
    "order_ack_latency_seconds",
    "Time from request entry to gateway ack.",
    buckets=_LAT_BUCKETS,
)

ORDER_SUBMITTED = Counter(
    "order_submitted_total",
    "Order submissions seen by the API, labelled by ack status.",
    labelnames=("status",),  # ACCEPTED / REJECTED / DUPLICATE / THROTTLED / KILLED
)

ORDER_REJECTED = Counter(
    "order_rejected_total",
    "Rejected order submissions, labelled by reason.",
    labelnames=("reason",),
)

WS_CLIENTS = Gauge(
    "ws_clients",
    "Currently connected WebSocket clients.",
)

WS_DROPPED = Counter(
    "ws_dropped_total",
    "Messages dropped because a client's outbound queue was full.",
)

PUMP_LAG = Gauge(
    "engine_pump_lag_events",
    "Engine seq minus last persisted watermark (≈ how far the pump is behind).",
)

PERSIST_FAILURES = Counter(
    "persist_failures_total",
    "Best-effort persistence failures, labelled by stage.",
    labelnames=("stage",),  # order_snapshot / trade / watermark
)


# ─── App wiring ──────────────────────────────────────────────────────────────
def instrument_app(app: FastAPI) -> None:
    """Attach the default HTTP-latency/throughput metrics + expose /metrics.

    The default instrumentator gives us:
      - http_requests_total{method,handler,status}
      - http_request_duration_seconds_bucket{...}
    plus per-handler labels. Cardinality stays bounded because we never label
    by user-controlled values.
    """
    Instrumentator(
        should_group_status_codes=True,
        should_ignore_untemplated=True,
        excluded_handlers=["/metrics", "/health", "/ready"],
    ).instrument(app).expose(app, "/metrics", include_in_schema=False)

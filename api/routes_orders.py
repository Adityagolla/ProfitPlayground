"""
routes_orders.py — REST routes for order lifecycle.

Every mutation goes through `engine.submit_order` / `cancel_order` /
`modify_order`, i.e. exactly the same contract the C pipeline exposes via
`gateway_submit`. The gateway ack status maps 1:1 onto HTTP status codes.
"""

import time

from fastapi import APIRouter, Depends, HTTPException, Response, status

from .auth import Principal, require_principal
from .engine_bridge import engine
from .observability import (
    log, ORDER_ACK_LATENCY, ORDER_SUBMITTED, ORDER_REJECTED,
)
from .schemas import (
    NewOrderRequest, PatchOrderRequest,
    OrderAck, OrderView, CancelResponse,
)

router = APIRouter(prefix="/orders", tags=["orders"])


async def _gate_player(principal: Principal) -> None:
    """Players may only trade while their match is live."""
    if principal.kind != "player":
        return
    from .rooms import registry
    await registry.finish_if_due(principal.room)
    if principal.room.status != "live":
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail={"error": {"code": "MATCH_NOT_LIVE",
                              "message": f"match is {principal.room.status}"}},
        )


async def _owned_order_or_404(order_id: int, principal: Principal) -> dict:
    """Fetch an order; players only see their own."""
    data = await engine.get_order(order_id)
    if data is None or (principal.kind == "player"
                        and data["account_id"] != principal.account_id):
        raise HTTPException(
            status_code=404,
            detail={"error": {"code": "NOT_FOUND",
                              "message": f"order {order_id} not found"}},
        )
    return data


def _ack_to_http_status(ack_status: str) -> int:
    """Map gw_ack_t.status -> HTTP code. Consistent with the API spec."""
    return {
        "ACCEPTED":  status.HTTP_202_ACCEPTED,
        "REJECTED":  status.HTTP_400_BAD_REQUEST,
        "DUPLICATE": status.HTTP_409_CONFLICT,
        "THROTTLED": status.HTTP_429_TOO_MANY_REQUESTS,
        "KILLED":    status.HTTP_503_SERVICE_UNAVAILABLE,
    }[ack_status]


@router.post("", response_model=OrderAck)
async def post_order(
    body: NewOrderRequest,
    response: Response,
    principal: Principal = Depends(require_principal),
) -> OrderAck:
    """Submit a new order — idempotent on `client_order_id`."""
    await _gate_player(principal)
    msg = body.model_dump()
    msg["action"] = "NEW"
    if principal.kind == "player":
        # Server-enforced identity: a player trades only as themselves,
        # only on their own room-scoped book.
        msg["account_id"] = principal.account_id
        msg["symbol"] = principal.player.engine_symbol

    t0 = time.perf_counter()
    ack = await engine.submit_order(msg)
    elapsed = time.perf_counter() - t0

    # Metrics: latency histogram + status counter + rejection reasons.
    ORDER_ACK_LATENCY.observe(elapsed)
    ORDER_SUBMITTED.labels(status=ack["status"]).inc()
    if ack["status"] != "ACCEPTED":
        reason = (ack.get("reject_reason") or ack["status"]).upper()[:32]
        ORDER_REJECTED.labels(reason=reason).inc()
        log.warning(
            "order_rejected",
            status=ack["status"], reason=reason,
            client_order_id=body.client_order_id,
            account_id=body.account_id, symbol=body.symbol,
            latency_ms=round(elapsed * 1000, 3),
        )
    else:
        log.info(
            "order_accepted",
            server_order_id=ack.get("server_order_id"),
            client_order_id=body.client_order_id,
            account_id=body.account_id, symbol=body.symbol,
            side=body.side, price=body.price, qty=body.qty,
            latency_ms=round(elapsed * 1000, 3),
        )

    response.status_code = _ack_to_http_status(ack["status"])
    return OrderAck(**ack)


@router.get("/{order_id}", response_model=OrderView)
async def get_order(order_id: int,
                    principal: Principal = Depends(require_principal),
                    ) -> OrderView:
    """Fetch the current state of an order by its server_order_id."""
    data = await _owned_order_or_404(order_id, principal)
    return OrderView(**data)


@router.patch("/{order_id}", response_model=OrderView)
async def patch_order(order_id: int, body: PatchOrderRequest,
                      principal: Principal = Depends(require_principal),
                      ) -> OrderView:
    """Amend price/qty. Implemented as cancel + re-add."""
    await _gate_player(principal)
    await _owned_order_or_404(order_id, principal)
    data = await engine.modify_order(order_id, body.new_price, body.new_qty)
    if data is None:
        raise HTTPException(
            status_code=404,
            detail={"error": {"code": "NOT_FOUND", "message": "order not modifiable"}},
        )
    return OrderView(**data)


@router.delete("/{order_id}", response_model=CancelResponse)
async def delete_order(order_id: int,
                       principal: Principal = Depends(require_principal),
                       ) -> CancelResponse:
    """Cancel a resting order. Returns the cancelled remaining qty."""
    await _gate_player(principal)
    await _owned_order_or_404(order_id, principal)
    state, qty = await engine.cancel_order(order_id)
    return CancelResponse(status=state, order_id=order_id, cancelled_qty=qty)

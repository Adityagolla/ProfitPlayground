"""
routes_portfolio.py — Private portfolio view.

Scope: the authenticated caller can only read their own portfolio. In this
scaffold we treat the bearer token as a proxy for the user; in production
the caller identity comes from the token subject.
"""

from fastapi import APIRouter, Depends, HTTPException

from .auth import Principal, require_principal
from .engine_bridge import engine
from .schemas import PortfolioView, Position

router = APIRouter(prefix="/portfolio", tags=["portfolio"])


@router.get("/{user_id}", response_model=PortfolioView)
async def get_portfolio(user_id: int,
                        principal: Principal = Depends(require_principal),
                        ) -> PortfolioView:
    """Return cash, positions, and P&L for `user_id`."""
    if principal.kind == "player" and user_id != principal.account_id:
        raise HTTPException(
            status_code=403,
            detail={"error": {"code": "FORBIDDEN",
                              "message": "players may only read their own portfolio"}},
        )
    data = await engine.portfolio(user_id)
    return PortfolioView(
        user_id=data["user_id"],
        cash=data["cash"],
        realised_pnl=data["realised_pnl"],
        open_buy_notional=data["open_buy_notional"],
        open_sell_qty=data["open_sell_qty"],
        positions=[Position(**p) for p in data["positions"]],
    )

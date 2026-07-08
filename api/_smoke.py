"""Smoke test: post an order, then check rows in Postgres. Safe to delete."""
import asyncio, json, random, sys, urllib.request
COID = random.randint(10_000, 9_000_000)
sys.path.insert(0, ".")
from api.config import settings
from sqlalchemy.ext.asyncio import create_async_engine
from sqlalchemy import text

URL = "http://127.0.0.1:8080"


def post_order():
    body = json.dumps({
        "client_order_id": COID,
        "account_id": 1,
        "symbol": "AAPL",
        "type": "LIMIT",
        "side": "ASK",
        "price": 10000,
        "qty": 50,
    }).encode()
    req = urllib.request.Request(
        f"{URL}/orders", data=body,
        headers={"Authorization": "Bearer dev-token",
                 "Content-Type": "application/json"},
    )
    try:
        resp = urllib.request.urlopen(req)
        print("POST /orders ->", resp.status, resp.read().decode())
    except urllib.error.HTTPError as e:
        print("POST /orders ->", e.code, e.read().decode())


async def check_db():
    e = create_async_engine(settings.database_url)
    async with e.connect() as c:
        for tbl in ("orders", "order_events", "trades", "portfolio_ledger"):
            n = (await c.execute(text(f"select count(*) from {tbl}"))).scalar()
            print(f"{tbl}: {n} rows")
        rows = (await c.execute(text(
            "select server_order_id, account_id, symbol, side, price, qty_remain, status "
            "from orders order by server_order_id"
        ))).all()
        for r in rows:
            print("  order:", dict(r._mapping))
    await e.dispose()


def post_buy():
    body = json.dumps({
        "client_order_id": COID + 1,
        "account_id": 2,
        "symbol": "AAPL",
        "type": "LIMIT",
        "side": "BID",
        "price": 10000,
        "qty": 30,
    }).encode()
    req = urllib.request.Request(
        f"{URL}/orders", data=body,
        headers={"Authorization": "Bearer dev-token",
                 "Content-Type": "application/json"},
    )
    try:
        resp = urllib.request.urlopen(req)
        print("POST /orders (buy) ->", resp.status, resp.read().decode())
    except urllib.error.HTTPError as e:
        print("POST /orders (buy) ->", e.code, e.read().decode())


post_order()
import time; time.sleep(0.3)
post_buy()
time.sleep(0.5)  # let pump persist
asyncio.run(check_db())

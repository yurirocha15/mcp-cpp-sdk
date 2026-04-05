#!/usr/bin/env python3
import asyncio
import json
import os
import time
from contextlib import asynccontextmanager

import httpx
import redis.asyncio as aioredis
import uvicorn
from mcp.server.fastmcp import FastMCP
from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.routing import Mount, Route

http_client: httpx.AsyncClient = None
redis_client: aioredis.Redis = None

mcp = FastMCP("BenchmarkPythonServer", stateless_http=True, json_response=True)


@mcp.tool()
async def search_products(
    category: str = "Electronics",
    min_price: float = 50.0,
    max_price: float = 500.0,
    limit: int = 10,
) -> str:
    """Search products by category and price range, merged with popularity data"""
    http_task = http_client.get(
        f"/products/search?category={category}&min_price={min_price}&max_price={max_price}&limit={limit}"
    )
    redis_task = redis_client.zrevrange("bench:popular", 0, 9, withscores=False)

    http_resp, popular_raw = await asyncio.gather(http_task, redis_task)

    products_data = http_resp.json()
    products = products_data.get("products", [])

    top10_popular_ids = []
    for item in popular_raw:
        item_str = item.decode("utf-8") if isinstance(item, bytes) else item
        if item_str.startswith("product:"):
            try:
                product_id = int(item_str.split(":")[1])
                top10_popular_ids.append(product_id)
            except (IndexError, ValueError):
                pass

    for product in products:
        product_id = product.get("id")
        if product_id in top10_popular_ids:
            product["popularity_rank"] = top10_popular_ids.index(product_id) + 1
        else:
            product["popularity_rank"] = 0

    result = {
        "category": category,
        "total_found": products_data.get("total_found", len(products)),
        "products": products,
        "top10_popular_ids": top10_popular_ids,
        "server_type": "python",
    }

    return json.dumps(result)


@mcp.tool()
async def get_user_cart(user_id: str = "user-00042") -> str:
    """Get user cart details with recent order history"""
    cart_hash = await redis_client.hgetall(f"bench:cart:{user_id}")

    cart_items = []
    item_count = 0
    estimated_total = 0.0
    first_product_id = None

    if cart_hash:
        items_json = cart_hash.get(b"items")
        if items_json:
            cart_items = json.loads(items_json.decode("utf-8"))
            item_count = len(cart_items)
            if cart_items:
                first_product_id = cart_items[0].get("product_id")
        total_raw = cart_hash.get(b"total")
        if total_raw:
            try:
                estimated_total = float(total_raw.decode("utf-8") if isinstance(total_raw, bytes) else total_raw)
            except (ValueError, AttributeError):
                pass

    if first_product_id:
        product_task = http_client.get(f"/products/{first_product_id}")
    else:
        async def dummy_task():
            return None
        product_task = dummy_task()

    history_task = redis_client.lrange(f"bench:history:{user_id}", 0, 4)

    product_resp, history_raw = await asyncio.gather(product_task, history_task)

    recent_history = []
    for entry in history_raw:
        entry_str = entry.decode("utf-8") if isinstance(entry, bytes) else entry
        try:
            recent_history.append(json.loads(entry_str))
        except json.JSONDecodeError:
            pass

    result = {
        "user_id": user_id,
        "cart": {
            "items": cart_items,
            "item_count": item_count,
            "estimated_total": estimated_total,
        },
        "recent_history": recent_history,
        "server_type": "python",
    }

    return json.dumps(result)


@mcp.tool()
async def checkout(
    user_id: str = "user-00042",
    items: list = [{"product_id": 42, "quantity": 2}, {"product_id": 1337, "quantity": 1}],
) -> str:
    """Process checkout: calculate total, update rate limit, record history"""
    try:
        user_num = int(user_id.split("-")[-1]) % 100
    except (ValueError, IndexError):
        user_num = 0

    rate_key = f"bench:ratelimit:user-{user_num:05d}"

    timestamp = int(time.time())
    order_id = f"ORD-{user_id}-{timestamp}"
    order_entry = {
        "order_id": order_id,
        "items": items,
        "ts": timestamp,
    }
    order_entry_json = json.dumps(order_entry)

    first_product_id = items[0]["product_id"] if items else None

    calc_task = http_client.post("/cart/calculate", json={"user_id": user_id, "items": items})
    incr_task = redis_client.incr(rate_key)
    history_task = redis_client.rpush(f"bench:history:{user_id}", order_entry_json)

    if first_product_id:
        popular_task = redis_client.zincrby("bench:popular", 1, f"product:{first_product_id}")
        calc_resp, rate_count, _, _ = await asyncio.gather(
            calc_task, incr_task, history_task, popular_task
        )
    else:
        calc_resp, rate_count, _ = await asyncio.gather(calc_task, incr_task, history_task)

    calc_data = calc_resp.json()
    total = calc_data.get("total", 0.0)
    calc_order_id = calc_data.get("order_id", order_id)

    result = {
        "order_id": calc_order_id,
        "user_id": user_id,
        "total": total,
        "items_count": len(items),
        "rate_limit_count": rate_count,
        "status": "confirmed",
        "server_type": "python",
    }

    return json.dumps(result)


async def health(request):
    return JSONResponse({"status": "ok", "server_type": "python"})


@asynccontextmanager
async def lifespan(app):
    global http_client, redis_client

    api_service_url = os.environ.get("API_SERVICE_URL", "http://api-service:8100")
    redis_url = os.environ.get("REDIS_URL", "redis://redis:6379")

    http_client = httpx.AsyncClient(
        base_url=api_service_url,
        limits=httpx.Limits(max_connections=200, max_keepalive_connections=100),
    )
    redis_client = aioredis.from_url(redis_url, max_connections=100)

    async with mcp.session_manager.run():
        yield

    await http_client.aclose()
    await redis_client.aclose()


app = Starlette(
    routes=[
        Route("/health", health),
        Mount("/", app=mcp.streamable_http_app()),
    ],
    lifespan=lifespan,
)


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "8081"))
    uvicorn.run(app, host="0.0.0.0", port=port)

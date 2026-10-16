from mcp.server.mcpserver import MCPServer
from contextlib import asynccontextmanager
from importlib.metadata import version
from starlette.applications import Starlette
from starlette.routing import Route, Mount
from starlette.responses import JSONResponse
import asyncio
import httpx
import json
import os
import time
import redis.asyncio as aioredis

API_SERVICE_URL = os.getenv("API_SERVICE_URL", "http://mcp-api-service:8100")
REDIS_URL = os.getenv("REDIS_URL", "redis://mcp-redis:6379")
SERVER_TYPE = os.getenv("SERVER_TYPE", "python")

# mcp 1.x reported its own package version when none was given; 2.x reports an empty one.
mcp = MCPServer("BenchmarkPythonServer", version=version("mcp"))
# mcp 2.x takes the transport options here; session_manager exists only after this call.
mcp_app = mcp.streamable_http_app(stateless_http=True, json_response=True)

_http: httpx.AsyncClient | None = None
_redis: aioredis.Redis | None = None

@asynccontextmanager
async def lifespan(app):
    global _http, _redis
    _http = httpx.AsyncClient(base_url=API_SERVICE_URL, timeout=10.0)
    _redis = aioredis.from_url(REDIS_URL, decode_responses=True)
    async with mcp.session_manager.run():
        yield
    await _http.aclose()
    await _redis.aclose()

@mcp.tool()
async def search_products(
    category: str = "Electronics",
    min_price: float = 50.0,
    max_price: float = 500.0,
    limit: int = 10,
) -> str:
    """Search products by category and price range, merged with popularity data."""
    params = {"category": category, "min_price": min_price, "max_price": max_price, "limit": limit}
    search_task = _http.get("/products/search", params=params)
    popular_task = _redis.zrevrangebyscore("bench:popular", "+inf", "-inf", start=0, num=10)
    (search_resp, popular_raw) = await asyncio.gather(search_task, popular_task)
    search_data = search_resp.json()
    top10_ids = [int(m.split(":")[1]) for m in popular_raw]
    top10_set = {pid: rank + 1 for rank, pid in enumerate(top10_ids)}
    products = [
        {
            "id": p["id"],
            "sku": p["sku"],
            "name": p["name"],
            "price": p["price"],
            "rating": p["rating"],
            "popularity_rank": top10_set.get(p["id"], 0),
        }
        for p in search_data.get("products", [])
    ]
    return json.dumps({
        "category": category,
        "total_found": search_data.get("total_found", 0),
        "products": products,
        "top10_popular_ids": top10_ids,
        "server_type": SERVER_TYPE,
    })

@mcp.tool()
async def get_user_cart(user_id: str = "user-00042") -> str:
    """Get user cart details with recent order history."""
    cart_key = f"bench:cart:{user_id}"
    history_key = f"bench:history:{user_id}"
    cart_hash = await _redis.hgetall(cart_key)
    items = json.loads(cart_hash.get("items", "[]"))
    first_product_id = items[0]["product_id"] if items else 1
    product_resp, history_raw = await asyncio.gather(
        _http.get(f"/products/{first_product_id}"),
        _redis.lrange(history_key, 0, 4),
    )
    recent_history = []
    for entry in history_raw:
        try:
            recent_history.append(json.loads(entry))
        except Exception:
            recent_history.append({"raw": entry})
    estimated_total = float(cart_hash.get("total", 0))
    return json.dumps({
        "user_id": user_id,
        "cart": {
            "items": items,
            "item_count": len(items),
            "estimated_total": estimated_total,
        },
        "recent_history": recent_history,
        "server_type": SERVER_TYPE,
    })

@mcp.tool()
async def checkout(
    user_id: str = "user-00042",
    items: list = None,
) -> str:
    """Process checkout: calculate total, update rate limit, record history."""
    if items is None:
        items = [{"product_id": 42, "quantity": 2}, {"product_id": 1337, "quantity": 1}]
    user_num = int(user_id.split("-")[1]) if "-" in user_id else 42
    rate_key = f"bench:ratelimit:user-{user_num % 100:05d}"
    history_key = f"bench:history:{user_id}"
    product_id = items[0]["product_id"] if items else 1
    order_entry = json.dumps({
        "order_id": f"ORD-{user_id}-{int(time.time())}",
        "items": items,
        "ts": int(time.time()),
    })
    calc_payload = {"user_id": user_id, "items": items}
    calc_resp, rate_count, _, _ = await asyncio.gather(
        _http.post("/cart/calculate", json=calc_payload),
        _redis.incr(rate_key),
        _redis.rpush(history_key, order_entry),
        _redis.zadd("bench:popular", {f"product:{product_id}": 1}, incr=True),
    )
    calc_data = calc_resp.json()
    return json.dumps({
        "order_id": calc_data.get("order_id", f"ORD-{user_id}-{int(time.time())}"),
        "user_id": user_id,
        "total": calc_data.get("total", 0),
        "items_count": len(items),
        "rate_limit_count": rate_count,
        "status": "confirmed",
        "server_type": SERVER_TYPE,
    })

async def health(request):
    return JSONResponse({"status": "ok", "server_type": SERVER_TYPE})

app = Starlette(
    lifespan=lifespan,
    routes=[
        Route("/health", health),
        Mount("/", mcp_app),
    ],
)

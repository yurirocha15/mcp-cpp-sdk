# MCP Benchmark — TM Dev Lab v2

Performance comparison of C++, Python, and Go MCP server implementations under identical I/O-bound workloads (Redis + HTTP).

Methodology mirrors [TM Dev Lab v2](https://github.com/thiagomendes/benchmark-mcp-servers-v2).

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  Docker network                     │
│                                                     │
│  ┌──────────┐   ┌──────────────────────────────┐    │
│  │  Redis   │   │        API Service           │    │
│  │  :6379   │   │  (Go stdlib, 100k products)  │    │
│  └────┬─────┘   │           :8100              │    │
│       │         └──────────────┬───────────────┘    │
│       │                        │                    │
│  ┌────┴──────┐  ┌──────────────┴──┐  ┌──────────┐   │
│  │ C++ MCP   │  │  Python MCP     │  │  Go MCP  │   │
│  │  :8080    │  │    :8081        │  │  :8082   │   │
│  └───────────┘  └─────────────────┘  └──────────┘   │
└─────────────────────────────────────────────────────┘
```

Each MCP server exposes the same three tools:

| Tool | Operations |
|---|---|
| `search_products` | Parallel: HTTP product search + Redis `ZREVRANGE` (popularity) |
| `get_user_cart` | Sequential Redis `HGETALL` (cart), then parallel: HTTP product lookup + Redis `LRANGE` (history) |
| `checkout` | All parallel: HTTP cart total + Redis `INCR` (rate limit) + `RPUSH` (history) + `ZADD` (popularity) |

---

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/) with Compose v2 (`docker compose`)
- [k6](https://k6.io/docs/get-started/installation/) for load testing

---

## Quick Start

### 1. Seed Redis (one-time)

```bash
cd benchmark/
docker compose --profile seeder up redis-seeder
```

This populates:
- `bench:cart:user-{00000..09999}` — 10,000 user carts (HASH)
- `bench:history:user-{00000..00999}` — 1,000 users × 20 order entries (LIST)
- `bench:popular` — 100,000 product popularity scores (ZSET)
- `bench:ratelimit:user-{00000..00099}` — 100 rate-limit counters (STRING)

> The seeder exits when done. Redis data is not persisted between `docker compose down` calls unless you add a volume.

### 2. Start all servers

```bash
docker compose up --build
```

This starts: Redis, API service, C++ MCP server, Python MCP server, Go MCP server.

Wait for all healthchecks to go green (≈30–60s for the first build):

```bash
docker compose ps
```

All services should show `healthy`.

### 3. Verify servers are up

```bash
# C++ — health on dedicated port 9080
curl http://localhost:9080/health

# Python
curl http://localhost:8081/health

# Go
curl http://localhost:8082/health

# API service
curl http://localhost:8100/health
```

---

## Server Ports

| Service | Port | Notes |
|---|---|---|
| Redis | 6379 | Internal |
| API service | 8100 | Go stdlib, no external deps |
| C++ MCP | 8080 | MCP (Streamable HTTP) |
| C++ health | 9080 | Separate health-only listener |
| Python MCP | 8081 | MCP + `/health` on same port |
| Go MCP | 8082 | MCP + `/health` on same port |

> The C++ server exposes health on `port + 1000` (9080) because `HttpServerTransport` routes all traffic as MCP — there is no path-based routing in the SDK transport.

---

## Running Individual Servers

Start only what you need (Redis and API service are always required):

```bash
# Just Redis + API + C++
docker compose up redis api-service cpp-server

# Just Redis + API + Python
docker compose up redis api-service python-server

# Just Redis + API + Go
docker compose up redis api-service go-server
```

---

## Manual MCP Tool Call

Use any MCP-compatible client or raw HTTP. Example with `curl` (Streamable HTTP protocol):

```bash
# 1. Initialize session (C++ server)
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2024-11-05",
      "clientInfo": {"name": "test", "version": "1.0"},
      "capabilities": {}
    }
  }'

# 2. Call search_products
curl -s -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -H "mcp-session-id: <session-id-from-step-1>" \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/call",
    "params": {
      "name": "search_products",
      "arguments": {"category": "Electronics", "min_price": 50, "max_price": 500, "limit": 5}
    }
  }'
```

---

## Load Testing

> **Coming soon** — k6 scripts and the Go test client

The benchmark profile (matching TM Dev Lab v2):
- **50 virtual users**, 5-minute sustained load
- 15s ramp-up, 10s ramp-down
- 60s warmup excluded from metrics
- Each VU cycles through all three tools

```bash
# Run benchmark against C++ server
k6 run benchmark/k6/benchmark.js -e SERVER_URL=http://localhost:8080

# Run benchmark against Python server
k6 run benchmark/k6/benchmark.js -e SERVER_URL=http://localhost:8081

# Run benchmark against Go server
k6 run benchmark/k6/benchmark.js -e SERVER_URL=http://localhost:8082
```

---

## Teardown

```bash
# Stop and remove containers (keeps Redis data if volume was configured)
docker compose down

# Full cleanup including images
docker compose down --rmi all --volumes
```

---

## Troubleshooting

**C++ server build is slow** — the first build compiles the full SDK via Conan inside Docker. Subsequent builds use the Docker layer cache. Expect 3–5 minutes on first run.

**`healthy` never appears for cpp-server** — the healthcheck pings port 9080, not 8080. If 9080 isn't reachable, the server process likely failed during startup. Check logs:
```bash
docker compose logs cpp-server
```

**Redis seeder exited with error** — ensure Redis is healthy before the seeder runs. If re-seeding, flush Redis first:
```bash
docker compose exec redis redis-cli FLUSHALL
docker compose --profile seeder up redis-seeder
```

**Port conflicts** — if 8080/8081/8082/8100/6379 are in use locally, edit the host-side port mappings in `docker-compose.yml` (left side of `host:container`).

#!/usr/bin/env python3

import json
import os

from redis.client import Redis


def product_price(product_id: int) -> float:
    return 1.0 + (product_id % 99900) / 100.0


def main() -> None:
    redis_url = os.getenv("REDIS_URL", "redis://redis:6379")
    r = Redis.from_url(redis_url, decode_responses=True)
    pipe = r.pipeline(transaction=False)

    pending_ops = 0
    total_ops = 0
    cart_count = 0
    history_users = 0
    history_entries = 0
    popular_count = 0
    ratelimit_count = 0

    def flush() -> None:
        nonlocal pending_ops, pipe
        if pending_ops > 0:
            pipe.execute()
            pipe = r.pipeline(transaction=False)
            pending_ops = 0

    def track_op() -> None:
        nonlocal pending_ops, total_ops
        pending_ops += 1
        total_ops += 1
        if total_ops % 1000 == 0:
            flush()
            print(f"Progress: {total_ops} operations")

    for n in range(10000):
        user_key = f"bench:cart:user-{n:05d}"

        p1 = (n * 7) % 100000
        p2 = (n * 13) % 100000
        p3 = (n * 17) % 100000

        items = [
            {"product_id": p1, "quantity": 1},
            {"product_id": p2, "quantity": 1},
            {"product_id": p3, "quantity": 1},
        ]

        total = product_price(p1) + product_price(p2) + product_price(p3)

        pipe.hset(
            user_key,
            mapping={
                "user_id": f"user-{n:05d}",
                "items": json.dumps(items, separators=(",", ":")),
                "created": "2024-01-01T00:00:00Z",
                "expires": "2025-01-01T00:00:00Z",
                "total": f"{total:.2f}",
            },
        )
        cart_count += 1
        track_op()

    for n in range(1000):
        key = f"bench:history:user-{n:05d}"
        for j in range(20):
            entry = {
                "order_id": f"ORD-{n:05d}-{j:02d}",
                "total": float(f"{(10 + n * 0.1 + j * 0.25):.2f}"),
                "items_count": 3,
                "status": "delivered",
                "created": f"2024-01-{(j % 28) + 1:02d}T00:00:00Z",
            }
            pipe.rpush(key, json.dumps(entry, separators=(",", ":")))
            history_entries += 1
            track_op()
        history_users += 1

    for i in range(100000):
        pipe.zadd("bench:popular", {f"product:{i}": i * 7 % 10000})
        popular_count += 1
        track_op()

    for n in range(100):
        pipe.set(f"bench:ratelimit:user-{n:05d}", "0")
        ratelimit_count += 1
        track_op()

    flush()

    print("Seeding complete")
    print(f"Summary: carts={cart_count}")
    print(f"Summary: history_users={history_users}, history_entries={history_entries}")
    print(f"Summary: popular_members={popular_count}")
    print(f"Summary: ratelimit_counters={ratelimit_count}")


if __name__ == "__main__":
    main()

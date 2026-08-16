#!/usr/bin/env python3
"""Fast correctness gate for the protocol-correct benchmark workload."""

from __future__ import annotations

import argparse
import json
import math
import re
import socket
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


PROTOCOL_VERSION = "2024-11-05"
ELIGIBILITY_CONTRACT = "upstream-v2-strict-mcp-v1"
SUPPLEMENTAL_CONTRACT = "adapter-exact-v1"
SUPPORTED_PROTOCOL_VERSIONS = {
    "2024-11-05",
    "2025-03-26",
    "2025-06-18",
    "2025-11-25",
}
MCP_RESPONSE_MEDIA_TYPES = {"application/json", "text/event-stream"}
EXPECTED_TOOL_SCHEMAS = {
    "search_products": {
        "category": {"type": "string"},
        "min_price": {"type": "number"},
        "max_price": {"type": "number"},
        "limit": {"type": "integer"},
    },
    "get_user_cart": {"user_id": {"type": "string"}},
    "checkout": {
        "user_id": {"type": "string"},
        "items": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "product_id": {"type": "integer"},
                    "quantity": {"type": "integer"},
                },
                "required": ["product_id", "quantity"],
            },
        },
    },
}
CANONICAL_TOOL_ARGUMENTS = {
    "search_products": {
        "category": "Electronics",
        "min_price": 50.0,
        "max_price": 500.0,
        "limit": 10,
    },
    "get_user_cart": {"user_id": "user-00001"},
    "checkout": {
        "user_id": "user-00001",
        "items": [
            {"product_id": 42, "quantity": 2},
            {"product_id": 1337, "quantity": 1},
        ],
    },
}
CANONICAL_POPULAR_IDS = [
    92857,
    82857,
    72857,
    62857,
    52857,
    42857,
    32857,
    2857,
    22857,
    12857,
]
SCHEMA_ANNOTATION_KEYWORDS = {
    "$id",
    "$schema",
    "$comment",
    "title",
    "description",
    "default",
    "examples",
    "deprecated",
    "readOnly",
    "writeOnly",
}
INVALID_JSON_POINTER_ESCAPE = re.compile(r"~(?![01])")
JSON_SCHEMA_2020_12_DIALECTS = {
    "https://json-schema.org/draft/2020-12/schema",
    "https://json-schema.org/draft/2020-12/schema#",
}


def parse_response(body: bytes, media_type: str) -> dict[str, Any] | None:
    text = body.decode("utf-8").strip()
    if not text:
        return None
    if media_type == "application/json":
        message = json.loads(text)
        if not isinstance(message, dict):
            raise RuntimeError("a non-batch request returned a batch response")
        return message
    if media_type != "text/event-stream":
        raise RuntimeError(f"unsupported MCP response media type: {media_type!r}")

    messages: list[dict[str, Any]] = []
    for event in text.replace("\r\n", "\n").split("\n\n"):
        data = "\n".join(
            line[5:].lstrip()
            for line in event.splitlines()
            if line.startswith("data:")
        )
        if not data:
            continue
        message = json.loads(data)
        if not isinstance(message, dict):
            raise RuntimeError("an SSE event contained a non-object JSON-RPC message")
        messages.append(message)
    if len(messages) != 1:
        raise RuntimeError(
            f"expected exactly one JSON-RPC response, received {len(messages)}"
        )
    return messages[0]


def response_status(response: Any) -> int:
    status = getattr(response, "status", None)
    if status is None:
        status = response.getcode()
    return int(status)


def response_media_type(response: Any) -> str:
    content_type = response.headers.get("Content-Type", "")
    return content_type.split(";", 1)[0].strip().lower()


def redis_command(redis_url: str, *arguments: str) -> str | int | None:
    """Execute the small RESP subset needed by the workload side-effect gate."""
    parsed = urllib.parse.urlparse(redis_url)
    if parsed.scheme != "redis" or not parsed.hostname:
        raise RuntimeError(f"unsupported Redis URL: {redis_url!r}")
    if parsed.username or parsed.password or parsed.path not in {"", "/", "/0"}:
        raise RuntimeError("the benchmark verifier supports only unauthenticated Redis DB 0")

    encoded = [argument.encode("utf-8") for argument in arguments]
    request = [f"*{len(encoded)}\r\n".encode("ascii")]
    for argument in encoded:
        request.extend(
            (f"${len(argument)}\r\n".encode("ascii"), argument, b"\r\n")
        )

    with socket.create_connection((parsed.hostname, parsed.port or 6379), timeout=5) as connection:
        connection.sendall(b"".join(request))
        with connection.makefile("rb") as response:
            prefix = response.read(1)
            line = response.readline()
            if not prefix or not line.endswith(b"\r\n"):
                raise RuntimeError("Redis returned a truncated response")
            value = line[:-2]
            if prefix == b"+":
                return value.decode("utf-8")
            if prefix == b"-":
                raise RuntimeError(f"Redis command failed: {value.decode('utf-8')}")
            if prefix == b":":
                return int(value)
            if prefix != b"$":
                raise RuntimeError(f"unsupported Redis response prefix: {prefix!r}")
            length = int(value)
            if length == -1:
                return None
            payload = response.read(length)
            terminator = response.read(2)
            if len(payload) != length or terminator != b"\r\n":
                raise RuntimeError("Redis returned a truncated bulk string")
            return payload.decode("utf-8")


def redis_workload_state(redis_url: str, user_id: str = "user-00001") -> dict[str, Any]:
    rate_value = redis_command(redis_url, "GET", f"bench:ratelimit:{user_id}")
    history_length = redis_command(redis_url, "LLEN", f"bench:history:{user_id}")
    popularity_score = redis_command(
        redis_url, "ZSCORE", "bench:popular", "product:42"
    )
    return {
        "rate_limit_count": 0 if rate_value is None else int(rate_value),
        "history_length": int(history_length),
        "product_42_popularity": (
            None if popularity_score is None else float(popularity_score)
        ),
    }


def valid_workload_side_effects(
    before: dict[str, Any], after: dict[str, Any]
) -> bool:
    before_popularity = before.get("product_42_popularity")
    after_popularity = after.get("product_42_popularity")
    return (
        isinstance(before.get("rate_limit_count"), int)
        and isinstance(after.get("rate_limit_count"), int)
        and after["rate_limit_count"] == before["rate_limit_count"] + 1
        and isinstance(before.get("history_length"), int)
        and isinstance(after.get("history_length"), int)
        and after["history_length"] == before["history_length"] + 1
        and isinstance(before_popularity, (int, float))
        and not isinstance(before_popularity, bool)
        and isinstance(after_popularity, (int, float))
        and not isinstance(after_popularity, bool)
        and same_number(after_popularity, float(before_popularity) + 1)
    )


def exact_keys(value: Any, expected: set[str]) -> bool:
    return isinstance(value, dict) and set(value) == expected


def same_number(actual: Any, expected: float) -> bool:
    return isinstance(actual, (int, float)) and not isinstance(actual, bool) and math.isclose(
        float(actual), expected, rel_tol=0.0, abs_tol=1e-9
    )


def expected_search_products() -> list[dict[str, Any]]:
    brands = ["Alpha", "Phi", "Cast", "Lambda", "Forge"]
    products = []
    for index in range(10):
        product_id = 4901 + index * 20
        brand = brands[index % len(brands)]
        products.append(
            {
                "id": product_id,
                "sku": f"SKU-{product_id:06d}",
                "name": f"{brand} Electronics Item {product_id}",
                "price": 50 + index * 0.2,
                "rating": 3 if index % 2 == 0 else 1,
                "popularity_rank": 0,
            }
        )
    return products


def exact_object(actual: Any, expected: dict[str, Any]) -> bool:
    if not exact_keys(actual, set(expected)):
        return False
    for key, value in expected.items():
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if not same_number(actual[key], float(value)):
                return False
        elif actual[key] != value:
            return False
    return True


def schema_fragment_matches(
    actual: Any,
    expected: Any,
    keyword: str | None = None,
    root_schema: Any | None = None,
    instances: list[Any] | None = None,
) -> bool:
    if root_schema is None:
        root_schema = actual
    resolved, actual = resolve_local_schema_reference(actual, root_schema)
    if not resolved:
        return False
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return False
        if not schema_extras_are_compatible(
            actual, set(expected), root_schema, instances
        ):
            return False
        for name, expected_value in expected.items():
            actual_value = actual.get(name)
            if name == "properties" and (
                not isinstance(actual_value, dict)
                or set(actual_value) != set(expected_value)
            ):
                return False
            child_instances = instances
            if keyword == "properties" and instances is not None:
                child_instances = [
                    instance[name]
                    for instance in instances
                    if isinstance(instance, dict) and name in instance
                ]
            elif name == "items" and instances is not None:
                child_instances = [
                    item
                    for instance in instances
                    if isinstance(instance, list)
                    for item in instance
                ]
            if not schema_fragment_matches(
                actual_value,
                expected_value,
                name,
                root_schema,
                child_instances,
            ):
                return False
        return True
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(actual) != len(expected):
            return False
        if keyword == "required":
            return set(actual) == set(expected)
        return all(
            schema_fragment_matches(
                value,
                expected[index],
                root_schema=root_schema,
            )
            for index, value in enumerate(actual)
        )
    return actual == expected


def schema_extras_are_compatible(
    schema: dict[str, Any],
    compared_keywords: set[str],
    root_schema: Any,
    instances: list[Any] | None,
) -> bool:
    """Allow only extras that cannot reject any benchmark request."""
    for keyword, value in schema.items():
        if keyword in compared_keywords or keyword in SCHEMA_ANNOTATION_KEYWORDS:
            continue
        if keyword in {"$defs", "definitions"}:
            if not isinstance(value, dict):
                return False
            continue
        if keyword == "format":
            dialect = root_schema.get("$schema") if isinstance(root_schema, dict) else None
            if (
                not isinstance(value, str)
                or (dialect is not None and dialect not in JSON_SCHEMA_2020_12_DIALECTS)
            ):
                return False
            continue
        if keyword == "minimum":
            if (
                schema.get("type") not in {"integer", "number"}
                or isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(float(value))
                or not instances
                or any(
                    isinstance(instance, bool)
                    or not isinstance(instance, (int, float))
                    or not math.isfinite(float(instance))
                    or instance < value
                    for instance in instances
                )
            ):
                return False
            continue
        if keyword == "additionalProperties":
            if not isinstance(value, bool):
                return False
            continue
        if keyword == "required":
            properties = schema.get("properties")
            if (
                not isinstance(value, list)
                or any(not isinstance(name, str) for name in value)
                or len(value) != len(set(value))
                or not isinstance(properties, dict)
                or not set(value).issubset(properties)
            ):
                return False
            continue
        return False
    return True


def resolve_local_schema_reference(
    value: Any,
    root_schema: Any,
) -> tuple[bool, Any]:
    """Resolve a chain of local JSON Pointer references without fetching schemas."""
    current = value
    seen: set[str] = set()
    while isinstance(current, dict) and "$ref" in current:
        reference = current.get("$ref")
        if (
            not isinstance(reference, str)
            or reference in seen
            or any(
                keyword not in SCHEMA_ANNOTATION_KEYWORDS and keyword != "$ref"
                for keyword in current
            )
        ):
            return False, None
        seen.add(reference)
        if reference == "#":
            current = root_schema
            continue
        if not reference.startswith("#/"):
            return False, None
        current = root_schema
        for encoded_token in reference[2:].split("/"):
            if INVALID_JSON_POINTER_ESCAPE.search(encoded_token):
                return False, None
            token = encoded_token.replace("~1", "/").replace("~0", "~")
            if not isinstance(current, dict) or token not in current:
                return False, None
            current = current[token]
    return True, current


def validate_tool_schema(tool: dict[str, Any]) -> bool:
    name = tool.get("name")
    expected = EXPECTED_TOOL_SCHEMAS.get(name)
    arguments = CANONICAL_TOOL_ARGUMENTS.get(name)
    schema = tool.get("inputSchema")
    properties = schema.get("properties") if isinstance(schema, dict) else None
    return (
        expected is not None
        and arguments is not None
        and isinstance(schema, dict)
        and schema.get("type") == "object"
        and isinstance(properties, dict)
        and set(properties) == set(expected)
        and schema_extras_are_compatible(
            schema, {"type", "properties"}, schema, [arguments]
        )
        and all(
            schema_fragment_matches(
                properties[property],
                property_schema,
                root_schema=schema,
                instances=[arguments[property]],
            )
            for property, property_schema in expected.items()
        )
    )


class McpSession:
    def __init__(self, url: str, expected_protocol_version: str | None = None) -> None:
        self.url = url
        self.session_id: str | None = None
        self.protocol_version: str | None = None
        response = self.post(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": PROTOCOL_VERSION,
                    "capabilities": {},
                    "clientInfo": {"name": "benchmark-verify", "version": "1.0"},
                },
            }
        )
        if not response or response.get("jsonrpc") != "2.0" or response.get("id") != 1:
            raise RuntimeError(f"initialize returned an invalid JSON-RPC response: {response!r}")
        if response.get("error") or not isinstance(response.get("result"), dict):
            raise RuntimeError(f"initialize failed: {response!r}")
        result = response["result"]
        negotiated_version = result.get("protocolVersion")
        capabilities = result.get("capabilities")
        server_info = result.get("serverInfo")
        if negotiated_version not in SUPPORTED_PROTOCOL_VERSIONS:
            raise RuntimeError(f"initialize negotiated an unsupported protocol: {response!r}")
        if (
            expected_protocol_version is not None
            and negotiated_version != expected_protocol_version
        ):
            raise RuntimeError(
                "initialize negotiation changed from "
                f"{expected_protocol_version} to {negotiated_version}"
            )
        if not isinstance(capabilities, dict) or not isinstance(
            capabilities.get("tools"), dict
        ):
            raise RuntimeError(f"initialize did not advertise tools: {response!r}")
        if (
            not isinstance(server_info, dict)
            or not isinstance(server_info.get("name"), str)
            or not server_info["name"]
            or not isinstance(server_info.get("version"), str)
            or not server_info["version"]
        ):
            raise RuntimeError(f"initialize returned invalid serverInfo: {response!r}")
        self.protocol_version = negotiated_version
        self.post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def post(self, payload: dict[str, Any]) -> dict[str, Any] | None:
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        if self.protocol_version:
            headers["MCP-Protocol-Version"] = self.protocol_version
        request = urllib.request.Request(
            self.url,
            data=json.dumps(payload).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            response_session_id = response.headers.get("Mcp-Session-Id")
            if (
                self.session_id
                and response_session_id
                and response_session_id != self.session_id
            ):
                raise RuntimeError("server changed the MCP session identifier")
            if response_session_id:
                self.session_id = response_session_id
            body = response.read()
            if "id" not in payload:
                if response_status(response) != 202 or body.strip():
                    raise RuntimeError(
                        "notification response must be HTTP 202 with an empty body"
                    )
                return None
            if response_status(response) != 200:
                raise RuntimeError(
                    f"request response must be HTTP 200, got {response_status(response)}"
                )
            media_type = response_media_type(response)
            if media_type not in MCP_RESPONSE_MEDIA_TYPES:
                raise RuntimeError(
                    f"invalid MCP response Content-Type: {media_type!r}"
                )
            message = parse_response(body, media_type)
            if (
                not message
                or message.get("jsonrpc") != "2.0"
                or message.get("id") != payload["id"]
            ):
                raise RuntimeError(f"invalid JSON-RPC response: {message!r}")
            return message

    def close(self) -> None:
        if not self.session_id:
            return
        headers = {"Mcp-Session-Id": self.session_id}
        if self.protocol_version:
            headers["MCP-Protocol-Version"] = self.protocol_version
        request = urllib.request.Request(self.url, headers=headers, method="DELETE")
        try:
            with urllib.request.urlopen(request, timeout=5):
                pass
        except urllib.error.HTTPError as error:
            if error.code != 405:
                raise

    def __enter__(self) -> "McpSession":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def call_tool(
    url: str,
    name: str,
    arguments: dict[str, Any],
    expected_protocol_version: str,
) -> dict[str, Any]:
    with McpSession(url, expected_protocol_version) as session:
        response = session.post(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {"name": name, "arguments": arguments},
            }
        )
    if not response or response.get("error") or "result" not in response:
        raise RuntimeError(f"{name} failed: {response!r}")
    result = response["result"]
    if not isinstance(result, dict):
        raise RuntimeError(f"{name} returned an invalid result: {response!r}")
    if result.get("isError") is True:
        raise RuntimeError(f"{name} returned isError: {response!r}")
    content = result.get("content", [])
    if (
        not isinstance(content, list)
        or not content
        or not isinstance(content[0], dict)
        or content[0].get("type") != "text"
        or not isinstance(content[0].get("text"), str)
    ):
        raise RuntimeError(f"{name} returned no text content: {response!r}")
    value = json.loads(content[0]["text"])
    if not isinstance(value, dict):
        raise RuntimeError(f"{name} text content is not a JSON object: {response!r}")
    return value


def valid_upstream_search(value: dict[str, Any]) -> bool:
    return (
        value.get("total_found") == 2251
        and isinstance(value.get("products"), list)
        and len(value["products"]) == 10
        and isinstance(value.get("top10_popular_ids"), list)
        and len(value["top10_popular_ids"]) == 10
    )


def valid_upstream_cart(value: dict[str, Any], user_id: str) -> bool:
    cart = value.get("cart")
    return (
        value.get("user_id") == user_id
        and isinstance(cart, dict)
        and isinstance(cart.get("items"), list)
        and len(cart["items"]) >= 1
        and isinstance(value.get("recent_history"), list)
        and len(value["recent_history"]) == 5
    )


def valid_upstream_checkout(value: dict[str, Any], user_id: str) -> bool:
    total = value.get("total")
    rate_limit_count = value.get("rate_limit_count")
    return (
        value.get("user_id") == user_id
        and value.get("status") == "confirmed"
        and isinstance(total, (int, float))
        and not isinstance(total, bool)
        and total > 0
        and value.get("items_count") == 2
        and isinstance(rate_limit_count, (int, float))
        and not isinstance(rate_limit_count, bool)
    )


def valid_exact_search(
    value: dict[str, Any], expected_server_type: str | None
) -> bool:
    expected_products = expected_search_products()
    return (
        exact_keys(
            value,
            {"category", "total_found", "products", "top10_popular_ids", "server_type"},
        )
        and value["category"] == "Electronics"
        and value["total_found"] == 2251
        and isinstance(value["products"], list)
        and len(value["products"]) == len(expected_products)
        and all(
            exact_object(product, expected_products[index])
            for index, product in enumerate(value["products"])
        )
        and value["top10_popular_ids"] == CANONICAL_POPULAR_IDS
        and (
            expected_server_type is None
            or value["server_type"] == expected_server_type
        )
    )


def valid_exact_cart(
    value: dict[str, Any], expected_server_type: str | None
) -> bool:
    expected_items = [{"product_id": 8, "qty": 2}, {"product_id": 14, "qty": 2}]
    expected_history = [
        {
            "order_id": f"ORD-00001-{entry:02d}",
            "product_id": entry * 7 + 1,
            "qty": 1 + entry % 3,
            "price": round((entry * 13 + 1) / 100, 2),
            "ts": 1740000000 + 86400 + entry * 3600,
        }
        for entry in range(1, 6)
    ]
    cart = value.get("cart")
    return (
        exact_keys(value, {"user_id", "cart", "recent_history", "server_type"})
        and value["user_id"] == "user-00001"
        and exact_keys(cart, {"items", "item_count", "estimated_total"})
        and cart["items"] == expected_items
        and cart["item_count"] == 2
        and same_number(cart["estimated_total"], 44.04)
        and isinstance(value["recent_history"], list)
        and len(value["recent_history"]) == len(expected_history)
        and all(
            exact_object(history, expected_history[index])
            for index, history in enumerate(value["recent_history"])
        )
        and (
            expected_server_type is None
            or value["server_type"] == expected_server_type
        )
    )


def valid_exact_checkout(
    value: dict[str, Any], expected_server_type: str | None
) -> bool:
    return (
        exact_keys(
            value,
            {
                "order_id",
                "user_id",
                "total",
                "items_count",
                "rate_limit_count",
                "status",
                "server_type",
            },
        )
        and value["order_id"] == "ORD-user00001-2"
        and value["user_id"] == "user-00001"
        and same_number(value["total"], 24.63)
        and value["items_count"] == 2
        and value["rate_limit_count"] == 1
        and value["status"] == "confirmed"
        and (
            expected_server_type is None
            or value["server_type"] == expected_server_type
        )
    )


def verify(
    url: str,
    expected_protocol_version: str | None = None,
    expected_server_type: str | None = None,
    redis_url: str | None = None,
) -> dict[str, Any]:
    with McpSession(url, expected_protocol_version) as session:
        negotiated_protocol_version = session.protocol_version
        response = session.post(
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}
        )
    if not response or response.get("error") or not isinstance(response.get("result"), dict):
        raise RuntimeError(f"tools/list failed: {response!r}")
    tools = response["result"].get("tools")
    if not isinstance(tools, list) or not all(isinstance(tool, dict) for tool in tools):
        raise RuntimeError(f"tools/list returned an invalid tools array: {response!r}")
    names = [tool.get("name") for tool in tools]
    required = set(EXPECTED_TOOL_SCHEMAS)
    if len(tools) != len(required) or set(names) != required:
        raise RuntimeError(f"tools/list returned the wrong workload tools: {names!r}")
    schema_objects = {
        str(tool["name"]): isinstance(tool.get("inputSchema"), dict) for tool in tools
    }
    if not all(schema_objects.values()):
        raise RuntimeError(f"tools/list returned an invalid input schema object: {tools!r}")
    exact_schema_checks = {
        str(tool["name"]): validate_tool_schema(tool) for tool in tools
    }

    assert negotiated_protocol_version is not None
    search = call_tool(
        url,
        "search_products",
        {"category": "Electronics", "min_price": 50, "max_price": 500, "limit": 10},
        negotiated_protocol_version,
    )
    if not valid_upstream_search(search):
        raise RuntimeError(f"invalid search_products result: {search!r}")

    cart = call_tool(
        url,
        "get_user_cart",
        {"user_id": "user-00001"},
        negotiated_protocol_version,
    )
    if not valid_upstream_cart(cart, "user-00001"):
        raise RuntimeError(f"invalid get_user_cart result: {cart!r}")

    redis_before = redis_workload_state(redis_url) if redis_url else None
    checkout = call_tool(
        url,
        "checkout",
        {
            "user_id": "user-00001",
            "items": [
                {"product_id": 42, "quantity": 2},
                {"product_id": 1337, "quantity": 1},
            ],
        },
        negotiated_protocol_version,
    )
    if not valid_upstream_checkout(checkout, "user-00001"):
        raise RuntimeError(f"invalid checkout result: {checkout!r}")
    redis_after = redis_workload_state(redis_url) if redis_url else None
    side_effects_valid = (
        redis_before is not None
        and redis_after is not None
        and valid_workload_side_effects(redis_before, redis_after)
    )
    if redis_url and not side_effects_valid:
        raise RuntimeError(
            "checkout did not produce the required Redis side effects: "
            f"before={redis_before!r}, after={redis_after!r}"
        )

    supplemental_checks = {
        "exact_tool_schemas": all(exact_schema_checks.values()),
        "exact_search_fixture": valid_exact_search(search, expected_server_type),
        "exact_cart_fixture": valid_exact_cart(cart, expected_server_type),
        "exact_checkout_fixture": valid_exact_checkout(
            checkout, expected_server_type
        ),
    }
    return {
        "negotiated_protocol_version": negotiated_protocol_version,
        "eligibility_contract": ELIGIBILITY_CONTRACT,
        "eligibility_valid": True,
        "eligibility_checks": {
            "exact_tool_names": True,
            "input_schemas_are_objects": schema_objects,
            "search_products": True,
            "get_user_cart": True,
            "checkout": True,
            "redis_side_effects": side_effects_valid if redis_url else None,
        },
        "supplemental_validation": {
            "contract": SUPPLEMENTAL_CONTRACT,
            "valid": all(supplemental_checks.values()),
            "checks": supplemental_checks,
            "tool_schema_checks": exact_schema_checks,
            "observations": {
                "checkout_rate_limit_count": checkout.get("rate_limit_count"),
                "redis_before": redis_before,
                "redis_after": redis_after,
            },
        },
    }


def initialize_only(url: str, expected_protocol_version: str | None = None) -> str:
    with McpSession(url, expected_protocol_version) as session:
        assert session.protocol_version is not None
        return session.protocol_version


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--name", default="server")
    parser.add_argument("--expected-protocol-version")
    parser.add_argument("--expected-server-type")
    parser.add_argument(
        "--eligibility-contract",
        choices=[ELIGIBILITY_CONTRACT],
        default=ELIGIBILITY_CONTRACT,
    )
    parser.add_argument("--redis-url")
    parser.add_argument("--require-supplemental", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--initialize-only", action="store_true")
    args = parser.parse_args()
    if args.initialize_only:
        negotiated_protocol_version = initialize_only(
            args.url, args.expected_protocol_version
        )
        result = {
            "schema_version": 2,
            "server": args.name,
            "requested_protocol_version": PROTOCOL_VERSION,
            "negotiated_protocol_version": negotiated_protocol_version,
            "eligibility_contract": ELIGIBILITY_CONTRACT,
            "eligibility_valid": None,
            "supplemental_validation": None,
        }
    else:
        result = verify(
            args.url,
            args.expected_protocol_version,
            args.expected_server_type,
            args.redis_url,
        )
        result.update(
            {
                "schema_version": 2,
                "server": args.name,
                "requested_protocol_version": PROTOCOL_VERSION,
                "expected_server_type": args.expected_server_type,
            }
        )
        result["supplemental_validation"]["required"] = (
            args.require_supplemental
        )
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if (
        not args.initialize_only
        and args.require_supplemental
        and not result["supplemental_validation"]["valid"]
    ):
        raise RuntimeError(
            f"required {SUPPLEMENTAL_CONTRACT} validation failed: "
            f"{result['supplemental_validation']['checks']!r}"
        )
    suffix = (
        "initialize lifecycle passed"
        if args.initialize_only
        else f"{ELIGIBILITY_CONTRACT} passed"
    )
    print(
        f"{args.name}: protocol {result['negotiated_protocol_version']}; {suffix}"
    )


if __name__ == "__main__":
    main()

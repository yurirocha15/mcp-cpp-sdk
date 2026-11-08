use std::sync::Arc;

use anyhow::Result;
use axum::serve::ListenerExt;
use axum::{Router, routing::get, Json};
use rmcp::{
    ErrorData as McpError, ServerHandler,
    handler::server::{router::tool::ToolRouter, wrapper::Parameters},
    model::*,
    tool, tool_handler, tool_router,
};
use rmcp::transport::streamable_http_server::{
    StreamableHttpServerConfig, StreamableHttpService,
    session::local::LocalSessionManager,
};
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::json;

// ─── Parameter types ──────────────────────────────────────────────────────────

#[derive(Deserialize, JsonSchema)]
struct SearchProductsParams {
    #[serde(default = "default_category")]
    category: String,
    #[serde(default = "default_min_price")]
    min_price: f64,
    #[serde(default = "default_max_price")]
    max_price: f64,
    #[serde(default = "default_limit")]
    limit: u32,
}

fn default_category() -> String { "Electronics".to_string() }
fn default_min_price() -> f64  { 50.0 }
fn default_max_price() -> f64  { 500.0 }
fn default_limit()     -> u32  { 10 }

#[derive(Deserialize, JsonSchema)]
struct GetUserCartParams {
    #[serde(default = "default_user_id")]
    user_id: String,
}
fn default_user_id() -> String { "user-00042".to_string() }

#[derive(Deserialize, JsonSchema)]
struct CheckoutItem {
    product_id: u32,
    quantity: u32,
}

#[derive(Deserialize, JsonSchema)]
struct CheckoutParams {
    #[serde(default = "default_user_id")]
    user_id: String,
    #[serde(default = "default_checkout_items")]
    items: Vec<CheckoutItem>,
}

fn default_checkout_items() -> Vec<CheckoutItem> {
    vec![
        CheckoutItem { product_id: 42, quantity: 2 },
        CheckoutItem { product_id: 1337, quantity: 1 },
    ]
}

// ─── Server ───────────────────────────────────────────────────────────────────

#[derive(Clone)]
struct BenchmarkServer {
    http: Arc<reqwest::Client>,
    redis: deadpool_redis::Pool,
    api_url: Arc<String>,
    tool_router: ToolRouter<BenchmarkServer>,
}

impl BenchmarkServer {
    async fn new() -> Result<Self> {
        let api_url = std::env::var("API_SERVICE_URL")
            .unwrap_or_else(|_| "http://mcp-api-service:8100".to_string());
        let redis_url = std::env::var("REDIS_URL")
            .unwrap_or_else(|_| "redis://mcp-redis:6379".to_string());

        let redis_cfg = deadpool_redis::Config::from_url(redis_url);
        let redis_pool = redis_cfg
            .create_pool(Some(deadpool_redis::Runtime::Tokio1))
            .map_err(|e| anyhow::anyhow!("Redis pool error: {e}"))?;
        // Pool de 20 conexões: suficiente para 50 VUs com até 3 ops Redis em paralelo por request
        // Pool sized to match Quarkus (quarkus.redis.max-pool-size=100).
        // With 50 VUs each potentially holding 2 concurrent connections
        // (HGETALL + parallel LRANGE in get_user_cart), 100 covers peak demand.
        redis_pool.resize(100);

        Ok(Self {
            http: Arc::new(reqwest::Client::builder()
                .pool_max_idle_per_host(50)
                .tcp_nodelay(true)          // desabilita Nagle — reduz latência TCP
                .build()?),
            redis: redis_pool,
            api_url: Arc::new(api_url),
            tool_router: Self::tool_router(),
        })
    }
}

#[tool_router]
impl BenchmarkServer {
    #[tool(description = "Search products by category and price range, merged with popularity data")]
    async fn search_products(
        &self,
        Parameters(p): Parameters<SearchProductsParams>,
    ) -> Result<CallToolResult, McpError> {
        // Spawn Redis ZREVRANGE como task independente — roda em paralelo com o HTTP search
        let pool = self.redis.clone();
        let redis_task = tokio::spawn(async move {
            let mut conn = pool.get().await.ok()?;
            let mut cmd = redis::cmd("ZREVRANGE");
            cmd.arg("bench:popular").arg(0i64).arg(9i64);
            cmd.query_async::<Vec<String>>(&mut *conn).await.ok()
        });

        // HTTP search (roda enquanto Redis está sendo buscado acima)
        let search_res = self.http
            .get(format!("{}/products/search", self.api_url))
            .query(&[
                ("category",  p.category.as_str()),
                ("min_price", &p.min_price.to_string()),
                ("max_price", &p.max_price.to_string()),
                ("limit",     &p.limit.to_string()),
            ])
            .send().await
            .map_err(|e| McpError::new(rmcp::model::ErrorCode::INTERNAL_ERROR, e.to_string(), None))?;
        let search_data: serde_json::Value = search_res.json().await
            .map_err(|e| McpError::new(rmcp::model::ErrorCode::INTERNAL_ERROR, e.to_string(), None))?;

        // Aguarda Redis (já deve ter terminado enquanto HTTP processava)
        let popular_ids: Vec<String> = redis_task.await.ok().flatten().unwrap_or_default();

        let top10_ids: Vec<u64> = popular_ids.iter()
            .filter_map(|m| m.strip_prefix("product:").and_then(|s| s.parse().ok()))
            .collect();
        let top10_set: std::collections::HashMap<u64, usize> = top10_ids.iter()
            .enumerate().map(|(i, &id)| (id, i + 1)).collect();

        let empty_vec = vec![];
        let raw_products = search_data["products"].as_array().unwrap_or(&empty_vec);
        let products: Vec<serde_json::Value> = raw_products.iter().map(|prod| {
            let id = prod["id"].as_u64().unwrap_or(0);
            json!({
                "id": id, "sku": prod["sku"], "name": prod["name"],
                "price": prod["price"], "rating": prod["rating"],
                "popularity_rank": top10_set.get(&id).copied().unwrap_or(0),
            })
        }).collect();

        let result = json!({
            "category": p.category,
            "total_found": search_data["total_found"],
            "products": products,
            "top10_popular_ids": top10_ids,
            "server_type": "rust",
        });
        Ok(CallToolResult::success(vec![ContentBlock::text(result.to_string())]))
    }

    #[tool(description = "Get user cart details with recent order history")]
    async fn get_user_cart(
        &self,
        Parameters(p): Parameters<GetUserCartParams>,
    ) -> Result<CallToolResult, McpError> {
        // Step 1: HGETALL (sequential — needed to determine first_product_id)
        let cart_hash: std::collections::HashMap<String, String> = {
            let mut conn = self.redis.get().await
                .map_err(|e| McpError::new(rmcp::model::ErrorCode::INTERNAL_ERROR, e.to_string(), None))?;
            let mut hgetall = redis::cmd("HGETALL");
            hgetall.arg(format!("bench:cart:{}", p.user_id));
            hgetall.query_async(&mut *conn).await.unwrap_or_default()
        }; // conn returned to pool before parallel step

        let items_json = cart_hash.get("items").map(|s| s.as_str()).unwrap_or("[]");
        let items: serde_json::Value = serde_json::from_str(items_json).unwrap_or(json!([]));
        let first_product_id = items[0]["product_id"].as_u64().unwrap_or(1);
        let estimated_total: f64 = cart_hash.get("total")
            .and_then(|s| s.parse().ok()).unwrap_or(0.0);

        // Step 2: parallel — Redis LRANGE + HTTP product detail (separate pool connection)
        let pool = self.redis.clone();
        let hist_key = format!("bench:history:{}", p.user_id);
        let history_task = tokio::spawn(async move {
            let mut conn = pool.get().await.ok()?;
            let mut lrange = redis::cmd("LRANGE");
            lrange.arg(&hist_key).arg(0i64).arg(4i64);
            lrange.query_async::<Vec<String>>(&mut *conn).await.ok()
        });

        let _ = self.http
            .get(format!("{}/products/{}", self.api_url, first_product_id))
            .send().await;

        let history_raw = history_task.await.ok().flatten().unwrap_or_default();

        let recent_history: Vec<serde_json::Value> = history_raw.iter()
            .map(|e| serde_json::from_str(e).unwrap_or(json!({"raw": e})))
            .collect();

        let item_count = items.as_array().map(|a| a.len()).unwrap_or(0);
        let result = json!({
            "user_id": p.user_id,
            "cart": {"items": items, "item_count": item_count, "estimated_total": estimated_total},
            "recent_history": recent_history,
            "server_type": "rust",
        });
        Ok(CallToolResult::success(vec![ContentBlock::text(result.to_string())]))
    }

    #[tool(description = "Process checkout: calculate total, update rate limit, record history")]
    async fn checkout(
        &self,
        Parameters(p): Parameters<CheckoutParams>,
    ) -> Result<CallToolResult, McpError> {
        let user_num: u64 = p.user_id.split('-').last()
            .and_then(|s| s.parse().ok()).unwrap_or(42);
        let rate_key = format!("bench:ratelimit:user-{:05}", user_num % 100);
        let hist_key = format!("bench:history:{}", p.user_id);
        let product_id = p.items.first().map(|i| i.product_id).unwrap_or(42);

        let ts = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default().as_secs();
        let order_entry = json!({
            "order_id": format!("ORD-{}-{}", p.user_id, ts),
            "items": p.items.iter().map(|i| json!({"product_id":i.product_id,"quantity":i.quantity})).collect::<Vec<_>>(),
            "ts": ts,
        }).to_string();

        let items_body: Vec<serde_json::Value> = p.items.iter()
            .map(|i| json!({"product_id": i.product_id, "quantity": i.quantity}))
            .collect();
        let calc_body = json!({"user_id": p.user_id, "items": items_body});

        // HTTP POST e pipeline Redis em paralelo (conn independente do HTTP)
        let calc_fut = self.http
            .post(format!("{}/cart/calculate", self.api_url))
            .json(&calc_body)
            .send();

        // Pipeline: INCR + RPUSH + ZADD em 1 round-trip, 1 conexão do pool
        let pool = self.redis.clone();
        let redis_fut = async move {
            let mut conn = pool.get().await
                .map_err(|e| redis::RedisError::from(std::io::Error::new(std::io::ErrorKind::Other, e.to_string())))?;
            let mut pipe = redis::pipe();
            pipe.cmd("INCR").arg(&rate_key)
                .cmd("RPUSH").arg(&hist_key).arg(&order_entry).ignore()
                .cmd("ZADD").arg("bench:popular").arg("INCR").arg(1i64)
                    .arg(format!("product:{}", product_id)).ignore();
            let (rate_count,): (i64,) = pipe.query_async(&mut *conn).await?;
            Ok::<i64, redis::RedisError>(rate_count)
        };

        let (calc_res, redis_res) = tokio::join!(calc_fut, redis_fut);
        let rate_count = redis_res.unwrap_or(0);

        let calc_data: serde_json::Value = calc_res
            .map_err(|e| McpError::new(rmcp::model::ErrorCode::INTERNAL_ERROR, e.to_string(), None))?
            .json().await
            .map_err(|e| McpError::new(rmcp::model::ErrorCode::INTERNAL_ERROR, e.to_string(), None))?;
        let total = calc_data["total"].as_f64().unwrap_or(0.0);
        let order_id = calc_data["order_id"].as_str().unwrap_or("ORD-unknown");

        let result = json!({
            "order_id": order_id,
            "user_id": p.user_id,
            "total": total,
            "items_count": p.items.len(),
            "rate_limit_count": rate_count,
            "status": "confirmed",
            "server_type": "rust",
        });
        Ok(CallToolResult::success(vec![ContentBlock::text(result.to_string())]))
    }
}

#[tool_handler]
impl ServerHandler for BenchmarkServer {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().build())
            .with_server_info(Implementation::new("benchmark-rust-sdk", "1.0.0"))
    }
}

// ─── Health endpoint ──────────────────────────────────────────────────────────

async fn health() -> Json<serde_json::Value> {
    Json(json!({"status": "ok", "server_type": "rust"}))
}

// ─── Main ─────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn")),
        )
        .init();

    let ct = tokio_util::sync::CancellationToken::new();
    let server = BenchmarkServer::new().await?;

    // Requests of 2026-07-28 have no session; legacy_session_mode only decides whether an older
    // client gets one. MCP_SESSION_MODE=stateful serves older clients with sessions; anything
    // else (the default) serves them without.
    let stateful = std::env::var("MCP_SESSION_MODE").is_ok_and(|mode| mode == "stateful");
    tracing::warn!("session mode for older protocol revisions: {}", if stateful { "stateful" } else { "stateless" });

    let service = StreamableHttpService::new(
        move || Ok(server.clone()),
        LocalSessionManager::default().into(),
        StreamableHttpServerConfig::default()
            .with_legacy_session_mode(stateful)
            .with_json_response(true)
            .with_cancellation_token(ct.child_token()),
    );

    let router = Router::new()
        .route("/health", get(health))
        .nest_service("/mcp", service);

    // Stateful responses go out in several writes; Go and the C++ servers already disable Nagle.
    let listener = tokio::net::TcpListener::bind("0.0.0.0:8095")
        .await?
        .tap_io(|tcp| {
            if let Err(err) = tcp.set_nodelay(true) {
                tracing::warn!("failed to set TCP_NODELAY: {err}");
            }
        });
    tracing::warn!("Rust MCP server (rmcp SDK) listening on 0.0.0.0:8095");

    axum::serve(listener, router)
        .with_graceful_shutdown(async move {
            tokio::signal::ctrl_c().await.unwrap();
            ct.cancel();
        })
        .await?;

    Ok(())
}

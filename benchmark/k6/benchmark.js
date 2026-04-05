import {textSummary} from 'https://jslib.k6.io/k6-summary/0.0.1/index.js';
import {check, sleep} from 'k6';
import http from 'k6/http';
import {Counter, Trend} from 'k6/metrics';

// Environment variables
const SERVER_URL = __ENV.SERVER_URL;
const SERVER_NAME = __ENV.SERVER_NAME || 'unknown';
const OUTPUT_PATH = __ENV.OUTPUT_PATH;
const WARMUP_SECONDS = 60;

if (!SERVER_URL) {
    throw new Error('SERVER_URL environment variable is required');
}

// Custom metrics
const mcpInitDuration = new Trend('mcp_init_duration');
const mcpSearchProductsDuration = new Trend('mcp_search_products_duration');
const mcpGetUserCartDuration = new Trend('mcp_get_user_cart_duration');
const mcpCheckoutDuration = new Trend('mcp_checkout_duration');
const mcpSessionDuration = new Trend('mcp_session_duration');
const mcpRequests = new Counter('mcp_requests');

// Load profile matching TM Dev Lab v2
export const options = {
    stages: [
        {duration: '15s', target: 50},  // ramp-up
        {duration: '5m', target: 50},   // sustained load
        {duration: '10s', target: 0},   // ramp-down
    ],
    thresholds: {
        'http_req_failed': ['rate<0.05'],
    },
    summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)', 'count'],
};

const BASE_HEADERS = {
    'Content-Type': 'application/json',
    'Accept': 'application/json, text/event-stream',
    'MCP-Protocol-Version': '2025-11-25',
};

export function setup() { return {startMs: Date.now()}; }

function parseBody(body) {
    if (!body)
        return null;
    const text = body.trim();
    if (text.length === 0)
        return null;
    if (text.startsWith('{') || text.startsWith('[')) {
        try {
            return JSON.parse(text);
        } catch (e) { /* fall through */
        }
    }
    // SSE format
    const lines = text.split('\n');
    for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed.startsWith('data:')) {
            const payload = trimmed.substring(5).trim();
            if (payload && payload.startsWith('{')) {
                try {
                    return JSON.parse(payload);
                } catch (e) { /* continue */
                }
            }
        }
    }
    return null;
}

function mcpSession(toolName, toolArgs, setupData) {
    const sessionStart = Date.now();
    const isWarmup = (Date.now() - setupData.startMs) <= WARMUP_SECONDS * 1000;

    // 1. Initialize
    const initPayload = JSON.stringify({
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
            protocolVersion: '2025-11-25',
            capabilities: {},
            clientInfo: {name: 'k6-bench', version: '1.0'}
        }
    });

    const initResp =
        http.post(SERVER_URL, initPayload, {headers: BASE_HEADERS, tags: {name: 'mcp_initialize'}});
    mcpRequests.add(1);

    check(initResp, {
        'initialize status 200': (r) => r.status === 200,
    });

    if (!isWarmup) {
        mcpInitDuration.add(initResp.timings.duration);
    }

    const sessionId = initResp.headers['Mcp-Session-Id'];
    const sessionHeaders =
        sessionId ? {...BASE_HEADERS, 'Mcp-Session-Id': sessionId} : {...BASE_HEADERS};

    // 2. Send initialized notification
    const initNotifyPayload = JSON.stringify({jsonrpc: '2.0', method: 'notifications/initialized'});

    const notifyResp = http.post(SERVER_URL, initNotifyPayload,
                                 {headers: sessionHeaders, tags: {name: 'protocol_overhead'}});
    mcpRequests.add(1);

    check(notifyResp, {
        'initialized notification accepted': (r) =>
            r.status === 200 || r.status === 202 || r.status === 204,
    });

    // 3. Call tool
    const toolPayload = JSON.stringify(
        {jsonrpc: '2.0', id: 2, method: 'tools/call', params: {name: toolName, arguments: toolArgs}});

    const toolResp =
        http.post(SERVER_URL, toolPayload, {headers: sessionHeaders, tags: {name: `mcp_${toolName}`}});
    mcpRequests.add(1);

    const toolSuccess = check(toolResp, {
        [`${toolName} status 200`]: (r) => r.status === 200,
    });

    if (!isWarmup) {
        if (toolName === 'search_products') {
            mcpSearchProductsDuration.add(toolResp.timings.duration);
        } else if (toolName === 'get_user_cart') {
            mcpGetUserCartDuration.add(toolResp.timings.duration);
        } else if (toolName === 'checkout') {
            mcpCheckoutDuration.add(toolResp.timings.duration);
        }
    }

    // Verify response correctness
    if (toolSuccess) {
        const data = parseBody(toolResp.body);
        if (data && data.result && data.result.content && data.result.content[0]) {
            const content = data.result.content[0];
            if (content.type === 'text' && content.text) {
                try {
                    const d = JSON.parse(content.text);

                    if (toolName === 'search_products') {
                        check(d, {
                            'search_products valid response': (obj) => obj.total_found > 0 &&
                                obj.products.length === 10 && obj.top10_popular_ids.length === 10
                        });
                    } else if (toolName === 'get_user_cart') {
                        check(d, {
                            'get_user_cart valid response': (obj) => !!obj.user_id &&
                                obj.cart.items.length >= 1 && obj.recent_history.length >= 1
                        });
                    } else if (toolName === 'checkout') {
                        check(d, {
                            'checkout valid response': (obj) =>
                                obj.status === 'confirmed' && obj.total > 0 && obj.items_count === 2
                        });
                    }
                } catch (e) {
                    console.error(`Failed to parse ${toolName} response: ${e}`);
                }
            }
        }
    }

    // 4. Delete session (only if server returned a session ID)
    if (sessionId) {
        const deleteResp =
            http.del(SERVER_URL, null, {headers: sessionHeaders, tags: {name: 'protocol_overhead'}});
        mcpRequests.add(1);

        check(deleteResp, {
            'delete session status 200': (r) => r.status === 200 || r.status === 204,
        });
    }

    if (!isWarmup) {
        mcpSessionDuration.add(Date.now() - sessionStart);
    }
}

export default function(setupData) {
    const userId = `user-${String(__VU % 1000).padStart(5, '0')}`;

    // Tool call 1: search_products
    mcpSession('search_products',
               {category: 'Electronics', min_price: 50.0, max_price: 500.0, limit: 10}, setupData);

    // Tool call 2: get_user_cart
    mcpSession('get_user_cart', {user_id: userId}, setupData);

    // Tool call 3: checkout
    mcpSession(
        'checkout',
        {user_id: userId, items: [{product_id: 42, quantity: 2}, {product_id: 1337, quantity: 1}]},
        setupData);

    // Tool call 4: tools/list (in separate session)
    const listSessionStart = Date.now();
    const isWarmup = (Date.now() - setupData.startMs) <= WARMUP_SECONDS * 1000;

    const initPayload = JSON.stringify({
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
            protocolVersion: '2025-11-25',
            capabilities: {},
            clientInfo: {name: 'k6-bench', version: '1.0'}
        }
    });

    const initResp =
        http.post(SERVER_URL, initPayload, {headers: BASE_HEADERS, tags: {name: 'mcp_initialize'}});
    mcpRequests.add(1);

    const sessionId = initResp.headers['Mcp-Session-Id'];
    if (sessionId) {
        const sessionHeaders = {...BASE_HEADERS, 'Mcp-Session-Id': sessionId};

        const initNotifyPayload = JSON.stringify({jsonrpc: '2.0', method: 'notifications/initialized'});
        http.post(SERVER_URL, initNotifyPayload,
                  {headers: sessionHeaders, tags: {name: 'protocol_overhead'}});
        mcpRequests.add(1);

        const listPayload = JSON.stringify({jsonrpc: '2.0', id: 2, method: 'tools/list', params: {}});
        http.post(SERVER_URL, listPayload, {headers: sessionHeaders, tags: {name: 'mcp_tools_list'}});
        mcpRequests.add(1);

        http.del(SERVER_URL, null, {headers: sessionHeaders, tags: {name: 'protocol_overhead'}});
        mcpRequests.add(1);
    }

    sleep(0.05);
}

export function handleSummary(data) {
    const summary = textSummary(data, {indent: ' ', enableColors: true});

    console.log(`\n=== Benchmark Summary for ${SERVER_NAME} ===`);
    console.log(summary);

    const result = {
        stdout: summary,
    };

    if (OUTPUT_PATH) {
        result[OUTPUT_PATH] = JSON.stringify(data, null, 2);
    }

    return result;
}

// Load profile for protocol revision 2026-07-28.
//
// The workload is benchmark.js's: each iteration calls the three tools and lists the tools, then
// pauses. The protocol is not: a 2026-07-28 request stands alone. There is no initialize, no
// notifications/initialized, no session and no DELETE; every request names the protocol version and
// the client's capabilities in `_meta` and mirrors the version, the method and, for tools/call, the
// tool name in the MCP-Protocol-Version, Mcp-Method and Mcp-Name headers. One operation is
// therefore one HTTP request.
import {check, sleep} from 'k6';
import http from 'k6/http';
import {Counter, Rate, Trend} from 'k6/metrics';

const SERVER_URL = __ENV.SERVER_URL || 'http://localhost:8080/mcp';
const SERVER_NAME = __ENV.SERVER_NAME || 'unknown';
const PROTOCOL_VERSION = __ENV.MCP_PROTOCOL_VERSION || '2026-07-28';
const EXPECTED_PROTOCOL_VERSION = __ENV.EXPECTED_PROTOCOL_VERSION || null;
const MODE = (__ENV.BENCHMARK_MODE || 'measurement').toLowerCase();
const BENCHMARK_CONTRACT = __ENV.BENCHMARK_CONTRACT || 'upstream-v2-strict-mcp-v1';

// Pause after each iteration, in seconds. The default paces every virtual user; 0 removes the
// ceiling that pacing sets on the rate.
const THINK_TIME_TEXT = __ENV.BENCHMARK_THINK_TIME === undefined ? '0.05' : __ENV.BENCHMARK_THINK_TIME;
const THINK_TIME_SECONDS = THINK_TIME_TEXT.trim() === '' ? Number.NaN : Number(THINK_TIME_TEXT);

const VUS = Number.parseInt(__ENV.BENCHMARK_VUS || '50', 10);
const WARMUP_RAMP_DURATION = __ENV.BENCHMARK_RAMP_DURATION || '15s';
const WARMUP_LOAD_DURATION = __ENV.BENCHMARK_WARMUP_DURATION || '60s';
const MEASUREMENT_DURATION = __ENV.BENCHMARK_MEASURE_DURATION || '5m';

if (!Number.isInteger(VUS) || VUS <= 0) {
    throw new Error(`BENCHMARK_VUS must be a positive integer, got "${__ENV.BENCHMARK_VUS}"`);
}

if (!Number.isFinite(THINK_TIME_SECONDS) || THINK_TIME_SECONDS < 0) {
    throw new Error(`BENCHMARK_THINK_TIME must be a non-negative number, got "${THINK_TIME_TEXT}"`);
}

if (MODE !== 'warmup' && MODE !== 'measurement') {
    throw new Error(
        `BENCHMARK_MODE must be "warmup" or "measurement", got "${MODE}"`,
    );
}

if (BENCHMARK_CONTRACT !== 'upstream-v2-strict-mcp-v1') {
    throw new Error(
        'BENCHMARK_CONTRACT must be "upstream-v2-strict-mcp-v1", ' +
            `got "${BENCHMARK_CONTRACT}"`,
    );
}

const scenario = MODE === 'warmup' ? {
    executor: 'ramping-vus',
    startVUs: 0,
    stages: [
        {duration: WARMUP_RAMP_DURATION, target: VUS},
        {duration: WARMUP_LOAD_DURATION, target: VUS},
    ],
    gracefulRampDown: '0s',
    gracefulStop: '0s',
} :
                                     {
                                         executor: 'constant-vus',
                                         vus: VUS,
                                         duration: MEASUREMENT_DURATION,
                                         gracefulStop: '0s',
                                     };

export const options = {
    scenarios: {
        workload: scenario,
    },
    thresholds: {
        checks: [{threshold: 'rate==1', abortOnFail: true}],
        http_req_failed: [{threshold: 'rate==0', abortOnFail: true}],
        mcp_error_rate: [{threshold: 'rate==0', abortOnFail: true}],
        mcp_errors: [{threshold: 'count==0', abortOnFail: true}],
    },
    summaryTrendStats: [
        'avg',
        'min',
        'med',
        'max',
        'p(90)',
        'p(95)',
        'p(99)',
        'count',
    ],
};

const toolsListDuration = new Trend('mcp_tools_list_duration', true);
const searchProductsDuration = new Trend('mcp_search_products_duration', true);
const getUserCartDuration = new Trend('mcp_get_user_cart_duration', true);
const checkoutDuration = new Trend('mcp_checkout_duration', true);
const combinedToolDuration = new Trend('mcp_tool_duration', true);

// An operation is one tools/call or tools/list request whose shape and contract checks have run.
const benchmarkOperations = new Counter('benchmark_operations');
const mcpMessages = new Counter('mcp_messages');
const mcpErrors = new Counter('mcp_errors');
const mcpErrorRate = new Rate('mcp_error_rate');

const BASE_HEADERS = {
    Accept: 'application/json, text/event-stream',
    'Content-Type': 'application/json',
    'MCP-Protocol-Version': PROTOCOL_VERSION,
};

const EXPECTED_TOOLS = ['search_products', 'get_user_cart', 'checkout'];

function responseHeader(response, name) {
    const wanted = name.toLowerCase();
    for (const [key, value] of Object.entries(response.headers || {})) {
        if (key.toLowerCase() === wanted)
            return value;
    }
    return null;
}

function parseJson(text) {
    try {
        return JSON.parse(text);
    } catch (_) {
        return null;
    }
}

function parseRpcMessage(body, expectedId, mediaType) {
    if (!body || !body.trim())
        return null;

    let candidates = [];
    if (mediaType === 'application/json') {
        const direct = parseJson(body.trim());
        if (direct === null || Array.isArray(direct) || typeof direct !== 'object') {
            return null;
        }
        candidates = [direct];
    } else if (mediaType === 'text/event-stream') {
        const events = body.replace(/\r\n/g, '\n').split('\n\n');
        for (const event of events) {
            const data = event.split('\n')
                             .filter((line) => line.startsWith('data:'))
                             .map((line) => line.slice(5).trimStart())
                             .join('\n');
            if (!data)
                continue;
            const message = parseJson(data);
            if (message === null || Array.isArray(message) || typeof message !== 'object') {
                return null;
            }
            candidates.push(message);
        }
    } else {
        return null;
    }

    if (candidates.length !== 1 || candidates[0].id !== expectedId)
        return null;
    return candidates[0];
}

function mcpResponseMediaType(response) {
    const contentType = responseHeader(response, 'Content-Type');
    if (typeof contentType !== 'string')
        return null;
    const mediaType = contentType.split(';', 1)[0].trim().toLowerCase();
    return mediaType === 'application/json' || mediaType === 'text/event-stream' ? mediaType : null;
}

function recordCheck(label, passed) {
    const checks = {};
    checks[label] = (value) => value === true;
    return check(passed, checks);
}

// The `_meta` every 2026-07-28 request carries.
function requestMeta() {
    return {
        'io.modelcontextprotocol/protocolVersion': PROTOCOL_VERSION,
        'io.modelcontextprotocol/clientCapabilities': {},
        'io.modelcontextprotocol/clientInfo': {name: 'mcp-cpp-sdk-benchmark', version: '1.0'},
    };
}

// Every request carries an id that no other request of the run shares: the number of the virtual
// user and a counter of that user's requests, as a client that numbers its requests would send.
let requestCounter = 0;

function nextRequestId() {
    requestCounter += 1;
    return __VU * 1e9 + requestCounter;
}

// One self-contained request. The headers mirror what the body says; a server must refuse a
// request whose headers and body disagree.
function modernRequest(method, params, name, metricName) {
    const id = nextRequestId();
    const headers = Object.assign({}, BASE_HEADERS, {'Mcp-Method': method});
    if (name !== undefined)
        headers['Mcp-Name'] = name;
    mcpMessages.add(1);
    const response = http.post(
        SERVER_URL,
        JSON.stringify({
            jsonrpc: '2.0',
            id,
            method,
            params: Object.assign({}, params, {_meta: requestMeta()}),
        }),
        {
            headers,
            timeout: '30s',
            tags: {name: metricName},
        },
    );
    return {
        message: parseRpcMessage(response.body, id, mcpResponseMediaType(response)),
        response,
    };
}

function finishOperation(failed) {
    mcpErrorRate.add(failed);
    mcpErrors.add(failed ? 1 : 0);
}

function validRpcResult(response, message) {
    return response.status === 200 && mcpResponseMediaType(response) !== null && message !== null &&
        message.jsonrpc === '2.0' && !message.error && message.result &&
        typeof message.result === 'object';
}

function callTool(tool) {
    const operation = modernRequest(
        'tools/call',
        {name: tool.name, arguments: tool.arguments},
        tool.name,
        `mcp_tool_${tool.name}`,
    );
    const shapeValid = validRpcResult(operation.response, operation.message) &&
        operation.message.result.isError !== true && Array.isArray(operation.message.result.content);
    const contractValid = shapeValid && tool.validateContract(operation.message.result);
    recordCheck(`${tool.name} returns a valid tool result`, shapeValid);
    recordCheck(`${tool.name} satisfies the benchmark contract`, contractValid);
    tool.duration.add(operation.response.timings.duration);
    combinedToolDuration.add(operation.response.timings.duration);
    benchmarkOperations.add(1);
    finishOperation(!shapeValid || !contractValid);
}

function listTools() {
    const operation = modernRequest('tools/list', {}, undefined, 'mcp_tools_list');
    const resultValid = validRpcResult(operation.response, operation.message) &&
        Array.isArray(operation.message.result.tools);
    const tools = resultValid ? operation.message.result.tools : [];
    const names = tools.map((tool) => tool.name);
    const expectedNamesPresent = resultValid && tools.length === EXPECTED_TOOLS.length &&
        new Set(names).size === EXPECTED_TOOLS.length &&
        EXPECTED_TOOLS.every((name) => names.includes(name));
    const contractValid = expectedNamesPresent &&
        tools.every(
            (tool) => tool.inputSchema && typeof tool.inputSchema === 'object' &&
                !Array.isArray(tool.inputSchema),
        );
    recordCheck('tools/list returns a valid tool collection', resultValid);
    recordCheck('tools/list satisfies the benchmark contract', contractValid);
    toolsListDuration.add(operation.response.timings.duration);
    benchmarkOperations.add(1);
    finishOperation(!resultValid || !contractValid);
}

function textContent(result) {
    if (!result || !Array.isArray(result.content))
        return null;
    const block = result.content.find(
        (content) => content && content.type === 'text' && typeof content.text === 'string',
    );
    return block ? parseJson(block.text) : null;
}

function toolsForUser(userId) {
    return [
        {
            name: 'search_products',
            arguments: {
                category: 'Electronics',
                min_price: 50.0,
                max_price: 500.0,
                limit: 10,
            },
            duration: searchProductsDuration,
            validateContract: (result) => {
                const value = textContent(result);
                return value !== null && value.total_found === 2251 && Array.isArray(value.products) &&
                    value.products.length === 10 && Array.isArray(value.top10_popular_ids) &&
                    value.top10_popular_ids.length === 10;
            },
        },
        {
            name: 'get_user_cart',
            arguments: {user_id: userId},
            duration: getUserCartDuration,
            validateContract: (result) => {
                const value = textContent(result);
                return value !== null && value.user_id === userId && value.cart &&
                    Array.isArray(value.cart.items) && value.cart.items.length >= 1 &&
                    Array.isArray(value.recent_history) && value.recent_history.length === 5;
            },
        },
        {
            name: 'checkout',
            arguments: {
                user_id: userId,
                items: [
                    {product_id: 42, quantity: 2},
                    {product_id: 1337, quantity: 1},
                ],
            },
            duration: checkoutDuration,
            validateContract: (result) => {
                const value = textContent(result);
                return value !== null && value.user_id === userId && value.status === 'confirmed' &&
                    typeof value.total === 'number' && value.total > 0 && value.items_count === 2 &&
                    typeof value.rate_limit_count === 'number';
            },
        },
    ];
}

// Fail before the load starts when the server does not serve the revision at all, instead of
// reporting a wall of failed checks.
export function setup() {
    const response = http.post(
        SERVER_URL,
        JSON.stringify({
            jsonrpc: '2.0',
            id: 1,
            method: 'server/discover',
            params: {_meta: requestMeta()},
        }),
        {headers: Object.assign({}, BASE_HEADERS, {'Mcp-Method': 'server/discover'})},
    );
    const message = parseRpcMessage(response.body, 1, mcpResponseMediaType(response));
    const supported = message && message.result && message.result.supportedVersions;
    if (response.status !== 200 || !Array.isArray(supported) || !supported.includes(PROTOCOL_VERSION)) {
        throw new Error(
            `server does not serve ${PROTOCOL_VERSION}: HTTP ${response.status} ${response.body}`);
    }
}

export default function() {
    const userNumber = ((__VU - 1) % 1000) + 1;
    const userId = `user-${String(userNumber).padStart(5, '0')}`;

    for (const tool of toolsForUser(userId))
        callTool(tool);
    listTools();
    if (THINK_TIME_SECONDS > 0)
        sleep(THINK_TIME_SECONDS);
}

function metricValue(data, metricName, valueName, fallback = 0) {
    const metric = data.metrics[metricName];
    if (!metric || metric.values[valueName] === undefined)
        return fallback;
    return metric.values[valueName];
}

function trend(data, metricName) {
    const metric = data.metrics[metricName];
    if (!metric)
        return null;
    return {
        count: metric.values.count,
        avg_ms: metric.values.avg,
        min_ms: metric.values.min,
        p50_ms: metric.values.med,
        p90_ms: metric.values['p(90)'],
        p95_ms: metric.values['p(95)'],
        p99_ms: metric.values['p(99)'],
        max_ms: metric.values.max,
    };
}

function counter(data, metricName) {
    return {
        count: metricValue(data, metricName, 'count'),
        per_second: metricValue(data, metricName, 'rate'),
    };
}

function actualDurationMs(data) {
    if (data.state && typeof data.state.testRunDurationMs === 'number') {
        return data.state.testRunDurationMs;
    }
    return null;
}

function checkBreakdown(group, output = {}) {
    if (!group || typeof group !== 'object')
        return output;
    for (const item of group.checks || []) {
        output[item.name] = {passes: item.passes || 0, fails: item.fails || 0};
    }
    for (const child of group.groups || [])
        checkBreakdown(child, output);
    return output;
}

export function handleSummary(data) {
    const outputPath = __ENV.OUTPUT_PATH || `${SERVER_NAME}_${MODE}_summary.json`;
    const durationMs = actualDurationMs(data);
    const summary = {
        schema_version: 2,
        server: SERVER_NAME,
        timestamp: new Date().toISOString(),
        config: {
            mode: MODE,
            server_url: SERVER_URL,
            requested_protocol_version: PROTOCOL_VERSION,
            expected_negotiated_protocol_version: EXPECTED_PROTOCOL_VERSION,
            eligibility_contract: BENCHMARK_CONTRACT,
            vus: VUS,
            think_time_seconds: THINK_TIME_SECONDS,
            executor: scenario.executor,
            ramp_duration: MODE === 'warmup' ? WARMUP_RAMP_DURATION : null,
            warmup_load_duration: MODE === 'warmup' ? WARMUP_LOAD_DURATION : null,
            measurement_duration: MODE === 'measurement' ? MEASUREMENT_DURATION : null,
            configured_duration: MODE === 'measurement' ?
                MEASUREMENT_DURATION :
                `${WARMUP_RAMP_DURATION} ramp + ${WARMUP_LOAD_DURATION} load`,
            actual_duration_seconds: durationMs === null ? null : durationMs / 1000,
        },
        rates: {
            operations: counter(data, 'benchmark_operations'),
            mcp_messages: counter(data, 'mcp_messages'),
            raw_http: counter(data, 'http_reqs'),
        },
        latency: {
            combined_tool_call: trend(data, 'mcp_tool_duration'),
            tools_list: trend(data, 'mcp_tools_list_duration'),
            tools: {
                search_products: trend(data, 'mcp_search_products_duration'),
                get_user_cart: trend(data, 'mcp_get_user_cart_duration'),
                checkout: trend(data, 'mcp_checkout_duration'),
            },
        },
        check_breakdown: checkBreakdown(data.root_group),
        errors: {
            mcp: metricValue(data, 'mcp_errors', 'count'),
            mcp_rate: metricValue(data, 'mcp_error_rate', 'rate'),
            http: metricValue(data, 'http_req_failed', 'passes'),
            http_rate: metricValue(data, 'http_req_failed', 'rate'),
            checks: metricValue(data, 'checks', 'fails'),
            check_pass_rate: metricValue(data, 'checks', 'rate'),
        },
    };

    const operations = summary.rates.operations;
    const rawHttp = summary.rates.raw_http;
    const combined = summary.latency.combined_tool_call;
    const latencyLine = combined === null ? 'tool latency: no samples' :
                                            `tool latency: p50=${combined.p50_ms.toFixed(2)}ms ` +
            `p95=${combined.p95_ms.toFixed(2)}ms p99=${combined.p99_ms.toFixed(2)}ms`;
    const stdout = [
        '',
        `${SERVER_NAME} ${MODE} benchmark (${PROTOCOL_VERSION})`,
        `actual duration: ${summary.config.actual_duration_seconds ?? 'unknown'}s`,
        `operations: ${operations.count} (${operations.per_second.toFixed(2)}/s)`,
        `raw HTTP requests: ${rawHttp.count} (${rawHttp.per_second.toFixed(2)}/s)`,
        latencyLine,
        `errors: MCP=${summary.errors.mcp} HTTP=${summary.errors.http} checks=${summary.errors.checks}`,
        '',
    ].join('\n');

    return {
        [outputPath]: JSON.stringify(summary, null, 2),
        stdout,
    };
}

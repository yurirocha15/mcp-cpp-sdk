package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
	"github.com/redis/go-redis/v9"
)

// --- I/O clients -------------------------------------------------------------

var (
	apiURL     string
	httpClient *http.Client
	rdb        *redis.Client
)

func init() {
	apiURL = os.Getenv("API_SERVICE_URL")
	if apiURL == "" {
		apiURL = "http://mcp-api-service:8100"
	}
	// Transport tuned for 50 VUs: pool keeps enough idle connections per host
	// so each request reuses an existing TCP connection instead of dialing fresh.
	// Default MaxIdleConnsPerHost=2 would cause connection churn under load.
	// TCP_NODELAY is enabled by default in Go's net.Dial (Nagle disabled).
	httpClient = &http.Client{
		Timeout: 10 * time.Second,
		Transport: &http.Transport{
			MaxIdleConns:        200,
			MaxIdleConnsPerHost: 100,
			IdleConnTimeout:     90 * time.Second,
		},
	}

	redisURL := os.Getenv("REDIS_URL")
	if redisURL == "" {
		redisURL = "redis://mcp-redis:6379"
	}
	opt, err := redis.ParseURL(redisURL)
	if err != nil {
		panic(fmt.Sprintf("invalid REDIS_URL: %v", err))
	}
	rdb = redis.NewClient(opt)
}

// --- Helpers -----------------------------------------------------------------

func httpGet(ctx context.Context, path string, params map[string]string) ([]byte, error) {
	u, _ := url.Parse(apiURL + path)
	if len(params) > 0 {
		q := u.Query()
		for k, v := range params {
			q.Set(k, v)
		}
		u.RawQuery = q.Encode()
	}
	req, _ := http.NewRequestWithContext(ctx, "GET", u.String(), nil)
	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}

func httpPost(ctx context.Context, path string, body any) ([]byte, error) {
	b, _ := json.Marshal(body)
	req, _ := http.NewRequestWithContext(ctx, "POST", apiURL+path, bytes.NewReader(b))
	req.Header.Set("Content-Type", "application/json")
	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}

// --- Tool arg/output types ---------------------------------------------------

type SearchProductsArgs struct {
	Category string  `json:"category"`
	MinPrice float64 `json:"min_price"`
	MaxPrice float64 `json:"max_price"`
	Limit    int     `json:"limit"`
}

type GetUserCartArgs struct {
	UserID string `json:"user_id"`
}

type CheckoutItem struct {
	ProductID int `json:"product_id"`
	Quantity  int `json:"quantity"`
}

type CheckoutArgs struct {
	UserID string         `json:"user_id"`
	Items  []CheckoutItem `json:"items"`
}

// --- Tool handlers -----------------------------------------------------------

func handleSearchProducts(ctx context.Context, _ *mcp.CallToolRequest, args SearchProductsArgs) (*mcp.CallToolResult, map[string]any, error) {
	if args.Category == "" {
		args.Category = "Electronics"
	}
	if args.MinPrice == 0 {
		args.MinPrice = 50.0
	}
	if args.MaxPrice == 0 {
		args.MaxPrice = 500.0
	}
	if args.Limit == 0 {
		args.Limit = 10
	}

	var (
		searchBytes []byte
		popularRaw  []string
		searchErr   error
		popularErr  error
		wg          sync.WaitGroup
	)
	wg.Add(2)
	go func() {
		defer wg.Done()
		searchBytes, searchErr = httpGet(ctx, "/products/search", map[string]string{
			"category":  args.Category,
			"min_price": strconv.FormatFloat(args.MinPrice, 'f', -1, 64),
			"max_price": strconv.FormatFloat(args.MaxPrice, 'f', -1, 64),
			"limit":     strconv.Itoa(args.Limit),
		})
	}()
	go func() {
		defer wg.Done()
		popularRaw, popularErr = rdb.ZRevRange(ctx, "bench:popular", 0, 9).Result()
	}()
	wg.Wait()
	if searchErr != nil {
		return nil, nil, searchErr
	}
	if popularErr != nil {
		return nil, nil, popularErr
	}

	var searchData map[string]any
	if err := json.Unmarshal(searchBytes, &searchData); err != nil {
		return nil, nil, err
	}

	top10IDs := make([]int, 0, 10)
	top10Set := make(map[int]int)
	for rank, member := range popularRaw {
		parts := strings.SplitN(member, ":", 2)
		if len(parts) == 2 {
			if id, err := strconv.Atoi(parts[1]); err == nil {
				top10IDs = append(top10IDs, id)
				top10Set[id] = rank + 1
			}
		}
	}

	rawProducts, _ := searchData["products"].([]any)
	products := make([]map[string]any, 0, len(rawProducts))
	for _, rp := range rawProducts {
		p, _ := rp.(map[string]any)
		id := int(p["id"].(float64))
		rank := top10Set[id]
		products = append(products, map[string]any{
			"id": id, "sku": p["sku"], "name": p["name"],
			"price": p["price"], "rating": p["rating"],
			"popularity_rank": rank,
		})
	}

	totalFound := int(searchData["total_found"].(float64))
	result := map[string]any{
		"category":          args.Category,
		"total_found":       totalFound,
		"products":          products,
		"top10_popular_ids": top10IDs,
		"server_type":       "go",
	}
	return nil, result, nil
}

func handleGetUserCart(ctx context.Context, _ *mcp.CallToolRequest, args GetUserCartArgs) (*mcp.CallToolResult, map[string]any, error) {
	if args.UserID == "" {
		args.UserID = "user-00042"
	}
	cartKey := "bench:cart:" + args.UserID
	histKey := "bench:history:" + args.UserID

	cartHash, err := rdb.HGetAll(ctx, cartKey).Result()
	if err != nil {
		return nil, nil, err
	}

	var items []map[string]any
	if itemsJSON, ok := cartHash["items"]; ok {
		json.Unmarshal([]byte(itemsJSON), &items)
	}

	firstProductID := 1
	if len(items) > 0 {
		if pid, ok := items[0]["product_id"].(float64); ok {
			firstProductID = int(pid)
		}
	}

	var (
		productBytes []byte
		historyRaw   []string
		productErr   error
		historyErr   error
		wg           sync.WaitGroup
	)
	wg.Add(2)
	go func() {
		defer wg.Done()
		productBytes, productErr = httpGet(ctx, fmt.Sprintf("/products/%d", firstProductID), nil)
	}()
	go func() {
		defer wg.Done()
		historyRaw, historyErr = rdb.LRange(ctx, histKey, 0, 4).Result()
	}()
	wg.Wait()
	if productErr != nil {
		return nil, nil, productErr
	}
	if historyErr != nil {
		return nil, nil, historyErr
	}

	recentHistory := make([]any, 0, len(historyRaw))
	for _, entry := range historyRaw {
		var parsed any
		if json.Unmarshal([]byte(entry), &parsed) == nil {
			recentHistory = append(recentHistory, parsed)
		}
	}

	estimatedTotal := 0.0
	if t, ok := cartHash["total"]; ok {
		estimatedTotal, _ = strconv.ParseFloat(t, 64)
	}
	_ = productBytes // product details fetched but not merged into response for simplicity

	return nil, map[string]any{
		"user_id": args.UserID,
		"cart": map[string]any{
			"items":           items,
			"item_count":      len(items),
			"estimated_total": estimatedTotal,
		},
		"recent_history": recentHistory,
		"server_type":    "go",
	}, nil
}

func handleCheckout(ctx context.Context, _ *mcp.CallToolRequest, args CheckoutArgs) (*mcp.CallToolResult, map[string]any, error) {
	if args.UserID == "" {
		args.UserID = "user-00042"
	}
	if len(args.Items) == 0 {
		args.Items = []CheckoutItem{{ProductID: 42, Quantity: 2}, {ProductID: 1337, Quantity: 1}}
	}

	// Parse numeric part of user_id
	parts := strings.Split(args.UserID, "-")
	userNum := 42
	if len(parts) >= 2 {
		userNum, _ = strconv.Atoi(parts[len(parts)-1])
	}
	rateKey := fmt.Sprintf("bench:ratelimit:user-%05d", userNum%100)
	histKey := "bench:history:" + args.UserID
	productID := args.Items[0].ProductID

	orderEntry, _ := json.Marshal(map[string]any{
		"order_id": fmt.Sprintf("ORD-%s-%d", args.UserID, time.Now().Unix()),
		"items":    args.Items,
		"ts":       time.Now().Unix(),
	})
	calcBody := map[string]any{"user_id": args.UserID, "items": args.Items}

	var (
		calcBytes []byte
		rateCount int64
		calcErr   error
		wg        sync.WaitGroup
	)
	wg.Add(4)
	go func() {
		defer wg.Done()
		calcBytes, calcErr = httpPost(ctx, "/cart/calculate", calcBody)
	}()
	go func() {
		defer wg.Done()
		rateCount, _ = rdb.Incr(ctx, rateKey).Result()
	}()
	go func() {
		defer wg.Done()
		rdb.RPush(ctx, histKey, string(orderEntry))
	}()
	go func() {
		defer wg.Done()
		rdb.ZIncrBy(ctx, "bench:popular", 1, fmt.Sprintf("product:%d", productID))
	}()
	wg.Wait()

	if calcErr != nil {
		return nil, nil, calcErr
	}

	var calcData map[string]any
	json.Unmarshal(calcBytes, &calcData)

	total := 0.0
	if t, ok := calcData["total"].(float64); ok {
		total = t
	}
	orderID, _ := calcData["order_id"].(string)
	if orderID == "" {
		orderID = fmt.Sprintf("ORD-%s-%d", args.UserID, time.Now().Unix())
	}

	return nil, map[string]any{
		"order_id":         orderID,
		"user_id":          args.UserID,
		"total":            total,
		"items_count":      len(args.Items),
		"rate_limit_count": rateCount,
		"status":           "confirmed",
		"server_type":      "go",
	}, nil
}

// --- Main -------------------------------------------------------------------

func main() {
	server := mcp.NewServer(&mcp.Implementation{
		Name:    "BenchmarkGoServer",
		Version: "1.0.0",
	}, nil)

	mcp.AddTool(server, &mcp.Tool{
		Name:        "search_products",
		Description: "Search products by category and price range, merged with popularity data",
	}, handleSearchProducts)

	mcp.AddTool(server, &mcp.Tool{
		Name:        "get_user_cart",
		Description: "Get user cart details with recent order history",
	}, handleGetUserCart)

	mcp.AddTool(server, &mcp.Tool{
		Name:        "checkout",
		Description: "Process checkout: calculate total, update rate limit, record history",
	}, handleCheckout)

	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok","server_type":"go"}`))
	})

	httpHandler := mcp.NewStreamableHTTPHandler(func(r *http.Request) *mcp.Server {
		return server
	}, &mcp.StreamableHTTPOptions{
		// MCP_SESSION_MODE=stateful issues a session per client on older protocol revisions;
		// anything else (the default) serves them without sessions.
		Stateless:    os.Getenv("MCP_SESSION_MODE") != "stateful",
		JSONResponse: true,
	})

	http.Handle("/mcp", httpHandler)

	fmt.Println("Go MCP server listening on port 8081")
	if err := http.ListenAndServe(":8081", nil); err != nil {
		panic(err)
	}
}

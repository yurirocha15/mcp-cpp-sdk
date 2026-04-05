package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
	"github.com/redis/go-redis/v9"
)

var (
	redisClient *redis.Client
	httpClient  *http.Client
	apiBaseURL  string
)

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

func main() {
	redisURL := os.Getenv("REDIS_URL")
	if redisURL == "" {
		redisURL = "redis://redis:6379"
	}
	opts, err := redis.ParseURL(redisURL)
	if err != nil {
		log.Fatalf("Failed to parse REDIS_URL: %v", err)
	}
	opts.PoolSize = 100
	redisClient = redis.NewClient(opts)

	httpClient = &http.Client{
		Transport: &http.Transport{
			MaxIdleConns:        200,
			MaxIdleConnsPerHost: 100,
			IdleConnTimeout:     90 * time.Second,
		},
	}

	apiBaseURL = os.Getenv("API_SERVICE_URL")
	if apiBaseURL == "" {
		apiBaseURL = "http://api-service:8100"
	}

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

	httpHandler := mcp.NewStreamableHTTPHandler(func(_ *http.Request) *mcp.Server {
		return server
	}, nil)

	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok","server_type":"go"}`))
	})
	mux.Handle("/mcp", httpHandler)

	port := os.Getenv("PORT")
	if port == "" {
		port = "8082"
	}
	log.Printf("Go MCP Benchmark Server listening on :%s", port)
	log.Fatal(http.ListenAndServe(":"+port, mux))
}

func handleSearchProducts(ctx context.Context, _ *mcp.ServerSession, params *mcp.CallToolParamsFor[SearchProductsArgs]) (*mcp.CallToolResultFor[any], error) {
	args := params.Arguments
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

	var wg sync.WaitGroup
	var products []map[string]any
	var popularIDs []string
	var productsErr, redisErr error
	var apiTotalFound int

	wg.Add(2)

	go func() {
		defer wg.Done()
		url := fmt.Sprintf("%s/products/search?category=%s&min_price=%.2f&max_price=%.2f&limit=%d",
			apiBaseURL, args.Category, args.MinPrice, args.MaxPrice, args.Limit)
		resp, err := httpClient.Get(url)
		if err != nil {
			productsErr = err
			return
		}
		defer resp.Body.Close()
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			productsErr = err
			return
		}
		var result map[string]any
		if err := json.Unmarshal(body, &result); err != nil {
			productsErr = err
			return
		}
		if p, ok := result["products"].([]any); ok {
			for _, item := range p {
				if m, ok := item.(map[string]any); ok {
					products = append(products, m)
				}
			}
		}
		if tf, ok := result["total_found"].(float64); ok {
			apiTotalFound = int(tf)
		}
	}()

	go func() {
		defer wg.Done()
		result, err := redisClient.ZRevRange(ctx, "bench:popular", 0, 9).Result()
		if err != nil {
			redisErr = err
			return
		}
		popularIDs = result
	}()

	wg.Wait()

	if productsErr != nil {
		return nil, productsErr
	}
	if redisErr != nil {
		return nil, redisErr
	}

	popularMap := make(map[int]int)
	for i, pid := range popularIDs {
		parts := strings.Split(pid, ":")
		if len(parts) == 2 {
			if id, err := strconv.Atoi(parts[1]); err == nil {
				popularMap[id] = i + 1
			}
		}
	}

	for _, product := range products {
		if id, ok := product["id"].(float64); ok {
			if rank, exists := popularMap[int(id)]; exists {
				product["popularity_rank"] = rank
			}
		}
	}

	totalFound := apiTotalFound
	if totalFound == 0 {
		totalFound = len(products)
	}

	output := map[string]any{
		"category":          args.Category,
		"total_found":       totalFound,
		"products":          products,
		"top10_popular_ids": popularIDs,
		"server_type":       "go",
	}

	resultJSON, _ := json.Marshal(output)
	return &mcp.CallToolResultFor[any]{
		Content: []mcp.Content{
			&mcp.TextContent{Text: string(resultJSON)},
		},
	}, nil
}

func handleGetUserCart(ctx context.Context, _ *mcp.ServerSession, params *mcp.CallToolParamsFor[GetUserCartArgs]) (*mcp.CallToolResultFor[any], error) {
	args := params.Arguments
	if args.UserID == "" {
		args.UserID = "user-00042"
	}

	cartData, err := redisClient.HGetAll(ctx, "bench:cart:"+args.UserID).Result()
	if err != nil {
		return nil, err
	}

	var cartItems []map[string]any
	var itemCount int
	var estimatedTotal float64
	var firstProductID int

	if itemsJSON, ok := cartData["items"]; ok {
		if err := json.Unmarshal([]byte(itemsJSON), &cartItems); err == nil {
			itemCount = len(cartItems)
			if len(cartItems) > 0 {
				if pid, ok := cartItems[0]["product_id"].(float64); ok {
					firstProductID = int(pid)
				}
			}
		}
	}
	if totalStr, ok := cartData["total"]; ok {
		if t, err := strconv.ParseFloat(totalStr, 64); err == nil {
			estimatedTotal = t
		}
	}

	var wg sync.WaitGroup
	var productDetails map[string]any
	var recentHistoryRaw []string
	var productErr, historyErr error

	wg.Add(2)

	go func() {
		defer wg.Done()
		if firstProductID == 0 {
			return
		}
		url := fmt.Sprintf("%s/products/%d", apiBaseURL, firstProductID)
		resp, err := httpClient.Get(url)
		if err != nil {
			productErr = err
			return
		}
		defer resp.Body.Close()
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			productErr = err
			return
		}
		if err := json.Unmarshal(body, &productDetails); err != nil {
			productErr = err
			return
		}
	}()

	go func() {
		defer wg.Done()
		result, err := redisClient.LRange(ctx, "bench:history:"+args.UserID, 0, 4).Result()
		if err != nil {
			historyErr = err
			return
		}
		recentHistoryRaw = result
	}()

	wg.Wait()

	if productErr != nil {
		return nil, productErr
	}
	if historyErr != nil {
		return nil, historyErr
	}

	var recentHistory []map[string]any
	for _, entry := range recentHistoryRaw {
		var parsed map[string]any
		if err := json.Unmarshal([]byte(entry), &parsed); err == nil {
			recentHistory = append(recentHistory, parsed)
		}
	}

	_ = productDetails

	output := map[string]any{
		"user_id": args.UserID,
		"cart": map[string]any{
			"items":           cartItems,
			"item_count":      itemCount,
			"estimated_total": estimatedTotal,
		},
		"recent_history": recentHistory,
		"server_type":    "go",
	}

	resultJSON, _ := json.Marshal(output)
	return &mcp.CallToolResultFor[any]{
		Content: []mcp.Content{
			&mcp.TextContent{Text: string(resultJSON)},
		},
	}, nil
}

func handleCheckout(ctx context.Context, _ *mcp.ServerSession, params *mcp.CallToolParamsFor[CheckoutArgs]) (*mcp.CallToolResultFor[any], error) {
	args := params.Arguments
	if args.UserID == "" {
		args.UserID = "user-00042"
	}
	if len(args.Items) == 0 {
		args.Items = []CheckoutItem{
			{ProductID: 42, Quantity: 2},
			{ProductID: 1337, Quantity: 1},
		}
	}

	parts := strings.Split(args.UserID, "-")
	var userNum int
	if len(parts) >= 2 {
		if num, err := strconv.Atoi(parts[len(parts)-1]); err == nil {
			userNum = num % 100
		}
	}
	userNumStr := fmt.Sprintf("%05d", userNum)

	timestamp := time.Now().Unix()
	orderID := fmt.Sprintf("ORD-%s-%d", args.UserID, timestamp)
	orderEntry := map[string]any{
		"order_id": orderID,
		"items":    args.Items,
		"ts":       timestamp,
	}
	entryJSON, _ := json.Marshal(orderEntry)

	var wg sync.WaitGroup
	var total float64
	var rateLimitCount int64
	var calcOrderID string
	var calculateErr, incrErr, pushErr, zincrErr error

	wg.Add(4)

	go func() {
		defer wg.Done()
		payload := map[string]any{
			"user_id": args.UserID,
			"items":   args.Items,
		}
		body, _ := json.Marshal(payload)
		resp, err := httpClient.Post(apiBaseURL+"/cart/calculate", "application/json", strings.NewReader(string(body)))
		if err != nil {
			calculateErr = err
			return
		}
		defer resp.Body.Close()
		respBody, err := io.ReadAll(resp.Body)
		if err != nil {
			calculateErr = err
			return
		}
		var result map[string]any
		if err := json.Unmarshal(respBody, &result); err != nil {
			calculateErr = err
			return
		}
		if t, ok := result["total"].(float64); ok {
			total = t
		}
		if oid, ok := result["order_id"].(string); ok {
			calcOrderID = oid
		}
	}()

	go func() {
		defer wg.Done()
		result, err := redisClient.Incr(ctx, "bench:ratelimit:user-"+userNumStr).Result()
		if err != nil {
			incrErr = err
			return
		}
		rateLimitCount = result
	}()

	go func() {
		defer wg.Done()
		_, err := redisClient.RPush(ctx, "bench:history:"+args.UserID, string(entryJSON)).Result()
		if err != nil {
			pushErr = err
			return
		}
	}()

	go func() {
		defer wg.Done()
		if len(args.Items) > 0 {
			firstProductID := args.Items[0].ProductID
			_, err := redisClient.ZIncrBy(ctx, "bench:popular", 1, fmt.Sprintf("product:%d", firstProductID)).Result()
			if err != nil {
				zincrErr = err
				return
			}
		}
	}()

	wg.Wait()

	if calculateErr != nil {
		return nil, calculateErr
	}
	if incrErr != nil {
		return nil, incrErr
	}
	if pushErr != nil {
		return nil, pushErr
	}
	if zincrErr != nil {
		return nil, zincrErr
	}

	if calcOrderID != "" {
		orderID = calcOrderID
	}

	output := map[string]any{
		"order_id":         orderID,
		"user_id":          args.UserID,
		"total":            total,
		"items_count":      len(args.Items),
		"rate_limit_count": rateLimitCount,
		"status":           "confirmed",
		"server_type":      "go",
	}

	resultJSON, _ := json.Marshal(output)
	return &mcp.CallToolResultFor[any]{
		Content: []mcp.Content{
			&mcp.TextContent{Text: string(resultJSON)},
		},
	}, nil
}

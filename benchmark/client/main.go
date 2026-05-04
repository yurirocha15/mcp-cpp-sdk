package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sort"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type ServerConfig struct {
	Name string
	URL  string
}

type ToolResult struct {
	Server       string   `json:"server"`
	Tool         string   `json:"tool"`
	LatencyMs    float64  `json:"latency_ms"`
	Success      bool     `json:"success"`
	Error        string   `json:"error,omitempty"`
	ResponseKeys []string `json:"response_keys,omitempty"`
}

func main() {
	url := flag.String("url", "http://localhost:8080/mcp", "MCP server URL")
	name := flag.String("name", "cpp", "Server name for output labeling")
	compare := flag.Bool("compare", false, "Run against all 3 servers and compare")
	flag.Parse()

	var servers []ServerConfig
	if *compare {
		servers = []ServerConfig{
			{Name: "cpp", URL: "http://localhost:8080/mcp"},
			{Name: "python", URL: "http://localhost:8081/mcp"},
			{Name: "go", URL: "http://localhost:8082/mcp"},
		}
	} else {
		servers = []ServerConfig{{Name: *name, URL: *url}}
	}

	var allResults []ToolResult
	exitCode := 0

	for _, server := range servers {
		fmt.Fprintf(os.Stderr, "\n=== Testing %s server at %s ===\n", server.Name, server.URL)
		results, err := testServer(server)
		if err != nil {
			fmt.Fprintf(os.Stderr, "ERROR: Failed to test %s server: %v\n", server.Name, err)
			exitCode = 1
			continue
		}
		allResults = append(allResults, results...)

		for _, r := range results {
			if !r.Success {
				exitCode = 1
			}
		}
	}

	output, _ := json.MarshalIndent(allResults, "", "  ")
	fmt.Println(string(output))

	fmt.Fprintf(os.Stderr, "\n=== Summary ===\n")
	successCount := 0
	for _, r := range allResults {
		status := "✓"
		if !r.Success {
			status = "✗"
		} else {
			successCount++
		}
		fmt.Fprintf(os.Stderr, "%s %s/%s: %.2fms\n", status, r.Server, r.Tool, r.LatencyMs)
	}
	fmt.Fprintf(os.Stderr, "\nTotal: %d/%d passed\n", successCount, len(allResults))

	os.Exit(exitCode)
}

func testServer(server ServerConfig) ([]ToolResult, error) {
	ctx := context.Background()
	transport := mcp.NewStreamableClientTransport(server.URL, nil)
	client := mcp.NewClient(&mcp.Implementation{
		Name:    "benchmark-client",
		Version: "1.0.0",
	}, nil)

	fmt.Fprintf(os.Stderr, "Initializing session...\n")
	session, err := client.Connect(ctx, transport)
	if err != nil {
		return nil, fmt.Errorf("failed to connect: %w", err)
	}
	defer session.Close()

	fmt.Fprintf(os.Stderr, "Session initialized, calling tools...\n")

	results := []ToolResult{
		callTool(ctx, session, server.Name, "search_products", map[string]any{
			"category":  "Electronics",
			"min_price": 50.0,
			"max_price": 500.0,
			"limit":     10,
		}),
		callTool(ctx, session, server.Name, "get_user_cart", map[string]any{
			"user_id": "user-00042",
		}),
		callTool(ctx, session, server.Name, "checkout", map[string]any{
			"user_id": "user-00042",
			"items": []map[string]any{
				{"product_id": 42, "quantity": 2},
				{"product_id": 1337, "quantity": 1},
			},
		}),
	}

	return results, nil
}

func callTool(ctx context.Context, session *mcp.ClientSession, serverName string, toolName string, args map[string]any) ToolResult {
	result := ToolResult{Server: serverName, Tool: toolName}

	start := time.Now()
	toolResult, err := session.CallTool(ctx, &mcp.CallToolParams{
		Name:      toolName,
		Arguments: args,
	})
	result.LatencyMs = float64(time.Since(start).Microseconds()) / 1000.0

	if err != nil {
		result.Error = fmt.Sprintf("tool call failed: %v", err)
		fmt.Fprintf(os.Stderr, "  ✗ %s: %v (%.2fms)\n", toolName, err, result.LatencyMs)
		return result
	}

	responseData, err := extractResponseData(toolResult)
	if err != nil {
		result.Error = err.Error()
		fmt.Fprintf(os.Stderr, "  ✗ %s: %s (%.2fms)\n", toolName, result.Error, result.LatencyMs)
		return result
	}

	keys := make([]string, 0, len(responseData))
	for key := range responseData {
		keys = append(keys, key)
	}
	sort.Strings(keys)

	result.ResponseKeys = keys
	result.Success = true
	fmt.Fprintf(os.Stderr, "  ✓ %s: %.2fms (keys: %v)\n", toolName, result.LatencyMs, keys)
	return result
}

func extractResponseData(toolResult *mcp.CallToolResult) (map[string]any, error) {
	if toolResult == nil {
		return nil, fmt.Errorf("no tool result returned")
	}
	if toolResult.IsError {
		return nil, fmt.Errorf("tool result marked as error")
	}
	if len(toolResult.Content) > 0 {
		if textContent, ok := toolResult.Content[0].(*mcp.TextContent); ok {
			var responseData map[string]any
			if err := json.Unmarshal([]byte(textContent.Text), &responseData); err != nil {
				return nil, fmt.Errorf("failed to parse response JSON: %w", err)
			}
			return responseData, nil
		}
	}

	responseData := make(map[string]any)
	raw, err := json.Marshal(toolResult.StructuredContent)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal structured content: %w", err)
	}
	if err := json.Unmarshal(raw, &responseData); err != nil {
		return nil, fmt.Errorf("failed to parse structured content: %w", err)
	}
	if len(responseData) > 0 {
		return responseData, nil
	}

	if len(toolResult.Content) == 0 {
		return nil, fmt.Errorf("tool result missing content and structured content")
	}

	if _, ok := toolResult.Content[0].(*mcp.TextContent); !ok {
		return nil, fmt.Errorf("tool result missing structured content")
	}

	return nil, fmt.Errorf("tool result missing usable payload")
}

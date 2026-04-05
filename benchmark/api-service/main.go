package main

import (
	"crypto/rand"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net/http"
	"os"
	"strconv"
)

type Product struct {
	ID          int     `json:"id"`
	SKU         string  `json:"sku"`
	Name        string  `json:"name"`
	Description string  `json:"description"`
	Price       float64 `json:"price"`
	Rating      float64 `json:"rating"`
	Category    string  `json:"category"`
	Brand       string  `json:"brand"`
	InStock     bool    `json:"in_stock"`
}

type SearchResponse struct {
	Products   []Product `json:"products"`
	TotalFound int       `json:"total_found"`
}

type CartItem struct {
	ProductID int `json:"product_id"`
	Quantity  int `json:"quantity"`
}

type CartCalculateRequest struct {
	UserID string     `json:"user_id"`
	Items  []CartItem `json:"items"`
}

type CartCalculateResponse struct {
	OrderID    string  `json:"order_id"`
	Subtotal   float64 `json:"subtotal"`
	Tax        float64 `json:"tax"`
	Shipping   float64 `json:"shipping"`
	Discount   float64 `json:"discount"`
	Total      float64 `json:"total"`
	ItemsCount int     `json:"items_count"`
}

var (
	products   []Product
	productMap map[int]Product
	categories = []string{
		"Electronics", "Books", "Clothing", "Home", "Sports", "Toys", "Food", "Beauty", "Health", "Automotive",
		"Garden", "Music", "Movies", "Games", "Software", "Office", "Pet", "Baby", "Industrial", "Other",
	}
)

func round2(value float64) float64 {
	return math.Round(value*100) / 100
}

func generateProducts() {
	products = make([]Product, 0, 100000)
	productMap = make(map[int]Product, 100000)

	for i := 0; i < 100000; i++ {
		id := i
		product := Product{
			ID:          id,
			SKU:         fmt.Sprintf("SKU-%06d", id),
			Name:        fmt.Sprintf("Product %d", id),
			Description: fmt.Sprintf("Description for Product %d", id),
			Price:       1.0 + float64(i%99900)/100.0,
			Rating:      1.0 + float64(i%50)/10.0,
			Category:    categories[i%len(categories)],
			Brand:       fmt.Sprintf("Brand%d", i%50),
			InStock:     id%10 != 0,
		}

		products = append(products, product)
		productMap[id] = product
	}
}

func writeJSON(w http.ResponseWriter, status int, payload interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		log.Printf("failed to encode response: %v", err)
	}
}

func parseOptionalFloat(raw string) (float64, bool) {
	if raw == "" {
		return 0, false
	}
	val, err := strconv.ParseFloat(raw, 64)
	if err != nil {
		return 0, false
	}
	return val, true
}

func handleSearchProducts(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	category := query.Get("category")
	minPrice, hasMin := parseOptionalFloat(query.Get("min_price"))
	maxPrice, hasMax := parseOptionalFloat(query.Get("max_price"))
	limit := 100
	if rawLimit := query.Get("limit"); rawLimit != "" {
		parsedLimit, err := strconv.Atoi(rawLimit)
		if err != nil || parsedLimit < 0 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid limit"})
			return
		}
		limit = parsedLimit
	}

	filtered := make([]Product, 0, limit)
	totalFound := 0

	for _, product := range products {
		if category != "" && product.Category != category {
			continue
		}
		if hasMin && product.Price < minPrice {
			continue
		}
		if hasMax && product.Price > maxPrice {
			continue
		}
		totalFound++
		if len(filtered) < limit {
			filtered = append(filtered, product)
		}
	}

	writeJSON(w, http.StatusOK, SearchResponse{
		Products:   filtered,
		TotalFound: totalFound,
	})
}

func handleGetProduct(w http.ResponseWriter, r *http.Request) {
	idRaw := r.PathValue("id")
	id, err := strconv.Atoi(idRaw)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid product id"})
		return
	}

	product, ok := productMap[id]
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "product not found"})
		return
	}

	writeJSON(w, http.StatusOK, product)
}

func handleCalculateCart(w http.ResponseWriter, r *http.Request) {
	var req CartCalculateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid json body"})
		return
	}

	subtotal := 0.0
	itemsCount := 0

	for _, item := range req.Items {
		if item.Quantity < 0 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "quantity must be non-negative"})
			return
		}
		product, ok := productMap[item.ProductID]
		if !ok {
			continue
		}
		subtotal += product.Price * float64(item.Quantity)
		itemsCount += item.Quantity
	}

	tax := subtotal * 0.085
	shipping := 5.99
	if subtotal >= 100.0 {
		shipping = 0
	}
	discount := 0.0
	if subtotal >= 100.0 {
		discount = subtotal * 0.10
	}
	total := subtotal + tax + shipping - discount

	response := CartCalculateResponse{
		OrderID:    "ORD-" + newUUID(),
		Subtotal:   round2(subtotal),
		Tax:        round2(tax),
		Shipping:   round2(shipping),
		Discount:   round2(discount),
		Total:      round2(total),
		ItemsCount: itemsCount,
	}

	writeJSON(w, http.StatusOK, response)
}

func newUUID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return "00000000-0000-4000-8000-000000000000"
	}
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%08x-%04x-%04x-%04x-%012x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{
		"status":      "ok",
		"server_type": "api-service",
	})
}

func main() {
	generateProducts()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /products/search", handleSearchProducts)
	mux.HandleFunc("GET /products/{id}", handleGetProduct)
	mux.HandleFunc("POST /cart/calculate", handleCalculateCart)
	mux.HandleFunc("GET /health", handleHealth)

	port := os.Getenv("PORT")
	if port == "" {
		port = "8100"
	}

	addr := ":" + port
	log.Printf("api-service listening on %s", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatalf("server failed: %v", err)
	}
}

package api

// next 路由的行情字段测试：验证 size 参数解析、行情窗口按设备传入，
// 无可用行情时省略 stocks 字段。使用假提供者，不访问真实数据源。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/stocks"
)

type fakeStockProvider struct {
	quotes   []stocks.Quote
	deviceID string
	size     int
}

func (f *fakeStockProvider) Window(deviceID string, size int) []stocks.Quote {
	f.deviceID = deviceID
	f.size = size
	return f.quotes
}

func doNextRequest(t *testing.T, deviceID, query string) (int, []byte) {
	t.Helper()
	setupTestDB(t)
	db := database.GetDB()
	// 清理设备状态，保证每次从最早条目开始，避免与其它用例相互影响
	db.Where("device_id = ?", deviceID).Delete(&models.Device{})

	path := "/v1/device/" + deviceID + "/next"
	if query != "" {
		path += "?" + query
	}
	req := httptest.NewRequest(http.MethodGet, path, nil)
	req.SetPathValue("device_id", deviceID)
	rr := httptest.NewRecorder()
	GetNextItem(rr, req)
	return rr.Code, rr.Body.Bytes()
}

func TestNextIncludesStocks(t *testing.T) {
	provider := &fakeStockProvider{quotes: []stocks.Quote{
		{Name: "上证指数", ChangePct: -0.82},
		{Name: "苹果", ChangePct: 1.52},
	}}
	StockQuotes = provider
	t.Cleanup(func() { StockQuotes = nil })

	code, body := doNextRequest(t, "dev-stocks", "size=2")
	if code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", code, body)
	}
	if provider.deviceID != "dev-stocks" || provider.size != 2 {
		t.Fatalf("expected device dev-stocks size 2, got %q %d", provider.deviceID, provider.size)
	}

	var resp NextItemResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if len(resp.Stocks) != 2 {
		t.Fatalf("expected 2 stocks, got %+v", resp.Stocks)
	}
	if resp.Stocks[0].Name != "上证指数" || resp.Stocks[0].ChangePct != -0.82 {
		t.Fatalf("unexpected first stock: %+v", resp.Stocks[0])
	}
	if resp.Stocks[1].Name != "苹果" || resp.Stocks[1].ChangePct != 1.52 {
		t.Fatalf("unexpected second stock: %+v", resp.Stocks[1])
	}
}

func TestNextOmitsStocksWhenUnavailable(t *testing.T) {
	// 提供者存在但窗口为空时，响应中不应出现 stocks 字段
	provider := &fakeStockProvider{}
	StockQuotes = provider
	t.Cleanup(func() { StockQuotes = nil })

	code, body := doNextRequest(t, "dev-no-stocks", "")
	if code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", code, body)
	}
	if provider.size != stocks.DefaultWindow {
		t.Fatalf("expected default size %d, got %d", stocks.DefaultWindow, provider.size)
	}

	var raw map[string]json.RawMessage
	if err := json.Unmarshal(body, &raw); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if _, ok := raw["stocks"]; ok {
		t.Fatalf("expected stocks field omitted, got %s", raw["stocks"])
	}
}

func TestNextAlwaysReturnsImageURL(t *testing.T) {
	setupTestDB(t)
	db := database.GetDB()
	feed := models.Feed{Name: "No Image Feed", URL: "http://feed.example/no-image", Enabled: true}
	if err := db.Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}
	item := models.Item{FeedID: feed.ID, Title: "no image", URL: "http://feed.example/no-image/1", ImagePath: ""}
	if err := db.Create(&item).Error; err != nil {
		t.Fatalf("create item: %v", err)
	}

	code, body := doNextRequest(t, "dev-image-url", "")
	if code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", code, body)
	}
	var resp NextItemResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.ImageURL == "" {
		t.Fatal("条目没有图片时 next 也应返回 image_url 兜底")
	}
}

func TestStockWindowSize(t *testing.T) {
	cases := []struct {
		raw  string
		want int
	}{
		{"", stocks.DefaultWindow},
		{"5", 5},
		{"abc", stocks.DefaultWindow},
		{"0", stocks.DefaultWindow},
		{"-3", stocks.DefaultWindow},
		{"999", stocks.MaxWindow},
	}
	for _, c := range cases {
		if got := stockWindowSize(c.raw); got != c.want {
			t.Fatalf("stockWindowSize(%q) = %d, want %d", c.raw, got, c.want)
		}
	}
}

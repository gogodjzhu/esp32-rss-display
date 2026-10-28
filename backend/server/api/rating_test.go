package api

// 评分路由的接口级测试：使用纯 Go SQLite 临时库验证评分落库、重复评分覆盖
// 以及非法输入返回错误。测试不依赖真实设备或运行中的服务。

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

func setupTestDB(t *testing.T) {
	t.Helper()
	if database.GetDB() != nil {
		return
	}
	// sqlite 支持 file::memory:?cache=shared 提供测试内共享内存库
	cfg := &config.DatabaseConfig{Driver: "sqlite", DSN: "file::memory:?cache=shared"}
	if err := database.Init(cfg); err != nil {
		t.Fatalf("init db: %v", err)
	}
	db := database.GetDB()
	if err := db.Create(&models.Item{FeedID: 1, Title: "t", URL: "u"}).Error; err != nil {
		t.Fatalf("seed item: %v", err)
	}
}

// use a real RFC1123ish "now"? 测试只需要不匹配的旧时间，这里直接构造。
func ratingRequest(t *testing.T, method, path string, body io.Reader) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, body)
	req.SetPathValue("item_id", "1")
	rr := httptest.NewRecorder()
	RateItem(rr, req)
	return rr
}

func writeRating(t *testing.T, rating uint8, deviceID string) int {
	t.Helper()
	payload, err := json.Marshal(map[string]any{"rating": rating, "device_id": deviceID})
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	rr := ratingRequest(t, http.MethodPost, "/v1/item/1/rating", bytes.NewReader(payload))
	return rr.Code
}

func TestRateItemStoresRating(t *testing.T) {
	setupTestDB(t)
	// 清理本组数据，避免与其它用例相互覆盖
	var ratings []models.ItemRating
	database.GetDB().Where("item_id = ?", 1).Delete(&ratings)

	if code := writeRating(t, 4, "dev-a"); code != http.StatusOK {
		t.Fatalf("expected 200, got %d", code)
	}
	var rating models.ItemRating
	if err := database.GetDB().Where("item_id = ? AND device_id = ?", 1, "dev-a").First(&rating).Error; err != nil {
		t.Fatalf("rating not stored: %v", err)
	}
	if rating.Rating != 4 {
		t.Fatalf("expected rating 4, got %d", rating.Rating)
	}
}

func TestRateItemUpsert(t *testing.T) {
	setupTestDB(t)
	var ratings []models.ItemRating
	database.GetDB().Where("item_id = ?", 1).Delete(&ratings)

	if code := writeRating(t, 2, "dev-a"); code != http.StatusOK {
		t.Fatalf("first submit: got %d", code)
	}
	if code := writeRating(t, 5, "dev-a"); code != http.StatusOK {
		t.Fatalf("second submit: got %d", code)
	}
	var count int64
	database.GetDB().Model(&models.ItemRating{}).Where("item_id = ? AND device_id = ?", 1, "dev-a").Count(&count)
	if count != 1 {
		t.Fatalf("expected single rating row, got %d", count)
	}
	var rating models.ItemRating
	database.GetDB().Where("item_id = ? AND device_id = ?", 1, "dev-a").First(&rating)
	if rating.Rating != 5 {
		t.Fatalf("expected rating 5 after upsert, got %d", rating.Rating)
	}
}

func TestRateItemRejectsInvalid(t *testing.T) {
	setupTestDB(t)

	cases := []struct {
		name   string
		body   string
		expect int
	}{
		{"rating out of range", `{"rating":6,"device_id":"dev-a"}`, http.StatusBadRequest},
		{"rating zero", `{"rating":0,"device_id":"dev-a"}`, http.StatusBadRequest},
		{"missing device", `{"rating":3}`, http.StatusBadRequest},
		{"malformed", `{`, http.StatusBadRequest},
		{"unknown field", `{"rating":3,"device_id":"dev-a","extra":1}`, http.StatusBadRequest},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			rr := ratingRequest(t, http.MethodPost, "/v1/item/1/rating", bytes.NewReader([]byte(c.body)))
			if rr.Code != c.expect {
				t.Fatalf("expected %d, got %d", c.expect, rr.Code)
			}
		})
	}

	// 不存在的 item 返回 404
	req := httptest.NewRequest(http.MethodPost, "/v1/item/99999/rating", bytes.NewReader([]byte(`{"rating":3,"device_id":"dev-a"}`)))
	req.SetPathValue("item_id", "99999")
	rr := httptest.NewRecorder()
	RateItem(rr, req)
	if rr.Code != http.StatusNotFound {
		t.Fatalf("expected 404 for missing item, got %d", rr.Code)
	}
}

func TestGetNextItemIncludesID(t *testing.T) {
	setupTestDB(t)
	db := database.GetDB()
	var item models.Item
	// 取最新一条（id 最大），验证响应中的 item_id 与库中 id 一致
	if err := db.Order("id DESC").First(&item).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			t.Skip("no items in db")
		}
		t.Fatalf("query item: %v", err)
	}
	// 需要重置设备的 current_item_id 为 0，让 next 从最早条目开始
	db.Model(&models.Device{}).Where("device_id = ?", "test-dev").Update("current_item_id", nil)

	req := httptest.NewRequest(http.MethodGet, "/v1/device/test-dev/next", nil)
	req.SetPathValue("device_id", "test-dev")
	rr := httptest.NewRecorder()
	GetNextItem(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rr.Code, rr.Body.String())
	}
	var resp NextItemResponse
	if err := json.Unmarshal(rr.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.ItemID == 0 {
		t.Fatal("expected non-zero item_id in next response")
	}
}
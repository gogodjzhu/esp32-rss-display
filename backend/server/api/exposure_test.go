package api

// 曝光模型的接口级测试：验证 next 优先返回新内容、不重复、看全后按最久
// 未展示轮换，以及未来发布时间的条目不会被永久排在队首。使用纯 Go SQLite
// 临时库，不访问网络。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

func seedItems(t *testing.T, feed models.Feed, published []time.Time) []uint {
	t.Helper()
	db := database.GetDB()
	if err := db.Create(&feed).Error; err != nil {
		t.Fatalf("seed feed: %v", err)
	}
	ids := make([]uint, 0, len(published))
	for i, p := range published {
		item := models.Item{FeedID: feed.ID, Title: "item", URL: feed.URL + "/" + string(rune('a'+i))}
		ts := p
		item.PublishedAt = &ts
		if err := db.Create(&item).Error; err != nil {
			t.Fatalf("seed item: %v", err)
		}
		ids = append(ids, item.ID)
	}
	return ids
}

func nextItemID(t *testing.T, deviceID string) uint {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, "/v1/device/"+deviceID+"/next", nil)
	req.SetPathValue("device_id", deviceID)
	rr := httptest.NewRecorder()
	GetNextItem(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rr.Code, rr.Body.String())
	}
	var resp NextItemResponse
	if err := json.Unmarshal(rr.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	return resp.ItemID
}

func TestNextPrefersNewestUnseen(t *testing.T) {
	db := setupExposureDB(t)
	now := time.Now()
	ids := seedItems(t, models.Feed{Name: "exp", URL: "http://exp.example/rss", Enabled: true},
		[]time.Time{now.Add(-3 * time.Hour), now.Add(-2 * time.Hour), now.Add(-1 * time.Hour)})

	device := "dev-exposure-newest"
	got := []uint{nextItemID(t, device), nextItemID(t, device), nextItemID(t, device)}
	want := []uint{ids[2], ids[1], ids[0]}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("第 %d 次 next = %d, want %d（应按发布时间倒序且不重复）", i+1, got[i], want[i])
		}
	}
	_ = db
}

func TestNextRecyclesOldestShown(t *testing.T) {
	setupExposureDB(t)
	now := time.Now()
	ids := seedItems(t, models.Feed{Name: "exp", URL: "http://exp2.example/rss", Enabled: true},
		[]time.Time{now.Add(-2 * time.Hour), now.Add(-1 * time.Hour)})

	device := "dev-exposure-recycle"
	for range ids {
		nextItemID(t, device)
	}
	// 两条都看过：下一次应回到最久未展示的那条（最早看过 = 最新发布的那条）
	first := nextItemID(t, device)
	if first != ids[0] {
		t.Fatalf("轮换第 3 次 = %d, want %d", first, ids[0])
	}
}

func TestNextSkipsFutureItems(t *testing.T) {
	setupExposureDB(t)
	now := time.Now()
	ids := seedItems(t, models.Feed{Name: "exp", URL: "http://exp3.example/rss", Enabled: true},
		[]time.Time{now.Add(-1 * time.Hour), now.Add(48 * time.Hour)})

	device := "dev-exposure-future"
	got := nextItemID(t, device)
	if got != ids[0] {
		t.Fatalf("未来发布时间的条目不应被返回，got %d want %d", got, ids[0])
	}
}

func TestNextSkipsItemsOlderThanMaxAge(t *testing.T) {
	setupExposureDB(t)
	now := time.Now()
	ids := seedItems(t, models.Feed{Name: "exp", URL: "http://exp4.example/rss", Enabled: true},
		[]time.Time{now.Add(-100 * time.Hour), now.Add(-1 * time.Hour)})

	device := "dev-exposure-maxage"
	got := nextItemID(t, device)
	if got != ids[1] {
		t.Fatalf("超过 max_age 的条目不应参与轮换，got %d want %d", got, ids[1])
	}
}

// setupExposureDB 确保库与种子数据可用，并清空曝光记录，避免用例互相干扰。
func setupExposureDB(t *testing.T) *gorm.DB {
	t.Helper()
	setupTestDB(t)
	db := database.GetDB()
	db.Where("1 = 1").Delete(&models.DeviceItemView{})
	return database.GetDB()
}

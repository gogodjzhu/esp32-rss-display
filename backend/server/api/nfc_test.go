package api

// NFC 打开记录与 next 新鲜度字段的接口级测试：验证 302 成功才计数、按设备
// 隔离、404 不记录，以及 next 下发 age_sec / open_count。使用纯 Go SQLite
// 临时库，不访问网络。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

// seedNfcDevice 建好设备与条目，返回设备的 current item id。
func seedNfcDevice(t *testing.T, deviceID string, item *models.Item) {
	t.Helper()
	db := database.GetDB()
	db.Where("device_id = ?", deviceID).Delete(&models.Device{})
	db.Where("device_id = ?", deviceID).Delete(&models.ItemNfcOpen{})
	now := time.Now()
	itemID := item.ID
	if err := db.Create(&models.Device{
		DeviceID:      deviceID,
		CurrentItemID: &itemID,
		LastSeen:      now,
		CreatedAt:     now,
	}).Error; err != nil {
		t.Fatalf("seed device: %v", err)
	}
}

func doNFCRedirect(t *testing.T, deviceID string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, "/nfc/"+deviceID, nil)
	req.SetPathValue("device_id", deviceID)
	rr := httptest.NewRecorder()
	NFCRedirect(rr, req)
	return rr
}

// nfcOpenCount 查询某设备对某条目的打开次数，无行返回 0。
func nfcOpenCount(t *testing.T, deviceID string, itemID uint) int {
	t.Helper()
	var opens models.ItemNfcOpen
	err := database.GetDB().
		Where("item_id = ? AND device_id = ?", itemID, deviceID).
		First(&opens).Error
	if err != nil {
		return 0
	}
	return opens.OpenCount
}

// firstSeedItem 取共享 setupTestDB 种下的那条 200h 前的条目。
func firstSeedItem(t *testing.T) models.Item {
	t.Helper()
	var item models.Item
	if err := database.GetDB().Order("id ASC").First(&item).Error; err != nil {
		t.Fatalf("get seed item: %v", err)
	}
	return item
}

// 本用例断言 next 选中「我们种的 2 小时前条目」。共享内存库中其它用例
// （按文件名字序先执行的 exposure/handlers）会留下更近期的条目，因此先清掉
// 除 setupTestDB 的 seed 条目（rating 测试依赖它，id=1）以外的所有条目与
// 曝光/NFC 记录，再种专属条目，保证断言不受执行顺序影响。
func TestNextFreshnessFields(t *testing.T) {
	setupTestDB(t)
	db := database.GetDB()

	var seed models.Item
	if err := db.Order("id ASC").First(&seed).Error; err != nil {
		t.Fatalf("get seed item: %v", err)
	}
	db.Where("id <> ?", seed.ID).Delete(&models.Item{})
	db.Where("1 = 1").Delete(&models.ItemNfcOpen{})
	db.Where("1 = 1").Delete(&models.DeviceItemView{})

	pub := time.Now().Add(-2 * time.Hour)
	item := models.Item{FeedID: seed.FeedID, Title: "fresh-item",
		URL: "http://fresh.example/1", PublishedAt: &pub}
	if err := db.Create(&item).Error; err != nil {
		t.Fatalf("seed item: %v", err)
	}

	code, body := doNextRequest(t, "dev-fresh", "")
	if code != http.StatusOK {
		t.Fatalf("next = %d, want 200: %s", code, body)
	}
	var resp NextItemResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if resp.ItemID != item.ID {
		t.Fatalf("item_id = %d, want %d（应选中新种的 2h 条目）", resp.ItemID, item.ID)
	}

	// 条目发布于 2 小时前，age_sec 应接近该差值且 >= 0
	if resp.AgeSec < 0 {
		t.Fatalf("age_sec = %d, want >= 0", resp.AgeSec)
	}
	want := int64(2 * 3600)
	if resp.AgeSec < want-120 || resp.AgeSec > want+120 {
		t.Fatalf("age_sec = %d, want ~%d (±120s)", resp.AgeSec, want)
	}
	if resp.OpenCount != 0 {
		t.Fatalf("open_count = %d, want 0", resp.OpenCount)
	}

	// NFC 打开两次后，下一次 next 返回同一条目且 open_count=2
	seedNfcDevice(t, "dev-fresh", &item)
	doNFCRedirect(t, "dev-fresh")
	doNFCRedirect(t, "dev-fresh")

	code, body = doNextRequest(t, "dev-fresh", "")
	if code != http.StatusOK {
		t.Fatalf("next after opens = %d, want 200: %s", code, body)
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode after opens: %v", err)
	}
	if resp.ItemID != item.ID {
		t.Fatalf("item_id = %d, want %d", resp.ItemID, item.ID)
	}
	if resp.OpenCount != 2 {
		t.Fatalf("open_count = %d, want 2", resp.OpenCount)
	}
}

func TestNFCRedirectRecordsOpen(t *testing.T) {
	setupTestDB(t)
	item := firstSeedItem(t)
	seedNfcDevice(t, "nfc-rec", &item)

	rr1 := doNFCRedirect(t, "nfc-rec")
	if rr1.Code != http.StatusFound {
		t.Fatalf("first redirect = %d, want 302", rr1.Code)
	}
	if loc := rr1.Header().Get("Location"); loc != item.URL {
		t.Fatalf("location = %q, want %q", loc, item.URL)
	}
	// 第二次打开：计数 +1，首次时间不变
	time.Sleep(2 * time.Millisecond)
	rr2 := doNFCRedirect(t, "nfc-rec")
	if rr2.Code != http.StatusFound {
		t.Fatalf("second redirect = %d, want 302", rr2.Code)
	}

	var opens models.ItemNfcOpen
	if err := database.GetDB().
		Where("item_id = ? AND device_id = ?", item.ID, "nfc-rec").
		First(&opens).Error; err != nil {
		t.Fatalf("read open row: %v", err)
	}
	if opens.OpenCount != 2 {
		t.Fatalf("open_count = %d, want 2", opens.OpenCount)
	}
	if opens.FirstOpenAt.IsZero() || opens.LastOpenAt.IsZero() {
		t.Fatalf("时间戳不应为零: first=%v last=%v", opens.FirstOpenAt, opens.LastOpenAt)
	}
	if opens.LastOpenAt.Before(opens.FirstOpenAt) {
		t.Fatalf("last_open_at %v 早于 first_open_at %v", opens.LastOpenAt, opens.FirstOpenAt)
	}
}

func TestNFCRedirectSeparatesPerDevice(t *testing.T) {
	setupTestDB(t)
	item := firstSeedItem(t)

	seedNfcDevice(t, "nfc-a", &item)
	seedNfcDevice(t, "nfc-b", &item)

	doNFCRedirect(t, "nfc-a")
	doNFCRedirect(t, "nfc-b")

	if got := nfcOpenCount(t, "nfc-a", item.ID); got != 1 {
		t.Fatalf("device a count = %d, want 1", got)
	}
	if got := nfcOpenCount(t, "nfc-b", item.ID); got != 1 {
		t.Fatalf("device b count = %d, want 1", got)
	}
	// 再开一次 a，a 变 2、b 仍是 1
	doNFCRedirect(t, "nfc-a")
	if got := nfcOpenCount(t, "nfc-a", item.ID); got != 2 {
		t.Fatalf("device a second count = %d, want 2", got)
	}
	if got := nfcOpenCount(t, "nfc-b", item.ID); got != 1 {
		t.Fatalf("device b unchanged count = %d, want 1", got)
	}
}

func TestNFCRedirectSkipsOn404(t *testing.T) {
	setupTestDB(t)
	item := firstSeedItem(t)

	// 1) 设备不存在
	if rr := doNFCRedirect(t, "nfc-missing"); rr.Code != http.StatusNotFound {
		t.Fatalf("unknown device = %d, want 404", rr.Code)
	}

	// 2) 设备存在但 current_item_id 为空：直接建无游标设备
	db := database.GetDB()
	if err := db.Create(&models.Device{DeviceID: "nfc-nocur"}).Error; err != nil {
		t.Fatalf("seed nil-current device: %v", err)
	}
	if rr := doNFCRedirect(t, "nfc-nocur"); rr.Code != http.StatusNotFound {
		t.Fatalf("nil current = %d, want 404", rr.Code)
	}

	// 3) URL 为空
	empty := models.Item{FeedID: item.FeedID, Title: "no-url", URL: ""}
	if err := db.Create(&empty).Error; err != nil {
		t.Fatalf("seed empty-url item: %v", err)
	}
	seedNfcDevice(t, "nfc-nourl", &empty)
	if rr := doNFCRedirect(t, "nfc-nourl"); rr.Code != http.StatusNotFound {
		t.Fatalf("empty url = %d, want 404", rr.Code)
	}

	for _, dev := range []string{"nfc-missing", "nfc-nocur", "nfc-nourl"} {
		if got := nfcOpenCount(t, dev, item.ID); got != 0 {
			t.Fatalf("device %s 不应有记录，got %d", dev, got)
		}
	}
}
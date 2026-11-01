package api

// 管理页只读接口的测试：运行状态聚合、单源条目列表、订阅源预览与创建校验。
// 预览用本地 httptest 服务返回 RSS，不依赖外网。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

const sampleRSS = `<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0">
  <channel>
    <title>示例订阅源</title>
    <link>https://example.com</link>
    <description>用于测试的订阅源</description>
    <item>
      <title>第一条</title>
      <link>https://example.com/1</link>
      <pubDate>Wed, 01 Nov 2026 08:00:00 GMT</pubDate>
    </item>
    <item>
      <title>第二条</title>
      <link>https://example.com/2</link>
      <pubDate>Wed, 01 Nov 2026 07:00:00 GMT</pubDate>
    </item>
  </channel>
</rss>`

// newRSSServer 起一个返回固定 RSS 的本地服务，返回其 URL。
func newRSSServer(t *testing.T, body string, status int) string {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/rss+xml")
		w.WriteHeader(status)
		w.Write([]byte(body))
	}))
	t.Cleanup(srv.Close)
	return srv.URL
}

// dropFeedData 删除用例创建的订阅源及其关联数据，避免污染共享内存库
// 里的其它用例（曝光模型会扫描全表）。
func dropFeedData(t *testing.T, feedID uint) {
	t.Helper()
	t.Cleanup(func() {
		db := database.GetDB()
		var ids []uint
		db.Model(&models.Item{}).Where("feed_id = ?", feedID).Pluck("id", &ids)
		if len(ids) > 0 {
			db.Where("item_id IN ?", ids).Delete(&models.DeviceItemView{})
			db.Where("item_id IN ?", ids).Delete(&models.ItemRating{})
			db.Where("item_id IN ?", ids).Delete(&models.ItemNfcOpen{})
			db.Where("id IN ?", ids).Delete(&models.Item{})
		}
		db.Delete(&models.Feed{}, feedID)
	})
}

func TestAdminPreviewFeed(t *testing.T) {
	setupTestDB(t)
	url := newRSSServer(t, sampleRSS, http.StatusOK)

	req := httptest.NewRequest(http.MethodPost, "/admin/feeds/preview",
		strings.NewReader(`{"url":"`+url+`"}`))
	rec := httptest.NewRecorder()
	AdminPreviewFeed(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}

	var preview feedPreview
	if err := json.Unmarshal(rec.Body.Bytes(), &preview); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if preview.Title != "示例订阅源" {
		t.Fatalf("title = %q", preview.Title)
	}
	if preview.ItemCount != 2 || len(preview.Items) != 2 {
		t.Fatalf("items = %d/%d, want 2/2", preview.ItemCount, len(preview.Items))
	}
	if preview.Items[0].Title != "第一条" || preview.Items[0].URL != "https://example.com/1" {
		t.Fatalf("first item = %+v", preview.Items[0])
	}
	if preview.Items[0].PublishedAt == nil {
		t.Fatal("first item published_at 为空")
	}
}

func TestAdminPreviewFeedRejectsNonFeed(t *testing.T) {
	setupTestDB(t)
	url := newRSSServer(t, `<html><body>not a feed</body></html>`, http.StatusOK)

	req := httptest.NewRequest(http.MethodPost, "/admin/feeds/preview",
		strings.NewReader(`{"url":"`+url+`"}`))
	rec := httptest.NewRecorder()
	AdminPreviewFeed(rec, req)
	if rec.Code != http.StatusUnprocessableEntity {
		t.Fatalf("status = %d, want 422, body = %s", rec.Code, rec.Body.String())
	}
}

func TestAdminPreviewFeedRejectsDuplicate(t *testing.T) {
	setupTestDB(t)
	url := newRSSServer(t, sampleRSS, http.StatusOK)

	feed := models.Feed{Name: "dup", URL: url, Enabled: true}
	if err := database.GetDB().Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}
	dropFeedData(t, feed.ID)

	req := httptest.NewRequest(http.MethodPost, "/admin/feeds/preview",
		strings.NewReader(`{"url":"`+url+`"}`))
	rec := httptest.NewRecorder()
	AdminPreviewFeed(rec, req)
	if rec.Code != http.StatusConflict {
		t.Fatalf("status = %d, want 409, body = %s", rec.Code, rec.Body.String())
	}
}

func TestCreateFeedValidatesSource(t *testing.T) {
	setupTestDB(t)
	bad := newRSSServer(t, `<html>nope</html>`, http.StatusOK)

	req := httptest.NewRequest(http.MethodPost, "/v1/feeds",
		strings.NewReader(`{"name":"bad","url":"`+bad+`"}`))
	rec := httptest.NewRecorder()
	CreateFeed(rec, req)
	if rec.Code != http.StatusUnprocessableEntity {
		t.Fatalf("status = %d, want 422, body = %s", rec.Code, rec.Body.String())
	}

	var count int64
	database.GetDB().Model(&models.Feed{}).Where("url = ?", bad).Count(&count)
	if count != 0 {
		t.Fatalf("非法源不应入库，实际 %d 条", count)
	}
}

func TestCreateFeedAcceptsValidSource(t *testing.T) {
	setupTestDB(t)
	good := newRSSServer(t, sampleRSS, http.StatusOK)

	req := httptest.NewRequest(http.MethodPost, "/v1/feeds",
		strings.NewReader(`{"name":"good","url":"`+good+`"}`))
	rec := httptest.NewRecorder()
	CreateFeed(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("status = %d, want 201, body = %s", rec.Code, rec.Body.String())
	}

	var created models.Feed
	if err := database.GetDB().Where("url = ?", good).First(&created).Error; err != nil {
		t.Fatalf("created feed: %v", err)
	}
	dropFeedData(t, created.ID)
}

// TestAdminFeedItems 覆盖排序、分页与单条目互动聚合。
func TestAdminFeedItems(t *testing.T) {
	setupTestDB(t)
	db := database.GetDB()

	feed := models.Feed{Name: "items", URL: "http://items.example/rss", Enabled: true}
	if err := db.Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}
	dropFeedData(t, feed.ID)

	now := time.Now()
	old := now.Add(-2 * time.Hour)
	newer := now.Add(-1 * time.Hour)
	items := []models.Item{
		{FeedID: feed.ID, Title: "老", URL: "http://items.example/old", PublishedAt: &old},
		{FeedID: feed.ID, Title: "新", URL: "http://items.example/new", PublishedAt: &newer},
	}
	for i := range items {
		if err := db.Create(&items[i]).Error; err != nil {
			t.Fatalf("create item: %v", err)
		}
	}

	newest := items[1]
	db.Create(&models.DeviceItemView{DeviceID: "dev-x", ItemID: newest.ID, ShowCount: 3, LastShownAt: now})
	db.Create(&models.DeviceItemView{DeviceID: "dev-y", ItemID: newest.ID, ShowCount: 2, LastShownAt: now})
	db.Create(&models.ItemNfcOpen{ItemID: newest.ID, DeviceID: "dev-x", OpenCount: 2,
		FirstOpenAt: now, LastOpenAt: now})
	db.Create(&models.ItemRating{ItemID: newest.ID, DeviceID: "dev-x", Rating: 4, CreatedAt: now})
	db.Create(&models.ItemRating{ItemID: newest.ID, DeviceID: "dev-y", Rating: 2, CreatedAt: now})

	req := httptest.NewRequest(http.MethodGet, "/admin/feeds/"+strconv.FormatUint(uint64(feed.ID), 10)+"/items?limit=1", nil)
	req.SetPathValue("id", strconv.FormatUint(uint64(feed.ID), 10))
	rec := httptest.NewRecorder()
	AdminFeedItems(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}

	var list adminItemList
	if err := json.Unmarshal(rec.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if list.Total != 2 {
		t.Fatalf("total = %d, want 2", list.Total)
	}
	if len(list.Items) != 1 || !list.HasMore {
		t.Fatalf("items = %d has_more = %v, want 1/true", len(list.Items), list.HasMore)
	}
	first := list.Items[0]
	if first.Title != "新" {
		t.Fatalf("首条应为最新条目，得到 %q", first.Title)
	}
	if first.ImageURL != "/image/"+strconv.FormatUint(uint64(newest.ID), 10)+".jpg" {
		t.Fatalf("image_url = %q", first.ImageURL)
	}
	if first.ShowCount != 5 || first.NfcOpens != 2 {
		t.Fatalf("show = %d nfc = %d, want 5/2", first.ShowCount, first.NfcOpens)
	}
	if first.RatingCount != 2 || first.RatingAvg != 3 {
		t.Fatalf("rating = %d/%v, want 2/3", first.RatingCount, first.RatingAvg)
	}
}

func TestAdminStatusAggregatesFeedStats(t *testing.T) {
	setupTestDB(t)
	db := database.GetDB()

	feed := models.Feed{Name: "status", URL: "http://status.example/rss", Enabled: true}
	if err := db.Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}
	dropFeedData(t, feed.ID)
	now := time.Now()
	pub := now.Add(-time.Hour)
	item := models.Item{FeedID: feed.ID, Title: "s", URL: "http://status.example/1", PublishedAt: &pub}
	if err := db.Create(&item).Error; err != nil {
		t.Fatalf("create item: %v", err)
	}
	db.Create(&models.DeviceItemView{DeviceID: "dev-s", ItemID: item.ID, ShowCount: 7, LastShownAt: now})
	db.Create(&models.ItemNfcOpen{ItemID: item.ID, DeviceID: "dev-s", OpenCount: 4,
		FirstOpenAt: now, LastOpenAt: now})
	db.Create(&models.ItemRating{ItemID: item.ID, DeviceID: "dev-s", Rating: 5, CreatedAt: now})

	rec := httptest.NewRecorder()
	AdminStatusHandler(rec, httptest.NewRequest(http.MethodGet, "/admin/status", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}

	var status AdminStatus
	if err := json.Unmarshal(rec.Body.Bytes(), &status); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(status.ItemsByDay) != dailyChartDays {
		t.Fatalf("items_by_day = %d, want %d", len(status.ItemsByDay), dailyChartDays)
	}
	if today := status.ItemsByDay[len(status.ItemsByDay)-1]; today.Count < 1 {
		t.Fatalf("今天的桶计数 = %d, 至少应为 1", today.Count)
	}
	if status.FetchMinutes <= 0 {
		t.Fatalf("fetch_interval_minutes = %d", status.FetchMinutes)
	}
	if status.Ratings.Count < 1 || status.Ratings.Avg <= 0 {
		t.Fatalf("ratings = %+v", status.Ratings)
	}

	var target *FeedStatus
	for i := range status.Feeds {
		if status.Feeds[i].ID == feed.ID {
			target = &status.Feeds[i]
		}
	}
	if target == nil {
		t.Fatal("状态里缺少新建的订阅源")
	}
	if target.ItemsTotal != 1 || target.ItemsToday != 1 {
		t.Fatalf("items = %d/%d, want 1/1", target.ItemsTotal, target.ItemsToday)
	}
	if target.ShownTotal != 7 || target.NfcOpens != 4 {
		t.Fatalf("shown = %d nfc = %d, want 7/4", target.ShownTotal, target.NfcOpens)
	}
	if target.RatingCount != 1 || target.RatingAvg != 5 {
		t.Fatalf("rating = %d/%v, want 1/5", target.RatingCount, target.RatingAvg)
	}
	if target.LastNewItemAt == nil {
		t.Fatal("last_new_item_at 为空")
	}
	if target.LastNewItemAt.IsZero() {
		t.Fatal("last_new_item_at 不应是零值")
	}
	if time.Since(*target.LastNewItemAt) > time.Hour {
		t.Fatalf("last_new_item_at = %v，应接近当前时间", target.LastNewItemAt)
	}
}

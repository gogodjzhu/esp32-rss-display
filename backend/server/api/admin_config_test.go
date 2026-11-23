package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

func resetAdminConfig(t *testing.T) {
	t.Helper()
	setupTestDB(t)
	db := database.GetDB()
	if err := db.Delete(&models.AppSettings{}, 1).Error; err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Delete(&models.AppSettings{}, 1) })
}

func TestAdminConfigRoundTrip(t *testing.T) {
	resetAdminConfig(t)
	read := func(want bool) {
		t.Helper()
		rec := httptest.NewRecorder()
		GetAdminConfig(rec, httptest.NewRequest("GET", "/admin/config", nil))
		var cfg models.AppSettings
		if rec.Code != http.StatusOK {
			t.Fatalf("GET: %d %s", rec.Code, rec.Body.String())
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &cfg); err != nil {
			t.Fatal(err)
		}
		if cfg.TranslationEnabled != want {
			t.Fatalf("enabled = %v, want %v", cfg.TranslationEnabled, want)
		}
	}
	read(false)
	notifications := 0
	for _, body := range []string{`{"translation_enabled":true}`, `{"translation_enabled":false}`} {
		rec := httptest.NewRecorder()
		PutAdminConfig(func() { notifications++ })(rec, httptest.NewRequest("PUT", "/admin/config", strings.NewReader(body)))
		if rec.Code != http.StatusOK {
			t.Fatalf("PUT: %d %s", rec.Code, rec.Body.String())
		}
		read(strings.Contains(body, "true"))
	}
	if notifications != 1 {
		t.Fatalf("开启时应唤醒一次，实际 %d", notifications)
	}
}

func TestAdminConfigRejectsInvalidPayloads(t *testing.T) {
	resetAdminConfig(t)
	if _, err := database.SaveSettings(database.GetDB(), true); err != nil {
		t.Fatal(err)
	}
	for _, body := range []string{
		`{}`, `null`, `{"translation_enabled":null}`, `{"translation_enabled":"true"}`,
		`{"translation_enabled":1}`, `{"translation_enabled":false,"unknown":1}`,
		`{"translation_enabled":false} {}`, `{"translation_enabled":`, strings.Repeat(" ", 4096) + `{}`,
	} {
		t.Run(body[:min(len(body), 60)], func(t *testing.T) {
			rec := httptest.NewRecorder()
			PutAdminConfig(nil)(rec, httptest.NewRequest("PUT", "/admin/config", strings.NewReader(body)))
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
			}
			cfg, err := database.GetSettings(database.GetDB())
			if err != nil || !cfg.TranslationEnabled {
				t.Fatalf("非法输入改变配置: %+v %v", cfg, err)
			}
		})
	}
}

func TestNextSelectsTranslatedTitleOnlyWhenEnabled(t *testing.T) {
	resetAdminConfig(t)
	db := database.GetDB()
	feed := models.Feed{Name: "translation-test", URL: "https://example.com/translation-test"}
	if err := db.Create(&feed).Error; err != nil {
		t.Fatal(err)
	}
	dropFeedData(t, feed.ID)
	// 使用最新发布时间，让本测试条目排在既有测试数据之前。
	now := time.Now()
	item := models.Item{FeedID: feed.ID, Title: "Original title", TranslatedTitle: "中文标题", URL: "https://example.com/title", PublishedAt: &now}
	if err := db.Create(&item).Error; err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		enabled          bool
		translated, want string
	}{
		{false, "中文标题", "Original title"}, {true, "中文标题", "中文标题"},
		{true, "", "Original title"}, {false, "中文标题", "Original title"},
	} {
		if _, err := database.SaveSettings(db, tc.enabled); err != nil {
			t.Fatal(err)
		}
		if err := db.Model(&item).Update("translated_title", tc.translated).Error; err != nil {
			t.Fatal(err)
		}
		// 每次清掉本测试设备的曝光记录，让它仍优先取最新条目。
		if err := db.Where("device_id = ?", "dev-translation").Delete(&models.DeviceItemView{}).Error; err != nil {
			t.Fatal(err)
		}
		req := httptest.NewRequest("GET", "/v1/device/dev-translation/next", nil)
		req.SetPathValue("device_id", "dev-translation")
		rec := httptest.NewRecorder()
		GetNextItem(rec, req)
		var resp NextItemResponse
		if rec.Code != http.StatusOK {
			t.Fatalf("next: %d %s", rec.Code, rec.Body.String())
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
			t.Fatal(err)
		}
		if resp.ItemID != item.ID || resp.Title != tc.want {
			t.Fatalf("next = %+v, want item=%d title=%q", resp, item.ID, tc.want)
		}
	}
}

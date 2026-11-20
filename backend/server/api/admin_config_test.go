package api

// 管理页配置接口与 next 译文返回的测试：开关持久化、非法请求拒绝，
// 以及开启后 next 返回缓存译文、关闭后恢复原标题。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/settings"
)

func setTranslationEnabled(t *testing.T, enabled bool) {
	t.Helper()
	if err := settings.Update(database.GetDB(), settings.Config{TranslationEnabled: enabled}); err != nil {
		t.Fatalf("update settings: %v", err)
	}
	t.Cleanup(func() {
		_ = settings.Update(database.GetDB(), settings.Config{TranslationEnabled: false})
	})
}

func TestAdminConfigRoundTrip(t *testing.T) {
	setupTestDB(t)
	setTranslationEnabled(t, false)

	rec := httptest.NewRecorder()
	GetAdminConfig(rec, httptest.NewRequest(http.MethodGet, "/admin/config", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("GET status = %d: %s", rec.Code, rec.Body.String())
	}
	var cfg AdminConfig
	if err := json.Unmarshal(rec.Body.Bytes(), &cfg); err != nil {
		t.Fatalf("decode GET: %v", err)
	}
	if cfg.TranslationEnabled {
		t.Fatal("默认翻译开关应为关闭")
	}

	rec = httptest.NewRecorder()
	UpdateAdminConfig(rec, httptest.NewRequest(http.MethodPut, "/admin/config",
		strings.NewReader(`{"translation_enabled":true}`)))
	if rec.Code != http.StatusOK {
		t.Fatalf("PUT status = %d: %s", rec.Code, rec.Body.String())
	}
	if !settings.TranslationEnabled() {
		t.Fatal("保存后内存配置应立即生效")
	}
	var row models.AppSettings
	if err := database.GetDB().First(&row, 1).Error; err != nil {
		t.Fatalf("reload settings row: %v", err)
	}
	if !row.TranslationEnabled {
		t.Fatal("配置应持久化到数据库")
	}

	rec = httptest.NewRecorder()
	GetAdminConfig(rec, httptest.NewRequest(http.MethodGet, "/admin/config", nil))
	if err := json.Unmarshal(rec.Body.Bytes(), &cfg); err != nil {
		t.Fatalf("decode GET after put: %v", err)
	}
	if !cfg.TranslationEnabled {
		t.Fatal("GET 应返回已保存的配置")
	}
}

func TestUpdateAdminConfigRejectsInvalidBodies(t *testing.T) {
	setupTestDB(t)
	setTranslationEnabled(t, false)

	cases := []struct {
		name string
		body string
	}{
		{"缺少字段", `{}`},
		{"未知字段", `{"translation_enabled":true,"other":1}`},
		{"格式错误", `not json`},
	}
	for _, tc := range cases {
		rec := httptest.NewRecorder()
		UpdateAdminConfig(rec, httptest.NewRequest(http.MethodPut, "/admin/config",
			strings.NewReader(tc.body)))
		if rec.Code != http.StatusBadRequest {
			t.Errorf("%s: status = %d, want 400: %s", tc.name, rec.Code, rec.Body.String())
		}
	}
}

func TestDisplayTitle(t *testing.T) {
	setupTestDB(t)
	item := &models.Item{Title: "Original title"}

	setTranslationEnabled(t, true)
	if got := displayTitle(item); got != "Original title" {
		t.Fatalf("无译文时应返回原标题，got %q", got)
	}
	item.TranslatedTitle = "中文标题"
	if got := displayTitle(item); got != "中文标题" {
		t.Fatalf("开启且有译文时应返回译文，got %q", got)
	}

	setTranslationEnabled(t, false)
	if got := displayTitle(item); got != "Original title" {
		t.Fatalf("关闭时应返回原标题，got %q", got)
	}
}

func TestNextReturnsTranslatedTitleWhenEnabled(t *testing.T) {
	setupTestDB(t)
	db := database.GetDB()

	// 清掉其它用例留下的近期条目，避免 next 选中它们；保留评级测试依赖的种子条目。
	var seed models.Item
	if err := db.Order("id ASC").First(&seed).Error; err != nil {
		t.Fatalf("get seed item: %v", err)
	}
	db.Where("id <> ?", seed.ID).Delete(&models.Item{})
	db.Where("1 = 1").Delete(&models.ItemNfcOpen{})
	db.Where("1 = 1").Delete(&models.DeviceItemView{})

	published := time.Now().Add(-2 * time.Hour)
	item := models.Item{
		FeedID:          seed.FeedID,
		Title:           "Fed raises interest rates",
		URL:             "http://translated.example/1",
		PublishedAt:     &published,
		TranslatedTitle: "美联储加息",
	}
	if err := db.Create(&item).Error; err != nil {
		t.Fatalf("create item: %v", err)
	}
	t.Cleanup(func() {
		db.Where("item_id = ?", item.ID).Delete(&models.DeviceItemView{})
		db.Where("id = ?", item.ID).Delete(&models.Item{})
	})

	setTranslationEnabled(t, true)
	code, body := doNextRequest(t, "dev-translate-on", "")
	if code != http.StatusOK {
		t.Fatalf("next = %d, want 200: %s", code, body)
	}
	var resp NextItemResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if resp.Title != "美联储加息" {
		t.Fatalf("开启时 title = %q, want 译文", resp.Title)
	}

	setTranslationEnabled(t, false)
	code, body = doNextRequest(t, "dev-translate-off", "")
	if code != http.StatusOK {
		t.Fatalf("next = %d, want 200: %s", code, body)
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode off: %v", err)
	}
	if resp.Title != "Fed raises interest rates" {
		t.Fatalf("关闭时 title = %q, want 原标题", resp.Title)
	}
}

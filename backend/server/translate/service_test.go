package translate

// 后台补译测试：覆盖英文翻译落库、中文标题跳过、失败记录与重试窗口。
// 使用独立内存库与假 Provider，不访问外网。

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/settings"
	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

func setupServiceDB(t *testing.T) *gorm.DB {
	t.Helper()
	dsn := fmt.Sprintf("file:translate_%d?mode=memory&cache=shared", time.Now().UnixNano())
	db, err := gorm.Open(sqlite.Open(dsn), &gorm.Config{Logger: logger.Default.LogMode(logger.Silent)})
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	if err := db.AutoMigrate(&models.Item{}, &models.AppSettings{}); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if err := settings.Load(db); err != nil {
		t.Fatalf("load settings: %v", err)
	}
	if err := settings.Update(db, settings.Config{TranslationEnabled: true}); err != nil {
		t.Fatalf("enable translation: %v", err)
	}
	t.Cleanup(func() {
		_ = settings.Update(db, settings.Config{TranslationEnabled: false})
	})
	return db
}

func seedTranslateItem(t *testing.T, db *gorm.DB, title string, age time.Duration) models.Item {
	t.Helper()
	published := time.Now().Add(-age)
	item := models.Item{
		FeedID:      1,
		Title:       title,
		URL:         fmt.Sprintf("http://example.com/%d", time.Now().UnixNano()),
		PublishedAt: &published,
	}
	if err := db.Create(&item).Error; err != nil {
		t.Fatalf("create item: %v", err)
	}
	return item
}

func TestProcessPendingTranslatesEnglishTitles(t *testing.T) {
	db := setupServiceDB(t)
	english := seedTranslateItem(t, db, "Fed raises interest rates", time.Hour)
	chinese := seedTranslateItem(t, db, "美联储宣布加息", 2*time.Hour)

	provider := &fakeProvider{name: "fake", out: "美联储加息"}
	service := NewService(db, NewClientWithProviders(provider))

	translated, skipped, failed := service.ProcessPending(context.Background())
	if translated != 1 || skipped != 1 || failed != 0 {
		t.Fatalf("translated/skipped/failed = %d/%d/%d, want 1/1/0", translated, skipped, failed)
	}
	if provider.calls != 1 {
		t.Fatalf("provider calls = %d, want 1（中文标题不应调用接口）", provider.calls)
	}

	var gotEnglish models.Item
	if err := db.First(&gotEnglish, english.ID).Error; err != nil {
		t.Fatalf("reload english item: %v", err)
	}
	if gotEnglish.TranslatedTitle != "美联储加息" {
		t.Fatalf("translated_title = %q", gotEnglish.TranslatedTitle)
	}
	if gotEnglish.TranslationAttemptedAt == nil {
		t.Fatal("translation_attempted_at 应写入")
	}

	var gotChinese models.Item
	if err := db.First(&gotChinese, chinese.ID).Error; err != nil {
		t.Fatalf("reload chinese item: %v", err)
	}
	if gotChinese.TranslatedTitle != "美联储宣布加息" {
		t.Fatalf("中文标题应原样标记为已处理，got %q", gotChinese.TranslatedTitle)
	}
}

func TestProcessPendingSkipsWhenDisabled(t *testing.T) {
	db := setupServiceDB(t)
	if err := settings.Update(db, settings.Config{TranslationEnabled: false}); err != nil {
		t.Fatalf("disable: %v", err)
	}
	seedTranslateItem(t, db, "Hello world", time.Hour)

	provider := &fakeProvider{name: "fake", out: "你好世界"}
	service := NewService(db, NewClientWithProviders(provider))
	if translated, skipped, failed := service.ProcessPending(context.Background()); translated+skipped+failed != 0 {
		t.Fatalf("关闭时不应处理，got %d/%d/%d", translated, skipped, failed)
	}
	if provider.calls != 0 {
		t.Fatalf("关闭时不应调用接口，calls = %d", provider.calls)
	}
}

func TestProcessPendingRecordsFailureAndRetryWindow(t *testing.T) {
	db := setupServiceDB(t)
	item := seedTranslateItem(t, db, "Hello world", time.Hour)

	provider := &fakeProvider{name: "fake", err: errors.New("接口不可用")}
	service := NewService(db, NewClientWithProviders(provider))

	if _, _, failed := service.ProcessPending(context.Background()); failed != 1 {
		t.Fatalf("failed = %d, want 1", failed)
	}

	var got models.Item
	if err := db.First(&got, item.ID).Error; err != nil {
		t.Fatalf("reload item: %v", err)
	}
	if got.TranslatedTitle != "" {
		t.Fatalf("失败时不应写入译文，got %q", got.TranslatedTitle)
	}
	if got.TranslationAttemptedAt == nil {
		t.Fatal("失败时应记录尝试时间")
	}

	// 重试间隔内不重复调用
	service.ProcessPending(context.Background())
	if provider.calls != 1 {
		t.Fatalf("重试窗口内 calls = %d, want 1", provider.calls)
	}

	// 把尝试时间挪到重试窗口之外，应再次尝试
	old := time.Now().Add(-2 * time.Hour)
	if err := db.Model(&models.Item{}).Where("id = ?", item.ID).
		Update("translation_attempted_at", old).Error; err != nil {
		t.Fatalf("rewind attempted_at: %v", err)
	}
	service.ProcessPending(context.Background())
	if provider.calls != 2 {
		t.Fatalf("超过重试窗口 calls = %d, want 2", provider.calls)
	}
}

func TestProcessPendingIgnoresExpiredItems(t *testing.T) {
	db := setupServiceDB(t)
	seedTranslateItem(t, db, "Old news", 100*time.Hour)

	provider := &fakeProvider{name: "fake", out: "旧闻"}
	service := NewService(db, NewClientWithProviders(provider))
	if translated, skipped, failed := service.ProcessPending(context.Background()); translated+skipped+failed != 0 {
		t.Fatalf("保留期外的条目不应翻译，got %d/%d/%d", translated, skipped, failed)
	}
	if provider.calls != 0 {
		t.Fatalf("provider calls = %d, want 0", provider.calls)
	}
}

package translate

// 补译函数测试：英文翻译落库、中文标题跳过、失败重试与保留期过滤。
// 使用独立内存库和本地 httptest 翻译端点，不访问外网。

import (
	"context"
	"fmt"
	"net/http"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/settings"
	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

func setupPendingDB(t *testing.T) *gorm.DB {
	t.Helper()
	dsn := fmt.Sprintf("file:pending_%d?mode=memory&cache=shared", time.Now().UnixNano())
	db, err := gorm.Open(sqlite.Open(dsn), &gorm.Config{Logger: logger.Default.LogMode(logger.Silent)})
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	if err := db.AutoMigrate(&models.Item{}, &models.AppSettings{}); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	return db
}

func seedPendingItem(t *testing.T, db *gorm.DB, title string, age time.Duration) models.Item {
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

func TestPendingTranslatesEnglishAndMarksChinese(t *testing.T) {
	db := setupPendingDB(t)
	if err := settings.Set(db, true); err != nil {
		t.Fatalf("enable translation: %v", err)
	}
	english := seedPendingItem(t, db, "Fed raises interest rates", time.Hour)
	chinese := seedPendingItem(t, db, "美联储宣布加息", 2*time.Hour)

	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) { w.Write([]byte(`["美联储加息"]`)) },
		func(w http.ResponseWriter, r *http.Request) { t.Error("不应调用 MyMemory") },
	)

	translated, skipped, failed := Pending(context.Background(), db)
	if translated != 1 || skipped != 1 || failed != 0 {
		t.Fatalf("translated/skipped/failed = %d/%d/%d, want 1/1/0", translated, skipped, failed)
	}

	var gotEnglish models.Item
	db.First(&gotEnglish, english.ID)
	if gotEnglish.TranslatedTitle != "美联储加息" {
		t.Fatalf("translated_title = %q", gotEnglish.TranslatedTitle)
	}

	var gotChinese models.Item
	db.First(&gotChinese, chinese.ID)
	if gotChinese.TranslatedTitle != "美联储宣布加息" {
		t.Fatalf("中文标题应原样标记为已处理，got %q", gotChinese.TranslatedTitle)
	}
}

func TestPendingSkipsWhenDisabled(t *testing.T) {
	db := setupPendingDB(t)
	if err := settings.Set(db, false); err != nil {
		t.Fatalf("disable translation: %v", err)
	}
	seedPendingItem(t, db, "Hello world", time.Hour)

	calls := 0
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) { calls++ },
		func(w http.ResponseWriter, r *http.Request) { calls++ },
	)

	if translated, skipped, failed := Pending(context.Background(), db); translated+skipped+failed != 0 {
		t.Fatalf("关闭时不应处理，got %d/%d/%d", translated, skipped, failed)
	}
	if calls != 0 {
		t.Fatalf("关闭时不应调用接口，calls = %d", calls)
	}
}

func TestPendingRetriesFailedItems(t *testing.T) {
	db := setupPendingDB(t)
	if err := settings.Set(db, true); err != nil {
		t.Fatalf("enable translation: %v", err)
	}
	item := seedPendingItem(t, db, "Hello world", time.Hour)

	fail := true
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) {
			if fail {
				http.Error(w, "down", http.StatusBadGateway)
				return
			}
			w.Write([]byte(`["你好世界"]`))
		},
		func(w http.ResponseWriter, r *http.Request) { http.Error(w, "down", http.StatusBadGateway) },
	)

	if _, _, failed := Pending(context.Background(), db); failed != 1 {
		t.Fatalf("failed = %d, want 1", failed)
	}
	var got models.Item
	db.First(&got, item.ID)
	if got.TranslatedTitle != "" {
		t.Fatalf("失败时不应写入译文，got %q", got.TranslatedTitle)
	}

	fail = false
	if translated, _, _ := Pending(context.Background(), db); translated != 1 {
		t.Fatalf("接口恢复后应重试成功，translated = %d", translated)
	}
	db.First(&got, item.ID)
	if got.TranslatedTitle != "你好世界" {
		t.Fatalf("translated_title = %q", got.TranslatedTitle)
	}
}

func TestPendingIgnoresExpiredItems(t *testing.T) {
	db := setupPendingDB(t)
	if err := settings.Set(db, true); err != nil {
		t.Fatalf("enable translation: %v", err)
	}
	seedPendingItem(t, db, "Old news", 100*time.Hour)

	calls := 0
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) { calls++ },
		func(w http.ResponseWriter, r *http.Request) { calls++ },
	)

	if translated, skipped, failed := Pending(context.Background(), db); translated+skipped+failed != 0 {
		t.Fatalf("保留期外的条目不应翻译，got %d/%d/%d", translated, skipped, failed)
	}
	if calls != 0 {
		t.Fatalf("calls = %d, want 0", calls)
	}
}

func TestBackfillProcessesMoreThanOneBatch(t *testing.T) {
	db := setupPendingDB(t)
	if err := settings.Set(db, true); err != nil {
		t.Fatalf("enable translation: %v", err)
	}
	for i := 0; i < pendingBatchSize+1; i++ {
		seedPendingItem(t, db, fmt.Sprintf("Hello world %d", i), time.Hour)
	}
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) { w.Write([]byte(`["你好世界"]`)) },
		func(w http.ResponseWriter, r *http.Request) { t.Error("不应调用 MyMemory") },
	)

	Backfill(context.Background(), db)

	var remaining int64
	db.Model(&models.Item{}).Where("translated_title = ''").Count(&remaining)
	if remaining != 0 {
		t.Fatalf("仍有 %d 条未翻译", remaining)
	}
}

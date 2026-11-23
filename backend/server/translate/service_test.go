package translate

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

func translationDB(t *testing.T) *gorm.DB {
	t.Helper()
	db, err := gorm.Open(sqlite.Open(filepath.Join(t.TempDir(), "translation.db")), &gorm.Config{Logger: logger.Default.LogMode(logger.Silent)})
	if err != nil {
		t.Fatal(err)
	}
	if err := db.AutoMigrate(&models.Item{}, &models.AppSettings{}); err != nil {
		t.Fatal(err)
	}
	sqlDB, err := db.DB()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { sqlDB.Close() })
	return db
}

func TestServiceBatchesCachesAndSkipsTitles(t *testing.T) {
	db := translationDB(t)
	calls := 0
	service := NewService(db, func(_ context.Context, title string) (string, error) {
		calls++
		return "译文：" + title, nil
	}, 72*time.Hour)
	service.delay = 0
	for i := 0; i < 40; i++ {
		if err := db.Create(&models.Item{Title: fmt.Sprintf("News %d", i)}).Error; err != nil {
			t.Fatal(err)
		}
	}
	old := time.Now().Add(-100 * time.Hour)
	for _, item := range []models.Item{
		{Title: "中文新闻"}, {Title: "AI 新闻"}, {Title: "Cached", TranslatedTitle: "已缓存"},
		{Title: "Expired", PublishedAt: &old},
	} {
		if err := db.Create(&item).Error; err != nil {
			t.Fatal(err)
		}
	}
	service.process(context.Background())
	if calls != 0 {
		t.Fatal("默认关闭时不应翻译")
	}
	if _, err := database.SaveSettings(db, true); err != nil {
		t.Fatal(err)
	}
	service.process(context.Background())
	if calls != 40 {
		t.Fatalf("跨批次应翻译 40 条，实际 %d", calls)
	}
	service.process(context.Background())
	if calls != 40 {
		t.Fatal("已缓存译文不应重复请求")
	}
	var item models.Item
	if err := db.First(&item, 1).Error; err != nil {
		t.Fatal(err)
	}
	if item.Title != "News 0" || item.TranslatedTitle != "译文：News 0" {
		t.Fatalf("原标题和译文应分开保存: %+v", item)
	}
}

func TestServiceStopsWhenDisabledAndRetriesFailures(t *testing.T) {
	db := translationDB(t)
	if _, err := database.SaveSettings(db, true); err != nil {
		t.Fatal(err)
	}
	for _, title := range []string{"First", "Second"} {
		if err := db.Create(&models.Item{Title: title}).Error; err != nil {
			t.Fatal(err)
		}
	}
	calls := 0
	service := NewService(db, func(_ context.Context, title string) (string, error) {
		calls++
		if calls == 1 {
			return "", errors.New("接口暂不可用")
		}
		return "中文：" + title, nil
	}, 72*time.Hour)
	service.delay = 0
	service.process(context.Background())
	var item models.Item
	db.First(&item, 1)
	if item.TranslatedTitle != "" {
		t.Fatal("失败不能写入译文缓存")
	}
	service.process(context.Background())
	db.First(&item, 1)
	if item.TranslatedTitle != "" {
		t.Fatal("失败标题应留到下轮扫描重试")
	}
	service.process(context.Background())
	db.First(&item, 1)
	if item.TranslatedTitle == "" {
		t.Fatal("下轮扫描应补齐之前失败的标题")
	}

	// 模拟翻译进行中关闭开关，后续条目不能再发起请求。
	if err := db.Model(&models.Item{}).Where("id > 0").Update("translated_title", "").Error; err != nil {
		t.Fatal(err)
	}
	calls = 0
	service.translator = func(_ context.Context, title string) (string, error) {
		calls++
		if _, err := database.SaveSettings(db, false); err != nil {
			t.Fatal(err)
		}
		return "中文：" + title, nil
	}
	service.process(context.Background())
	if calls != 1 {
		t.Fatalf("关闭后仍发起请求，calls=%d", calls)
	}
}

func TestServiceNotifyAndCancellation(t *testing.T) {
	db := translationDB(t)
	if _, err := database.SaveSettings(db, true); err != nil {
		t.Fatal(err)
	}
	if err := db.Create(&models.Item{Title: "News"}).Error; err != nil {
		t.Fatal(err)
	}
	started := make(chan struct{})
	service := NewService(db, func(ctx context.Context, _ string) (string, error) {
		close(started)
		<-ctx.Done()
		return "", ctx.Err()
	}, 72*time.Hour)
	// 重复唤醒必须合并且立即返回。
	for i := 0; i < 100; i++ {
		service.Notify()
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { defer close(done); service.Run(ctx) }()
	select {
	case <-started:
	case <-time.After(3 * time.Second):
		t.Fatal("后台任务未启动")
	}
	cancel()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("取消后任务未停止")
	}
}

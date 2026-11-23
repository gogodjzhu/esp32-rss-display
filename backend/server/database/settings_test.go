package database

import (
	"path/filepath"
	"testing"

	"github.com/esp32-rss-display/backend/server/models"
	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

func TestSettingsPersistAcrossConnections(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.db")
	open := func() *gorm.DB {
		t.Helper()
		db, err := gorm.Open(sqlite.Open(path), &gorm.Config{Logger: logger.Default.LogMode(logger.Silent)})
		if err != nil {
			t.Fatal(err)
		}
		if err := db.AutoMigrate(&models.AppSettings{}); err != nil {
			t.Fatal(err)
		}
		sqlDB, err := db.DB()
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { sqlDB.Close() })
		return db
	}
	closeDB := func(db *gorm.DB) {
		t.Helper()
		sqlDB, err := db.DB()
		if err != nil {
			t.Fatal(err)
		}
		if err := sqlDB.Close(); err != nil {
			t.Fatal(err)
		}
	}
	db := open()
	cfg, err := GetSettings(db)
	if err != nil || cfg.TranslationEnabled {
		t.Fatalf("初始值应关闭: %+v %v", cfg, err)
	}
	if _, err := SaveSettings(db, true); err != nil {
		t.Fatal(err)
	}
	closeDB(db)
	db = open()
	cfg, err = GetSettings(db)
	if err != nil || !cfg.TranslationEnabled {
		t.Fatalf("重新连接后丢失配置: %+v %v", cfg, err)
	}
	if _, err := SaveSettings(db, false); err != nil {
		t.Fatal(err)
	}
	cfg, err = GetSettings(db)
	if err != nil || cfg.TranslationEnabled {
		t.Fatalf("应保存 false: %+v %v", cfg, err)
	}
	var count int64
	if err := db.Model(&models.AppSettings{}).Count(&count).Error; err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("应只有一行配置，实际 %d", count)
	}
	closeDB(db)
	if _, err := GetSettings(db); err == nil {
		t.Fatal("数据库故障不能静默作为默认配置")
	}
}

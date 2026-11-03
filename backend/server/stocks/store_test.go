package stocks

// 行情配置持久化测试：首次启动用 config.yaml 种子、之后以数据库为准，
// 保存能覆盖旧配置，坏数据不会让服务起不来。

import (
	"testing"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

func setupStockDB(t *testing.T) {
	t.Helper()
	if database.GetDB() == nil {
		cfg := &config.DatabaseConfig{Driver: "sqlite", DSN: "file::memory:?cache=shared"}
		if err := database.Init(cfg); err != nil {
			t.Fatalf("init db: %v", err)
		}
	}
	database.GetDB().Where("id = ?", settingID).Delete(&models.StockSetting{})
	t.Cleanup(func() {
		database.GetDB().Where("id = ?", settingID).Delete(&models.StockSetting{})
	})
}

func TestLoadConfigSeedsOnlyFirstTime(t *testing.T) {
	setupStockDB(t)

	seed := config.StockConfig{
		Enabled:        true,
		RefreshSeconds: 60,
		Symbols:        []config.StockSymbol{{Code: "sh000001", Name: "上证"}},
	}
	cfg, err := LoadConfig(seed)
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if !cfg.Enabled || cfg.RefreshSeconds != 60 || len(cfg.Symbols) != 1 {
		t.Fatalf("unexpected seeded config: %+v", cfg)
	}

	// 数据库里已经有配置后，config.yaml 的新内容不再覆盖它
	changed := config.StockConfig{
		Enabled:        false,
		RefreshSeconds: 30,
		Symbols:        []config.StockSymbol{{Code: "usAAPL", Name: "苹果"}},
	}
	cfg, err = LoadConfig(changed)
	if err != nil {
		t.Fatalf("LoadConfig again: %v", err)
	}
	if !cfg.Enabled || len(cfg.Symbols) != 1 || cfg.Symbols[0].Code != "sh000001" {
		t.Fatalf("数据库配置被 config.yaml 覆盖了: %+v", cfg)
	}
}

func TestSaveConfigOverridesAndNormalizes(t *testing.T) {
	setupStockDB(t)

	if _, err := LoadConfig(config.StockConfig{}); err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}

	if err := SaveConfig(config.StockConfig{
		Enabled:        true,
		RefreshSeconds: 1,
		Symbols: []config.StockSymbol{
			{Code: "usTSLA", Name: " 特斯拉 "},
			{Code: "hk00700", Name: "腾讯"},
		},
	}); err != nil {
		t.Fatalf("SaveConfig: %v", err)
	}

	cfg, found, err := ReadConfig()
	if err != nil || !found {
		t.Fatalf("ReadConfig = %+v, found=%v, err=%v", cfg, found, err)
	}
	if !cfg.Enabled || cfg.RefreshSeconds != int(MinRefresh.Seconds()) {
		t.Fatalf("unexpected config: %+v", cfg)
	}
	if len(cfg.Symbols) != 2 || cfg.Symbols[0] != (config.StockSymbol{Code: "usTSLA", Name: "特斯拉"}) {
		t.Fatalf("unexpected symbols: %+v", cfg.Symbols)
	}
}

func TestReadConfigBadJSON(t *testing.T) {
	setupStockDB(t)

	row := models.StockSetting{ID: settingID, Enabled: true, RefreshSeconds: 30, Symbols: "{not json"}
	if err := database.GetDB().Create(&row).Error; err != nil {
		t.Fatalf("seed bad row: %v", err)
	}

	if _, found, err := ReadConfig(); err == nil || found {
		t.Fatalf("expected parse error, found=%v err=%v", found, err)
	}
	// 坏行读取失败时 LoadConfig 退回 config.yaml 种子，不让服务起不来
	cfg, err := LoadConfig(config.StockConfig{
		Enabled: true,
		Symbols: []config.StockSymbol{{Code: "sh000001"}},
	})
	if err == nil {
		t.Fatal("expected LoadConfig to report the corrupt row")
	}
	if !cfg.Enabled || len(cfg.Symbols) != 1 {
		t.Fatalf("expected fallback seed, got %+v", cfg)
	}
}

package stocks

// 行情配置持久化：管理页保存的配置写入数据库的 stock_settings 单行表，
// 启动时优先读库；库里没有（首次启动）才用 config.yaml 的 stocks 作种子。
// 与订阅源一致：配置文件只做首次种子，页面上的改动不会被重启覆盖。

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

// settingID 是 stock_settings 固定使用的单行主键。
const settingID = 1

// ReadConfig 读取已保存的行情配置。found 为 false 表示还没有保存过
// （首次启动且尚未落种子），此时返回的配置是零值，不读取 config.yaml。
func ReadConfig() (cfg config.StockConfig, found bool, err error) {
	db := database.GetDB()
	if db == nil {
		return config.StockConfig{}, false, nil
	}

	var row models.StockSetting
	err = db.First(&row, settingID).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		return config.StockConfig{}, false, nil
	}
	if err != nil {
		return config.StockConfig{}, false, err
	}

	cfg, err = rowConfig(row)
	if err != nil {
		return config.StockConfig{}, false, err
	}
	return NormalizeConfig(cfg), true, nil
}

// LoadConfig 读取行情配置；首次启动（库里没有）时用 seed 初始化并落库。
// 返回的配置总是规范化的，可直接交给 New/Apply 使用；出错时返回种子配置
// 供调用方兜底。
func LoadConfig(seed config.StockConfig) (config.StockConfig, error) {
	cfg, found, err := ReadConfig()
	if err != nil {
		return NormalizeConfig(seed), fmt.Errorf("读取行情配置: %w", err)
	}
	if found {
		return cfg, nil
	}

	cfg = NormalizeConfig(seed)
	if err := SaveConfig(cfg); err != nil {
		return cfg, fmt.Errorf("写入行情配置种子: %w", err)
	}
	return cfg, nil
}

// SaveConfig 覆盖保存行情配置，入库前先规范化。
func SaveConfig(cfg config.StockConfig) error {
	db := database.GetDB()
	if db == nil {
		return errors.New("数据库未初始化")
	}

	cfg = NormalizeConfig(cfg)
	raw, err := json.Marshal(cfg.Symbols)
	if err != nil {
		return fmt.Errorf("编码行情编号: %w", err)
	}

	row := models.StockSetting{
		ID:             settingID,
		Enabled:        cfg.Enabled,
		RefreshSeconds: cfg.RefreshSeconds,
		Symbols:        string(raw),
		UpdatedAt:      time.Now(),
	}
	return db.Clauses(clause.OnConflict{
		Columns:   []clause.Column{{Name: "id"}},
		DoUpdates: clause.AssignmentColumns([]string{"enabled", "refresh_seconds", "symbols", "updated_at"}),
	}).Create(&row).Error
}

// rowConfig 把数据库行还原成配置，并解析 JSON 编号列表。
func rowConfig(row models.StockSetting) (config.StockConfig, error) {
	symbols := make([]config.StockSymbol, 0)
	if raw := strings.TrimSpace(row.Symbols); raw != "" {
		if err := json.Unmarshal([]byte(raw), &symbols); err != nil {
			return config.StockConfig{}, fmt.Errorf("解析行情编号: %w", err)
		}
	}
	return config.StockConfig{
		Enabled:        row.Enabled,
		RefreshSeconds: row.RefreshSeconds,
		Symbols:        symbols,
	}, nil
}

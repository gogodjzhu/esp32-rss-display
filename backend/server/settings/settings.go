// Package settings 保存管理页可在线修改的运行期配置。
//
// 配置落在数据库单行表 app_settings，服务启动时载入内存、修改后立即生效，
// 因此管理页保存的开关不需要重启，也不会被 config.yaml 覆盖。
package settings

import (
	"errors"
	"sync/atomic"

	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

// Config 是当前支持在线修改的配置集合。新增开关时在这里加字段，
// 并在 models.AppSettings 中补列，管理页的 GET/PUT /admin/config 会一并返回。
type Config struct {
	TranslationEnabled bool
}

// current 保存配置快照；next 等高频路径只读内存，不查数据库。
var current atomic.Pointer[Config]

// Load 从数据库载入配置；首次启动时创建默认行（翻译默认关闭）。
func Load(db *gorm.DB) error {
	var row models.AppSettings
	err := db.First(&row, 1).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		row = models.AppSettings{TranslationEnabled: false}
		if err := db.Create(&row).Error; err != nil {
			return err
		}
	} else if err != nil {
		return err
	}
	current.Store(&Config{TranslationEnabled: row.TranslationEnabled})
	return nil
}

// Current 返回当前配置快照；尚未加载时按默认值处理，便于单元测试。
func Current() Config {
	if c := current.Load(); c != nil {
		return *c
	}
	return Config{}
}

// TranslationEnabled 报告标题翻译开关是否开启。
func TranslationEnabled() bool {
	return Current().TranslationEnabled
}

// Update 持久化配置并立即更新内存快照。
func Update(db *gorm.DB, cfg Config) error {
	row := models.AppSettings{ID: 1, TranslationEnabled: cfg.TranslationEnabled}
	if err := db.Save(&row).Error; err != nil {
		return err
	}
	current.Store(&cfg)
	return nil
}

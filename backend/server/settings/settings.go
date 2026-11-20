// Package settings 保存管理页可在线修改的运行期配置。
//
// 配置落在数据库单行表 app_settings；读写都直接查库，避免进程内缓存与
// 数据库两份状态不一致，也让单元测试无需重置全局变量。
package settings

import (
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

// Get 返回标题翻译开关；未初始化或查询失败时按关闭处理。
func Get(db *gorm.DB) bool {
	if db == nil {
		return false
	}
	var row models.AppSettings
	if err := db.Where("id = ?", 1).Limit(1).Find(&row).Error; err != nil {
		return false
	}
	return row.TranslationEnabled
}

// Set 持久化标题翻译开关。
func Set(db *gorm.DB, enabled bool) error {
	return db.Save(&models.AppSettings{ID: 1, TranslationEnabled: enabled}).Error
}

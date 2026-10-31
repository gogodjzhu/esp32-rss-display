package database

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/cover"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/glebarez/sqlite"
	"gorm.io/driver/mysql"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

var DB *gorm.DB

func Init(cfg *config.DatabaseConfig) error {
	var db *gorm.DB
	var err error

	if err := ensureDSNDir(cfg); err != nil {
		return err
	}

	switch cfg.Driver {
	case "mysql":
		db, err = gorm.Open(mysql.Open(cfg.DSN), &gorm.Config{
			Logger: logger.Default.LogMode(logger.Info),
		})
	case "sqlite":
		fallthrough
	default:
		db, err = gorm.Open(sqlite.Open(cfg.DSN), &gorm.Config{
			Logger: logger.Default.LogMode(logger.Info),
		})
	}

	if err != nil {
		return fmt.Errorf("failed to connect to database: %w", err)
	}

	if err := db.AutoMigrate(
		&models.Device{},
		&models.Feed{},
		&models.Item{},
		&models.ItemRating{},
		&models.DeviceItemView{},
		&models.ItemNfcOpen{},
	); err != nil {
		return fmt.Errorf("failed to migrate database: %w", err)
	}

	DB = db
	log.Println("Database connected and migrated successfully")

	if cfg.Driver == "sqlite" || cfg.Driver == "" {
		// WAL 让读写并发更稳，避免抓取写入时阻塞 next 读取
		if err := db.Exec("PRAGMA journal_mode=WAL").Error; err != nil {
			log.Printf("[db] 设置 WAL 失败: %v", err)
		}
	}

	// (feed_id, url) 唯一索引让抓取去重在并发下也成立；
	// 历史库可能已有重复行，索引建不起来时只告警，不删除任何数据。
	if err := db.Exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_items_feed_url ON items(feed_id, url)").Error; err != nil {
		log.Printf("[db] 唯一索引 idx_items_feed_url 创建失败（库中可能有重复条目）: %v", err)
	}

	return nil
}

// ensureDSNDir 保证 sqlite 的库文件目录存在，避免默认配置首次启动即失败。
func ensureDSNDir(cfg *config.DatabaseConfig) error {
	if cfg.Driver == "mysql" {
		return nil
	}
	dsn := cfg.DSN
	if dsn == "" || strings.Contains(dsn, "mode=memory") || strings.HasPrefix(dsn, "file::memory:") {
		return nil
	}
	// 去掉查询参数后取目录
	if idx := strings.IndexByte(dsn, '?'); idx != -1 {
		dsn = dsn[:idx]
	}
	dir := filepath.Dir(dsn)
	if dir == "." || dir == "" {
		return nil
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("failed to create database directory %s: %w", dir, err)
	}
	return nil
}

func GetDB() *gorm.DB {
	return DB
}

// Cleanup 清理超过 maxAge 的条目及其评分/曝光记录，再回收不再被任何条目
// 引用的图片文件。返回删除的条目数与图片文件数；出错时已完成的部分保留。
func Cleanup(maxAge time.Duration, imageDir string) (int64, int, error) {
	db := DB
	if db == nil {
		return 0, 0, fmt.Errorf("database not initialized")
	}
	cutoff := time.Now().Add(-maxAge)

	var expired []models.Item
	if err := db.Select("id", "image_path").
		Where("COALESCE(published_at, created_at) < ?", cutoff).
		Find(&expired).Error; err != nil {
		return 0, 0, fmt.Errorf("query expired items: %w", err)
	}

	var deleted int64
	if len(expired) > 0 {
		ids := make([]uint, 0, len(expired))
		for _, item := range expired {
			ids = append(ids, item.ID)
		}
		err := db.Transaction(func(tx *gorm.DB) error {
			if err := tx.Where("item_id IN ?", ids).Delete(&models.DeviceItemView{}).Error; err != nil {
				return err
			}
			if err := tx.Where("item_id IN ?", ids).Delete(&models.ItemRating{}).Error; err != nil {
				return err
			}
			if err := tx.Where("item_id IN ?", ids).Delete(&models.ItemNfcOpen{}).Error; err != nil {
				return err
			}
			res := tx.Where("id IN ?", ids).Delete(&models.Item{})
			deleted = res.RowsAffected
			return res.Error
		})
		if err != nil {
			return 0, 0, fmt.Errorf("delete expired items: %w", err)
		}
	}

	removed, err := gcImages(imageDir)
	if err != nil {
		return deleted, removed, err
	}
	coverRemoved, err := gcCovers(imageDir)
	if err != nil {
		return deleted, removed, err
	}
	if deleted > 0 || removed > 0 || coverRemoved > 0 {
		log.Printf("[cleanup] 清理过期条目 %d 条，回收图片 %d 个、封面 %d 个", deleted, removed, coverRemoved)
	}
	return deleted, removed, nil
}

// gcCovers 回收 covers 目录里不再被订阅源引用的封面：
// 自定义封面以 cover_image_path 为准，生成封面以订阅源当前名称推导。
func gcCovers(imageDir string) (int, error) {
	if imageDir == "" {
		return 0, nil
	}
	var feeds []models.Feed
	if err := DB.Select("name", "cover_image_path").Find(&feeds).Error; err != nil {
		return 0, fmt.Errorf("query feed covers: %w", err)
	}
	keep := make(map[string]struct{}, len(feeds)*2)
	for _, feed := range feeds {
		if feed.CoverImagePath != "" {
			keep[filepath.Base(feed.CoverImagePath)] = struct{}{}
		}
		keep[filepath.Base(cover.GeneratedRelPath(feed.Name))] = struct{}{}
	}
	return cover.Cleanup(imageDir, keep)
}

// gcImages 删除 imageDir 下没有任何条目引用的 jpg 文件。
// 只处理本地产物，绝不动 http(s) 形式的 image_path。
func gcImages(imageDir string) (int, error) {
	if imageDir == "" {
		return 0, nil
	}
	if _, err := os.Stat(imageDir); err != nil {
		if os.IsNotExist(err) {
			return 0, nil
		}
		return 0, err
	}

	var referenced []string
	if err := DB.Model(&models.Item{}).
		Where("image_path <> '' AND image_path NOT LIKE 'http%'").
		Pluck("image_path", &referenced).Error; err != nil {
		return 0, fmt.Errorf("query referenced images: %w", err)
	}
	keep := make(map[string]struct{}, len(referenced))
	for _, p := range referenced {
		keep[filepath.Base(p)] = struct{}{}
	}

	removed := 0
	err := filepath.WalkDir(imageDir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			// covers 目录由 gcCovers 单独管理，不能按条目引用关系清理
			if path != imageDir && d.Name() == cover.DirName {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(strings.ToLower(d.Name()), ".jpg") {
			return nil
		}
		if _, ok := keep[d.Name()]; ok {
			return nil
		}
		if err := os.Remove(path); err != nil {
			log.Printf("[cleanup] 删除孤儿图片失败 %s: %v", path, err)
			return nil
		}
		removed++
		return nil
	})
	return removed, err
}

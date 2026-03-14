package database

import (
	"fmt"
	"log"

	"github.com/esp32-rss-display/backend/server/config"
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
	); err != nil {
		return fmt.Errorf("failed to migrate database: %w", err)
	}

	DB = db
	log.Println("Database connected and migrated successfully")
	return nil
}

func GetDB() *gorm.DB {
	return DB
}

package models

import (
	"time"
)

type Device struct {
	DeviceID      string    `gorm:"primaryKey;column:device_id" json:"device_id"`
	CurrentItemID *uint     `gorm:"column:current_item_id" json:"current_item_id"`
	LastSeen      time.Time `gorm:"column:last_seen" json:"last_seen"`
	CreatedAt     time.Time `gorm:"column:created_at" json:"created_at"`
}

func (Device) TableName() string {
	return "devices"
}

type Feed struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	Name      string    `gorm:"column:name" json:"name"`
	URL       string    `gorm:"column:url" json:"url"`
	Enabled   bool      `gorm:"column:enabled" json:"enabled"`
	CreatedAt time.Time `gorm:"column:created_at" json:"created_at"`
}

func (Feed) TableName() string {
	return "feeds"
}

type Item struct {
	ID          uint       `gorm:"primaryKey" json:"id"`
	FeedID      uint       `gorm:"column:feed_id" json:"feed_id"`
	Title       string     `gorm:"column:title" json:"title"`
	URL         string     `gorm:"column:url" json:"url"`
	ImagePath   string     `gorm:"column:image_path" json:"image_path"`
	PublishedAt *time.Time `gorm:"column:published_at" json:"published_at"`
	CreatedAt   time.Time  `gorm:"column:created_at" json:"created_at"`
}

func (Item) TableName() string {
	return "items"
}

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
	// CoverImagePath 为管理页上传的自定义封面（相对 imageDir 的路径）。
	// 为空时后端按订阅源名称动态生成封面，作为条目无图时的兜底。
	CoverImagePath string `gorm:"column:cover_image_path" json:"cover_image_path"`
	// 抓取健康状态：成功时刷新 LastSuccessAt 并清空 LastError，
	// 失败时记录 LastError 与 LastErrorAt，供 /admin/status 展示。
	LastSuccessAt *time.Time `gorm:"column:last_success_at" json:"last_success_at"`
	LastErrorAt   *time.Time `gorm:"column:last_error_at" json:"last_error_at"`
	LastError     string     `gorm:"column:last_error" json:"last_error"`
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

type ItemRating struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	ItemID    uint      `gorm:"column:item_id;index" json:"item_id"`
	DeviceID  string    `gorm:"column:device_id;index" json:"device_id"`
	Rating    uint8     `gorm:"column:rating" json:"rating"`
	CreatedAt time.Time `gorm:"column:created_at" json:"created_at"`
}

func (ItemRating) TableName() string {
	return "item_ratings"
}

// DeviceItemView 记录某台设备展示过哪些条目，是 next 曝光模型的依据：
// 未看过的条目优先，全部看过之后按最久未展示轮换。
type DeviceItemView struct {
	ID          uint      `gorm:"primaryKey" json:"id"`
	DeviceID    string    `gorm:"column:device_id;uniqueIndex:uk_view_device_item,priority:1" json:"device_id"`
	ItemID      uint      `gorm:"column:item_id;uniqueIndex:uk_view_device_item,priority:2;index" json:"item_id"`
	LastShownAt time.Time `gorm:"column:last_shown_at;index" json:"last_shown_at"`
	ShowCount   int       `gorm:"column:show_count" json:"show_count"`
}

func (DeviceItemView) TableName() string {
	return "device_item_views"
}

// ItemNfcOpen 记录某台设备对某条目的 NFC 打开次数，作为标题浮层新鲜度的依据：
// 0 次绿色、1 次黄色、>=2 次红色；与条目的发布年龄取较旧一档。
type ItemNfcOpen struct {
	ID          uint      `gorm:"primaryKey" json:"id"`
	ItemID      uint      `gorm:"column:item_id;uniqueIndex:uk_nfc_open_item_device,priority:1" json:"item_id"`
	DeviceID    string    `gorm:"column:device_id;uniqueIndex:uk_nfc_open_item_device,priority:2;index" json:"device_id"`
	OpenCount   int       `gorm:"column:open_count" json:"open_count"`
	FirstOpenAt time.Time `gorm:"column:first_open_at" json:"first_open_at"`
	LastOpenAt  time.Time `gorm:"column:last_open_at;index" json:"last_open_at"`
}

func (ItemNfcOpen) TableName() string {
	return "item_nfc_opens"
}

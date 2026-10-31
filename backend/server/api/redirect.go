package api

import (
	"log"
	"net/http"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

func NFCRedirect(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("device_id")
	if deviceID == "" {
		http.Error(w, "device_id is required", http.StatusBadRequest)
		return
	}

	db := database.GetDB()

	var device models.Device
	if err := db.Where("device_id = ?", deviceID).First(&device).Error; err != nil {
		http.Error(w, "Device not found", http.StatusNotFound)
		return
	}

	if device.CurrentItemID == nil {
		http.Error(w, "No current item for device", http.StatusNotFound)
		return
	}

	var item models.Item
	if err := db.First(&item, *device.CurrentItemID).Error; err != nil {
		http.Error(w, "Item not found", http.StatusNotFound)
		return
	}

	if item.URL == "" {
		http.Error(w, "No URL for current item", http.StatusNotFound)
		return
	}

	http.Redirect(w, r, item.URL, http.StatusFound)
	// 只有跳转成功才记录；失败只记日志，绝不阻塞 302 本身。
	recordNfcOpen(db, deviceID, item.ID, time.Now())
}

// recordNfcOpen 累加一次成功的 NFC 打开；同 (item_id, device_id) 计数 +1 并
// 刷新最近打开时间，首次打开时间保留。并发安全：原子 +1，首开竞争时后到者
// 命中唯一约束走 DoUpdates（同 recordView 模式）。
func recordNfcOpen(db *gorm.DB, deviceID string, itemID uint, now time.Time) {
	err := db.Clauses(clause.OnConflict{
		Columns: []clause.Column{{Name: "item_id"}, {Name: "device_id"}},
		DoUpdates: clause.Assignments(map[string]any{
			"open_count":   gorm.Expr("open_count + 1"),
			"last_open_at": now,
		}),
	}).Create(&models.ItemNfcOpen{
		ItemID:      itemID,
		DeviceID:    deviceID,
		OpenCount:   1,
		FirstOpenAt: now,
		LastOpenAt:  now,
	}).Error
	if err != nil {
		log.Printf("[nfc] 记录打开失败 device=%s item=%d: %v", deviceID, itemID, err)
	}
}

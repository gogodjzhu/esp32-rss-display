package api

// 评分路由：接收设备端在展示期间给出的 1~5 分评价并持久化。
// 评分只写入评分表，不修改条目的展示状态，也不移动设备的 current_item_id
// （条目推进仍由 next 请求负责，评分失败不应影响轮播进度）。

import (
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/metrics"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

type ratingRequest struct {
	Rating   uint8  `json:"rating"`
	DeviceID string `json:"device_id"`
}

type ratingResponse struct {
	ItemID   uint      `json:"item_id"`
	Rating   uint8     `json:"rating"`
	DeviceID string    `json:"device_id"`
	Created  time.Time `json:"created"`
}

func RateItem(w http.ResponseWriter, r *http.Request) {
	itemID, err := strconv.ParseUint(r.PathValue("item_id"), 10, 32)
	if err != nil || itemID == 0 {
		http.Error(w, "invalid item_id", http.StatusBadRequest)
		return
	}

	var req ratingRequest
	d := json.NewDecoder(r.Body)
	d.DisallowUnknownFields()
	if err := d.Decode(&req); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}
	if req.Rating < 1 || req.Rating > 5 {
		http.Error(w, "rating must be between 1 and 5", http.StatusBadRequest)
		return
	}
	if req.DeviceID == "" {
		http.Error(w, "device_id is required", http.StatusBadRequest)
		return
	}

	db := database.GetDB()
	var item models.Item
	if err := db.First(&item, itemID).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			http.Error(w, "item not found", http.StatusNotFound)
		} else {
			http.Error(w, "database error", http.StatusInternalServerError)
		}
		return
	}

	// 同一设备对同一条目只保留最新一次评分，便于重复上报时收敛结果。
	var rating models.ItemRating
	err = db.Where("item_id = ? AND device_id = ?", uint(itemID), req.DeviceID).First(&rating).Error
	switch {
	case err == nil:
		rating.Rating = req.Rating
		rating.CreatedAt = time.Now()
		if err := db.Save(&rating).Error; err != nil {
			http.Error(w, "failed to save rating", http.StatusInternalServerError)
			return
		}
	case errors.Is(err, gorm.ErrRecordNotFound):
		rating = models.ItemRating{
			ItemID:    uint(itemID),
			DeviceID:  req.DeviceID,
			Rating:    req.Rating,
			CreatedAt: time.Now(),
		}
		if err := db.Create(&rating).Error; err != nil {
			http.Error(w, "failed to save rating", http.StatusInternalServerError)
			return
		}
	default:
		http.Error(w, "database error", http.StatusInternalServerError)
		return
	}

	metrics.DeviceRatingTotal.Add(1)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(ratingResponse{
		ItemID:   rating.ItemID,
		Rating:   rating.Rating,
		DeviceID: rating.DeviceID,
		Created:  rating.CreatedAt,
	})
}
package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/metrics"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/stocks"
)

// StockProvider 为行情窗口来源，由 main 在启动时注入；测试可替换为假实现。
// 未注入时 next 响应不带 stocks 字段。
type StockProvider interface {
	Window(deviceID string, size int) []stocks.Quote
}

var StockQuotes StockProvider

type NextItemResponse struct {
	// ItemID 为条目的稳定整型标识，设备端评分时原样回传。
	ItemID   uint   `json:"item_id"`
	Title    string `json:"title"`
	ImageURL string `json:"image_url"`
	Source   string `json:"source"`
	// PublishedAt 为条目发布时间的本地时区展示字符串（YYYY-MM-DD HH:MM），
	// 供设备端在标题浮层首行显示；无发布时间时回退为入库时间。
	PublishedAt string `json:"published_at"`
	// Stocks 为按设备轮换的行情窗口；没有可用行情时省略该字段。
	Stocks []stocks.Quote `json:"stocks,omitempty"`
}

func GetNextItem(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("device_id")
	if deviceID == "" {
		http.Error(w, "device_id is required", http.StatusBadRequest)
		return
	}

	db := database.GetDB()
	metrics.DeviceNextRequestTotal.Add(1)

	var device models.Device
	if err := db.Where("device_id = ?", deviceID).First(&device).Error; err != nil {
		device = models.Device{
			DeviceID:  deviceID,
			CreatedAt: time.Now(),
		}
		if err := db.Create(&device).Error; err != nil {
			http.Error(w, "Failed to register device", http.StatusInternalServerError)
			return
		}
		metrics.DeviceRegisteredTotal.Add(1)
	}

	var item models.Item
	var feed models.Feed

	if device.CurrentItemID != nil {
		if err := db.Where("id > ?", *device.CurrentItemID).Order("id ASC").First(&item).Error; err != nil {
			if err := db.Order("id ASC").First(&item).Error; err != nil {
				http.Error(w, "No items available", http.StatusNotFound)
				return
			}
		}
	} else {
		if err := db.Order("id ASC").First(&item).Error; err != nil {
			http.Error(w, "No items available", http.StatusNotFound)
			return
		}
	}

	if err := db.First(&feed, item.FeedID).Error; err != nil {
		http.Error(w, "Feed not found", http.StatusInternalServerError)
		return
	}

	imageURL := ""
	if item.ImagePath != "" {
		scheme := "http"
		if r.TLS != nil {
			scheme = "https"
		}
		imageURL = fmt.Sprintf("%s://%s/image/%d.jpg", scheme, r.Host, item.ID)
	}

	// 发布时间的展示字符串：优先使用 feed 的发布时间，缺失时回退为入库时间
	pubTime := item.CreatedAt
	if item.PublishedAt != nil {
		pubTime = *item.PublishedAt
	}

	device.CurrentItemID = &item.ID
	device.LastSeen = time.Now()
	db.Save(&device)

	resp := NextItemResponse{
		ItemID:      item.ID,
		Title:       item.Title,
		ImageURL:    imageURL,
		Source:      feed.Name,
		PublishedAt: pubTime.Local().Format("2006-01-02 15:04"),
	}
	// 行情为可选装饰字段，缺失或拉取失败不影响内容本身
	if StockQuotes != nil {
		resp.Stocks = StockQuotes.Window(deviceID, stockWindowSize(r.URL.Query().Get("size")))
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

// stockWindowSize 解析可选的 size 查询参数：缺失或非法时回退默认值，
// 过大时截断，避免异常请求撑大 next 响应。
func stockWindowSize(raw string) int {
	if raw == "" {
		return stocks.DefaultWindow
	}
	size, err := strconv.Atoi(raw)
	if err != nil {
		return stocks.DefaultWindow
	}
	return stocks.ClampWindow(size)
}

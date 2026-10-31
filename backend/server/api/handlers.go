package api

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strconv"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/metrics"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/stocks"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
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
	// AgeSec 为条目的已发布秒数（服务端相对 now 计算，设备没有时钟）；
	// 设备据此分档显示新鲜度颜色与相对时间文案。
	AgeSec int64 `json:"age_sec"`
	// OpenCount 为本设备对该条目的 NFC 打开次数，0 表示从未打开。
	OpenCount int `json:"open_count"`
	// Stocks 为按设备轮换的行情窗口；没有可用行情时省略该字段。
	Stocks []stocks.Quote `json:"stocks,omitempty"`
}

// 曝光模型：用 device_item_views 取代旧的“id 递增”游标。
//  1. 优先返回本设备未看过的条目，按发布时间倒序（新内容先进）；
//  2. 全部看过之后，返回最久未展示的条目；
//  3. 超过 max_age 的条目不再参与轮换。
//
// 排序键统一用 COALESCE(published_at, created_at)：不少源不提供发布时间，
// 若直接按 published_at 过滤会让这些条目永远轮不到。
const itemOrderExpr = "COALESCE(i.published_at, i.created_at)"

func pickItem(db *gorm.DB, deviceID string, now time.Time, cutoff time.Duration) (*models.Item, error) {
	// cutoff <= 0 表示不设下限（兜底取全量历史）
	lower := now.Add(-100 * 365 * 24 * time.Hour)
	if cutoff > 0 {
		lower = now.Add(-cutoff)
	}

	var item models.Item
	if err := db.Raw(`
		SELECT i.* FROM items i
		WHERE COALESCE(i.published_at, i.created_at) <= ?
		  AND COALESCE(i.published_at, i.created_at) > ?
		  AND NOT EXISTS (
		      SELECT 1 FROM device_item_views v
		      WHERE v.item_id = i.id AND v.device_id = ?
		  )
		ORDER BY `+itemOrderExpr+` DESC, i.id DESC
		LIMIT 1
	`, now, lower, deviceID).Scan(&item).Error; err != nil {
		return nil, err
	}
	if item.ID != 0 {
		return &item, nil
	}

	// 全部看过：取最久未展示的一条
	item = models.Item{}
	if err := db.Raw(`
		SELECT i.* FROM items i
		INNER JOIN device_item_views v ON v.item_id = i.id AND v.device_id = ?
		WHERE COALESCE(i.published_at, i.created_at) <= ?
		  AND COALESCE(i.published_at, i.created_at) > ?
		ORDER BY v.last_shown_at ASC
		LIMIT 1
	`, deviceID, now, lower).Scan(&item).Error; err != nil {
		return nil, err
	}
	if item.ID == 0 {
		return nil, gorm.ErrRecordNotFound
	}
	return &item, nil
}

// recordView 记录一次展示；设备重复看同一条时累加次数并刷新时间。
func recordView(db *gorm.DB, deviceID string, itemID uint, now time.Time) {
	err := db.Clauses(clause.OnConflict{
		Columns: []clause.Column{{Name: "device_id"}, {Name: "item_id"}},
		DoUpdates: clause.Assignments(map[string]any{
			"last_shown_at": now,
			"show_count":    gorm.Expr("show_count + 1"),
		}),
	}).Create(&models.DeviceItemView{
		DeviceID:    deviceID,
		ItemID:      itemID,
		LastShownAt: now,
		ShowCount:   1,
	}).Error
	if err != nil {
		log.Printf("[exposure] 记录展示失败 device=%s item=%d: %v", deviceID, itemID, err)
	}
}

// itemPubTime 返回条目的发布时间（缺失回退入库时间）；未来时间钳制为 now，
// 保证由此计算的 age_sec 永远 >= 0。
func itemPubTime(item *models.Item, now time.Time) time.Time {
	pubTime := item.CreatedAt
	if item.PublishedAt != nil {
		pubTime = *item.PublishedAt
	}
	if pubTime.After(now) {
		pubTime = now
	}
	return pubTime
}

func GetNextItem(w http.ResponseWriter, r *http.Request) {
	deviceID := r.PathValue("device_id")
	if deviceID == "" {
		http.Error(w, "device_id is required", http.StatusBadRequest)
		return
	}

	db := database.GetDB()
	metrics.DeviceNextRequestTotal.Add(1)
	now := time.Now()

	var device models.Device
	if err := db.Where("device_id = ?", deviceID).First(&device).Error; err != nil {
		device = models.Device{
			DeviceID:  deviceID,
			CreatedAt: now,
			LastSeen:  now,
		}
		if err := db.Create(&device).Error; err != nil {
			http.Error(w, "Failed to register device", http.StatusInternalServerError)
			return
		}
		metrics.DeviceRegisteredTotal.Add(1)
	}

	maxAge := config.MaxAge()

	item, err := pickItem(db, deviceID, now, maxAge)
	if err != nil {
		// 兜底：没有 max_age 内的内容时（如停机很久），放宽到全量历史，
		// 宁可播旧内容也不要让设备停在错误页。
		item, err = pickItem(db, deviceID, now, 0)
		if err != nil {
			http.Error(w, "No items available", http.StatusNotFound)
			return
		}
	}

	recordView(db, deviceID, item.ID, now)

	var feed models.Feed
	if err := db.First(&feed, item.FeedID).Error; err != nil {
		http.Error(w, "Feed not found", http.StatusInternalServerError)
		return
	}

	// 条目没有图片时由 /image/{id}.jpg 回退到订阅源封面，
	// 因此这里始终返回图片地址，设备端不需要再处理“无图”分支。
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	imageURL := fmt.Sprintf("%s://%s/image/%d.jpg", scheme, r.Host, item.ID)

	// 发布时间的展示字符串：优先使用 feed 的发布时间，缺失时回退为入库时间
	pubTime := itemPubTime(item, now)

	// NFC 打开次数：缺失行按 0 处理；查询失败降级为 0，不影响本次展示
	var openCount int
	if err := db.Model(&models.ItemNfcOpen{}).
		Where("item_id = ? AND device_id = ?", item.ID, deviceID).
		Pluck("open_count", &openCount).Error; err != nil {
		log.Printf("[next] 查询 NFC 打开次数失败 device=%s item=%d: %v", deviceID, item.ID, err)
	}

	// 更新设备游标（NFC 跳转仍依赖 current_item_id）
	device.CurrentItemID = &item.ID
	device.LastSeen = now
	if err := db.Save(&device).Error; err != nil {
		log.Printf("[next] 更新设备游标失败 device=%s: %v", deviceID, err)
	}

	resp := NextItemResponse{
		ItemID:      item.ID,
		Title:       item.Title,
		ImageURL:    imageURL,
		Source:      feed.Name,
		PublishedAt: pubTime.Local().Format("2006-01-02 15:04"),
		AgeSec:      int64(now.Sub(pubTime).Seconds()),
		OpenCount:   openCount,
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

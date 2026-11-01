package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/mmcdole/gofeed"
	"gorm.io/gorm"
)

// 管理页只读接口：运行状态聚合、单源条目列表、新增前的订阅源预览。
// 都挂在 /admin 前缀下，由前置反代的 basic_auth 保护，不对外暴露。

// AdminStatus 汇总运行状态：每个源的健康度、条目与设备规模、图片目录占用。
// 纯只读接口，供管理页与 curl 排障使用。
type AdminStatus struct {
	Time         time.Time    `json:"time"`
	MaxAgeH      int          `json:"max_age_hours"`
	FetchMinutes int          `json:"fetch_interval_minutes"`
	LastFetchAt  *time.Time   `json:"last_fetch_at"`
	Feeds        []FeedStatus `json:"feeds"`
	Items        ItemStats    `json:"items"`
	Devices      DeviceStats  `json:"devices"`
	Images       ImageStats   `json:"images"`
	Database     DBStats      `json:"database"`
	Ratings      RatingStats  `json:"ratings"`
	ItemsByDay   []DailyCount `json:"items_by_day"`
}

type FeedStatus struct {
	ID          uint    `json:"id"`
	Name        string  `json:"name"`
	URL         string  `json:"url"`
	Enabled     bool    `json:"enabled"`
	CoverPath   string  `json:"cover_image_path"`
	ItemsToday  int64   `json:"items_today"`
	ItemsTotal  int64   `json:"items_total"`
	ShownTotal  int64   `json:"shown_total"`
	NfcOpens    int64   `json:"nfc_opens"`
	RatingCount int64   `json:"rating_count"`
	RatingAvg   float64 `json:"rating_avg"`
	// LastSuccessAt 为最近一次抓取成功时间。
	LastSuccessAt *time.Time `json:"last_success_at"`
	LastErrorAt   *time.Time `json:"last_error_at"`
	LastError     string     `json:"last_error,omitempty"`
	// LastNewItemAt 为最近一次抓到新条目的入库时间。
	LastNewItemAt *time.Time `json:"last_new_item_at"`
}

type ItemStats struct {
	Total   int64 `json:"total"`
	Last24h int64 `json:"last_24h"`
}

type DeviceStats struct {
	Total    int64 `json:"total"`
	Active1h int64 `json:"active_1h"`
}

type ImageStats struct {
	Files int   `json:"files"`
	Bytes int64 `json:"bytes"`
}

type DBStats struct {
	File  string `json:"file,omitempty"`
	Bytes int64  `json:"bytes"`
}

// RatingStats 为全局评分汇总，Dist 固定 5 个桶对应 1~5 星。
type RatingStats struct {
	Count int64    `json:"count"`
	Avg   float64  `json:"avg"`
	Dist  [5]int64 `json:"dist"`
}

// DailyCount 是「近 N 天每日新增条目」图表的一个点，Date 为 YYYY-MM-DD。
type DailyCount struct {
	Date  string `json:"date"`
	Count int64  `json:"count"`
}

// feedIntAgg 承接按 feed 分组的计数聚合结果。
type feedIntAgg struct {
	FeedID uint  `gorm:"column:feed_id"`
	Total  int64 `gorm:"column:total"`
}

// dailyChartDays 为概览图表的回溯天数。
const dailyChartDays = 14

func AdminStatusHandler(w http.ResponseWriter, r *http.Request) {
	db := database.GetDB()
	now := time.Now()
	dayStart := time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, now.Location())

	var feeds []models.Feed
	if err := db.Order("id ASC").Find(&feeds).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	status := AdminStatus{
		Time:         now,
		MaxAgeH:      int(config.MaxAge().Hours()),
		FetchMinutes: fetchIntervalMinutes(),
		Feeds:        make([]FeedStatus, 0, len(feeds)),
	}

	// 先把各维度的分组统计一次性取出来，避免每个源都发一轮查询。
	byFeed := map[uint]*FeedStatus{}
	for _, f := range feeds {
		fs := FeedStatus{
			ID:            f.ID,
			Name:          f.Name,
			URL:           f.URL,
			Enabled:       f.Enabled,
			CoverPath:     f.CoverImagePath,
			LastSuccessAt: f.LastSuccessAt,
			LastErrorAt:   f.LastErrorAt,
			LastError:     f.LastError,
		}
		status.Feeds = append(status.Feeds, fs)
	}
	for i := range status.Feeds {
		byFeed[status.Feeds[i].ID] = &status.Feeds[i]
	}

	// 每个源的条目总数与今日新增
	var itemAggs []struct {
		FeedID uint  `gorm:"column:feed_id"`
		Total  int64 `gorm:"column:total"`
		Today  int64 `gorm:"column:today"`
	}
	db.Raw(`
		SELECT feed_id,
		       COUNT(*) AS total,
		       SUM(CASE WHEN COALESCE(published_at, created_at) >= ? THEN 1 ELSE 0 END) AS today
		FROM items GROUP BY feed_id
	`, dayStart).Scan(&itemAggs)
	for _, row := range itemAggs {
		if fs := byFeed[row.FeedID]; fs != nil {
			fs.ItemsTotal = row.Total
			fs.ItemsToday = row.Today
		}
	}

	// 最近有新内容的入库时间：用模型查询承接 MAX(created_at)，
	// 让 GORM 按字段类型解析时间字符串（raw Scan 到 *time.Time 会失败）。
	if len(feeds) > 0 {
		ids := make([]uint, 0, len(feeds))
		for _, f := range feeds {
			ids = append(ids, f.ID)
		}
		var lastItems []models.Item
		db.Model(&models.Item{}).
			Select("feed_id", "created_at").
			Where("created_at IN (?)",
				db.Model(&models.Item{}).Select("MAX(created_at)").Where("feed_id IN ?", ids).Group("feed_id")).
			Find(&lastItems)
		for _, row := range lastItems {
			fs := byFeed[row.FeedID]
			if fs == nil || row.CreatedAt.IsZero() {
				continue
			}
			t := row.CreatedAt
			// 同一时间戳可能被多个源共享，取较大值保证结果与行顺序无关
			if fs.LastNewItemAt == nil || t.After(*fs.LastNewItemAt) {
				fs.LastNewItemAt = &t
			}
		}
	}

	// 展示次数（next 曝光）
	var shownAggs []feedIntAgg
	db.Raw(`
		SELECT i.feed_id, COALESCE(SUM(v.show_count), 0) AS total
		FROM device_item_views v INNER JOIN items i ON i.id = v.item_id
		GROUP BY i.feed_id
	`).Scan(&shownAggs)
	for _, row := range shownAggs {
		if fs := byFeed[row.FeedID]; fs != nil {
			fs.ShownTotal = row.Total
		}
	}

	// NFC 打开次数
	var nfcAggs []feedIntAgg
	db.Raw(`
		SELECT i.feed_id, COALESCE(SUM(o.open_count), 0) AS total
		FROM item_nfc_opens o INNER JOIN items i ON i.id = o.item_id
		GROUP BY i.feed_id
	`).Scan(&nfcAggs)
	for _, row := range nfcAggs {
		if fs := byFeed[row.FeedID]; fs != nil {
			fs.NfcOpens = row.Total
		}
	}

	// 评分次数与均分
	var ratingAggs []struct {
		FeedID uint    `gorm:"column:feed_id"`
		Count  int64   `gorm:"column:cnt"`
		Avg    float64 `gorm:"column:avg"`
	}
	db.Raw(`
		SELECT i.feed_id, COUNT(*) AS cnt, AVG(r.rating) AS avg
		FROM item_ratings r INNER JOIN items i ON i.id = r.item_id
		GROUP BY i.feed_id
	`).Scan(&ratingAggs)
	for _, row := range ratingAggs {
		if fs := byFeed[row.FeedID]; fs != nil {
			fs.RatingCount = row.Count
			fs.RatingAvg = row.Avg
		}
	}

	db.Model(&models.Item{}).Count(&status.Items.Total)
	db.Model(&models.Item{}).
		Where("COALESCE(published_at, created_at) >= ?", now.Add(-24*time.Hour)).
		Count(&status.Items.Last24h)

	db.Model(&models.Device{}).Count(&status.Devices.Total)
	db.Model(&models.Device{}).Where("last_seen >= ?", now.Add(-time.Hour)).Count(&status.Devices.Active1h)

	// 全局最近一次抓取成功时间 = 各源最近成功的最大值
	for _, fs := range status.Feeds {
		if fs.LastSuccessAt != nil && (status.LastFetchAt == nil || fs.LastSuccessAt.After(*status.LastFetchAt)) {
			status.LastFetchAt = fs.LastSuccessAt
		}
	}

	status.Ratings = loadRatingStats(db)
	status.ItemsByDay = loadDailyCounts(db, now, dailyChartDays)

	if dir := ImageDir(); dir != "" {
		status.Images = cachedScanImages(dir)
	}
	if config.AppConfig != nil && config.AppConfig.Database.Driver != "mysql" {
		dsn := config.AppConfig.Database.DSN
		if idx := strings.IndexByte(dsn, '?'); idx != -1 {
			dsn = dsn[:idx]
		}
		if info, err := os.Stat(dsn); err == nil && !info.IsDir() {
			status.Database = DBStats{File: filepath.Base(dsn), Bytes: info.Size()}
		}
	}

	writeJSON(w, http.StatusOK, status)
}

// 图片目录扫描有 TTL 缓存：管理页每 30 秒自动刷新，不必每次都遍历整个目录。
const imageScanTTL = 30 * time.Second

var (
	imageScanMu    sync.Mutex
	imageScanAt    time.Time
	imageScanDir   string
	imageScanValue ImageStats
)

func cachedScanImages(dir string) ImageStats {
	imageScanMu.Lock()
	defer imageScanMu.Unlock()
	if dir == imageScanDir && time.Since(imageScanAt) < imageScanTTL {
		return imageScanValue
	}
	imageScanValue = scanImages(dir)
	imageScanDir = dir
	imageScanAt = time.Now()
	return imageScanValue
}

// loadRatingStats 汇总评分总数、均分与 1~5 星分布。
func loadRatingStats(db *gorm.DB) RatingStats {
	var stats RatingStats
	var rows []struct {
		Rating uint8 `gorm:"column:rating"`
		Count  int64 `gorm:"column:cnt"`
	}
	if err := db.Raw(`SELECT rating, COUNT(*) AS cnt FROM item_ratings GROUP BY rating`).
		Scan(&rows).Error; err != nil {
		return stats
	}
	var sum int64
	for _, row := range rows {
		stats.Count += row.Count
		sum += int64(row.Rating) * row.Count
		if row.Rating >= 1 && row.Rating <= 5 {
			stats.Dist[row.Rating-1] = row.Count
		}
	}
	if stats.Count > 0 {
		stats.Avg = float64(sum) / float64(stats.Count)
	}
	return stats
}

// loadDailyCounts 按天统计近 days 天的新增条目。
// 用 Go 侧分桶而不是 SQL 日期函数，兼容 SQLite 与 MySQL 的方言差异。
func loadDailyCounts(db *gorm.DB, now time.Time, days int) []DailyCount {
	local := now
	start := time.Date(local.Year(), local.Month(), local.Day(), 0, 0, 0, 0, local.Location()).
		AddDate(0, 0, -(days - 1))

	buckets := make(map[string]int64, days)
	out := make([]DailyCount, 0, days)
	for i := 0; i < days; i++ {
		key := start.AddDate(0, 0, i).Format("2006-01-02")
		buckets[key] = 0
		out = append(out, DailyCount{Date: key})
	}

	var items []models.Item
	if err := db.Model(&models.Item{}).
		Select("published_at", "created_at").
		Where("COALESCE(published_at, created_at) >= ?", start).
		Find(&items).Error; err != nil {
		return out
	}
	for _, item := range items {
		when := item.CreatedAt
		if item.PublishedAt != nil {
			when = *item.PublishedAt
		}
		key := when.Local().Format("2006-01-02")
		if _, ok := buckets[key]; ok {
			buckets[key]++
		}
	}
	for i := range out {
		out[i].Count = buckets[out[i].Date]
	}
	return out
}

// fetchIntervalMinutes 返回抓取间隔（分钟），配置未加载时回退默认值。
func fetchIntervalMinutes() int {
	if config.AppConfig == nil || config.AppConfig.RSS.FetchIntervalMinutes <= 0 {
		return 15
	}
	return config.AppConfig.RSS.FetchIntervalMinutes
}

// ---------------------------------------------------------------------------
// 单个订阅源的条目列表（管理页抽屉）
// ---------------------------------------------------------------------------
type adminItem struct {
	ID          uint       `json:"id"`
	Title       string     `json:"title"`
	URL         string     `json:"url"`
	ImageURL    string     `json:"image_url"`
	PublishedAt *time.Time `json:"published_at"`
	CreatedAt   time.Time  `json:"created_at"`
	ShowCount   int64      `json:"show_count"`
	NfcOpens    int64      `json:"nfc_opens"`
	RatingCount int64      `json:"rating_count"`
	RatingAvg   float64    `json:"rating_avg"`
}

type adminItemList struct {
	Items   []adminItem `json:"items"`
	Total   int64       `json:"total"`
	HasMore bool        `json:"has_more"`
}

const (
	adminItemsDefaultLimit = 50
	adminItemsMaxLimit     = 200
)

// AdminFeedItems 返回某个订阅源最近的条目，供管理页查看该源的全部内容。
// 按 COALESCE(published_at, created_at) 倒序，与 next 的排序口径一致。
func AdminFeedItems(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "非法的 feed id")
		return
	}

	db := database.GetDB()
	var feed models.Feed
	if err := db.First(&feed, id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			writeError(w, http.StatusNotFound, "订阅源不存在")
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	limit := adminItemsDefaultLimit
	if raw := r.URL.Query().Get("limit"); raw != "" {
		if v, err := strconv.Atoi(raw); err == nil && v > 0 {
			limit = min(v, adminItemsMaxLimit)
		}
	}
	offset := 0
	if raw := r.URL.Query().Get("offset"); raw != "" {
		if v, err := strconv.Atoi(raw); err == nil && v > 0 {
			offset = v
		}
	}

	var items []models.Item
	if err := db.Where("feed_id = ?", id).
		Order("COALESCE(published_at, created_at) DESC, id DESC").
		Limit(limit).Offset(offset).
		Find(&items).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	var total int64
	db.Model(&models.Item{}).Where("feed_id = ?", id).Count(&total)

	list := adminItemList{
		Items: make([]adminItem, 0, len(items)),
		Total: total,
	}
	index := make(map[uint]int, len(items))
	ids := make([]uint, 0, len(items))
	for i, item := range items {
		list.Items = append(list.Items, adminItem{
			ID:          item.ID,
			Title:       item.Title,
			URL:         item.URL,
			ImageURL:    "/image/" + strconv.FormatUint(uint64(item.ID), 10) + ".jpg",
			PublishedAt: item.PublishedAt,
			CreatedAt:   item.CreatedAt,
		})
		index[item.ID] = i
		ids = append(ids, item.ID)
	}

	if len(ids) > 0 {
		var shown []struct {
			ItemID uint  `gorm:"column:item_id"`
			Total  int64 `gorm:"column:total"`
		}
		db.Raw(`SELECT item_id, COALESCE(SUM(show_count),0) AS total FROM device_item_views WHERE item_id IN ? GROUP BY item_id`, ids).Scan(&shown)
		for _, row := range shown {
			if i, ok := index[row.ItemID]; ok {
				list.Items[i].ShowCount = row.Total
			}
		}

		var nfc []struct {
			ItemID uint  `gorm:"column:item_id"`
			Total  int64 `gorm:"column:total"`
		}
		db.Raw(`SELECT item_id, COALESCE(SUM(open_count),0) AS total FROM item_nfc_opens WHERE item_id IN ? GROUP BY item_id`, ids).Scan(&nfc)
		for _, row := range nfc {
			if i, ok := index[row.ItemID]; ok {
				list.Items[i].NfcOpens = row.Total
			}
		}

		var ratings []struct {
			ItemID uint    `gorm:"column:item_id"`
			Count  int64   `gorm:"column:cnt"`
			Avg    float64 `gorm:"column:avg"`
		}
		db.Raw(`SELECT item_id, COUNT(*) AS cnt, AVG(rating) AS avg FROM item_ratings WHERE item_id IN ? GROUP BY item_id`, ids).Scan(&ratings)
		for _, row := range ratings {
			if i, ok := index[row.ItemID]; ok {
				list.Items[i].RatingCount = row.Count
				list.Items[i].RatingAvg = row.Avg
			}
		}
	}

	list.HasMore = int64(offset+len(items)) < total
	writeJSON(w, http.StatusOK, list)
}

// ---------------------------------------------------------------------------
// 新增订阅源前的预览与校验
// ---------------------------------------------------------------------------

// previewHTTPTimeout 是抓取订阅源做预览的超时时间：
// 新增源是同步操作，不能让一个慢源卡死管理页。
const (
	previewHTTPTimeout = 10 * time.Second
	previewMaxItems    = 10
	previewMaxDesc     = 500
)

type previewItem struct {
	Title       string     `json:"title"`
	URL         string     `json:"url"`
	PublishedAt *time.Time `json:"published_at"`
}

type feedPreview struct {
	URL         string        `json:"url"`
	Title       string        `json:"title"`
	Description string        `json:"description"`
	Items       []previewItem `json:"items"`
	ItemCount   int           `json:"item_count"`
}

// fetchFeedPreview 抓取并解析订阅源，返回源标题、描述与若干样例条目。
// 解析失败即认为该 URL 不是可用的 RSS/Atom 订阅源。
func fetchFeedPreview(rawURL string) (*feedPreview, error) {
	ctx, cancel := context.WithTimeout(context.Background(), previewHTTPTimeout)
	defer cancel()

	parser := gofeed.NewParser()
	parser.Client = &http.Client{Timeout: previewHTTPTimeout}
	parsed, err := parser.ParseURLWithContext(rawURL, ctx)
	if err != nil {
		return nil, err
	}
	if parsed == nil {
		return nil, errors.New("订阅源内容为空")
	}

	preview := &feedPreview{
		URL:         rawURL,
		Title:       strings.TrimSpace(parsed.Title),
		Description: truncateRunes(strings.TrimSpace(parsed.Description), previewMaxDesc),
		ItemCount:   len(parsed.Items),
		Items:       make([]previewItem, 0, previewMaxItems),
	}
	if preview.Title == "" {
		preview.Title = fallbackFeedTitle(rawURL)
	}
	for _, item := range parsed.Items {
		if len(preview.Items) >= previewMaxItems {
			break
		}
		title := strings.TrimSpace(item.Title)
		if title == "" {
			title = strings.TrimSpace(item.Link)
		}
		preview.Items = append(preview.Items, previewItem{
			Title:       truncateRunes(title, 200),
			URL:         item.Link,
			PublishedAt: item.PublishedParsed,
		})
	}
	return preview, nil
}

// fallbackFeedTitle 在源没有标题时用主机名兜底，避免预览里出现空白。
func fallbackFeedTitle(rawURL string) string {
	if idx := strings.Index(rawURL, "://"); idx != -1 {
		rest := rawURL[idx+3:]
		if end := strings.IndexAny(rest, "/?#"); end != -1 {
			return rest[:end]
		}
		return rest
	}
	return rawURL
}

func truncateRunes(s string, n int) string {
	runes := []rune(s)
	if len(runes) <= n {
		return s
	}
	return string(runes[:n]) + "…"
}

// AdminPreviewFeed 在新增订阅源前做一次真实抓取，返回解析结果。
// URL 已存在时返回 409，与创建接口保持一致。
func AdminPreviewFeed(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<16))
	if err != nil {
		writeError(w, http.StatusBadRequest, "读取请求失败: "+err.Error())
		return
	}
	var req struct {
		URL string `json:"url"`
	}
	dec := json.NewDecoder(strings.NewReader(string(body)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "请求体格式错误: "+err.Error())
		return
	}
	raw := strings.TrimSpace(req.URL)
	if raw == "" {
		writeError(w, http.StatusBadRequest, "url 必填")
		return
	}
	if err := validateFeedURL(raw); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	var existing models.Feed
	if err := database.GetDB().Where("url = ?", raw).First(&existing).Error; err == nil {
		writeError(w, http.StatusConflict, "该 url 的订阅源已存在")
		return
	}

	preview, err := fetchFeedPreview(raw)
	if err != nil {
		writeError(w, http.StatusUnprocessableEntity, "无法解析为有效的订阅源: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, preview)
}

// validateFeedSource 在创建订阅源时同步抓取一次，确认 URL 确实是可用的订阅源。
// 返回的错误会原样展示给管理页。
func validateFeedSource(rawURL string) error {
	if _, err := fetchFeedPreview(rawURL); err != nil {
		return errors.New("无法解析为有效的订阅源: " + err.Error())
	}
	return nil
}

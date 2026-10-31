package image

import (
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/esp32-rss-display/backend/server/cover"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

const (
	itemImageMaxAge = 604800 // 条目图片按内容哈希落盘，可以长缓存
	coverMaxAge     = 3600   // 封面可能被管理页替换，用较短缓存
)

type Handler struct {
	imageDir string
}

func New(imageDir string) *Handler {
	return &Handler{imageDir: imageDir}
}

// ServeHTTP 返回条目的图片。条目自身没有图片（或文件已丢失）时，
// 依次回退到订阅源自定义封面、按订阅源名称生成的封面，保证永远有图可显示。
func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	id, err := parseImageID(r.URL.Path)
	if err != nil {
		http.Error(w, "Invalid item ID", http.StatusBadRequest)
		return
	}

	db := database.GetDB()
	var item models.Item
	if err := db.Select("id", "feed_id", "image_path").First(&item, id).Error; err != nil {
		http.NotFound(w, r)
		return
	}

	if path, ok := h.resolve(item.ImagePath); ok {
		h.serveFile(w, r, path, itemImageMaxAge)
		return
	}

	if path, ok := h.feedCover(item.FeedID); ok {
		h.serveFile(w, r, path, coverMaxAge)
		return
	}

	http.Error(w, "Image unavailable", http.StatusInternalServerError)
}

// FeedHandler 返回订阅源当前封面，供管理页预览。
// 复用 Handler 的路径解析与文件服务逻辑。
type FeedHandler struct {
	*Handler
}

func NewFeed(imageDir string) *FeedHandler {
	return &FeedHandler{Handler: New(imageDir)}
}

func (h *FeedHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	id, err := parseImageID(r.URL.Path)
	if err != nil {
		http.Error(w, "Invalid feed ID", http.StatusBadRequest)
		return
	}

	var feed models.Feed
	if err := database.GetDB().Select("id", "name", "cover_image_path").First(&feed, id).Error; err != nil {
		http.NotFound(w, r)
		return
	}

	path, ok := h.coverPath(&feed)
	if !ok {
		http.Error(w, "Image unavailable", http.StatusInternalServerError)
		return
	}
	h.serveFile(w, r, path, coverMaxAge)
}

func (h *Handler) feedCover(feedID uint) (string, bool) {
	var feed models.Feed
	if err := database.GetDB().Select("id", "name", "cover_image_path").First(&feed, feedID).Error; err != nil {
		// 数据不一致时仍给一张通用封面，不让设备缺图
		return h.generatedCover("RSS")
	}
	return h.coverPath(&feed)
}

func (h *Handler) coverPath(feed *models.Feed) (string, bool) {
	if path, ok := h.resolve(feed.CoverImagePath); ok {
		return path, true
	}
	return h.generatedCover(feed.Name)
}

func (h *Handler) generatedCover(name string) (string, bool) {
	path, err := cover.EnsureGenerated(h.imageDir, name)
	if err != nil {
		log.Printf("[image] 生成封面失败 feed=%q: %v", name, err)
		return "", false
	}
	return path, true
}

func (h *Handler) serveFile(w http.ResponseWriter, r *http.Request, path string, maxAge int) {
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Cache-Control", "public, max-age="+strconv.Itoa(maxAge))
	http.ServeFile(w, r, path)
}

func Mount(mux *http.ServeMux, imageDir string) {
	mux.Handle("/image/feed/", NewFeed(imageDir))
	mux.Handle("/image/", New(imageDir))
}

// parseImageID 从 /image/123.jpg 或 /image/feed/12.jpg 中取出数字 ID。
func parseImageID(urlPath string) (uint64, error) {
	base := filepath.Base(urlPath)
	idStr := strings.TrimSuffix(base, filepath.Ext(base))
	return strconv.ParseUint(idStr, 10, 64)
}

// resolve 把 image_path 解析成可读的实际文件。新条目存相对 imageDir 的
// "<日期>/<哈希>.jpg"；老条目可能是绝对路径，或带 "data/images/" 前缀的
// 历史写法，逐一尝试后再放弃。
func (h *Handler) resolve(imagePath string) (string, bool) {
	if imagePath == "" {
		return "", false
	}
	candidates := []string{}
	if filepath.IsAbs(imagePath) {
		candidates = append(candidates, imagePath)
	} else {
		candidates = append(candidates, filepath.Join(h.imageDir, imagePath))
		// 兼容 "data/images/20261030/x.jpg" 这类带目录前缀的旧路径
		legacy := filepath.Join(h.imageDir, filepath.Base(filepath.Dir(imagePath)), filepath.Base(imagePath))
		candidates = append(candidates, legacy)
	}
	for _, p := range candidates {
		if info, err := os.Stat(p); err == nil && !info.IsDir() {
			return p, true
		}
	}
	return "", false
}

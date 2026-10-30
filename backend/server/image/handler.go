package image

import (
	"net/http"
	"os"
	"path/filepath"
	"strconv"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

type Handler struct {
	imageDir string
}

func New(imageDir string) *Handler {
	return &Handler{imageDir: imageDir}
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	base := filepath.Base(r.URL.Path)
	idStr := base[:len(base)-len(filepath.Ext(base))]

	id, err := strconv.ParseUint(idStr, 10, 64)
	if err != nil {
		http.Error(w, "Invalid item ID", http.StatusBadRequest)
		return
	}

	db := database.GetDB()
	var item models.Item
	if err := db.Select("image_path").First(&item, id).Error; err != nil {
		http.NotFound(w, r)
		return
	}

	if item.ImagePath == "" {
		http.NotFound(w, r)
		return
	}

	path, ok := h.resolve(item.ImagePath)
	if !ok {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Cache-Control", "public, max-age=604800")
	http.ServeFile(w, r, path)
}

func Mount(mux *http.ServeMux, imageDir string) {
	mux.Handle("/image/", New(imageDir))
}

// resolve 把 image_path 解析成可读的实际文件。新条目存相对 imageDir 的
// "<日期>/<哈希>.jpg"；老条目可能是绝对路径，或带 "data/images/" 前缀的
// 历史写法，逐一尝试后再放弃。
func (h *Handler) resolve(imagePath string) (string, bool) {
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

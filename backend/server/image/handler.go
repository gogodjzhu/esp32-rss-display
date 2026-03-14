package image

import (
	"net/http"
	"path/filepath"
	"strconv"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

type Handler struct{}

func New() *Handler {
	return &Handler{}
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

	w.Header().Set("Content-Type", "image/jpeg")
	http.ServeFile(w, r, item.ImagePath)
}

func Mount(mux *http.ServeMux) {
	handler := New()
	mux.Handle("/image/", handler)
}

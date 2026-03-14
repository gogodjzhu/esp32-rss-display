package api

import (
	"net/http"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
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
}

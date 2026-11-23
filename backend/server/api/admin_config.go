package api

import (
	"encoding/json"
	"io"
	"net/http"

	"github.com/esp32-rss-display/backend/server/database"
)

func GetAdminConfig(w http.ResponseWriter, r *http.Request) {
	cfg, err := database.GetSettings(database.GetDB().WithContext(r.Context()))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "读取配置失败")
		return
	}
	writeJSON(w, http.StatusOK, cfg)
}

func PutAdminConfig(notify func()) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			TranslationEnabled *bool `json:"translation_enabled"`
		}
		dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&req); err != nil {
			writeError(w, http.StatusBadRequest, "请求体必须是配置 JSON")
			return
		}
		if err := dec.Decode(new(any)); err != io.EOF {
			writeError(w, http.StatusBadRequest, "请求体只能包含一个 JSON 对象")
			return
		}
		if req.TranslationEnabled == nil {
			writeError(w, http.StatusBadRequest, "translation_enabled 必须是布尔值")
			return
		}
		cfg, err := database.SaveSettings(database.GetDB().WithContext(r.Context()), *req.TranslationEnabled)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "保存配置失败")
			return
		}
		if cfg.TranslationEnabled && notify != nil {
			notify()
		}
		writeJSON(w, http.StatusOK, cfg)
	}
}

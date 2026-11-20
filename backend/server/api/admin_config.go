package api

// 管理页「配置」tab 的读写接口。
//
// 配置保存在数据库（settings 包），保存后立即生效；翻译开关打开时会
// 异步补译历史条目，新条目在抓取结束后自动处理。

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/settings"
	"github.com/esp32-rss-display/backend/server/translate"
)

// AdminConfig 是 /admin/config 返回的可在线修改配置。
type AdminConfig struct {
	TranslationEnabled bool `json:"translation_enabled"`
}

// GetAdminConfig 返回当前配置。
func GetAdminConfig(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, AdminConfig{
		TranslationEnabled: settings.Get(database.GetDB()),
	})
}

// UpdateAdminConfig 保存配置；目前只支持翻译开关。
func UpdateAdminConfig(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<16))
	if err != nil {
		writeError(w, http.StatusBadRequest, "读取请求失败: "+err.Error())
		return
	}

	var req struct {
		TranslationEnabled *bool `json:"translation_enabled"`
	}
	dec := json.NewDecoder(strings.NewReader(string(body)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "请求体格式错误: "+err.Error())
		return
	}
	if req.TranslationEnabled == nil {
		writeError(w, http.StatusBadRequest, "translation_enabled 必填")
		return
	}

	if err := settings.Set(database.GetDB(), *req.TranslationEnabled); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	// 开启后异步回填历史条目；关闭时保留已缓存译文，重新开启可继续使用。
	if *req.TranslationEnabled {
		go translate.Backfill(context.Background(), database.GetDB())
	}
	writeJSON(w, http.StatusOK, AdminConfig{TranslationEnabled: *req.TranslationEnabled})
}

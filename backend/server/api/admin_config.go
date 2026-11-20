package api

// 管理页「配置」tab 的读写接口。
//
// 配置保存在数据库（settings 包），保存后立即生效；翻译开关打开时会唤醒
// 后台补译任务，历史条目按保留期逐批翻译，新条目在抓取结束后自动处理。

import (
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
		TranslationEnabled: settings.TranslationEnabled(),
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

	cfg := settings.Current()
	cfg.TranslationEnabled = *req.TranslationEnabled
	if err := settings.Update(database.GetDB(), cfg); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	// 开启后立即补译历史条目；关闭时保留已缓存的译文，重新开启可继续使用。
	if cfg.TranslationEnabled {
		translate.Notify()
	}
	writeJSON(w, http.StatusOK, AdminConfig{TranslationEnabled: cfg.TranslationEnabled})
}

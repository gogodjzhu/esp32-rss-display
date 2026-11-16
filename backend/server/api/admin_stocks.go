package api

// 管理页的行情配置接口：读取当前配置与拉取状态，保存后立即生效。
// 配置持久化在数据库（见 stocks/store.go），config.yaml 只做首次种子。

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/stocks"
)

// StockAdmin 为管理页可操作的行情服务：应用新配置并读取拉取状态。
// 由 main 在启动时注入；测试可替换为假实现。
type StockAdmin interface {
	Apply(cfg *config.StockConfig)
	Status() stocks.Status
}

var StockAdminService StockAdmin

const (
	maxStockConfigBody = 64 << 10
	maxStockNameRunes  = 40
)

type adminStockSymbol struct {
	Code string `json:"code"`
	Name string `json:"name"`
}

type adminStockConfig struct {
	Enabled        bool               `json:"enabled"`
	RefreshSeconds int                `json:"refresh_seconds"`
	Symbols        []adminStockSymbol `json:"symbols"`
	// Runtime 为后台拉取状态，只读，不受保存影响。
	Runtime stocks.Status `json:"runtime"`
}

// AdminGetStocks 返回已保存的行情配置与当前拉取状态。
func AdminGetStocks(w http.ResponseWriter, r *http.Request) {
	cfg, _, err := stocks.ReadConfig()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "读取行情配置失败: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, buildAdminStockConfig(cfg))
}

// AdminUpdateStocks 校验并保存整份行情配置，随后立即应用到运行中的服务，
// 后台刷新协程会马上按新编号拉取一次，无需重启。
func AdminUpdateStocks(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, maxStockConfigBody))
	if err != nil {
		writeError(w, http.StatusBadRequest, "读取请求失败: "+err.Error())
		return
	}

	var req struct {
		Enabled        bool               `json:"enabled"`
		RefreshSeconds int                `json:"refresh_seconds"`
		Symbols        []adminStockSymbol `json:"symbols"`
	}
	dec := json.NewDecoder(strings.NewReader(string(body)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "请求体格式错误: "+err.Error())
		return
	}

	cfg, err := validateStockConfig(req.Enabled, req.RefreshSeconds, req.Symbols)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	if err := stocks.SaveConfig(cfg); err != nil {
		writeError(w, http.StatusInternalServerError, "保存行情配置失败: "+err.Error())
		return
	}
	if StockAdminService != nil {
		StockAdminService.Apply(&cfg)
	}
	writeJSON(w, http.StatusOK, buildAdminStockConfig(cfg))
}

// validateStockConfig 校验管理页提交的配置：编号合法且不重复、显示名不过长、
// 启用时至少一个编号；刷新间隔夹到下限。
func validateStockConfig(enabled bool, refreshSeconds int, in []adminStockSymbol) (config.StockConfig, error) {
	if len(in) > stocks.MaxSymbols {
		return config.StockConfig{}, fmt.Errorf("最多配置 %d 个行情编号", stocks.MaxSymbols)
	}

	cfg := config.StockConfig{Enabled: enabled, RefreshSeconds: refreshSeconds}
	seen := make(map[string]bool, len(in))
	for i, item := range in {
		code, err := stocks.ValidateSymbol(item.Code)
		if err != nil {
			return config.StockConfig{}, fmt.Errorf("第 %d 个编号无效：%w", i+1, err)
		}
		if seen[code] {
			return config.StockConfig{}, fmt.Errorf("编号 %s 重复", code)
		}
		seen[code] = true

		name := strings.TrimSpace(item.Name)
		if utf8.RuneCountInString(name) > maxStockNameRunes {
			return config.StockConfig{}, fmt.Errorf("编号 %s 的显示名称过长（最多 %d 个字）", code, maxStockNameRunes)
		}
		cfg.Symbols = append(cfg.Symbols, config.StockSymbol{Code: code, Name: name})
	}

	if cfg.Enabled && len(cfg.Symbols) == 0 {
		return config.StockConfig{}, errors.New("启用行情至少需要一个行情编号")
	}
	if refreshSeconds > 0 && refreshSeconds < int(stocks.MinRefresh/time.Second) {
		cfg.RefreshSeconds = int(stocks.MinRefresh / time.Second)
	}
	return stocks.NormalizeConfig(cfg), nil
}

// buildAdminStockConfig 把持久化配置和运行状态组装成管理页响应。
func buildAdminStockConfig(cfg config.StockConfig) adminStockConfig {
	out := adminStockConfig{
		Enabled:        cfg.Enabled,
		RefreshSeconds: cfg.RefreshSeconds,
		Symbols:        make([]adminStockSymbol, 0, len(cfg.Symbols)),
	}
	if out.RefreshSeconds <= 0 {
		out.RefreshSeconds = int(stocks.DefaultRefresh / time.Second)
	}
	for _, sym := range cfg.Symbols {
		out.Symbols = append(out.Symbols, adminStockSymbol{Code: sym.Code, Name: sym.Name})
	}
	if StockAdminService != nil {
		out.Runtime = StockAdminService.Status()
	}
	return out
}

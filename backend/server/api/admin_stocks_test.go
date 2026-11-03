package api

// 管理页行情配置接口的测试：首次返回、保存校验、持久化与立即生效。
// 用假实现替身记录 Apply 调用，不依赖真实行情源。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/stocks"
)

type fakeStockAdmin struct {
	applied *config.StockConfig
	status  stocks.Status
}

func (f *fakeStockAdmin) Apply(cfg *config.StockConfig) {
	clone := *cfg
	f.applied = &clone
}

func (f *fakeStockAdmin) Status() stocks.Status {
	return f.status
}

// setupStockAdminTest 复用接口测试的共享内存库，并清理行情配置单行表。
func setupStockAdminTest(t *testing.T) *fakeStockAdmin {
	t.Helper()
	setupTestDB(t)
	database.GetDB().Where("id = ?", 1).Delete(&models.StockSetting{})
	t.Cleanup(func() {
		database.GetDB().Where("id = ?", 1).Delete(&models.StockSetting{})
	})

	fake := &fakeStockAdmin{}
	prev := StockAdminService
	StockAdminService = fake
	t.Cleanup(func() { StockAdminService = prev })
	return fake
}

func decodeAdminStock(t *testing.T, rec *httptest.ResponseRecorder) adminStockConfig {
	t.Helper()
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	var got adminStockConfig
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode: %v", err)
	}
	return got
}

func TestAdminGetStocksWithoutSavedConfig(t *testing.T) {
	setupStockAdminTest(t)

	rec := httptest.NewRecorder()
	AdminGetStocks(rec, httptest.NewRequest(http.MethodGet, "/admin/stocks", nil))
	got := decodeAdminStock(t, rec)

	if got.Enabled || len(got.Symbols) != 0 {
		t.Fatalf("unexpected config: %+v", got)
	}
	if got.RefreshSeconds != int(stocks.DefaultRefresh.Seconds()) {
		t.Fatalf("refresh_seconds = %d, want default", got.RefreshSeconds)
	}
}

func TestAdminUpdateStocksSavesAndApplies(t *testing.T) {
	fake := setupStockAdminTest(t)

	body := `{
		"enabled": true,
		"refresh_seconds": 1,
		"symbols": [
			{"code": " SH000001 ", "name": " 上证 "},
			{"code": "usaapl", "name": "苹果"}
		]
	}`
	rec := httptest.NewRecorder()
	AdminUpdateStocks(rec, httptest.NewRequest(http.MethodPut, "/admin/stocks", strings.NewReader(body)))
	got := decodeAdminStock(t, rec)

	if !got.Enabled || len(got.Symbols) != 2 {
		t.Fatalf("unexpected response: %+v", got)
	}
	if got.Symbols[0] != (adminStockSymbol{Code: "sh000001", Name: "上证"}) {
		t.Fatalf("symbol 0 = %+v", got.Symbols[0])
	}
	if got.Symbols[1].Code != "usAAPL" {
		t.Fatalf("symbol 1 = %+v", got.Symbols[1])
	}
	// 低于下限的刷新间隔被夹到 5 秒
	if got.RefreshSeconds != int(stocks.MinRefresh.Seconds()) {
		t.Fatalf("refresh_seconds = %d, want %d", got.RefreshSeconds, int(stocks.MinRefresh.Seconds()))
	}

	// 落库：再读一次接口结果仍是新配置
	rec = httptest.NewRecorder()
	AdminGetStocks(rec, httptest.NewRequest(http.MethodGet, "/admin/stocks", nil))
	if again := decodeAdminStock(t, rec); len(again.Symbols) != 2 || again.Symbols[1].Code != "usAAPL" {
		t.Fatalf("saved config not returned: %+v", again)
	}

	// 立即应用到运行中的服务
	if fake.applied == nil {
		t.Fatal("保存后没有调用 Apply")
	}
	if !fake.applied.Enabled || len(fake.applied.Symbols) != 2 || fake.applied.Symbols[1].Code != "usAAPL" {
		t.Fatalf("applied config = %+v", fake.applied)
	}
}

func TestAdminUpdateStocksRejectsInvalidInput(t *testing.T) {
	fake := setupStockAdminTest(t)

	cases := []struct {
		name string
		body string
		want string
	}{
		{
			name: "非法编号",
			body: `{"enabled":true,"symbols":[{"code":"bad"}]}`,
			want: "编号无效",
		},
		{
			name: "前缀不支持",
			body: `{"enabled":true,"symbols":[{"code":"xx123456"}]}`,
			want: "编号无效",
		},
		{
			name: "编号重复",
			body: `{"enabled":true,"symbols":[{"code":"sh000001"},{"code":"SH000001"}]}`,
			want: "重复",
		},
		{
			name: "启用但没有编号",
			body: `{"enabled":true,"symbols":[]}`,
			want: "至少需要一个",
		},
		{
			name: "名称过长",
			body: `{"enabled":true,"symbols":[{"code":"sh000001","name":"` + strings.Repeat("长", 41) + `"}]}`,
			want: "过长",
		},
		{
			name: "未知字段",
			body: `{"enabled":true,"extra":1,"symbols":[]}`,
			want: "请求体格式错误",
		},
		{
			name: "非法 JSON",
			body: `{`,
			want: "请求体格式错误",
		},
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			rec := httptest.NewRecorder()
			AdminUpdateStocks(rec, httptest.NewRequest(http.MethodPut, "/admin/stocks", strings.NewReader(c.body)))
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
			}
			if !strings.Contains(rec.Body.String(), c.want) {
				t.Fatalf("body = %s, want contains %q", rec.Body.String(), c.want)
			}
		})
	}

	if fake.applied != nil {
		t.Fatalf("非法配置不应被应用: %+v", fake.applied)
	}
}

// 停用行情可以保留已配置的编号，方便下次直接打开。
func TestAdminUpdateStocksCanDisableKeepingSymbols(t *testing.T) {
	fake := setupStockAdminTest(t)

	body := `{"enabled":false,"refresh_seconds":30,"symbols":[{"code":"sh000001","name":"上证"}]}`
	rec := httptest.NewRecorder()
	AdminUpdateStocks(rec, httptest.NewRequest(http.MethodPut, "/admin/stocks", strings.NewReader(body)))
	got := decodeAdminStock(t, rec)

	if got.Enabled || len(got.Symbols) != 1 {
		t.Fatalf("unexpected response: %+v", got)
	}
	if fake.applied == nil || fake.applied.Enabled || len(fake.applied.Symbols) != 1 {
		t.Fatalf("applied config = %+v", fake.applied)
	}
}

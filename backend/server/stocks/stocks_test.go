package stocks

// 行情服务测试：验证成功拉取写入缓存、失败保留旧缓存、窗口轮换、
// 运行时配置切换，以及未启用时返回 nil。

import (
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
)

// 未启用或没有合法编号时服务仍然返回，只是没有行情可返回；
// 这样管理页可以在运行时把行情打开。
func TestNewDisabledServiceHasNoQuotes(t *testing.T) {
	for _, cfg := range []*config.StockConfig{
		nil,
		{Enabled: false},
		{Enabled: true, Symbols: []config.StockSymbol{{Code: "bad"}}},
	} {
		svc := New(cfg)
		if svc == nil {
			t.Fatalf("expected non-nil service for %+v", cfg)
		}
		if got := svc.Window("dev", DefaultWindow); got != nil {
			t.Fatalf("expected no quotes for %+v, got %+v", cfg, got)
		}
		if got := svc.Status(); got.QuoteCount != 0 || got.LastError != "" {
			t.Fatalf("unexpected status for %+v: %+v", cfg, got)
		}
	}
}

func TestServiceApplyClearsStaleQuotes(t *testing.T) {
	body := encodeGBK(t, quoteLine("sh000001", "上证指数", "-0.82"))
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(body)
	}))
	defer server.Close()

	svc := New(&config.StockConfig{
		Enabled: true,
		Symbols: []config.StockSymbol{{Code: "sh000001"}},
	})
	svc.endpoint = server.URL + "/q="
	svc.client = server.Client()
	svc.refresh()

	if got := svc.Window("dev", DefaultWindow); len(got) != 1 {
		t.Fatalf("expected cached quote before reconfiguration, got %+v", got)
	}

	// 换成另一支股票后旧快照立即失效，不能继续把已删除的编号发给设备
	svc.Apply(&config.StockConfig{
		Enabled: true,
		Symbols: []config.StockSymbol{{Code: "sz399001", Name: "深证"}},
	})
	if got := svc.Window("dev", DefaultWindow); got != nil {
		t.Fatalf("expected quotes cleared after symbol change, got %+v", got)
	}

	// 关闭行情同样清空缓存
	svc.Apply(&config.StockConfig{Symbols: []config.StockSymbol{{Code: "sh000001"}}})
	if got := svc.Window("dev", DefaultWindow); got != nil {
		t.Fatalf("expected quotes cleared after disable, got %+v", got)
	}
}

// Apply 后后台循环应立即按新配置拉取，不用等一个刷新周期。
func TestServiceApplyRefreshesRunningLoop(t *testing.T) {
	body := encodeGBK(t, quoteLine("sh000001", "上证指数", "-0.82"))
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(body)
	}))
	defer server.Close()

	// 以停用状态启动：完全等待管理页的 Apply 来打开行情
	svc := New(&config.StockConfig{})
	svc.endpoint = server.URL + "/q="
	svc.client = server.Client()
	svc.Start()
	defer svc.Stop()

	svc.Apply(&config.StockConfig{
		Enabled:        true,
		RefreshSeconds: 30,
		Symbols:        []config.StockSymbol{{Code: "sh000001"}},
	})

	deadline := time.Now().Add(2 * time.Second)
	for {
		if got := svc.Window("dev", DefaultWindow); len(got) == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("行情服务没有在 Apply 后拉取新配置")
		}
		time.Sleep(10 * time.Millisecond)
	}

	status := svc.Status()
	if status.LastSuccessAt == nil || status.QuoteCount != 1 {
		t.Fatalf("unexpected status after apply: %+v", status)
	}
}

func TestValidateSymbol(t *testing.T) {
	cases := []struct {
		in      string
		want    string
		wantErr bool
	}{
		{"sh000001", "sh000001", false},
		{" SH000001 ", "sh000001", false},
		{"usaapl", "usAAPL", false},
		{"hk00700", "hk00700", false},
		{"bj430047", "bj430047", false},
		{"bad", "bad", true},
		{"xx123456", "xx123456", true},
		{"", "", true},
	}
	for _, c := range cases {
		got, err := ValidateSymbol(c.in)
		if c.wantErr {
			if err == nil {
				t.Fatalf("ValidateSymbol(%q) 期望报错，得到 %q", c.in, got)
			}
			continue
		}
		if err != nil || got != c.want {
			t.Fatalf("ValidateSymbol(%q) = %q, %v; want %q", c.in, got, err, c.want)
		}
	}
}

func TestNormalizeConfig(t *testing.T) {
	cfg := NormalizeConfig(config.StockConfig{
		Enabled:        true,
		RefreshSeconds: 1,
		Symbols: []config.StockSymbol{
			{Code: "SH000001", Name: " 上证 "},
			{Code: "sh000001", Name: "重复"},
			{Code: "usaapl", Name: "苹果"},
			{Code: "bad", Name: "非法"},
		},
	})
	if !cfg.Enabled {
		t.Fatal("启用状态应保留")
	}
	if cfg.RefreshSeconds != int(MinRefresh/time.Second) {
		t.Fatalf("refresh_seconds = %d, want %d", cfg.RefreshSeconds, int(MinRefresh/time.Second))
	}
	if len(cfg.Symbols) != 2 {
		t.Fatalf("symbols = %+v, want 2 条（去重并跳过非法）", cfg.Symbols)
	}
	if cfg.Symbols[0].Code != "sh000001" || cfg.Symbols[0].Name != "上证" {
		t.Fatalf("first symbol = %+v", cfg.Symbols[0])
	}
	if cfg.Symbols[1].Code != "usAAPL" {
		t.Fatalf("second symbol = %+v", cfg.Symbols[1])
	}

	// 启用但没有合法编号时退回停用，避免服务空转
	empty := NormalizeConfig(config.StockConfig{Enabled: true})
	if empty.Enabled || len(empty.Symbols) != 0 {
		t.Fatalf("expected disabled empty config, got %+v", empty)
	}
	if empty.RefreshSeconds != int(DefaultRefresh/time.Second) {
		t.Fatalf("default refresh = %d", empty.RefreshSeconds)
	}
}

func TestServiceRefreshKeepsLastQuotesOnError(t *testing.T) {
	var fail atomic.Bool
	body := encodeGBK(t, quoteLine("sh000001", "上证指数", "-0.82"))

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if fail.Load() {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		w.Write(body)
	}))
	defer server.Close()

	svc := New(&config.StockConfig{
		Enabled: true,
		Symbols: []config.StockSymbol{{Code: "sh000001"}},
	})
	if svc == nil {
		t.Fatal("expected enabled service")
	}
	// 指向测试服务器，避免真实网络请求
	svc.endpoint = server.URL + "/q="
	svc.client = server.Client()

	if got := svc.Window("test-device", DefaultWindow); got != nil {
		t.Fatalf("expected empty window before first fetch, got %+v", got)
	}

	svc.refresh()
	got := svc.Window("test-device", DefaultWindow)
	if len(got) != 1 || got[0].Name != "上证指数" || got[0].ChangePct != -0.82 {
		t.Fatalf("unexpected window after fetch: %+v", got)
	}

	// 数据源故障时保留上一份快照，设备端不会看到行情栏突然消失
	fail.Store(true)
	svc.refresh()
	got = svc.Window("test-device", DefaultWindow)
	if len(got) != 1 || got[0].Name != "上证指数" {
		t.Fatalf("window must survive fetch failure, got %+v", got)
	}
}

func TestServiceWindowReturnsCopy(t *testing.T) {
	svc := &Service{quotes: []Quote{{Name: "上证指数", ChangePct: -0.82}}}

	got := svc.Window("dev", DefaultWindow)
	got[0].Name = "被调用方修改"

	if svc.Window("dev", DefaultWindow)[0].Name != "上证指数" {
		t.Fatal("Window must return a copy of cached quotes")
	}
}

// 同一设备按 size 轮换并回绕；不同设备的轮换位置互不影响。
func TestServiceWindowRotation(t *testing.T) {
	quotes := make([]Quote, 7)
	for i := range quotes {
		quotes[i] = Quote{Name: string(rune('A' + i))}
	}
	svc := &Service{quotes: quotes}

	assertWindow := func(device string, size int, want ...string) {
		t.Helper()
		got := svc.Window(device, size)
		if len(got) != len(want) {
			t.Fatalf("device %s: expected %d quotes, got %+v", device, len(want), got)
		}
		for i := range want {
			if got[i].Name != want[i] {
				t.Fatalf("device %s: position %d = %q, want %q", device, i, got[i].Name, want[i])
			}
		}
	}

	assertWindow("dev-a", 3, "A", "B", "C")
	assertWindow("dev-a", 3, "D", "E", "F")
	assertWindow("dev-a", 3, "G", "A", "B")
	// 另一台设备从自己的起点开始，不受 dev-a 影响
	assertWindow("dev-b", 3, "A", "B", "C")
	// 窗口覆盖全部行情时直接返回全部，不产生重复
	assertWindow("dev-a", 10, "A", "B", "C", "D", "E", "F", "G")
}

func TestClampWindow(t *testing.T) {
	cases := []struct {
		in   int
		want int
	}{
		{0, DefaultWindow},
		{-1, DefaultWindow},
		{1, 1},
		{MaxWindow, MaxWindow},
		{MaxWindow + 1, MaxWindow},
	}
	for _, c := range cases {
		if got := ClampWindow(c.in); got != c.want {
			t.Fatalf("ClampWindow(%d) = %d, want %d", c.in, got, c.want)
		}
	}
	if got := (&Service{}).Window("dev", 3); got != nil {
		t.Fatalf("expected nil window without quotes, got %+v", got)
	}
	var nilService *Service
	if got := nilService.Window("dev", 3); got != nil {
		t.Fatalf("expected nil window for nil service, got %+v", got)
	}
}

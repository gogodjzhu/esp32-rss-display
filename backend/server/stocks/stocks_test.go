package stocks

// 行情服务测试：验证成功拉取写入缓存、失败保留旧缓存、窗口轮换，以及未启用时返回 nil。

import (
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"

	"github.com/esp32-rss-display/backend/server/config"
)

func TestNewDisabledReturnsNil(t *testing.T) {
	if svc := New(nil); svc != nil {
		t.Fatalf("expected nil service for nil config, got %+v", svc)
	}
	if svc := New(&config.StockConfig{Enabled: false}); svc != nil {
		t.Fatalf("expected nil service when disabled, got %+v", svc)
	}
	if svc := New(&config.StockConfig{Enabled: true, Symbols: []config.StockSymbol{{Code: "bad"}}}); svc != nil {
		t.Fatalf("expected nil service without valid symbols, got %+v", svc)
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

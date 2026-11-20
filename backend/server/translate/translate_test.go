package translate

// 翻译客户端测试：端点回退、跳过非英文标题与异常响应处理。
// 使用本地 httptest 服务，不访问外网。

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

// stubEndpoints 把两个公开接口替换为本地服务，测试结束后恢复。
func stubEndpoints(t *testing.T, google, myMemory http.HandlerFunc) {
	t.Helper()
	oldGoogle, oldMyMemory := googleEndpoint, myMemoryEndpoint
	googleSrv := httptest.NewServer(google)
	myMemorySrv := httptest.NewServer(myMemory)
	googleEndpoint, myMemoryEndpoint = googleSrv.URL, myMemorySrv.URL
	t.Cleanup(func() {
		googleEndpoint, myMemoryEndpoint = oldGoogle, oldMyMemory
		googleSrv.Close()
		myMemorySrv.Close()
	})
}

func TestNeedsTranslation(t *testing.T) {
	cases := []struct {
		title string
		want  bool
	}{
		{"Fed raises interest rates", true},
		{"NASA's new rocket", true},
		{"美联储宣布加息", false},
		{"AI 新突破", false},
		{"", false},
		{"123 456", false},
		{"🚀🚀", false},
		{"  Hello world  ", true},
	}
	for _, tc := range cases {
		if got := NeedsTranslation(tc.title); got != tc.want {
			t.Errorf("NeedsTranslation(%q) = %v, want %v", tc.title, got, tc.want)
		}
	}
}

func TestTranslateUsesGoogleFirst(t *testing.T) {
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) {
			if got := r.URL.Query().Get("q"); got != "Hello world" {
				t.Errorf("q = %q", got)
			}
			w.Write([]byte(`["你好世界"]`))
		},
		func(w http.ResponseWriter, r *http.Request) {
			t.Error("Google 成功时不应调用 MyMemory")
		},
	)

	got, err := Translate(context.Background(), "Hello world")
	if err != nil {
		t.Fatalf("Translate: %v", err)
	}
	if got != "你好世界" {
		t.Fatalf("got %q", got)
	}
}

func TestTranslateFallsBackToMyMemory(t *testing.T) {
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "blocked", http.StatusForbidden)
		},
		func(w http.ResponseWriter, r *http.Request) {
			w.Write([]byte(`{"responseData":{"translatedText":"你好世界"},"responseStatus":200}`))
		},
	)

	got, err := Translate(context.Background(), "Hello world")
	if err != nil {
		t.Fatalf("Translate: %v", err)
	}
	if got != "你好世界" {
		t.Fatalf("got %q", got)
	}
}

func TestTranslateReportsFailureWhenAllProvidersFail(t *testing.T) {
	stubEndpoints(t,
		func(w http.ResponseWriter, r *http.Request) { http.Error(w, "down", http.StatusBadGateway) },
		func(w http.ResponseWriter, r *http.Request) { http.Error(w, "down", http.StatusBadGateway) },
	)

	if _, err := Translate(context.Background(), "Hello"); err == nil {
		t.Fatal("全部接口失败时应返回错误")
	}
}

func TestParseMyMemoryTreatsQuotaWarningAsError(t *testing.T) {
	body := `{"responseData":{"translatedText":"MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY"},"responseStatus":200}`
	if _, err := parseMyMemory([]byte(body)); err == nil {
		t.Fatal("额度提示应视为失败")
	}
}

package translate

// 翻译客户端测试：Provider 回退、跳过非英文标题、解析各公开接口的响应格式。
// 全部使用本地假实现或 httptest，不访问外网。

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

type fakeProvider struct {
	name  string
	out   string
	err   error
	calls int
}

func (f *fakeProvider) Name() string { return f.name }

func (f *fakeProvider) Translate(ctx context.Context, text string) (string, error) {
	f.calls++
	if f.err != nil {
		return "", f.err
	}
	return f.out, nil
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

func TestClientFallsBackToNextProvider(t *testing.T) {
	first := &fakeProvider{name: "first", err: errors.New("boom")}
	second := &fakeProvider{name: "second", out: "中文译文"}
	client := NewClientWithProviders(first, second)

	got, err := client.Translate(context.Background(), "Hello world")
	if err != nil {
		t.Fatalf("Translate: %v", err)
	}
	if got != "中文译文" {
		t.Fatalf("got %q, want 中文译文", got)
	}
	if first.calls != 1 || second.calls != 1 {
		t.Fatalf("calls = %d/%d, want 1/1", first.calls, second.calls)
	}
}

func TestClientReportsAllFailures(t *testing.T) {
	client := NewClientWithProviders(
		&fakeProvider{name: "a", err: errors.New("a 失败")},
		&fakeProvider{name: "b", out: "  "},
	)
	if _, err := client.Translate(context.Background(), "Hello"); err == nil {
		t.Fatal("全部 Provider 失败时应返回错误")
	}
}

func TestParseGoogleDict(t *testing.T) {
	cases := []string{
		`["你好世界"]`,
		`[["你好世界","en"]]`,
	}
	for _, body := range cases {
		got, err := parseGoogleDict([]byte(body))
		if err != nil {
			t.Fatalf("parseGoogleDict(%s): %v", body, err)
		}
		if got != "你好世界" {
			t.Fatalf("parseGoogleDict(%s) = %q", body, got)
		}
	}
	if _, err := parseGoogleDict([]byte(`{"error":"rate limited"}`)); err == nil {
		t.Fatal("异常响应应报错")
	}
}

func TestParseGoogleGtxConcatenatesSegments(t *testing.T) {
	body := `[[["第一段","first",null,null,10],["第二段","second",null,null,10]],null,"en"]`
	got, err := parseGoogleGtx([]byte(body))
	if err != nil {
		t.Fatalf("parseGoogleGtx: %v", err)
	}
	if got != "第一段第二段" {
		t.Fatalf("got %q", got)
	}
}

func TestParseMyMemory(t *testing.T) {
	okBody := `{"responseData":{"translatedText":"你好世界"},"responseStatus":200}`
	got, err := parseMyMemory([]byte(okBody))
	if err != nil || got != "你好世界" {
		t.Fatalf("parseMyMemory = %q, %v", got, err)
	}

	warnBody := `{"responseData":{"translatedText":"MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY"},"responseStatus":200}`
	if _, err := parseMyMemory([]byte(warnBody)); err == nil {
		t.Fatal("额度提示应视为失败")
	}
}

func TestHTTPProviderRequestsAndParses(t *testing.T) {
	var gotQuery string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotQuery = r.URL.Query().Get("q")
		if r.Header.Get("User-Agent") == "" {
			t.Error("请求应带 User-Agent")
		}
		w.Write([]byte(`["译文"]`))
	}))
	defer srv.Close()

	provider := newHTTPProvider("test", srv.URL, googleDictURL, parseGoogleDict, &http.Client{}, 2*time.Second)
	got, err := provider.Translate(context.Background(), "Hello world")
	if err != nil {
		t.Fatalf("Translate: %v", err)
	}
	if got != "译文" {
		t.Fatalf("got %q", got)
	}
	if gotQuery != "Hello world" {
		t.Fatalf("query = %q", gotQuery)
	}
}

func TestHTTPProviderRejectsNon200(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "blocked", http.StatusForbidden)
	}))
	defer srv.Close()

	provider := newHTTPProvider("test", srv.URL, googleDictURL, parseGoogleDict, &http.Client{}, 2*time.Second)
	if _, err := provider.Translate(context.Background(), "Hello"); err == nil {
		t.Fatal("非 200 响应应报错")
	}
}

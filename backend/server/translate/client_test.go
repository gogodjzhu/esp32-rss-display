package translate

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

func TestClientTranslate(t *testing.T) {
	for _, tc := range []struct {
		name, google            string
		status                  int
		wantFallback, wantError bool
	}{
		{"google", `[[["你好，","Hello,",null,null],["世界！","world!",null,null]],null,"en"]`, 200, false, false},
		{"fallback_http", ``, 429, true, false},
		{"fallback_malformed", `{"error":"bad"}`, 200, true, false},
		{"fallback_empty", `[[]]`, 200, true, false},
		{"fallback_nonchinese", `[[["QUOTA EXCEEDED"]]]`, 200, true, false},
		{"both_fail", ``, 503, true, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var fallbackCalls atomic.Int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Query().Get("q") != "Hello, world! & more?" {
					t.Errorf("标题查询参数未正确编码: %q", r.URL.RawQuery)
				}
				if r.URL.Path == "/google" {
					if r.URL.Query().Get("tl") != "zh-CN" {
						t.Error("目标语言应为中文")
					}
					w.WriteHeader(tc.status)
					fmt.Fprint(w, tc.google)
					return
				}
				fallbackCalls.Add(1)
				if r.URL.Query().Get("langpair") != "en|zh-CN" {
					t.Error("备用语言方向错误")
				}
				if tc.wantError {
					fmt.Fprint(w, `{"responseStatus":403,"responseData":{"translatedText":"LIMIT"}}`)
					return
				}
				fmt.Fprint(w, `{"responseStatus":200,"responseData":{"translatedText":"你好，世界！"}}`)
			}))
			defer srv.Close()
			c := &Client{http: srv.Client(), googleURL: srv.URL + "/google", memoryURL: srv.URL + "/memory"}
			got, err := c.Translate(context.Background(), "Hello, world! & more?")
			if (err != nil) != tc.wantError {
				t.Fatalf("err = %v", err)
			}
			if !tc.wantError && got != "你好，世界！" {
				t.Fatalf("got = %q", got)
			}
			if (fallbackCalls.Load() > 0) != tc.wantFallback {
				t.Fatalf("fallback calls = %d", fallbackCalls.Load())
			}
		})
	}
}

func TestClientCancellationAndResponseLimit(t *testing.T) {
	var calls atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		fmt.Fprint(w, strings.Repeat(" ", 65<<10))
	}))
	defer srv.Close()
	c := &Client{http: srv.Client(), googleURL: srv.URL, memoryURL: srv.URL}
	if _, err := c.Translate(context.Background(), "Hello"); err == nil {
		t.Fatal("过大响应应返回错误")
	}
	before := calls.Load()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := c.Translate(ctx, "Hello"); err == nil {
		t.Fatal("取消后应返回错误")
	}
	if calls.Load() != before {
		t.Fatal("取消后不应发起备用请求")
	}
}

func TestNeedsTranslation(t *testing.T) {
	for _, tc := range []struct {
		title string
		want  bool
	}{
		{"Hello, world!", true}, {"What's new in Go 1.26?", true}, {"中文标题", false},
		{"AI 发展新动态", false}, {"2026-11-23", false}, {"", false}, {"😀", false}, {"こんにちは", false},
	} {
		if got := NeedsTranslation(tc.title); got != tc.want {
			t.Errorf("NeedsTranslation(%q) = %v", tc.title, got)
		}
	}
}

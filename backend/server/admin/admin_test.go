package admin

import (
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
	"testing"
)

func newMux() *http.ServeMux {
	mux := http.NewServeMux()
	Mount(mux)
	return mux
}

func TestAdminRedirectsToSlash(t *testing.T) {
	rec := httptest.NewRecorder()
	newMux().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/admin", nil))

	if rec.Code != http.StatusMovedPermanently {
		t.Fatalf("status = %d, want %d", rec.Code, http.StatusMovedPermanently)
	}
	if loc := rec.Header().Get("Location"); loc != "/admin/" {
		t.Fatalf("location = %q, want /admin/", loc)
	}
}

// TestAdminServesPageAndAssets 校验构建产物确实被打进二进制：
// 页面里的每个 /admin/assets/* 引用都必须能取到，否则部署后是白屏。
func TestAdminServesPageAndAssets(t *testing.T) {
	rec := httptest.NewRecorder()
	newMux().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/admin/", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	page := rec.Body.String()
	for _, needle := range []string{"esp32-rss", "/admin/assets/"} {
		if !strings.Contains(page, needle) {
			t.Fatalf("页面缺少 %q", needle)
		}
	}

	assets := regexp.MustCompile(`/admin/assets/[A-Za-z0-9._-]+`).FindAllString(page, -1)
	if len(assets) == 0 {
		t.Fatal("页面没有引用任何构建产物")
	}
	for _, path := range assets {
		assetRec := httptest.NewRecorder()
		newMux().ServeHTTP(assetRec, httptest.NewRequest(http.MethodGet, path, nil))
		if assetRec.Code != http.StatusOK {
			t.Fatalf("%s: status = %d, want 200", path, assetRec.Code)
		}
		if assetRec.Body.Len() == 0 {
			t.Fatalf("%s 内容为空", path)
		}
	}
}

// TestAdminBundleHasChineseUI 确认打包进的是中文界面，避免构建时漏掉源码。
func TestAdminBundleHasChineseUI(t *testing.T) {
	rec := httptest.NewRecorder()
	newMux().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/admin/", nil))
	assets := regexp.MustCompile(`/admin/assets/[A-Za-z0-9._-]+\.js`).
		FindAllString(rec.Body.String(), -1)
	if len(assets) == 0 {
		t.Fatal("页面没有引用 JS 产物")
	}

	var bundle strings.Builder
	for _, path := range assets {
		jsRec := httptest.NewRecorder()
		newMux().ServeHTTP(jsRec, httptest.NewRequest(http.MethodGet, path, nil))
		bundle.WriteString(jsRec.Body.String())
	}
	for _, needle := range []string{"概览", "订阅源", "新增订阅源"} {
		if !strings.Contains(bundle.String(), needle) {
			t.Fatalf("打包结果缺少文案 %q", needle)
		}
	}
}

func TestAdminPageIsNotCached(t *testing.T) {
	rec := httptest.NewRecorder()
	newMux().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/admin/", nil))

	if got := rec.Header().Get("Cache-Control"); got != "no-cache" {
		t.Fatalf("Cache-Control = %q, want no-cache", got)
	}
}

func TestAdminUnknownAssetIs404(t *testing.T) {
	rec := httptest.NewRecorder()
	newMux().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/admin/missing.css", nil))

	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

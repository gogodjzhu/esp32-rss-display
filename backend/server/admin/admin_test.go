package admin

import (
	"net/http"
	"net/http/httptest"
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

func TestAdminServesPageAndAssets(t *testing.T) {
	cases := []struct {
		path    string
		needles []string
	}{
		{"/admin/", []string{"订阅源", "app.js", "admin.css"}},
		{"/admin/app.js", []string{"loadAll", "/v1/feeds"}},
		{"/admin/admin.css", []string{".grid"}},
	}

	for _, tc := range cases {
		rec := httptest.NewRecorder()
		newMux().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, tc.path, nil))

		if rec.Code != http.StatusOK {
			t.Fatalf("%s: status = %d, want 200", tc.path, rec.Code)
		}
		body := rec.Body.String()
		for _, needle := range tc.needles {
			if !strings.Contains(body, needle) {
				t.Fatalf("%s: body 缺少 %q", tc.path, needle)
			}
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

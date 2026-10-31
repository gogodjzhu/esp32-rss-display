package admin

import (
	"embed"
	"io/fs"
	"net/http"
	"strings"
)

// 管理页静态资源随二进制发布：容器里不需要额外的目录或前端构建链。
//
//go:embed static
var staticFS embed.FS

// Mount 注册管理页路由。/admin 重定向到 /admin/，其余交给静态文件服务。
// 鉴权不在应用内做，由前置反代（Caddy basic_auth）罩住 /admin 前缀。
func Mount(mux *http.ServeMux) {
	sub, err := fs.Sub(staticFS, "static")
	if err != nil {
		panic(err)
	}
	files := http.FileServer(http.FS(sub))

	mux.HandleFunc("GET /admin", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/admin/", http.StatusMovedPermanently)
	})
	mux.Handle("GET /admin/", noCache(http.StripPrefix("/admin/", files)))
}

// noCache 避免升级后浏览器继续用旧页面。
func noCache(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, ".html") || r.URL.Path == "/" || strings.HasSuffix(r.URL.Path, "/") {
			w.Header().Set("Cache-Control", "no-cache")
		}
		next.ServeHTTP(w, r)
	})
}

package rssworker

// 图片处理测试：验证下载→居中裁剪→缩放→内容哈希落盘的链路，
// 以及同一张图重复处理时只落一份文件、返回相对路径。

import (
	"image"
	"image/color"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
)

func testWorker(t *testing.T) *Worker {
	t.Helper()
	cfg := &config.RSSConfig{
		FetchIntervalMinutes: 15,
		ImageWidth:           320,
		ImageHeight:          240,
		ImageResizeFilter:    "catmullrom",
	}
	return New(cfg, t.TempDir())
}

func TestDownloadAndResizeImage(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 800, 200))
	for y := 0; y < 200; y++ {
		for x := 0; x < 800; x++ {
			src.Set(x, y, color.RGBA{R: uint8(x % 256), G: 128, B: 64, A: 255})
		}
	}

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("User-Agent") == "" {
			t.Errorf("请求应带 User-Agent")
		}
		w.Header().Set("Content-Type", "image/png")
		png.Encode(w, src)
	}))
	defer srv.Close()

	worker := testWorker(t)
	path := worker.downloadAndResizeImage(srv.URL+"/a.png", time.Now())
	if path == "" {
		t.Fatal("expected image path, got empty")
	}
	if filepath.IsAbs(path) {
		t.Fatalf("image_path 应存相对路径, got %s", path)
	}

	full := filepath.Join(worker.imageDir, path)
	f, err := os.Open(full)
	if err != nil {
		t.Fatalf("open saved image: %v", err)
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		t.Fatalf("decode saved image: %v", err)
	}
	if got := img.Bounds().Size(); got.X != 320 || got.Y != 240 {
		t.Fatalf("resized size = %v, want 320x240", got)
	}

	// 同一张图再处理一次应复用同一文件
	again := worker.downloadAndResizeImage(srv.URL+"/a.png", time.Now())
	if again != path {
		t.Fatalf("重复图片应复用同一文件，got %s want %s", again, path)
	}
	entries, err := os.ReadDir(filepath.Dir(full))
	if err != nil {
		t.Fatalf("read image dir: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("图片目录应只有 1 个文件, got %d", len(entries))
	}
}

func TestDownloadAndResizeImageRejectsBadResponses(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/404":
			http.NotFound(w, r)
		case "/html":
			w.Header().Set("Content-Type", "text/html")
			w.Write([]byte("<html>not an image</html>"))
		}
	}))
	defer srv.Close()

	worker := testWorker(t)
	for _, u := range []string{srv.URL + "/404", srv.URL + "/html", ""} {
		if got := worker.downloadAndResizeImage(u, time.Now()); got != "" {
			t.Fatalf("%s 应返回空路径, got %s", u, got)
		}
	}
}

package image

import (
	"bytes"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/cover"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

func setupTestDB(t *testing.T) {
	t.Helper()
	if database.GetDB() != nil {
		return
	}
	cfg := &config.DatabaseConfig{Driver: "sqlite", DSN: "file::memory:?cache=shared"}
	if err := database.Init(cfg); err != nil {
		t.Fatalf("init db: %v", err)
	}
}

func createFeedItem(t *testing.T, feed models.Feed, imagePath string) models.Item {
	t.Helper()
	db := database.GetDB()
	if err := db.Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}
	item := models.Item{FeedID: feed.ID, Title: "t", URL: "http://example.com/" + feed.Name, ImagePath: imagePath}
	if err := db.Create(&item).Error; err != nil {
		t.Fatalf("create item: %v", err)
	}
	return item
}

func serveItem(h *Handler, itemID uint) *httptest.ResponseRecorder {
	req := httptest.NewRequest(http.MethodGet, "/image/"+itoa(itemID)+".jpg", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func itoa(v uint) string {
	if v == 0 {
		return "0"
	}
	var buf [20]byte
	i := len(buf)
	for v > 0 {
		i--
		buf[i] = byte('0' + v%10)
		v /= 10
	}
	return string(buf[i:])
}

func TestItemWithoutImageFallsBackToGeneratedCover(t *testing.T) {
	setupTestDB(t)
	dir := t.TempDir()
	item := createFeedItem(t, models.Feed{Name: "少数派", URL: "http://example.com/feed-a", Enabled: true}, "")

	rec := serveItem(New(dir), item.ID)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	if ct := rec.Header().Get("Content-Type"); ct != "image/jpeg" {
		t.Fatalf("Content-Type = %q", ct)
	}
	img, format, err := image.Decode(bytes.NewReader(rec.Body.Bytes()))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if format != "jpeg" {
		t.Fatalf("format = %s", format)
	}
	if got := img.Bounds().Size(); got.X != cover.DefaultWidth || got.Y != cover.DefaultHeight {
		t.Fatalf("size = %v", got)
	}

	generated := filepath.Join(dir, cover.GeneratedRelPath("少数派"))
	if _, err := os.Stat(generated); err != nil {
		t.Fatalf("generated cover not written: %v", err)
	}
}

func TestItemImageWinsOverCover(t *testing.T) {
	setupTestDB(t)
	dir := t.TempDir()
	dayDir := filepath.Join(dir, "20260101")
	if err := os.MkdirAll(dayDir, 0755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	src := image.NewRGBA(image.Rect(0, 0, 320, 240))
	for y := 0; y < 240; y++ {
		for x := 0; x < 320; x++ {
			src.Set(x, y, color.RGBA{R: 200, G: 10, B: 10, A: 255})
		}
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, src, nil); err != nil {
		t.Fatalf("encode: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dayDir, "item.jpg"), buf.Bytes(), 0644); err != nil {
		t.Fatalf("write: %v", err)
	}

	item := createFeedItem(t, models.Feed{Name: "With Image", URL: "http://example.com/feed-b", Enabled: true}, "20260101/item.jpg")
	rec := serveItem(New(dir), item.ID)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	if !bytes.Equal(rec.Body.Bytes(), buf.Bytes()) {
		t.Fatal("应优先返回条目自身图片")
	}
}

func TestCustomCoverWinsOverGenerated(t *testing.T) {
	setupTestDB(t)
	dir := t.TempDir()

	src := image.NewRGBA(image.Rect(0, 0, 640, 480))
	for y := 0; y < 480; y++ {
		for x := 0; x < 640; x++ {
			src.Set(x, y, color.RGBA{R: 10, G: 200, B: 20, A: 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, src); err != nil {
		t.Fatalf("encode png: %v", err)
	}
	rel, err := cover.SaveUpload(dir, buf.Bytes())
	if err != nil {
		t.Fatalf("save upload: %v", err)
	}

	feed := models.Feed{Name: "Custom", URL: "http://example.com/feed-c", Enabled: true, CoverImagePath: rel}
	item := createFeedItem(t, feed, "")
	rec := serveItem(New(dir), item.ID)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}

	want, err := os.ReadFile(filepath.Join(dir, rel))
	if err != nil {
		t.Fatalf("read cover: %v", err)
	}
	if !bytes.Equal(rec.Body.Bytes(), want) {
		t.Fatal("应优先返回自定义封面")
	}
}

func TestFeedHandlerServesCurrentCover(t *testing.T) {
	setupTestDB(t)
	dir := t.TempDir()
	feed := models.Feed{Name: "Feed Cover", URL: "http://example.com/feed-d", Enabled: true}
	if err := database.GetDB().Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}

	req := httptest.NewRequest(http.MethodGet, "/image/feed/"+itoa(feed.ID)+".jpg", nil)
	rec := httptest.NewRecorder()
	NewFeed(dir).ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	if _, format, err := image.Decode(bytes.NewReader(rec.Body.Bytes())); err != nil || format != "jpeg" {
		t.Fatalf("decode: format=%s err=%v", format, err)
	}
}

func TestMissingItemIs404(t *testing.T) {
	setupTestDB(t)
	rec := serveItem(New(t.TempDir()), 999999)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

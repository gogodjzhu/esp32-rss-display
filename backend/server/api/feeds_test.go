package api

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/esp32-rss-display/backend/server/cover"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
)

func coverRequestBody(t *testing.T, data []byte) (*bytes.Buffer, string) {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	part, err := writer.CreateFormFile("image", "cover.png")
	if err != nil {
		t.Fatalf("create form file: %v", err)
	}
	if _, err := part.Write(data); err != nil {
		t.Fatalf("write form file: %v", err)
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("close writer: %v", err)
	}
	return &body, writer.FormDataContentType()
}

func testPNG(t *testing.T) []byte {
	t.Helper()
	src := image.NewRGBA(image.Rect(0, 0, 640, 480))
	for y := 0; y < 480; y++ {
		for x := 0; x < 640; x++ {
			src.Set(x, y, color.RGBA{R: uint8(x % 256), G: 120, B: uint8(y % 256), A: 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, src); err != nil {
		t.Fatalf("encode png: %v", err)
	}
	return buf.Bytes()
}

func TestUploadAndDeleteFeedCover(t *testing.T) {
	setupTestDB(t)
	dir := t.TempDir()
	oldDir := ImageDir()
	SetImageDir(dir)
	t.Cleanup(func() { SetImageDir(oldDir) })

	feed := models.Feed{Name: "Cover Feed", URL: "http://example.com/cover-feed", Enabled: true}
	if err := database.GetDB().Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}

	body, contentType := coverRequestBody(t, testPNG(t))
	req := httptest.NewRequest(http.MethodPost, "/v1/feeds/"+strconv.FormatUint(uint64(feed.ID), 10)+"/cover", body)
	req.Header.Set("Content-Type", contentType)
	req.SetPathValue("id", strconv.FormatUint(uint64(feed.ID), 10))
	rec := httptest.NewRecorder()
	UploadFeedCover(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}

	var updated models.Feed
	if err := database.GetDB().First(&updated, feed.ID).Error; err != nil {
		t.Fatalf("reload feed: %v", err)
	}
	if updated.CoverImagePath == "" {
		t.Fatal("cover_image_path should be set")
	}
	imgPath := filepath.Join(dir, updated.CoverImagePath)
	f, err := os.Open(imgPath)
	if err != nil {
		t.Fatalf("open cover: %v", err)
	}
	defer f.Close()
	img, format, err := image.Decode(f)
	if err != nil {
		t.Fatalf("decode cover: %v", err)
	}
	if format != "jpeg" {
		t.Fatalf("format = %s, want jpeg", format)
	}
	if got := img.Bounds().Size(); got.X != cover.DefaultWidth || got.Y != cover.DefaultHeight {
		t.Fatalf("size = %v", got)
	}

	delReq := httptest.NewRequest(http.MethodDelete, "/v1/feeds/"+strconv.FormatUint(uint64(feed.ID), 10)+"/cover", nil)
	delReq.SetPathValue("id", strconv.FormatUint(uint64(feed.ID), 10))
	delRec := httptest.NewRecorder()
	DeleteFeedCover(delRec, delReq)
	if delRec.Code != http.StatusOK {
		t.Fatalf("delete status = %d, body = %s", delRec.Code, delRec.Body.String())
	}
	if err := database.GetDB().First(&updated, feed.ID).Error; err != nil {
		t.Fatalf("reload feed: %v", err)
	}
	if updated.CoverImagePath != "" {
		t.Fatalf("cover_image_path should be cleared, got %q", updated.CoverImagePath)
	}
}

func TestUploadFeedCoverRejectsInvalidImage(t *testing.T) {
	setupTestDB(t)
	oldDir := ImageDir()
	SetImageDir(t.TempDir())
	t.Cleanup(func() { SetImageDir(oldDir) })

	feed := models.Feed{Name: "Bad Cover", URL: "http://example.com/bad-cover", Enabled: true}
	if err := database.GetDB().Create(&feed).Error; err != nil {
		t.Fatalf("create feed: %v", err)
	}

	body, contentType := coverRequestBody(t, []byte("<html>not an image</html>"))
	req := httptest.NewRequest(http.MethodPost, "/v1/feeds/"+strconv.FormatUint(uint64(feed.ID), 10)+"/cover", body)
	req.Header.Set("Content-Type", contentType)
	req.SetPathValue("id", strconv.FormatUint(uint64(feed.ID), 10))
	rec := httptest.NewRecorder()
	UploadFeedCover(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
}

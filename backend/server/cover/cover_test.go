package cover

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestGenerateProducesJPEG(t *testing.T) {
	data, err := Generate("少数派")
	if err != nil {
		t.Fatalf("generate: %v", err)
	}
	img, format, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if format != "jpeg" {
		t.Fatalf("format = %s, want jpeg", format)
	}
	if got := img.Bounds().Size(); got.X != DefaultWidth || got.Y != DefaultHeight {
		t.Fatalf("size = %v, want %dx%d", got, DefaultWidth, DefaultHeight)
	}
	if len(data) > MaxJPEGBytes {
		t.Fatalf("cover size = %d, want <= %d", len(data), MaxJPEGBytes)
	}

	again, err := Generate("少数派")
	if err != nil {
		t.Fatalf("generate again: %v", err)
	}
	if !bytes.Equal(data, again) {
		t.Fatal("同名封面应稳定生成相同字节")
	}

	other, err := Generate("HackerNews")
	if err != nil {
		t.Fatalf("generate other: %v", err)
	}
	if bytes.Equal(data, other) {
		t.Fatal("不同名称应生成不同封面")
	}
}

func TestNormalizeCenterCrops(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 800, 200))
	for y := 0; y < 200; y++ {
		for x := 0; x < 800; x++ {
			src.Set(x, y, color.RGBA{R: uint8(x % 256), G: 90, B: 40, A: 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, src); err != nil {
		t.Fatalf("encode png: %v", err)
	}

	data, err := Normalize(buf.Bytes())
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	img, format, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if format != "jpeg" {
		t.Fatalf("format = %s, want jpeg", format)
	}
	if got := img.Bounds().Size(); got.X != DefaultWidth || got.Y != DefaultHeight {
		t.Fatalf("size = %v, want %dx%d", got, DefaultWidth, DefaultHeight)
	}
}

func TestNormalizeRejectsInvalidImage(t *testing.T) {
	if _, err := Normalize([]byte("<html>not an image</html>")); err == nil {
		t.Fatal("expected error for invalid image")
	}
}

func TestEnsureGeneratedCachesFile(t *testing.T) {
	dir := t.TempDir()
	path, err := EnsureGenerated(dir, "Example Feed")
	if err != nil {
		t.Fatalf("ensure generated: %v", err)
	}
	if filepath.Dir(path) != filepath.Join(dir, DirName) {
		t.Fatalf("unexpected path: %s", path)
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("cached file missing: %v", err)
	}

	again, err := EnsureGenerated(dir, "Example Feed")
	if err != nil {
		t.Fatalf("ensure generated again: %v", err)
	}
	if again != path {
		t.Fatalf("path changed: %s -> %s", path, again)
	}
}

func TestCleanupRemovesUnreferencedCovers(t *testing.T) {
	dir := t.TempDir()
	keepPath, err := EnsureGenerated(dir, "keep")
	if err != nil {
		t.Fatalf("ensure keep: %v", err)
	}
	dropPath, err := EnsureGenerated(dir, "drop")
	if err != nil {
		t.Fatalf("ensure drop: %v", err)
	}

	removed, err := Cleanup(dir, map[string]struct{}{
		filepath.Base(keepPath): {},
	})
	if err != nil {
		t.Fatalf("cleanup: %v", err)
	}
	if removed != 1 {
		t.Fatalf("removed = %d, want 1", removed)
	}
	if _, err := os.Stat(keepPath); err != nil {
		t.Fatalf("kept cover missing: %v", err)
	}
	if _, err := os.Stat(dropPath); !os.IsNotExist(err) {
		t.Fatalf("unreferenced cover should be removed, err=%v", err)
	}
}

func TestSaveUploadReusesContentHash(t *testing.T) {
	dir := t.TempDir()
	src := image.NewRGBA(image.Rect(0, 0, 640, 480))
	for y := 0; y < 480; y++ {
		for x := 0; x < 640; x++ {
			src.Set(x, y, color.RGBA{R: 10, G: uint8(y % 256), B: uint8(x % 256), A: 255})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, src); err != nil {
		t.Fatalf("encode png: %v", err)
	}

	rel, err := SaveUpload(dir, buf.Bytes())
	if err != nil {
		t.Fatalf("save upload: %v", err)
	}
	again, err := SaveUpload(dir, buf.Bytes())
	if err != nil {
		t.Fatalf("save upload again: %v", err)
	}
	if again != rel {
		t.Fatalf("same image should reuse path: %s -> %s", rel, again)
	}
	entries, err := os.ReadDir(filepath.Join(dir, DirName))
	if err != nil {
		t.Fatalf("read covers: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("covers dir should contain 1 file, got %d", len(entries))
	}
}

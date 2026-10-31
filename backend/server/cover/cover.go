// Package cover 负责生成和保存订阅源封面图。
//
// 条目没有可用图片时，后端用订阅源名称生成一张确定性的封面图作为兜底；
// 用户也可以在管理页上传自定义封面。所有封面都会裁剪成设备需要的
// 目标尺寸（默认 320×240）并以 JPEG 落盘，因此 next 永远能给出 image_url。
package cover

import (
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/fnv"
	"image"
	"image/color"
	"image/jpeg"
	_ "image/png"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"unicode"

	"github.com/esp32-rss-display/backend/server/config"
	xdraw "golang.org/x/image/draw"
	"golang.org/x/image/font"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
)

const (
	// 默认尺寸与固件要求一致：320×240 JPEG。
	DefaultWidth  = 320
	DefaultHeight = 240
	// coverVersion 参与生成图缓存命名；修改绘制逻辑时递增可让旧缓存失效。
	coverVersion = "v1"
	// DirName 是 imageDir 下存放封面的子目录。
	DirName = "covers"
	// MaxJPEGBytes 与固件图片上限一致，超过会被设备判为非法响应。
	MaxJPEGBytes = 32 * 1024
	jpegQuality  = 85
)

//go:embed assets/cjk_subset.ttf
var fontData []byte

var (
	fontOnce sync.Once
	fontFace *opentype.Font
	fontErr  error
)

// Dimensions 返回封面目标尺寸；配置未加载或非法时回退 320×240。
func Dimensions() (int, int) {
	width, height := DefaultWidth, DefaultHeight
	if config.AppConfig != nil {
		if config.AppConfig.RSS.ImageWidth > 0 {
			width = config.AppConfig.RSS.ImageWidth
		}
		if config.AppConfig.RSS.ImageHeight > 0 {
			height = config.AppConfig.RSS.ImageHeight
		}
	}
	return width, height
}

// GeneratedRelPath 返回按订阅源名称生成的封面相对路径。
// 名称相同即得到同一张图，重命名后自然切换到新缓存。
func GeneratedRelPath(name string) string {
	width, height := Dimensions()
	key := fmt.Sprintf("%s|%dx%d|%s", coverVersion, width, height, strings.TrimSpace(name))
	sum := sha256.Sum256([]byte(key))
	return filepath.Join(DirName, "generated-"+hex.EncodeToString(sum[:8])+".jpg")
}

// EnsureGenerated 确保名称对应的生成封面已落盘，返回绝对路径。
func EnsureGenerated(imageDir, name string) (string, error) {
	if strings.TrimSpace(imageDir) == "" {
		return "", errors.New("图片目录未配置")
	}
	abs := filepath.Join(imageDir, GeneratedRelPath(name))
	if info, err := os.Stat(abs); err == nil && !info.IsDir() {
		return abs, nil
	}

	data, err := Generate(name)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Dir(abs), 0755); err != nil {
		return "", err
	}
	if err := os.WriteFile(abs, data, 0644); err != nil {
		return "", err
	}
	return abs, nil
}

// SaveUpload 校验并归一化用户上传的封面，按内容哈希落盘，返回相对 imageDir 的路径。
func SaveUpload(imageDir string, data []byte) (string, error) {
	if strings.TrimSpace(imageDir) == "" {
		return "", errors.New("图片目录未配置")
	}
	normalized, err := Normalize(data)
	if err != nil {
		return "", err
	}

	sum := sha256.Sum256(normalized)
	rel := filepath.Join(DirName, "upload-"+hex.EncodeToString(sum[:8])+".jpg")
	abs := filepath.Join(imageDir, rel)
	if info, err := os.Stat(abs); err == nil && !info.IsDir() {
		return rel, nil
	}
	if err := os.MkdirAll(filepath.Dir(abs), 0755); err != nil {
		return "", err
	}
	if err := os.WriteFile(abs, normalized, 0644); err != nil {
		return "", err
	}
	return rel, nil
}

// Cleanup 删除 covers 目录中不再被任何订阅源使用的封面文件。
// keep 存放需要保留的文件名（不含目录），返回删除数量。
func Cleanup(imageDir string, keep map[string]struct{}) (int, error) {
	dir := filepath.Join(imageDir, DirName)
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return 0, nil
		}
		return 0, err
	}

	removed := 0
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(strings.ToLower(entry.Name()), ".jpg") {
			continue
		}
		if _, ok := keep[entry.Name()]; ok {
			continue
		}
		if err := os.Remove(filepath.Join(dir, entry.Name())); err != nil {
			return removed, err
		}
		removed++
	}
	return removed, nil
}

// Normalize 解码 JPEG/PNG，居中裁剪并缩放到目标尺寸后重新编码为 JPEG。
func Normalize(data []byte) ([]byte, error) {
	img, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, errors.New("不支持的图片格式，请上传 JPEG 或 PNG")
	}
	width, height := Dimensions()
	resized := centerCrop(img, width, height)
	return encodeJPEG(resized)
}

// Generate 按订阅源名称生成一张确定性的渐变封面图。
func Generate(name string) ([]byte, error) {
	width, height := Dimensions()
	name = sanitizeName(name)
	if name == "" {
		name = "RSS"
	}

	img := image.NewRGBA(image.Rect(0, 0, width, height))
	seed := hashName(name)
	hue := float64(seed % 360)
	base := hsl(hue, 0.55, 0.28)
	accent := hsl(math.Mod(hue+38, 360), 0.62, 0.46)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			t := (float64(x)/float64(maxInt(width-1, 1)) + float64(y)/float64(maxInt(height-1, 1))) / 2
			img.SetRGBA(x, y, blend(base, accent, t))
		}
	}
	drawDecoration(img, int(seed>>8), width, height)

	if err := drawCoverText(img, name); err != nil {
		// 字体渲染失败时仍返回渐变底图，保证前端永远有一张可用的图。
		return encodeJPEG(img)
	}
	return encodeJPEG(img)
}

func encodeJPEG(img image.Image) ([]byte, error) {
	// 从画质优先逐级降到固件可接受的体积以内；极端噪点图仍超限时返回错误，
	// 避免把设备无法解码的图片写进封面。
	for _, quality := range []int{jpegQuality, 80, 72, 64, 55, 45, 35, 25, 15, 5} {
		var buf bytes.Buffer
		if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: quality}); err != nil {
			return nil, err
		}
		if buf.Len() <= MaxJPEGBytes {
			return buf.Bytes(), nil
		}
	}
	return nil, fmt.Errorf("封面压缩后仍超过 %d KB，请换一张更简单的图片", MaxJPEGBytes/1024)
}

func loadFont() (*opentype.Font, error) {
	fontOnce.Do(func() {
		fontFace, fontErr = opentype.Parse(fontData)
	})
	return fontFace, fontErr
}

// drawCoverText 在封面中央绘制名称；按可用宽高自动换行并缩小字号。
func drawCoverText(dst *image.RGBA, name string) error {
	f, err := loadFont()
	if err != nil {
		return err
	}

	width, height := dst.Bounds().Dx(), dst.Bounds().Dy()
	maxWidth := width - 48
	maxHeight := height - 40
	if maxWidth < 40 || maxHeight < 20 {
		return errors.New("封面尺寸过小")
	}

	sizes := []float64{44, 40, 36, 32, 28, 24, 20, 17}
	var face font.Face
	var lines []string
	var lineHeight int
	for _, size := range sizes {
		candidate, err := opentype.NewFace(f, &opentype.FaceOptions{
			Size:    size,
			DPI:     72,
			Hinting: font.HintingFull,
		})
		if err != nil {
			continue
		}
		candidateLines := wrapText(name, maxWidth, func(s string) int {
			return font.MeasureString(candidate, s).Ceil()
		})
		metrics := candidate.Metrics()
		candidateLineHeight := metrics.Height.Ceil() + 6
		if len(candidateLines) <= 3 && candidateLineHeight*len(candidateLines) <= maxHeight {
			if face != nil {
				face.Close()
			}
			face = candidate
			lines = candidateLines
			lineHeight = candidateLineHeight
			break
		}
		candidate.Close()
	}
	if face == nil {
		return errors.New("无法创建字体")
	}
	defer face.Close()

	metrics := face.Metrics()
	ascent := metrics.Ascent.Ceil()
	blockHeight := lineHeight*len(lines) - 6
	top := (height - blockHeight) / 2

	for i, line := range lines {
		lineWidth := font.MeasureString(face, line).Ceil()
		x := (width - lineWidth) / 2
		y := top + i*lineHeight + ascent

		shadow := &font.Drawer{
			Dst:  dst,
			Src:  image.NewUniform(color.RGBA{R: 0, G: 0, B: 0, A: 110}),
			Face: face,
			Dot:  fixed.P(x+2, y+2),
		}
		shadow.DrawString(line)

		drawer := &font.Drawer{
			Dst:  dst,
			Src:  image.NewUniform(color.RGBA{R: 255, G: 255, B: 255, A: 255}),
			Face: face,
			Dot:  fixed.P(x, y),
		}
		drawer.DrawString(line)
	}
	return nil
}

// wrapText 按可用宽度逐字符换行，优先在空白处断开；最多返回 3 行。
func wrapText(text string, maxWidth int, measure func(string) int) []string {
	const maxLines = 3
	runes := []rune(text)
	var lines []string
	var current []rune

	for i := 0; i < len(runes); i++ {
		r := runes[i]
		if r == '\n' {
			if len(current) > 0 {
				lines = append(lines, strings.TrimSpace(string(current)))
				current = current[:0]
			}
			continue
		}

		candidate := append(current, r)
		if len(current) > 0 && measure(string(candidate)) > maxWidth {
			// 英文尽量在空格处断行，中文则直接按字符断开。
			cut := len(current)
			for j := len(current) - 1; j >= 0; j-- {
				if unicode.IsSpace(current[j]) {
					cut = j
					break
				}
			}
			line := current
			var rest []rune
			if cut < len(current) {
				line = current[:cut]
				rest = append([]rune{}, current[cut+1:]...)
			}
			lines = append(lines, strings.TrimSpace(string(line)))
			current = rest
			if len(lines) >= maxLines {
				// 还要继续写时，把剩余内容压进最后一行并在末尾加省略号。
				tail := string(current) + string(runes[i:])
				lines[maxLines-1] = truncateWithEllipsis(lines[maxLines-1]+tail, maxWidth, measure)
				return lines
			}
		}
		current = append(current, r)
	}

	if len(current) > 0 {
		lines = append(lines, strings.TrimSpace(string(current)))
	}
	if len(lines) > maxLines {
		lines = lines[:maxLines]
		lines[maxLines-1] = truncateWithEllipsis(lines[maxLines-1], maxWidth, measure)
	}
	return lines
}

// truncateWithEllipsis 截断到可用宽度，保留省略号。
func truncateWithEllipsis(text string, maxWidth int, measure func(string) int) string {
	const ellipsis = "…"
	runes := []rune(text)
	if len(runes) == 0 {
		return ellipsis
	}
	for len(runes) > 0 && measure(string(runes)+ellipsis) > maxWidth {
		runes = runes[:len(runes)-1]
	}
	return string(runes) + ellipsis
}

func sanitizeName(name string) string {
	name = strings.TrimSpace(name)
	var b strings.Builder
	for _, r := range name {
		if unicode.IsControl(r) {
			continue
		}
		b.WriteRune(r)
	}
	return strings.TrimSpace(b.String())
}

func hashName(name string) uint64 {
	h := fnv.New64a()
	_, _ = h.Write([]byte(name))
	return h.Sum64()
}

// hsl 把 HSL 颜色转成 RGBA，只用于生成背景渐变。
func hsl(h, s, l float64) color.RGBA {
	h = math.Mod(h, 360) / 360
	if h < 0 {
		h += 1
	}
	c := (1 - math.Abs(2*l-1)) * s
	x := c * (1 - math.Abs(math.Mod(h*6, 2)-1))
	m := l - c/2
	var r, g, b float64
	switch {
	case h < 1.0/6:
		r, g, b = c, x, 0
	case h < 2.0/6:
		r, g, b = x, c, 0
	case h < 3.0/6:
		r, g, b = 0, c, x
	case h < 4.0/6:
		r, g, b = 0, x, c
	case h < 5.0/6:
		r, g, b = x, 0, c
	default:
		r, g, b = c, 0, x
	}
	return color.RGBA{
		R: uint8(math.Round((r + m) * 255)),
		G: uint8(math.Round((g + m) * 255)),
		B: uint8(math.Round((b + m) * 255)),
		A: 255,
	}
}

func blend(a, b color.RGBA, t float64) color.RGBA {
	if t < 0 {
		t = 0
	}
	if t > 1 {
		t = 1
	}
	return color.RGBA{
		R: uint8(float64(a.R) + (float64(b.R)-float64(a.R))*t),
		G: uint8(float64(a.G) + (float64(b.G)-float64(a.G))*t),
		B: uint8(float64(a.B) + (float64(b.B)-float64(a.B))*t),
		A: 255,
	}
}

// drawDecoration 添加两个低对比度圆形，让不同来源的封面更容易区分。
func drawDecoration(dst *image.RGBA, seed int, width, height int) {
	circles := []struct {
		cx, cy, r int
		alpha     uint8
	}{
		{width/4 + seed%80 - 40, height/3 + (seed>>4)%50 - 25, minInt(width, height) / 3, 28},
		{width*3/4 + (seed>>8)%60 - 30, height*2/3 + (seed>>12)%40 - 20, minInt(width, height) / 4, 22},
	}
	for _, c := range circles {
		for y := c.cy - c.r; y <= c.cy+c.r; y++ {
			if y < 0 || y >= height {
				continue
			}
			for x := c.cx - c.r; x <= c.cx+c.r; x++ {
				if x < 0 || x >= width {
					continue
				}
				dx, dy := x-c.cx, y-c.cy
				if dx*dx+dy*dy > c.r*c.r {
					continue
				}
				dst.SetRGBA(x, y, overlay(dst.RGBAAt(x, y), color.RGBA{255, 255, 255, c.alpha}))
			}
		}
	}
}

func overlay(dst, src color.RGBA) color.RGBA {
	a := float64(src.A) / 255
	return color.RGBA{
		R: uint8(float64(src.R)*a + float64(dst.R)*(1-a)),
		G: uint8(float64(src.G)*a + float64(dst.G)*(1-a)),
		B: uint8(float64(src.B)*a + float64(dst.B)*(1-a)),
		A: 255,
	}
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// centerCrop 按目标宽高比居中裁剪后缩放到目标尺寸，避免比例失真。
func centerCrop(src image.Image, width, height int) image.Image {
	b := src.Bounds()
	srcW, srcH := b.Dx(), b.Dy()
	dst := image.NewRGBA(image.Rect(0, 0, width, height))
	if srcW == 0 || srcH == 0 {
		return dst
	}

	srcAR := float64(srcW) / float64(srcH)
	dstAR := float64(width) / float64(height)

	crop := b
	if srcAR > dstAR {
		cropW := int(float64(srcH) * dstAR)
		if cropW > srcW {
			cropW = srcW
		}
		x0 := b.Min.X + (srcW-cropW)/2
		crop = image.Rect(x0, b.Min.Y, x0+cropW, b.Max.Y)
	} else if srcAR < dstAR {
		cropH := int(float64(srcW) / dstAR)
		if cropH > srcH {
			cropH = srcH
		}
		y0 := b.Min.Y + (srcH-cropH)/2
		crop = image.Rect(b.Min.X, y0, b.Max.X, y0+cropH)
	}

	xdraw.CatmullRom.Scale(dst, dst.Bounds(), src, crop, xdraw.Over, nil)
	return dst
}

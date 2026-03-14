package rssworker

import (
	"bytes"
	"fmt"
	"image"
	"image/jpeg"
	_ "image/png"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/metrics"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/mattn/go-isatty"
	"github.com/mmcdole/gofeed"
	"golang.org/x/image/draw"
	"golang.org/x/net/html"
)

type Worker struct {
	fetchInterval time.Duration
	imageWidth    int
	imageHeight   int
	imageDir      string
	stopCh        chan struct{}
}

func New(cfg *config.RSSConfig, imageDir string) *Worker {
	return &Worker{
		fetchInterval: time.Duration(cfg.FetchIntervalMinutes) * time.Minute,
		imageWidth:    cfg.ImageWidth,
		imageHeight:   cfg.ImageHeight,
		imageDir:      imageDir,
		stopCh:        make(chan struct{}),
	}
}

func (w *Worker) Start() {
	go w.run()
}

func (w *Worker) Stop() {
	close(w.stopCh)
}

func (w *Worker) run() {
	w.backfillImages()
	w.fetchAllFeeds()
	ticker := time.NewTicker(w.fetchInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			w.fetchAllFeeds()
		case <-w.stopCh:
			return
		}
	}
}

// backfillImages re-downloads images for items whose image_path is a remote URL
// (i.e. was never downloaded to a local file). This handles rows created by an
// older version of the code that skipped the download step.
func (w *Worker) backfillImages() {
	db := database.GetDB()
	var items []models.Item
	if err := db.Where("image_path LIKE 'http://%' OR image_path LIKE 'https://%'").Find(&items).Error; err != nil {
		log.Printf("[backfill] error querying items: %v", err)
		return
	}
	if len(items) == 0 {
		return
	}
	log.Printf("[backfill] re-downloading images for %d items", len(items))
	for _, item := range items {
		pubDate := item.CreatedAt
		if item.PublishedAt != nil {
			pubDate = *item.PublishedAt
		}
		localPath := w.downloadAndResizeImage(item.ImagePath, pubDate)
		if localPath == "" {
			log.Printf("[backfill] failed to download image for item %d: %s", item.ID, item.ImagePath)
			continue
		}
		if err := db.Model(&item).Update("image_path", localPath).Error; err != nil {
			log.Printf("[backfill] failed to update item %d: %v", item.ID, err)
		} else {
			log.Printf("[backfill] item %d image saved to %s", item.ID, localPath)
		}
	}
}

func (w *Worker) fetchAllFeeds() {
	db := database.GetDB()
	var feeds []models.Feed

	if err := db.Where("enabled = ?", true).Find(&feeds).Error; err != nil {
		log.Printf("Error fetching feeds: %v", err)
		metrics.RSSFetchError.Add(1)
		return
	}

	for _, feed := range feeds {
		w.fetchFeed(feed)
	}
}

func (w *Worker) fetchFeed(feed models.Feed) {
	metrics.RSSFetchTotal.Add(1)

	parser := gofeed.NewParser()
	parsed, err := parser.ParseURL(feed.URL)
	if err != nil {
		log.Printf("Error parsing feed %s: %v", feed.Name, err)
		metrics.RSSFetchError.Add(1)
		return
	}

	db := database.GetDB()

	for _, item := range parsed.Items {
		title := item.Title
		url := item.Link

		if url == "" {
			continue
		}

		var existingItem models.Item
		if err := db.Where("feed_id = ? AND url = ?", feed.ID, url).First(&existingItem).Error; err == nil {
			continue
		}

		imageURL := w.extractImage(item)

		var publishedAt *time.Time
		if item.PublishedParsed != nil {
			t := *item.PublishedParsed
			publishedAt = &t
		}

		pubDate := time.Now()
		if publishedAt != nil {
			pubDate = *publishedAt
		}
		imagePath := w.downloadAndResizeImage(imageURL, pubDate)

		newItem := models.Item{
			FeedID:      feed.ID,
			Title:       title,
			URL:         url,
			ImagePath:   imagePath,
			PublishedAt: publishedAt,
		}

		if err := db.Create(&newItem).Error; err != nil {
			log.Printf("Error saving item: %v", err)
			continue
		}

		metrics.RSSItemsParsedTotal.Add(1)
	}
}

// downloadAndResizeImage downloads the image at url, resizes it to the
// configured dimensions, and saves it under a YYYYMMDD sub-directory derived
// from date. Returns the full filesystem path on success, or "" on failure.
func (w *Worker) downloadAndResizeImage(url string, date time.Time) string {
	if url == "" {
		return ""
	}

	resp, err := http.Get(url)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return ""
	}

	img, format, err := image.Decode(resp.Body)
	if err != nil {
		return ""
	}

	if format != "jpeg" && format != "png" {
		return ""
	}

	resized := image.NewRGBA(image.Rect(0, 0, w.imageWidth, w.imageHeight))
	draw.NearestNeighbor.Scale(resized, resized.Bounds(), img, img.Bounds(), draw.Over, nil)

	var buf bytes.Buffer
	encoder := jpeg.Options{Quality: 80}
	if err := jpeg.Encode(&buf, resized, &encoder); err != nil {
		return ""
	}

	dateDir := filepath.Join(w.imageDir, date.UTC().Format("20060102"))
	if err := os.MkdirAll(dateDir, 0755); err != nil {
		return ""
	}

	id := fmt.Sprintf("%d", time.Now().UnixNano())
	filename := filepath.Join(dateDir, id+".jpg")
	if err := os.WriteFile(filename, buf.Bytes(), 0644); err != nil {
		return ""
	}

	return filename
}

func (w *Worker) extractImage(item *gofeed.Item) string {
	if item == nil {
		return ""
	}

	// Stage 1: <media:thumbnail> — used by BBC, Yahoo, and many major news feeds
	if imgURL := w.extractFromMediaThumbnail(item); imgURL != "" {
		log.Printf("[image] media:thumbnail: %s", imgURL)
		return imgURL
	}

	// Stage 2: <media:content> with medium="image" — another Media RSS variant
	if imgURL := w.extractFromMediaContent(item); imgURL != "" {
		log.Printf("[image] media:content: %s", imgURL)
		return imgURL
	}

	// Stage 3: gofeed native item.Image field
	if imgURL := w.extractFromItemImage(item); imgURL != "" {
		log.Printf("[image] item.Image: %s", imgURL)
		return imgURL
	}

	// Stage 4: RSS <enclosure type="image/...">
	if imgURL := w.extractFromEnclosures(item); imgURL != "" {
		log.Printf("[image] enclosure: %s", imgURL)
		return imgURL
	}

	// Stage 5: <img src> inside item HTML content / description
	content := item.Content
	if content == "" {
		content = item.Description
	}
	if imgURL := w.extractImgFromHTML(content); imgURL != "" {
		log.Printf("[image] html img: %s", imgURL)
		return imgURL
	}

	// Stage 6: iTunes-style <itunes:image href="...">
	if imgURL := w.extractFromITunes(item); imgURL != "" {
		log.Printf("[image] itunes:image: %s", imgURL)
		return imgURL
	}

	// Stage 7: scrape the linked article page for og:image / twitter:image
	if item.Link != "" {
		if imgURL := w.extractFromWebPage(item.Link); imgURL != "" {
			log.Printf("[image] webpage: %s", imgURL)
			return imgURL
		}
	}

	log.Printf("[image] no image found for item: %s", item.Link)
	return ""
}

// extractFromMediaThumbnail handles <media:thumbnail url="..."> used by BBC,
// Yahoo Media RSS, Reuters, and many other major news publishers.
func (w *Worker) extractFromMediaThumbnail(item *gofeed.Item) string {
	if item.Extensions == nil {
		return ""
	}
	mediaNS, ok := item.Extensions["media"]
	if !ok {
		return ""
	}
	thumbnails, ok := mediaNS["thumbnail"]
	if !ok || len(thumbnails) == 0 {
		return ""
	}
	// Pick the largest thumbnail if multiple are present
	best := ""
	bestArea := 0
	for _, t := range thumbnails {
		u := t.Attrs["url"]
		if !w.isValidImageURL(u) {
			continue
		}
		tw, th := parseIntAttr(t.Attrs["width"]), parseIntAttr(t.Attrs["height"])
		area := tw * th
		if area > bestArea || best == "" {
			best = u
			bestArea = area
		}
	}
	return best
}

// extractFromMediaContent handles <media:content url="..." medium="image">.
func (w *Worker) extractFromMediaContent(item *gofeed.Item) string {
	if item.Extensions == nil {
		return ""
	}
	mediaNS, ok := item.Extensions["media"]
	if !ok {
		return ""
	}
	contents, ok := mediaNS["content"]
	if !ok || len(contents) == 0 {
		return ""
	}
	best := ""
	bestArea := 0
	for _, c := range contents {
		medium := c.Attrs["medium"]
		typ := c.Attrs["type"]
		// Accept if medium="image" or type starts with "image/"
		if medium != "image" && !strings.HasPrefix(typ, "image/") {
			continue
		}
		u := c.Attrs["url"]
		if !w.isValidImageURL(u) {
			continue
		}
		pw, ph := parseIntAttr(c.Attrs["width"]), parseIntAttr(c.Attrs["height"])
		area := pw * ph
		if area > bestArea || best == "" {
			best = u
			bestArea = area
		}
	}
	return best
}

// extractFromItemImage uses gofeed's native item.Image field, which some
// parsers populate from <image> child elements inside an <item>.
func (w *Worker) extractFromItemImage(item *gofeed.Item) string {
	if item.Image != nil && w.isValidImageURL(item.Image.URL) {
		return item.Image.URL
	}
	return ""
}

// parseIntAttr converts a string attribute value to int, returning 0 on error.
func parseIntAttr(s string) int {
	n := 0
	for _, c := range s {
		if c < '0' || c > '9' {
			break
		}
		n = n*10 + int(c-'0')
	}
	return n
}

func (w *Worker) extractFromEnclosures(item *gofeed.Item) string {
	if item.Enclosures == nil || len(item.Enclosures) == 0 {
		return ""
	}

	for _, enc := range item.Enclosures {
		if strings.HasPrefix(enc.Type, "image/") && enc.URL != "" {
			return enc.URL
		}
	}

	return ""
}

func (w *Worker) extractFromITunes(item *gofeed.Item) string {
	content := item.Content
	if content == "" {
		content = item.Description
	}

	if content == "" {
		return ""
	}

	itunesImgPattern := regexp.MustCompile(`<itunes:image[^>]+href=["']([^"']+)["']`)
	if matches := itunesImgPattern.FindStringSubmatch(content); len(matches) > 1 {
		return matches[1]
	}

	return ""
}

func (w *Worker) extractImgFromHTML(htmlContent string) string {
	if htmlContent == "" {
		return ""
	}

	reader := strings.NewReader(htmlContent)
	tokenizer := html.NewTokenizer(reader)

	for {
		token := tokenizer.Next()
		if token == html.ErrorToken {
			break
		}

		if token == html.StartTagToken || token == html.SelfClosingTagToken {
			t := tokenizer.Token()
			if t.Data == "img" {
				for _, attr := range t.Attr {
					if attr.Key == "src" {
						url := attr.Val
						if w.isValidImageURL(url) {
							return url
						}
					}
					if attr.Key == "data-src" || attr.Key == "data-original" || attr.Key == "data-lazy-src" {
						url := attr.Val
						if w.isValidImageURL(url) {
							return url
						}
					}
				}
			}
		}
	}

	return ""
}

func (w *Worker) isValidImageURL(rawURL string) bool {
	if rawURL == "" {
		return false
	}
	if !strings.HasPrefix(rawURL, "http://") && !strings.HasPrefix(rawURL, "https://") {
		return false
	}
	// Check only the path component (before any '?') to avoid false positives
	// on query parameters like "?format=webp".
	path := rawURL
	if idx := strings.IndexByte(rawURL, '?'); idx != -1 {
		path = rawURL[:idx]
	}
	lower := strings.ToLower(path)
	// Reject formats that Go's image decoder cannot handle, or that are
	// generally unsuitable for display (animated gif, vector svg).
	if strings.HasSuffix(lower, ".gif") || strings.HasSuffix(lower, ".svg") {
		return false
	}
	return true
}

var (
	ogImagePattern   = regexp.MustCompile(`(?i)<meta[^>]+property=["']og:image["'][^>]+content=["']([^"']+)["']`)
	ogImagePattern2  = regexp.MustCompile(`(?i)<meta[^>]+content=["']([^"']+)["'][^>]+property=["']og:image["']`)
	twitterImagePat  = regexp.MustCompile(`(?i)<meta[^>]+name=["']twitter:image["'][^>]+content=["']([^"']+)["']`)
	twitterImagePat2 = regexp.MustCompile(`(?i)<meta[^>]+content=["']([^"']+)["'][^>]+name=["']twitter:image["']`)
	itemPropPattern  = regexp.MustCompile(`(?i)<meta[^>]+itemprop=["']image["'][^>]+content=["']([^"']+)["']`)
	itemPropPattern2 = regexp.MustCompile(`(?i)<meta[^>]+content=["']([^"']+)["'][^>]+itemprop=["']image["']`)
)

func (w *Worker) extractFromWebPage(url string) string {
	if url == "" {
		return ""
	}

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return ""
	}
	req.Header.Set("User-Agent", "Mozilla/5.0 (compatible; RSS-Bot/1.0)")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return ""
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 50000))
	if err != nil {
		return ""
	}

	htmlContent := string(body)

	patterns := []*regexp.Regexp{ogImagePattern, ogImagePattern2, twitterImagePat, twitterImagePat2, itemPropPattern, itemPropPattern2}
	for _, pattern := range patterns {
		if matches := pattern.FindStringSubmatch(htmlContent); len(matches) > 1 {
			url := matches[1]
			if w.isValidImageURL(url) {
				return url
			}
		}
	}

	if imgURL := w.extractImgFromHTML(htmlContent); imgURL != "" {
		return imgURL
	}

	return ""
}

func init() {
	if isatty.IsTerminal(os.Stderr.Fd()) {
		log.SetFlags(0)
		log.SetOutput(io.Discard)
	}
}

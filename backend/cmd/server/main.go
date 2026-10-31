package main

import (
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	"github.com/esp32-rss-display/backend/server/admin"
	"github.com/esp32-rss-display/backend/server/api"
	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/image"
	"github.com/esp32-rss-display/backend/server/models"
	rssworker "github.com/esp32-rss-display/backend/server/rss"
	"github.com/esp32-rss-display/backend/server/stocks"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/spf13/cobra"
)

var (
	configPath string
	imageDir   string
)

var rootCmd = &cobra.Command{
	Run: runServer,
}

func init() {
	rootCmd.Flags().StringVar(&configPath, "config", "config.yaml", "Path to config file")
	rootCmd.Flags().StringVar(&imageDir, "image-dir", "data/images", "Path to image directory")
}

func main() {
	if err := rootCmd.Execute(); err != nil {
		log.Fatal(err)
	}
}

func runServer(cmd *cobra.Command, args []string) {
	cfg, err := config.Load(configPath)
	if err != nil {
		log.Fatalf("Failed to load config: %v", err)
	}

	if err := os.MkdirAll(imageDir, 0755); err != nil {
		log.Fatalf("Failed to create image directory: %v", err)
	}

	if err := database.Init(&cfg.Database); err != nil {
		log.Fatalf("Failed to initialize database: %v", err)
	}

	api.SetImageDir(imageDir)
	initFeeds(cfg.Feeds)

	mux := http.NewServeMux()

	mux.Handle("/metrics", promhttp.Handler())
	mux.HandleFunc("GET /v1/device/{device_id}/next", api.GetNextItem)
	mux.HandleFunc("POST /v1/item/{item_id}/rating", api.RateItem)
	mux.HandleFunc("GET /v1/feeds", api.ListFeeds)
	mux.HandleFunc("POST /v1/feeds", api.CreateFeed)
	mux.HandleFunc("PATCH /v1/feeds/{id}", api.UpdateFeed)
	mux.HandleFunc("DELETE /v1/feeds/{id}", api.DeleteFeed)
	mux.HandleFunc("POST /v1/feeds/{id}/cover", api.UploadFeedCover)
	mux.HandleFunc("DELETE /v1/feeds/{id}/cover", api.DeleteFeedCover)
	mux.HandleFunc("GET /admin/status", api.AdminStatusHandler)
	admin.Mount(mux)
	image.Mount(mux, imageDir)
	mux.HandleFunc("GET /nfc/{device_id}", api.NFCRedirect)

	addr := fmt.Sprintf("%s:%d", cfg.Server.Host, cfg.Server.Port)
	log.Printf("Starting server on %s", addr)

	worker := rssworker.New(&cfg.RSS, imageDir)
	worker.Start()

	// 行情服务使用内存快照，启动失败或未配置时 next 响应不携带 stocks
	stockService := stocks.New(&cfg.Stocks)
	if stockService != nil {
		stockService.Start()
		api.StockQuotes = stockService
	}

	go func() {
		if err := http.ListenAndServe(addr, mux); err != nil {
			log.Fatalf("Server error: %v", err)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	worker.Stop()
	stockService.Stop()
	log.Println("Server stopped")
}

// initFeeds 把配置里的订阅源作为首次种子写入数据库：仅当 URL 不存在时插入。
// 已存在的源不再被 config.yaml 覆盖——页面上的改名、禁用必须能活过重启。
func initFeeds(feeds []config.FeedConfig) {
	db := database.GetDB()

	for _, f := range feeds {
		var existing models.Feed
		if err := db.Where("url = ?", f.URL).First(&existing).Error; err == nil {
			continue
		}
		if err := db.Create(&models.Feed{
			Name:    f.Name,
			URL:     f.URL,
			Enabled: f.Enabled,
		}).Error; err != nil {
			log.Printf("创建订阅源 %s 失败: %v", f.Name, err)
		}
	}
}

package main

import (
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	"github.com/esp32-rss-display/backend/server/api"
	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/image"
	"github.com/esp32-rss-display/backend/server/models"
	rssworker "github.com/esp32-rss-display/backend/server/rss"
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

	initFeeds(cfg.Feeds)

	mux := http.NewServeMux()

	mux.Handle("/metrics", promhttp.Handler())
	mux.HandleFunc("GET /v1/device/{device_id}/next", api.GetNextItem)
	image.Mount(mux)
	mux.HandleFunc("GET /nfc/{device_id}", api.NFCRedirect)

	addr := fmt.Sprintf("%s:%d", cfg.Server.Host, cfg.Server.Port)
	log.Printf("Starting server on %s", addr)

	worker := rssworker.New(&cfg.RSS, imageDir)
	worker.Start()

	go func() {
		if err := http.ListenAndServe(addr, mux); err != nil {
			log.Fatalf("Server error: %v", err)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	worker.Stop()
	log.Println("Server stopped")
}

func initFeeds(feeds []config.FeedConfig) {
	db := database.GetDB()

	for _, f := range feeds {
		var existing models.Feed
		if err := db.Where("url = ?", f.URL).First(&existing).Error; err != nil {
			db.Create(&models.Feed{
				Name:    f.Name,
				URL:     f.URL,
				Enabled: f.Enabled,
			})
		}
	}
}

package config

import (
	"os"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Database DatabaseConfig `yaml:"database"`
	Server   ServerConfig   `yaml:"server"`
	RSS      RSSConfig      `yaml:"rss"`
	Stocks   StockConfig    `yaml:"stocks"`
	Feeds    []FeedConfig   `yaml:"feeds"`
}

type DatabaseConfig struct {
	Driver string `yaml:"driver"`
	DSN    string `yaml:"dsn"`
}

type ServerConfig struct {
	Host string `yaml:"host"`
	Port int    `yaml:"port"`
}

type RSSConfig struct {
	FetchIntervalMinutes int `yaml:"fetch_interval_minutes"`
	ImageWidth           int `yaml:"image_width"`
	ImageHeight          int `yaml:"image_height"`
}

// StockConfig 配置 next 响应附带的行情栏；服务后台缓存全部编号，
// next 按设备请求的 size 轮换返回，顺序即轮播顺序。
type StockConfig struct {
	Enabled        bool          `yaml:"enabled"`
	RefreshSeconds int           `yaml:"refresh_seconds"`
	Symbols        []StockSymbol `yaml:"symbols"`
}

// StockSymbol 使用腾讯行情前缀编号：sh/sz/bj 为 A 股与指数，
// hk 为港股，us 为美股；name 留空时使用数据源返回的名称。
type StockSymbol struct {
	Code string `yaml:"code"`
	Name string `yaml:"name"`
}

type FeedConfig struct {
	Name    string `yaml:"name"`
	URL     string `yaml:"url"`
	Enabled bool   `yaml:"enabled"`
}

var AppConfig *Config

func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	var cfg Config
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return nil, err
	}

	AppConfig = &cfg
	return &cfg, nil
}

package config

import (
	"os"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Database DatabaseConfig `yaml:"database"`
	Server   ServerConfig   `yaml:"server"`
	RSS      RSSConfig      `yaml:"rss"`
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

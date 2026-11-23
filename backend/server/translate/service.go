package translate

import (
	"context"
	"log"
	"time"

	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

// Service 串行分批翻译标题，并合并重复通知。
type Service struct {
	db         *gorm.DB
	translator func(context.Context, string) (string, error)
	maxAge     time.Duration
	wake       chan struct{}
	cursor     uint
	delay      time.Duration
}

func NewService(db *gorm.DB, translator func(context.Context, string) (string, error), maxAge time.Duration) *Service {
	return &Service{db: db, translator: translator, maxAge: maxAge, wake: make(chan struct{}, 1), delay: 300 * time.Millisecond}
}

func (s *Service) Notify() {
	select {
	case s.wake <- struct{}{}:
	default:
	}
}

func (s *Service) Run(ctx context.Context) {
	s.Notify()
	ticker := time.NewTicker(time.Minute)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-s.wake:
			s.process(ctx)
		case <-ticker.C:
			s.process(ctx)
		}
	}
}

func (s *Service) enabled(ctx context.Context) bool {
	cfg, err := database.GetSettings(s.db.WithContext(ctx))
	if err != nil && ctx.Err() == nil {
		log.Printf("[translation] 读取配置失败: %v", err)
	}
	return err == nil && cfg.TranslationEnabled
}

func (s *Service) process(ctx context.Context) {
	if !s.enabled(ctx) {
		return
	}
	for ctx.Err() == nil {
		var items []models.Item
		query := s.db.WithContext(ctx).Select("id", "title").
			Where("id > ? AND (translated_title IS NULL OR translated_title = '')", s.cursor)
		if s.maxAge > 0 {
			query = query.Where("COALESCE(published_at, created_at) > ?", time.Now().Add(-s.maxAge))
		}
		if err := query.Order("id ASC").Limit(32).Find(&items).Error; err != nil {
			if ctx.Err() == nil {
				log.Printf("[translation] 查询待翻译条目失败: %v", err)
			}
			return
		}
		if len(items) == 0 {
			s.cursor = 0
			return
		}
		for _, item := range items {
			if !NeedsTranslation(item.Title) {
				s.cursor = item.ID
				continue
			}
			if !s.enabled(ctx) {
				return
			}
			s.cursor = item.ID
			text, err := s.translator(ctx, item.Title)
			if err != nil {
				if ctx.Err() == nil {
					log.Printf("[translation] 翻译失败 item=%d: %v", item.ID, err)
				}
				// 失败时暂停，游标前移让单条异常不阻塞后续标题。
				return
			}
			if err := s.db.WithContext(ctx).Model(&models.Item{}).
				Where("id = ? AND title = ?", item.ID, item.Title).
				Update("translated_title", text).Error; err != nil {
				log.Printf("[translation] 保存译文失败 item=%d: %v", item.ID, err)
				return
			}
			timer := time.NewTimer(s.delay)
			select {
			case <-ctx.Done():
				timer.Stop()
				return
			case <-timer.C:
			}
		}
	}
}

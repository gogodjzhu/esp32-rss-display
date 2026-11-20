package translate

import (
	"context"
	"log"
	"sync"
	"sync/atomic"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/settings"
	"gorm.io/gorm"
)

const (
	// translationBatchSize 限制单轮处理的条目数，避免开关刚打开时长时间占用接口。
	translationBatchSize = 50
	// translationPollInterval 是后台补译的轮询周期；启用/抓取完成时还会主动唤醒。
	translationPollInterval = 30 * time.Second
	// translationRetryAfter 是单条翻译失败后的重试间隔。
	translationRetryAfter = time.Hour
	// translationFailureBackoff 是连续失败后的冷却时间，避免接口故障时疯狂重试。
	translationFailureBackoff = 10 * time.Minute
	// translationFailureThreshold 连续失败达到该次数后进入冷却。
	translationFailureThreshold = 3
)

// Service 在后台把尚未翻译的条目补译为中文。
//
// 翻译不在 next 请求里同步执行：公开接口的延迟不可控，而设备对 next 有
// 10 秒超时。后台完成翻译后，next 直接返回缓存译文，接口失败则回退原文。
type Service struct {
	db     *gorm.DB
	client *Client

	wake chan struct{}
	stop chan struct{}
	done chan struct{}

	startOnce sync.Once
	stopOnce  sync.Once
	started   atomic.Bool
	cancel    context.CancelFunc

	mu       sync.Mutex
	failures int
	retryAt  time.Time
	now      func() time.Time
}

// NewService 创建翻译补译服务；db 与 client 必须非空。
func NewService(db *gorm.DB, client *Client) *Service {
	return &Service{
		db:     db,
		client: client,
		wake:   make(chan struct{}, 1),
		stop:   make(chan struct{}),
		done:   make(chan struct{}),
		now:    time.Now,
	}
}

// Start 启动后台循环，并立即执行一轮补译（若翻译已开启）。
func (s *Service) Start() {
	s.startOnce.Do(func() {
		s.started.Store(true)
		ctx, cancel := context.WithCancel(context.Background())
		s.cancel = cancel
		go s.loop(ctx)
	})
}

// Stop 停止后台循环并等待进行中的翻译请求结束。
func (s *Service) Stop() {
	if !s.started.Load() {
		return
	}
	s.stopOnce.Do(func() {
		close(s.stop)
		if s.cancel != nil {
			s.cancel()
		}
		<-s.done
	})
}

// Notify 主动唤醒后台循环；用于开关打开、抓取结束后尽快处理新条目。
// 通知不阻塞调用方，重复通知会合并为一次。
func (s *Service) Notify() {
	select {
	case s.wake <- struct{}{}:
	default:
	}
}

func (s *Service) loop(ctx context.Context) {
	defer close(s.done)

	s.ProcessPending(ctx)

	ticker := time.NewTicker(translationPollInterval)
	defer ticker.Stop()

	for {
		select {
		case <-s.stop:
			return
		case <-s.wake:
		case <-ticker.C:
		}
		s.ProcessPending(ctx)
	}
}

// ProcessPending 翻译一批最近的未处理条目，返回翻译成功、跳过与失败的条数。
// 翻译开关关闭、接口冷却中或没有待处理条目时直接返回。
func (s *Service) ProcessPending(ctx context.Context) (translated, skipped, failed int) {
	if s == nil || s.db == nil || s.client == nil || !settings.TranslationEnabled() {
		return 0, 0, 0
	}

	s.mu.Lock()
	now := s.now()
	if now.Before(s.retryAt) {
		s.mu.Unlock()
		return 0, 0, 0
	}
	s.mu.Unlock()

	// 只补译仍在保留期内的条目：更早的内容通常不再参与轮播。
	lower := now.Add(-config.MaxAge())
	retryBefore := now.Add(-translationRetryAfter)

	var items []models.Item
	if err := s.db.
		Where("translated_title = ''").
		Where("COALESCE(published_at, created_at) > ?", lower).
		Where("(translation_attempted_at IS NULL OR translation_attempted_at < ?)", retryBefore).
		Order("COALESCE(published_at, created_at) DESC, id DESC").
		Limit(translationBatchSize).
		Find(&items).Error; err != nil {
		log.Printf("[translate] 查询待翻译条目失败: %v", err)
		return 0, 0, 0
	}

	for i := range items {
		if err := ctx.Err(); err != nil {
			break
		}

		item := items[i]
		if !NeedsTranslation(item.Title) {
			// 非英文标题无需调用接口，但写入译文列作为“已处理”标记，
			// 否则它们会永久占据每轮查询的前 N 条。
			s.save(item.ID, item.Title)
			skipped++
			continue
		}

		out, err := s.client.Translate(ctx, item.Title)
		if err != nil {
			s.recordFailure(item.ID, err)
			failed++
			continue
		}
		s.save(item.ID, out)
		translated++

		s.mu.Lock()
		s.failures = 0
		s.mu.Unlock()
	}
	return translated, skipped, failed
}

// recordFailure 记录失败尝试；连续失败达到阈值后短暂冷却，避免拖垮翻译接口。
func (s *Service) recordFailure(itemID uint, err error) {
	s.save(itemID, "")

	s.mu.Lock()
	s.failures++
	cooldown := s.failures >= translationFailureThreshold
	if cooldown {
		s.retryAt = s.now().Add(translationFailureBackoff)
		s.failures = 0
	}
	s.mu.Unlock()

	if cooldown {
		log.Printf("[translate] 连续翻译失败，暂停 %s 后重试: %v", translationFailureBackoff, err)
	} else {
		log.Printf("[translate] 条目 %d 翻译失败，稍后重试: %v", itemID, err)
	}
}

// save 写入译文与尝试时间；translated 为空表示本次失败，只更新尝试时间。
func (s *Service) save(itemID uint, translated string) {
	now := s.now()
	updates := map[string]any{"translation_attempted_at": now}
	if translated != "" {
		updates["translated_title"] = translated
	}
	if err := s.db.Model(&models.Item{}).Where("id = ?", itemID).Updates(updates).Error; err != nil {
		log.Printf("[translate] 保存条目 %d 的译文失败: %v", itemID, err)
	}
}

// currentService 是 main 注入的默认服务；admin 接口与抓取循环通过它唤醒补译。
var currentService atomic.Pointer[Service]

// SetService 注入全局翻译服务（在启动 worker 之前调用）。
func SetService(service *Service) {
	currentService.Store(service)
}

// Notify 唤醒全局翻译服务；未初始化或已停止时安全忽略。
func Notify() {
	if service := currentService.Load(); service != nil {
		service.Notify()
	}
}

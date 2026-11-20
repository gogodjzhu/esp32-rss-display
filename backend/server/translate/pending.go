package translate

import (
	"context"
	"log"
	"sync"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/models"
	"github.com/esp32-rss-display/backend/server/settings"
	"gorm.io/gorm"
)

// pendingBatchSize 限制单轮补译条目数，避免开关刚打开时长时间占用公开接口。
const pendingBatchSize = 50

// pendingMu 让抓取循环与管理页触发的补译串行执行。
var pendingMu sync.Mutex

// Pending 翻译一批保留期内尚未处理的条目，返回成功、跳过与失败条数。
//
// 非英文标题写入原文作为已处理标记；失败条目不写库，下次触发时重试。
// 调用方在抓取结束和管理页开启开关时异步执行即可。
func Pending(ctx context.Context, db *gorm.DB) (translated, skipped, failed int) {
	if db == nil {
		return 0, 0, 0
	}

	pendingMu.Lock()
	defer pendingMu.Unlock()
	if !settings.Get(db) {
		return 0, 0, 0
	}

	var items []models.Item
	if err := db.
		Where("translated_title = ''").
		Where("COALESCE(published_at, created_at) > ?", time.Now().Add(-config.MaxAge())).
		Order("COALESCE(published_at, created_at) DESC, id DESC").
		Limit(pendingBatchSize).
		Find(&items).Error; err != nil {
		log.Printf("[translate] 查询待翻译条目失败: %v", err)
		return 0, 0, 0
	}

	for i := range items {
		if ctx.Err() != nil {
			break
		}

		item := items[i]
		text := item.Title
		if NeedsTranslation(item.Title) {
			out, err := Translate(ctx, item.Title)
			if err != nil {
				log.Printf("[translate] 条目 %d 翻译失败: %v", item.ID, err)
				failed++
				continue
			}
			text = out
			translated++
		} else {
			skipped++
		}

		if err := db.Model(&models.Item{}).Where("id = ?", item.ID).
			Update("translated_title", text).Error; err != nil {
			log.Printf("[translate] 保存条目 %d 的译文失败: %v", item.ID, err)
		}
	}
	return translated, skipped, failed
}

// Backfill 连续补译直到没有待处理条目或遇到失败，供开关打开后回填历史内容。
// 失败时留给下一轮抓取重试，避免接口故障时无休止调用。
func Backfill(ctx context.Context, db *gorm.DB) {
	for {
		translated, skipped, failed := Pending(ctx, db)
		if failed > 0 || translated+skipped == 0 {
			return
		}
	}
}

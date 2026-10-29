package stocks

// 行情服务：定时从公开数据源拉取配置的股票/指数涨跌，并把最近一次成功结果
// 保存在内存中供 next 路由读取。拉取失败时保留上一份快照，设备请求不会因
// 行情源异常而阻塞或失败。
//
// 数据源为腾讯行情 qt.gtimg.cn：免费、无需 key，一次请求可查询 A 股/指数、
// 港股和美股；拉取与解析细节见 tencent.go。

import (
	"log"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/esp32-rss-display/backend/server/config"
	"github.com/esp32-rss-display/backend/server/metrics"
)

const (
	defaultRefresh = 30 * time.Second
	minRefresh     = 5 * time.Second
	// DefaultWindow 为 next 未带 size 参数时的行情条数，兼容旧固件。
	DefaultWindow = 3
	// MaxWindow 限制单次响应的行情条数，避免异常 size 撑大 JSON。
	MaxWindow       = 32
	maxSymbols      = 100
	fetchTimeout    = 5 * time.Second
	defaultEndpoint = "https://qt.gtimg.cn/q="
)

// Quote 对应固件 content_item_parse 中的 stocks 项：
// 名称原样展示（由固件按像素宽度裁剪），change_pct 为百分比，
// 正数上涨、负数下跌、0 为平盘。
type Quote struct {
	Name      string  `json:"name"`
	ChangePct float64 `json:"change_pct"`
}

type symbol struct {
	code string // 数据源编号，如 sh000001、usAAPL
	name string // 可选的展示名覆盖
}

var symbolPattern = regexp.MustCompile(`^[A-Za-z]{2}[A-Za-z0-9._-]{1,15}$`)

type Service struct {
	symbols  []symbol
	interval time.Duration
	endpoint string
	client   *http.Client

	mu      sync.RWMutex
	quotes  []Quote
	cursors map[string]int // 每个设备的下一个窗口起点，重启后归零

	// 以下字段只在刷新协程内读写，用于避免故障期间每轮重复刷日志
	lastErr     string
	lastMissing string

	stopCh   chan struct{}
	stopOnce sync.Once
	wg       sync.WaitGroup
}

// New 根据配置构造行情服务；未启用或没有合法编号时返回 nil，
// 调用方此时不注入行情，next 响应将不带 stocks 字段。
func New(cfg *config.StockConfig) *Service {
	if cfg == nil || !cfg.Enabled {
		return nil
	}

	symbols := normalizeSymbols(cfg.Symbols)
	if len(symbols) == 0 {
		log.Printf("[stocks] enabled but no valid symbols configured, stock banner disabled")
		return nil
	}

	interval := defaultRefresh
	if cfg.RefreshSeconds > 0 {
		interval = time.Duration(cfg.RefreshSeconds) * time.Second
	}
	if interval < minRefresh {
		interval = minRefresh
	}

	return &Service{
		symbols:  symbols,
		interval: interval,
		endpoint: defaultEndpoint,
		client:   &http.Client{Timeout: fetchTimeout},
		cursors:  make(map[string]int),
		stopCh:   make(chan struct{}),
	}
}

// ClampWindow 把请求的行情条数限制在 [1, MaxWindow]；非正数回退为默认值。
func ClampWindow(size int) int {
	if size <= 0 {
		return DefaultWindow
	}
	if size > MaxWindow {
		return MaxWindow
	}
	return size
}

// Window 返回该设备本次应显示的行情窗口，并按 size 推进该设备的轮换位置。
// 配置数量不超过 size 时直接返回全部；没有可用行情时返回 nil。
func (s *Service) Window(deviceID string, size int) []Quote {
	if s == nil {
		return nil
	}
	size = ClampWindow(size)

	s.mu.Lock()
	defer s.mu.Unlock()

	count := len(s.quotes)
	if count == 0 {
		return nil
	}
	if s.cursors == nil {
		s.cursors = make(map[string]int)
	}

	// 窗口覆盖全部行情时不需要轮换，同时把游标留在起点
	if size >= count {
		out := make([]Quote, count)
		copy(out, s.quotes)
		s.cursors[deviceID] = 0
		return out
	}

	start := s.cursors[deviceID] % count
	out := make([]Quote, size)
	for i := 0; i < size; i++ {
		out[i] = s.quotes[(start+i)%count]
	}
	s.cursors[deviceID] = (start + size) % count
	return out
}

// Start 启动后台刷新：立即拉取一次，之后按配置间隔刷新。
func (s *Service) Start() {
	if s == nil {
		return
	}
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		s.run()
	}()
}

// Stop 停止后台刷新并等待退出。
func (s *Service) Stop() {
	if s == nil {
		return
	}
	s.stopOnce.Do(func() { close(s.stopCh) })
	s.wg.Wait()
}

func (s *Service) run() {
	s.refresh()

	ticker := time.NewTicker(s.interval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			s.refresh()
		case <-s.stopCh:
			return
		}
	}
}

func (s *Service) refresh() {
	metrics.StockFetchTotal.Add(1)

	quotes, missing, err := fetchTencent(s.client, s.endpoint, s.symbols)
	if err != nil {
		metrics.StockFetchError.Add(1)
		s.logError(err)
		return
	}
	if len(quotes) == 0 {
		metrics.StockFetchError.Add(1)
		s.logError(errNoQuotes)
		return
	}

	if s.lastErr != "" {
		log.Printf("[stocks] fetch recovered")
		s.lastErr = ""
	}

	s.mu.Lock()
	s.quotes = quotes
	s.mu.Unlock()
	metrics.StockQuotes.Set(float64(len(quotes)))
	s.logMissing(missing)
}

// logError 只在错误内容变化时输出，避免行情源长时间不可用时刷屏。
func (s *Service) logError(err error) {
	if err.Error() == s.lastErr {
		return
	}
	s.lastErr = err.Error()
	log.Printf("[stocks] fetch failed: %v", err)
}

// logMissing 记录配置了但没有拿到行情的编号，同样只在集合变化时输出。
func (s *Service) logMissing(missing []string) {
	joined := strings.Join(missing, ",")
	if joined == s.lastMissing {
		return
	}
	s.lastMissing = joined
	if joined != "" {
		log.Printf("[stocks] no quote for configured symbols: %s", joined)
	}
}

// normalizeSymbols 校验、去重并截断配置编号：
// 前缀统一小写，美股代码统一大写（数据源只认 usAAPL 这类写法）。
func normalizeSymbols(in []config.StockSymbol) []symbol {
	out := make([]symbol, 0, len(in))
	seen := make(map[string]bool, len(in))

	for _, item := range in {
		code := strings.TrimSpace(item.Code)
		if !symbolPattern.MatchString(code) {
			log.Printf("[stocks] invalid symbol %q skipped", item.Code)
			continue
		}

		prefix := strings.ToLower(code[:2])
		switch prefix {
		case "sh", "sz", "bj", "hk":
			// A 股、指数与港股编号由前缀加数字组成，保持原样即可
		case "us":
			code = prefix + strings.ToUpper(code[2:])
		default:
			log.Printf("[stocks] unsupported market prefix in %q skipped", item.Code)
			continue
		}

		if seen[code] {
			continue
		}
		seen[code] = true
		out = append(out, symbol{code: code, name: strings.TrimSpace(item.Name)})
	}

	if len(out) > maxSymbols {
		log.Printf("[stocks] %d symbols configured, only first %d are fetched", len(out), maxSymbols)
		out = out[:maxSymbols]
	}
	return out
}

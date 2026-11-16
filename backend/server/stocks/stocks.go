package stocks

// 行情服务：定时从公开数据源拉取配置的股票/指数涨跌，并把最近一次成功结果
// 保存在内存中供 next 路由读取。拉取失败时保留上一份快照，设备请求不会因
// 行情源异常而阻塞或失败。
//
// 行情配置可以在运行时通过 Apply 更新：管理页保存后立即生效，无需重启服务。
// 配置的持久化见 store.go：数据库是唯一数据源，config.yaml 只做首次种子。
//
// 数据源为腾讯行情 qt.gtimg.cn：免费、无需 key，一次请求可查询 A 股/指数、
// 港股和美股；拉取与解析细节见 tencent.go。

import (
	"errors"
	"fmt"
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
	// DefaultRefresh 为未配置 refresh_seconds 时的拉取间隔。
	DefaultRefresh = 30 * time.Second
	// MinRefresh 为拉取间隔下限，避免异常配置频繁请求行情源。
	MinRefresh = 5 * time.Second
	// DefaultWindow 为 next 未带 size 参数时的行情条数，兼容旧固件。
	DefaultWindow = 3
	// MaxWindow 限制单次响应的行情条数，避免异常 size 撑大 JSON。
	MaxWindow = 32
	// MaxSymbols 为可配置的行情编号上限。
	MaxSymbols      = 100
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

// Status 是行情拉取的运行快照，供管理页展示配置是否生效；
// 配置本身保存在数据库里，不在这里。
type Status struct {
	// LastAttemptAt 为最近一次拉取尝试（无论成功失败）的时间。
	LastAttemptAt *time.Time `json:"last_attempt_at"`
	// LastSuccessAt 为最近一次成功拉取的时间。
	LastSuccessAt *time.Time `json:"last_success_at"`
	// QuoteCount 为当前缓存的行情条数。
	QuoteCount int `json:"quote_count"`
	// LastError 为最近一次失败原因，成功后清空。
	LastError string `json:"last_error,omitempty"`
	// MissingCodes 为上次成功拉取时数据源没有返回的编号。
	MissingCodes []string `json:"missing_codes,omitempty"`
}

type symbol struct {
	code string // 数据源编号，如 sh000001、usAAPL
	name string // 可选的展示名覆盖
}

var symbolPattern = regexp.MustCompile(`^[A-Za-z]{2}[A-Za-z0-9._-]{1,15}$`)

// Service 持有当前行情配置和最近一次成功快照，并负责后台定时刷新。
// 配置可通过 Apply 在运行时替换；后台循环会因此立即重新拉取。
type Service struct {
	mu sync.Mutex

	// 配置状态：Apply 写入，刷新协程与 Window 读取
	symbols  []symbol
	interval time.Duration
	enabled  bool
	gen      uint64 // 配置代次，用于丢弃按旧配置拉取的结果

	// 行情快照与拉取状态
	quotes        []Quote
	cursors       map[string]int // 每个设备的下一个窗口起点，重启后归零
	lastAttemptAt time.Time
	lastSuccessAt time.Time
	lastStatusErr string
	lastMissing   []string
	// 以下字段只在刷新时读写，用于避免故障期间每轮重复刷日志
	lastErr        string
	lastMissingLog string

	endpoint string
	client   *http.Client

	wake     chan struct{}
	started  bool
	stopCh   chan struct{}
	stopOnce sync.Once
	wg       sync.WaitGroup
}

// New 根据配置构造行情服务。未启用或没有合法编号时服务仍然存在，
// 只是不拉取、Window 返回 nil；管理页可以在运行时启用或修改它。
func New(cfg *config.StockConfig) *Service {
	s := &Service{
		interval: DefaultRefresh,
		endpoint: defaultEndpoint,
		client:   &http.Client{Timeout: fetchTimeout},
		cursors:  make(map[string]int),
		wake:     make(chan struct{}, 1),
		stopCh:   make(chan struct{}),
	}
	if cfg != nil {
		s.apply(*cfg)
	}
	return s
}

// Apply 应用一份新配置：规范化编号、更新间隔与启用状态，并唤醒后台循环
// 立即按新配置拉取一次。编号或启用状态变化时会清空旧快照，避免设备响应里
// 继续出现已删除的编号。
func (s *Service) Apply(cfg *config.StockConfig) {
	if s == nil || cfg == nil {
		return
	}
	s.apply(*cfg)
	s.notify()
}

// apply 完成配置切换但不唤醒后台循环，构造服务时复用。
func (s *Service) apply(cfg config.StockConfig) {
	symbols := normalizeSymbols(cfg.Symbols)
	interval := intervalOf(cfg.RefreshSeconds)
	enabled := cfg.Enabled && len(symbols) > 0
	if cfg.Enabled && len(symbols) == 0 {
		log.Printf("[stocks] enabled but no valid symbols configured, stock banner disabled")
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	symbolsChanged := !sameSymbols(s.symbols, symbols)
	if !symbolsChanged && s.enabled == enabled && s.interval == interval {
		return
	}

	s.symbols = symbols
	s.interval = interval
	s.enabled = enabled
	s.gen++

	// 旧快照已经不对应当前配置，先清空，等后台按新配置拉取
	if symbolsChanged || !enabled {
		s.quotes = nil
		s.cursors = make(map[string]int)
		metrics.StockQuotes.Set(0)
	}
	if !enabled {
		s.lastStatusErr = ""
		s.lastMissing = nil
		s.lastErr = ""
		s.lastMissingLog = ""
	}
}

// notify 非阻塞地唤醒后台刷新循环；通道满说明已经有一个待处理的唤醒。
func (s *Service) notify() {
	select {
	case s.wake <- struct{}{}:
	default:
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

// Status 返回当前拉取状态；服务未启动或从未拉取时字段为零值。
func (s *Service) Status() Status {
	if s == nil {
		return Status{}
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	status := Status{
		QuoteCount: len(s.quotes),
		LastError:  s.lastStatusErr,
	}
	if !s.lastAttemptAt.IsZero() {
		t := s.lastAttemptAt
		status.LastAttemptAt = &t
	}
	if !s.lastSuccessAt.IsZero() {
		t := s.lastSuccessAt
		status.LastSuccessAt = &t
	}
	if len(s.lastMissing) > 0 {
		status.MissingCodes = append([]string(nil), s.lastMissing...)
	}
	return status
}

// Start 启动后台刷新：立即按当前配置拉取一次，之后按配置间隔刷新。
func (s *Service) Start() {
	if s == nil {
		return
	}

	s.mu.Lock()
	if s.started {
		s.mu.Unlock()
		return
	}
	s.started = true
	if s.wake == nil {
		s.wake = make(chan struct{}, 1)
	}
	if s.stopCh == nil {
		s.stopCh = make(chan struct{})
	}
	s.mu.Unlock()

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

	s.mu.Lock()
	stopCh := s.stopCh
	s.mu.Unlock()

	if stopCh == nil {
		return
	}
	s.stopOnce.Do(func() { close(stopCh) })
	s.wg.Wait()
}

// run 是后台刷新循环：每次先按当前配置刷新一次，然后等待间隔到期或配置变化。
// 配置被停用或没有编号时不设定时器，只等待唤醒或退出。
func (s *Service) run() {
	for {
		s.mu.Lock()
		enabled := s.enabled && len(s.symbols) > 0
		interval := s.interval
		stopCh := s.stopCh
		wake := s.wake
		s.mu.Unlock()

		if enabled {
			s.refresh()
		}

		var timer *time.Timer
		var tick <-chan time.Time
		if enabled {
			timer = time.NewTimer(interval)
			tick = timer.C
		}

		select {
		case <-tick:
		case <-wake:
		case <-stopCh:
			if timer != nil {
				timer.Stop()
			}
			return
		}
		if timer != nil {
			timer.Stop()
		}
	}
}

func (s *Service) refresh() {
	s.mu.Lock()
	if !s.enabled || len(s.symbols) == 0 {
		s.mu.Unlock()
		return
	}
	symbols := append([]symbol(nil), s.symbols...)
	gen := s.gen
	s.mu.Unlock()

	metrics.StockFetchTotal.Add(1)
	quotes, missing, err := fetchTencent(s.client, s.endpoint, symbols)

	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()

	// 拉取期间配置被改过：结果对应旧编号，丢弃后等新配置的拉取
	if gen != s.gen {
		return
	}
	s.lastAttemptAt = now

	if err != nil {
		metrics.StockFetchError.Add(1)
		s.lastStatusErr = err.Error()
		s.logError(err)
		return
	}
	if len(quotes) == 0 {
		metrics.StockFetchError.Add(1)
		s.lastStatusErr = errNoQuotes.Error()
		s.logError(errNoQuotes)
		return
	}

	if s.lastErr != "" {
		log.Printf("[stocks] fetch recovered")
		s.lastErr = ""
	}
	s.quotes = quotes
	s.lastStatusErr = ""
	s.lastSuccessAt = now
	s.lastMissing = append([]string(nil), missing...)
	metrics.StockQuotes.Set(float64(len(quotes)))
	s.logMissing(missing)
}

// logError 只在错误内容变化时输出，避免行情源长时间不可用时刷屏。
// 调用方需持有 s.mu。
func (s *Service) logError(err error) {
	if err.Error() == s.lastErr {
		return
	}
	s.lastErr = err.Error()
	log.Printf("[stocks] fetch failed: %v", err)
}

// logMissing 记录配置了但没有拿到行情的编号，同样只在集合变化时输出。
// 调用方需持有 s.mu。
func (s *Service) logMissing(missing []string) {
	joined := strings.Join(missing, ",")
	if joined == s.lastMissingLog {
		return
	}
	s.lastMissingLog = joined
	if joined != "" {
		log.Printf("[stocks] no quote for configured symbols: %s", joined)
	}
}

// ValidateSymbol 校验并规范化单个行情编号：前缀统一小写，美股代码统一大写
// （数据源只认 usAAPL 这类写法）。管理页保存前用它给出明确错误。
func ValidateSymbol(raw string) (string, error) {
	code := strings.TrimSpace(raw)
	if !symbolPattern.MatchString(code) {
		return "", errors.New("编号格式不正确，应为 sh/sz/bj/hk/us 前缀加代码，例如 sh000001、usAAPL")
	}

	prefix := strings.ToLower(code[:2])
	switch prefix {
	case "sh", "sz", "bj", "hk":
		// A 股、指数与港股编号由前缀加数字组成，前缀统一小写
		code = prefix + code[2:]
	case "us":
		code = prefix + strings.ToUpper(code[2:])
	default:
		return "", fmt.Errorf("不支持的市场前缀 %q，仅支持 sh/sz/bj/hk/us", code[:2])
	}
	return code, nil
}

// NormalizeConfig 返回规范化后的配置：非法编号被跳过、编号与名称去空白、
// 刷新间隔夹到下限；启用但没有合法编号时退回停用。
func NormalizeConfig(cfg config.StockConfig) config.StockConfig {
	symbols := normalizeSymbols(cfg.Symbols)
	out := config.StockConfig{
		Enabled:        cfg.Enabled && len(symbols) > 0,
		RefreshSeconds: int(intervalOf(cfg.RefreshSeconds) / time.Second),
		Symbols:        make([]config.StockSymbol, 0, len(symbols)),
	}
	for _, sym := range symbols {
		out.Symbols = append(out.Symbols, config.StockSymbol{Code: sym.code, Name: sym.name})
	}
	return out
}

// normalizeSymbols 校验、规范化、去重并截断配置编号。
func normalizeSymbols(in []config.StockSymbol) []symbol {
	out := make([]symbol, 0, len(in))
	seen := make(map[string]bool, len(in))

	for _, item := range in {
		code, err := ValidateSymbol(item.Code)
		if err != nil {
			log.Printf("[stocks] invalid symbol %q skipped: %v", item.Code, err)
			continue
		}
		if seen[code] {
			continue
		}
		seen[code] = true
		out = append(out, symbol{code: code, name: strings.TrimSpace(item.Name)})
	}

	if len(out) > MaxSymbols {
		log.Printf("[stocks] %d symbols configured, only first %d are fetched", len(out), MaxSymbols)
		out = out[:MaxSymbols]
	}
	return out
}

// sameSymbols 判断两份编号列表是否完全一致（顺序也算，顺序即轮播顺序）。
func sameSymbols(a, b []symbol) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// intervalOf 把配置的秒数转换成拉取间隔，非正数用默认值，低于下限夹到下限。
func intervalOf(seconds int) time.Duration {
	if seconds <= 0 {
		return DefaultRefresh
	}
	interval := time.Duration(seconds) * time.Second
	if interval < MinRefresh {
		return MinRefresh
	}
	return interval
}

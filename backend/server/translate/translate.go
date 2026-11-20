// Package translate 把英文标题翻译成中文。
//
// 使用无需 API Key 的公开翻译接口：优先 Google 的轻量接口（响应快），
// 失败时依次回退到 Google gtx 与 MyMemory。接口不可用或返回异常时调用方
// 应保留原标题，翻译是增强功能，绝不能阻塞内容展示。
package translate

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"html"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
	"unicode"
)

const (
	// maxTextRunes 是免费接口对单次请求长度的保守上限；标题通常远小于此。
	maxTextRunes = 400
	// maxResponseBytes 限制翻译接口的响应体，避免异常响应占满内存。
	maxResponseBytes = 1 << 20
	// userAgent 部分接口会拒绝空 UA。
	userAgent = "Mozilla/5.0 (compatible; esp32-rss-display/1.0)"
)

// Provider 是单个翻译服务实现；Translate 返回中文译文或错误。
type Provider interface {
	Name() string
	Translate(ctx context.Context, text string) (string, error)
}

// Client 按顺序尝试 Provider，任意一个成功即返回。
type Client struct {
	providers []Provider
}

// NewClient 返回使用默认公开接口的翻译客户端。
func NewClient() *Client {
	return NewClientWithProviders(defaultProviders()...)
}

// NewClientWithProviders 便于测试注入假实现或本地 httptest 服务。
func NewClientWithProviders(providers ...Provider) *Client {
	return &Client{providers: append([]Provider(nil), providers...)}
}

// Translate 依次调用 Provider，全部失败时返回聚合错误。
func (c *Client) Translate(ctx context.Context, text string) (string, error) {
	if c == nil || len(c.providers) == 0 {
		return "", errors.New("没有可用的翻译接口")
	}

	var errs []error
	for _, provider := range c.providers {
		out, err := provider.Translate(ctx, text)
		if err == nil && strings.TrimSpace(out) != "" {
			return strings.TrimSpace(out), nil
		}
		if err == nil {
			err = errors.New("翻译结果为空")
		}
		errs = append(errs, fmt.Errorf("%s: %w", provider.Name(), err))
	}
	return "", errors.Join(errs...)
}

// NeedsTranslation 判断标题是否需要翻译：包含汉字（视为已是中文）或
// 完全不含拉丁字母（纯数字/符号/emoji）的标题原样保留。
func NeedsTranslation(title string) bool {
	title = strings.TrimSpace(title)
	if title == "" {
		return false
	}

	hasHan, hasLatin := false, false
	for _, r := range title {
		if unicode.Is(unicode.Han, r) {
			hasHan = true
		}
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
			hasLatin = true
		}
		if hasHan {
			return false
		}
	}
	return hasLatin
}

// truncated 按字符截断，避免超长标题被公开接口拒绝。
func truncated(text string) string {
	runes := []rune(strings.TrimSpace(text))
	if len(runes) <= maxTextRunes {
		return string(runes)
	}
	return string(runes[:maxTextRunes])
}

// defaultProviders 的顺序是响应速度优先：两个 Google 接口通常很快，
// MyMemory 较慢但可用性更广，作为最后的兜底。
func defaultProviders() []Provider {
	client := &http.Client{}
	return []Provider{
		newHTTPProvider("google-dict", "https://clients5.google.com/translate_a/t",
			googleDictURL, parseGoogleDict, client, 3*time.Second),
		newHTTPProvider("google-gtx", "https://translate.googleapis.com/translate_a/single",
			googleGtxURL, parseGoogleGtx, client, 2*time.Second),
		newHTTPProvider("mymemory", "https://api.mymemory.translated.net/get",
			myMemoryURL, parseMyMemory, client, 10*time.Second),
	}
}

type httpProvider struct {
	name     string
	url      string
	buildURL func(endpoint, text string) string
	parse    func([]byte) (string, error)
	client   *http.Client
	timeout  time.Duration
}

func newHTTPProvider(name, endpoint string, buildURL func(string, string) string,
	parse func([]byte) (string, error), client *http.Client, timeout time.Duration) Provider {
	return &httpProvider{
		name:     name,
		url:      endpoint,
		buildURL: buildURL,
		parse:    parse,
		client:   client,
		timeout:  timeout,
	}
}

func (p *httpProvider) Name() string {
	return p.name
}

func (p *httpProvider) Translate(ctx context.Context, text string) (string, error) {
	text = truncated(text)
	if text == "" {
		return "", errors.New("空文本")
	}

	ctx, cancel := context.WithTimeout(ctx, p.timeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.buildURL(p.url, text), nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", userAgent)

	resp, err := p.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxResponseBytes))
	if err != nil {
		return "", err
	}
	return p.parse(body)
}

func googleDictURL(endpoint, text string) string {
	q := url.Values{}
	q.Set("client", "dict-chrome-ex")
	q.Set("sl", "en")
	q.Set("tl", "zh-CN")
	q.Set("q", text)
	return endpoint + "?" + q.Encode()
}

func googleGtxURL(endpoint, text string) string {
	q := url.Values{}
	q.Set("client", "gtx")
	q.Set("sl", "en")
	q.Set("tl", "zh-CN")
	q.Set("dt", "t")
	q.Set("q", text)
	return endpoint + "?" + q.Encode()
}

func myMemoryURL(endpoint, text string) string {
	q := url.Values{}
	q.Set("q", text)
	q.Set("langpair", "en|zh-CN")
	return endpoint + "?" + q.Encode()
}

// parseGoogleDict 解析 clients5 的 dict-chrome-ex 响应：
// 单条文本返回 ["译文"]，自动检测语言时返回 [["译文","en"]]。
func parseGoogleDict(body []byte) (string, error) {
	var raw any
	if err := json.Unmarshal(body, &raw); err != nil {
		return "", fmt.Errorf("响应不是 JSON: %w", err)
	}
	arr, ok := raw.([]any)
	if !ok || len(arr) == 0 {
		return "", errors.New("响应格式异常")
	}
	switch first := arr[0].(type) {
	case string:
		return first, nil
	case []any:
		if len(first) > 0 {
			if text, ok := first[0].(string); ok {
				return text, nil
			}
		}
	}
	return "", errors.New("响应缺少译文")
}

// parseGoogleGtx 解析 translate_a/single 的分段响应：
// [[["译文1","原文1",...],["译文2","原文2",...]], ...]，按顺序拼接各段译文。
func parseGoogleGtx(body []byte) (string, error) {
	var raw []any
	if err := json.Unmarshal(body, &raw); err != nil {
		return "", fmt.Errorf("响应不是 JSON: %w", err)
	}
	if len(raw) == 0 {
		return "", errors.New("响应格式异常")
	}
	segments, ok := raw[0].([]any)
	if !ok {
		return "", errors.New("响应缺少译文分段")
	}

	var b strings.Builder
	for _, segment := range segments {
		pair, ok := segment.([]any)
		if !ok || len(pair) == 0 {
			continue
		}
		if text, ok := pair[0].(string); ok {
			b.WriteString(text)
		}
	}
	if b.Len() == 0 {
		return "", errors.New("响应缺少译文")
	}
	return b.String(), nil
}

// parseMyMemory 解析 MyMemory 的响应，并把常见的“额度用尽”等
// 200 响应识别为失败，交给下一个 Provider 或回退原文。
func parseMyMemory(body []byte) (string, error) {
	var resp struct {
		ResponseData struct {
			TranslatedText string `json:"translatedText"`
		} `json:"responseData"`
		ResponseStatus json.RawMessage `json:"responseStatus"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("响应不是 JSON: %w", err)
	}
	text := strings.TrimSpace(html.UnescapeString(resp.ResponseData.TranslatedText))
	if text == "" {
		return "", errors.New("响应缺少译文")
	}
	upper := strings.ToUpper(text)
	for _, marker := range []string{
		"MYMEMORY WARNING",
		"QUERY LENGTH LIMIT",
		"INVALID LANGUAGE PAIR",
		"PLEASE SELECT TWO DISTINCT LANGUAGES",
	} {
		if strings.Contains(upper, marker) {
			return "", errors.New("翻译接口返回错误提示")
		}
	}
	return text, nil
}

// Package translate 把英文标题翻译成中文。
//
// 依次尝试免 Key 的公开接口（Google 轻量接口、MyMemory）；全部失败时调用方
// 保留原标题。翻译是增强功能，接口不可用绝不能影响内容展示。
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
	// maxTextRunes 是免费接口对单次请求长度的保守上限。
	maxTextRunes     = 400
	maxResponseBytes = 1 << 20
	userAgent        = "Mozilla/5.0 (compatible; esp32-rss-display/1.0)"
)

// 变量便于测试替换为 httptest 地址。
var (
	googleEndpoint   = "https://clients5.google.com/translate_a/t"
	myMemoryEndpoint = "https://api.mymemory.translated.net/get"
	httpClient       = &http.Client{Timeout: 12 * time.Second}
)

type endpoint struct {
	name   string
	url    string
	params func(string) url.Values
	parse  func([]byte) (string, error)
}

// endpoints 按响应速度排序：Google 通常很快，MyMemory 作为兜底。
func endpoints() []endpoint {
	return []endpoint{
		{
			name: "google",
			url:  googleEndpoint,
			params: func(text string) url.Values {
				return url.Values{"client": {"dict-chrome-ex"}, "sl": {"en"}, "tl": {"zh-CN"}, "q": {text}}
			},
			parse: parseGoogle,
		},
		{
			name: "mymemory",
			url:  myMemoryEndpoint,
			params: func(text string) url.Values {
				return url.Values{"q": {text}, "langpair": {"en|zh-CN"}}
			},
			parse: parseMyMemory,
		},
	}
}

// Translate 依次尝试公开接口，返回第一个成功的译文；全部失败时返回聚合错误。
func Translate(ctx context.Context, text string) (string, error) {
	text = truncated(text)
	if text == "" {
		return "", errors.New("空标题")
	}

	var errs []error
	for _, ep := range endpoints() {
		out, err := fetch(ctx, ep, text)
		if err == nil {
			return out, nil
		}
		errs = append(errs, fmt.Errorf("%s: %w", ep.name, err))
	}
	return "", errors.Join(errs...)
}

func fetch(ctx context.Context, ep endpoint, text string) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, ep.url+"?"+ep.params(text).Encode(), nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", userAgent)

	resp, err := httpClient.Do(req)
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
	out, err := ep.parse(body)
	if err != nil {
		return "", err
	}
	out = strings.TrimSpace(out)
	if out == "" {
		return "", errors.New("译文为空")
	}
	return out, nil
}

// parseGoogle 解析 dict-chrome-ex 响应：单条文本返回 ["译文"]，
// 自动检测语言时返回 [["译文","en"]]。
func parseGoogle(body []byte) (string, error) {
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

// parseMyMemory 解析 MyMemory 响应，并把“额度用尽”等 200 响应识别为失败。
func parseMyMemory(body []byte) (string, error) {
	var resp struct {
		ResponseData struct {
			TranslatedText string `json:"translatedText"`
		} `json:"responseData"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("响应不是 JSON: %w", err)
	}

	text := strings.TrimSpace(html.UnescapeString(resp.ResponseData.TranslatedText))
	upper := strings.ToUpper(text)
	for _, marker := range []string{"MYMEMORY WARNING", "QUERY LENGTH LIMIT", "INVALID LANGUAGE PAIR"} {
		if strings.Contains(upper, marker) {
			return "", errors.New("接口返回错误提示")
		}
	}
	return text, nil
}

// NeedsTranslation 判断标题是否需要翻译：含汉字或完全不含拉丁字母的标题
// （已是中文、纯数字/符号/emoji）原样保留。
func NeedsTranslation(title string) bool {
	title = strings.TrimSpace(title)
	if title == "" {
		return false
	}

	hasLatin := false
	for _, r := range title {
		if unicode.Is(unicode.Han, r) {
			return false
		}
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
			hasLatin = true
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

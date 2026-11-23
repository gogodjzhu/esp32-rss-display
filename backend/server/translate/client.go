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
	"unicode/utf8"
)

// Client 使用免密钥公开接口；主接口不可用时尝试备用接口。
type Client struct {
	http      *http.Client
	googleURL string
	memoryURL string
}

func NewClient() *Client {
	return &Client{
		http:      &http.Client{Timeout: 8 * time.Second},
		googleURL: "https://translate.googleapis.com/translate_a/single",
		memoryURL: "https://api.mymemory.translated.net/get",
	}
}

// NeedsTranslation 跳过已有汉字、无字母及非拉丁文字的标题，避免重复翻译中文。
func NeedsTranslation(title string) bool {
	hasLatin := false
	for _, r := range title {
		if unicode.Is(unicode.Han, r) {
			return false
		}
		if unicode.IsLetter(r) {
			if !unicode.Is(unicode.Latin, r) {
				return false
			}
			hasLatin = true
		}
	}
	return hasLatin
}

func (c *Client) Translate(ctx context.Context, title string) (string, error) {
	text, err := c.google(ctx, title)
	if err == nil {
		return text, nil
	}
	if ctx.Err() != nil {
		return "", ctx.Err()
	}
	// MyMemory 对每次查询限制为 500 字节，过长标题保留原文等待下次重试。
	if len(title) > 500 {
		return "", err
	}
	return c.memory(ctx, title)
}

func (c *Client) get(ctx context.Context, endpoint string, query url.Values, out any) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint+"?"+query.Encode(), nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "RSS-Translation/1.0")
	resp, err := c.http.Do(req)
	if err != nil {
		// 请求错误可能包含 URL 和标题，日志只返回不含请求内容的错误。
		return errors.New("翻译接口连接失败")
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("翻译接口返回 HTTP %d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, (64<<10)+1))
	if err != nil {
		return errors.New("读取翻译响应失败")
	}
	if len(body) > 64<<10 {
		return errors.New("翻译响应超出大小限制")
	}
	if err := json.Unmarshal(body, out); err != nil {
		return errors.New("翻译响应不是有效 JSON")
	}
	return nil
}

func (c *Client) google(ctx context.Context, title string) (string, error) {
	var result []json.RawMessage
	if err := c.get(ctx, c.googleURL, url.Values{
		"client": {"gtx"}, "sl": {"auto"}, "tl": {"zh-CN"}, "dt": {"t"}, "q": {title},
	}, &result); err != nil {
		return "", err
	}
	if len(result) == 0 {
		return "", errors.New("翻译响应为空")
	}
	var segments [][]json.RawMessage
	if err := json.Unmarshal(result[0], &segments); err != nil {
		return "", errors.New("翻译分段格式错误")
	}
	var text strings.Builder
	for _, segment := range segments {
		if len(segment) == 0 {
			return "", errors.New("翻译分段为空")
		}
		var part string
		if err := json.Unmarshal(segment[0], &part); err != nil {
			return "", errors.New("翻译分段内容错误")
		}
		text.WriteString(part)
	}
	return validate(text.String())
}

func (c *Client) memory(ctx context.Context, title string) (string, error) {
	var result struct {
		Status int `json:"responseStatus"`
		Data   struct {
			Text string `json:"translatedText"`
		} `json:"responseData"`
	}
	if err := c.get(ctx, c.memoryURL, url.Values{"q": {title}, "langpair": {"en|zh-CN"}}, &result); err != nil {
		return "", err
	}
	if result.Status != http.StatusOK {
		return "", errors.New("备用翻译接口拒绝请求")
	}
	return validate(result.Data.Text)
}

func validate(text string) (string, error) {
	text = strings.TrimSpace(html.UnescapeString(text))
	if text == "" || !utf8.ValidString(text) || utf8.RuneCountInString(text) > 4096 {
		return "", errors.New("译文为空或过长")
	}
	// 配额提示等英文文本不能当作中文译文写入数据库。
	if !strings.ContainsFunc(text, func(r rune) bool { return unicode.Is(unicode.Han, r) }) {
		return "", errors.New("翻译接口未返回中文")
	}
	return text, nil
}

package stocks

// 腾讯行情（qt.gtimg.cn）拉取与解析。
//
// 请求：GET https://qt.gtimg.cn/q=sh000001,sz399001,usAAPL
// 响应为 GBK 编码的多行文本，每行形如：
//
//	v_sh000001="1~上证指数~000001~3651.43~3681.47~...~-30.04~-0.82~...";
//
// 用 "~" 分割后，三个市场实测字段位置一致：
//   [1] 名称、[3] 现价、[4] 昨收、[32] 涨跌幅（百分比）。
//
// 无效编号不会出现在响应里；单行字段残缺或涨跌幅非法时跳过该条。

import (
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"strconv"
	"strings"

	"golang.org/x/text/encoding/simplifiedchinese"
)

const (
	maxBodyBytes = 1 << 20
	// changePctField 为 "~" 分割后的涨跌幅下标
	changePctField = 32
)

var errNoQuotes = errors.New("no valid quotes in response")

func fetchTencent(client *http.Client, endpoint string, symbols []symbol) ([]Quote, []string, error) {
	codes := make([]string, len(symbols))
	for i, sym := range symbols {
		codes[i] = sym.code
	}

	req, err := http.NewRequest(http.MethodGet, endpoint+strings.Join(codes, ","), nil)
	if err != nil {
		return nil, nil, err
	}
	// 部分 CDN 会拒绝空 User-Agent，带上便于排查的来源标识
	req.Header.Set("User-Agent", "esp32-rss-display/1.0")

	resp, err := client.Do(req)
	if err != nil {
		return nil, nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, nil, fmt.Errorf("unexpected status %s", resp.Status)
	}

	raw, err := io.ReadAll(io.LimitReader(resp.Body, maxBodyBytes))
	if err != nil {
		return nil, nil, err
	}

	// 行情接口固定返回 GBK，显式转成 UTF-8 后再按字符串解析
	text, err := simplifiedchinese.GBK.NewDecoder().Bytes(raw)
	if err != nil {
		return nil, nil, fmt.Errorf("decode gbk: %w", err)
	}

	return parseTencent(text, symbols)
}

// parseTencent 把原始响应转换成按配置顺序排列的行情，并返回缺失的编号。
func parseTencent(text []byte, symbols []symbol) ([]Quote, []string, error) {
	rows := make(map[string][]string)
	for _, line := range strings.Split(string(text), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "v_") {
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			continue
		}

		key := strings.TrimSpace(line[2:eq])
		body := strings.TrimSpace(line[eq+1:])
		body = strings.TrimSuffix(body, ";")
		body = strings.Trim(body, "\"")
		if key == "" || body == "" {
			continue
		}
		// 数据源通常保留美股大小写，这里统一小写匹配以兼容不同写法
		rows[strings.ToLower(key)] = strings.Split(body, "~")
	}

	if len(rows) == 0 {
		return nil, nil, errNoQuotes
	}

	quotes := make([]Quote, 0, len(symbols))
	var missing []string
	for _, sym := range symbols {
		fields, ok := rows[strings.ToLower(sym.code)]
		if !ok {
			missing = append(missing, sym.code)
			continue
		}

		quote, ok := quoteFromFields(fields)
		if !ok {
			missing = append(missing, sym.code)
			continue
		}
		if sym.name != "" {
			quote.Name = sym.name
		}
		quotes = append(quotes, quote)
	}

	return quotes, missing, nil
}

func quoteFromFields(fields []string) (Quote, bool) {
	if len(fields) <= changePctField {
		return Quote{}, false
	}

	name := strings.TrimSpace(fields[1])
	if name == "" {
		return Quote{}, false
	}

	pct, err := strconv.ParseFloat(strings.TrimSpace(fields[changePctField]), 64)
	if err != nil || math.IsNaN(pct) || math.IsInf(pct, 0) {
		return Quote{}, false
	}

	return Quote{Name: name, ChangePct: pct}, true
}

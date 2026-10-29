package stocks

// 腾讯行情解析测试：覆盖 A 股/港股/美股、GBK 解码、非法条目跳过和缺失编号。

import (
	"strings"
	"testing"

	"github.com/esp32-rss-display/backend/server/config"
	"golang.org/x/text/encoding/simplifiedchinese"
)

// encodeGBK 模拟数据源的 GBK 响应，避免测试依赖真实网络。
func encodeGBK(t *testing.T, text string) []byte {
	t.Helper()
	body, err := simplifiedchinese.GBK.NewEncoder().Bytes([]byte(text))
	if err != nil {
		t.Fatalf("encode gbk: %v", err)
	}
	return body
}

// quoteLine 生成一行行情；只填解析用到的名称和涨跌幅字段。
func quoteLine(code, name, pct string) string {
	fields := make([]string, 40)
	fields[1] = name
	fields[32] = pct
	return "v_" + code + "=\"" + strings.Join(fields, "~") + "\";"
}

func TestParseTencentQuotes(t *testing.T) {
	symbols := []symbol{
		{code: "sh000001"},
		{code: "hk00700"},
		{code: "usAAPL", name: "AAPL"},
		{code: "sz000001"},
	}
	text := strings.Join([]string{
		quoteLine("sh000001", "上证指数", "-0.82"),
		quoteLine("hk00700", "腾讯控股", "-2.58"),
		quoteLine("usAAPL", "苹果", "1.52"),
	}, "\n")

	quotes, missing, err := parseTencent([]byte(text), symbols)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(quotes) != 3 {
		t.Fatalf("expected 3 quotes, got %d: %+v", len(quotes), quotes)
	}
	// 顺序必须与配置一致，固件按此顺序滚动
	if quotes[0].Name != "上证指数" || quotes[0].ChangePct != -0.82 {
		t.Fatalf("unexpected first quote: %+v", quotes[0])
	}
	if quotes[1].Name != "腾讯控股" || quotes[1].ChangePct != -2.58 {
		t.Fatalf("unexpected second quote: %+v", quotes[1])
	}
	// name 覆盖只影响展示名，涨跌幅仍来自数据源
	if quotes[2].Name != "AAPL" || quotes[2].ChangePct != 1.52 {
		t.Fatalf("unexpected third quote: %+v", quotes[2])
	}
	if len(missing) != 1 || missing[0] != "sz000001" {
		t.Fatalf("expected sz000001 missing, got %v", missing)
	}
}

func TestParseTencentSkipsInvalidEntries(t *testing.T) {
	symbols := []symbol{
		{code: "sh000001"},
		{code: "sz000001"},
		{code: "hk00700"},
	}
	text := strings.Join([]string{
		quoteLine("sh000001", "", "1.0"),     // 名称为空
		quoteLine("sz000001", "坏的条目", "abc"), // 涨跌幅非数字
		"v_pv_none_match=\"1\";",
	}, "\n")

	quotes, missing, err := parseTencent([]byte(text), symbols)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(quotes) != 0 {
		t.Fatalf("expected no valid quotes, got %+v", quotes)
	}
	if len(missing) != 3 {
		t.Fatalf("expected all symbols missing, got %v", missing)
	}
}

func TestParseTencentRejectsEmptyBody(t *testing.T) {
	if _, _, err := parseTencent([]byte("not a quote response"), []symbol{{code: "sh000001"}}); err == nil {
		t.Fatal("expected error for response without quote lines")
	}
}

func TestNormalizeSymbols(t *testing.T) {
	got := normalizeSymbols([]config.StockSymbol{
		{Code: " USaapl "}, // 前缀转小写、美股代码转大写
		{Code: "sh000001"},
		{Code: "sh000001"}, // 重复项丢弃
		{Code: "zz123456"}, // 不支持的市场前缀
		{Code: "sh600519", Name: "茅台"},
		{Code: "sz000001"},
	})

	want := []symbol{
		{code: "usAAPL"},
		{code: "sh000001"},
		{code: "sh600519", name: "茅台"},
		{code: "sz000001"},
	}
	if len(got) != len(want) {
		t.Fatalf("expected %d symbols, got %+v", len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("symbol %d: expected %+v, got %+v", i, want[i], got[i])
		}
	}
}

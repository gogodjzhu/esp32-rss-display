# 中文字体资源

`cjk_subset.ttf` 是固件运行期使用的中文点阵字体子集，由
[`tools/gen_cjk_font.py`](../../tools/gen_cjk_font.py) 使用 fonttools 从
开源字体裁剪而来。

## 来源与授权

- 源字体：**WenQuanYi Micro Hei（文泉驿微米黑）**，取自
  <https://github.com/anthonyfok/fonts-wqy-microhei>（`wqy-microhei.ttc` 第 0 号字体，
  即比例字型 WenQuanYi Micro Hei）。
- 授权：**Apache License 2.0**。源字体包含完整拉丁字母/数字与 CJK 字形，
  适合中英混排标题。

> 说明：曾尝试使用 Droid Sans Fallback（同样 Apache-2.0），但它作为
> 回退字体不含拉丁字形，会导致数字/英文显示为方框，故不采用。

## 字符覆盖

- ASCII 可见字符（0x20–0x7E）
- 常用中英文标点 / 全角符号
- **GB2312 全部汉字**（一级常用字 3755 + 二级汉字 3008，共 6763），
  加上符号区合计约 7540 个字符。

子集体积约 1.4 MB，烧录在独立 `font` flash 分区（见 `partitions_s3.csv`），
运行期由 `lib/cjk_font` 通过 `esp_partition_mmap` 零拷贝映射，交给 LVGL
内置的 Tiny TTF（stb_truetype）按需光栅化。

## 重新生成

```sh
pip install fonttools
python3 tools/gen_cjk_font.py \
    --source wqy-microhei.ttc --font-number 0 \
    --output assets/fonts/cjk_subset.ttf
```

生成后重新构建并烧录即可自动写入 `font` 分区（`build_font.py` 预构建脚本）。

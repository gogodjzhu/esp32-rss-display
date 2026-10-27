#!/usr/bin/env python3
"""生成固件使用的中文子集字体。

本脚本把一份开源中文字体裁剪成覆盖 GB2312 全部汉字 + 常用标点 + ASCII
的子集 TTF，输出到 assets/fonts/cjk_subset.ttf。裁剪使用 fonttools（工业
标准字体工具），避免手写字形。

当前采用 WenQuanYi Micro Hei（文泉驿微米黑，Apache-2.0），它同时包含
拉丁字母/数字与 CJK，适合中英混排标题。源文件（wqy-microhei.ttc）可从
https://github.com/anthonyfok/fonts-wqy-microhei 获取。

注意：Droid Sans Fallback 不含拉丁字形，不适用于中英混排，勿再使用。

依赖：
    pip install fonttools

用法：
    python3 tools/gen_cjk_font.py \
        --source /path/to/wqy-microhei.ttc --font-number 0 \
        --output assets/fonts/cjk_subset.ttf

固件运行期通过 esp_partition_mmap 映射 font 分区里的该 TTF，
再用 LVGL 内置的 Tiny TTF（stb_truetype）按需光栅化。
"""

import argparse
import subprocess
import sys


def gb2312_charset(include_level2: bool = True) -> set:
    """返回 GB2312 编码空间内可解码的字符集合。

    GB2312 0xA1-0xA9 区为符号，0xB0-0xD7 区为一级常用汉字（3755 个），
    0xD8-0xF7 区为二级汉字（3008 个）。一级为「常用字」，二级覆盖更多
    生僻字，默认一并纳入。
    """
    chars = set()

    # ASCII 可见字符
    chars.update(chr(cp) for cp in range(0x20, 0x7F))

    # 常用中英文标点 / 全角符号
    chars.update(
        chr(cp)
        for cp in (
            0x00B7, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
            0x3001, 0x3002, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E, 0x300F,
            0x3010, 0x3011, 0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B,
            0xFF1F, 0xFF5E,
        )
    )

    hi_end = 0xF8 if include_level2 else 0xD8
    for hi in range(0xA1, hi_end):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, help="源 TrueType/OpenType 字体")
    ap.add_argument("--output", required=True, help="输出子集 TTF 路径")
    ap.add_argument(
        "--font-number", type=int, default=-1,
        help="源为 TTC 字体集合时选择第几个字体（默认 -1：非集合）",
    )
    ap.add_argument(
        "--level1-only",
        action="store_true",
        help="只保留 GB2312 一级常用汉字（体积更小）",
    )
    args = ap.parse_args()

    charset = gb2312_charset(include_level2=not args.level1_only)
    text = "".join(sorted(charset))
    print(f"字符集大小: {len(charset)} (level1_only={args.level1_only})")

    cmd = [
        sys.executable, "-m", "fontTools.subset", args.source,
        f"--text={text}",
        f"--output-file={args.output}",
        "--no-hinting",            # stb_truetype 不做 hinting，直接丢弃
        "--layout-features=",      # 丢弃 GSUB/GPOS，减小体积
        "--glyph-names=0",
        "--desubroutinize",        # CFF 源字体去子程序化，便于 stb 解析
    ]
    if args.font_number >= 0:
        cmd.append(f"--font-number={args.font_number}")
    print("运行:", " ".join(cmd[:4]), "...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode
    print("已生成:", args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

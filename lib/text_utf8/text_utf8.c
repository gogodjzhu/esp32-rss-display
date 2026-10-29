#include "text_utf8.h"
#include <string.h>

uint32_t text_utf8_next(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t cp = *p;

    if (cp < 0x80) {
        *pp = (const char *)(p + 1);
        return cp;
    } else if ((cp & 0xE0) == 0xC0 && p[1]) {
        cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
        *pp = (const char *)(p + 2);
    } else if ((cp & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp = (const char *)(p + 3);
    } else if ((cp & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((cp & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *pp = (const char *)(p + 4);
    } else {
        *pp = (const char *)(p + 1);
    }
    return cp;
}

bool text_utf8_is_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

bool text_utf8_is_cjk(uint32_t cp)
{
    return (cp >= 0x1100  && cp <= 0x11FF) ||   /* 谚文字母 */
           (cp >= 0x2E80  && cp <= 0x303F) ||   /* CJK 部首扩展 / 标点 */
           (cp >= 0x3040  && cp <= 0x30FF) ||   /* 日文假名 */
           (cp >= 0x3130  && cp <= 0x318F) ||   /* 谚文兼容字母 */
           (cp >= 0x3400  && cp <= 0x4DBF) ||   /* CJK 扩展 A */
           (cp >= 0x4E00  && cp <= 0x9FFF) ||   /* CJK 基本区 */
           (cp >= 0xAC00  && cp <= 0xD7FF) ||   /* 谚文音节 */
           (cp >= 0xF900  && cp <= 0xFAFF) ||   /* CJK 兼容表意 */
           (cp >= 0xFE30  && cp <= 0xFE4F) ||   /* CJK 兼容符号 */
           (cp >= 0xFF00  && cp <= 0xFFEF) ||   /* 全角字符 */
           (cp >= 0x20000 && cp <= 0x2FA1F);    /* CJK 扩展 B~F */
}

void text_utf8_copy(const char *src, char *dst, size_t size)
{
    if (!dst || size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    const char *p = src;
    size_t used = 0;

    while (*p) {
        const char *q = p;
        (void)text_utf8_next(&q);

        size_t n = (size_t)(q - p);
        if (used + n + 1 > size) break;
        memcpy(dst + used, p, n);
        used += n;
        p = q;
    }
    dst[used] = '\0';
}

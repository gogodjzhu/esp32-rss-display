#ifndef TEXT_UTF8_H
#define TEXT_UTF8_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 解码一个 UTF-8 码点；非法序列按单字节处理。*pp 始终前进至少一个字节 */
uint32_t text_utf8_next(const char **pp);

/* 是否为 CJK 字符：可在任意两字之间断行（无需空格），覆盖常用扩展区 */
bool text_utf8_is_cjk(uint32_t cp);

/* 是否为可断行的空白字符 */
bool text_utf8_is_space(uint32_t cp);

/* 按完整码点复制，最多写入 size-1 字节，超长时在码点边界截断。
 * dst 始终以 NUL 结尾；非法 UTF-8 按单字节保留。 */
void text_utf8_copy(const char *src, char *dst, size_t size);

#ifdef __cplusplus
}
#endif
#endif

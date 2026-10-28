#ifndef HOST_ESP_LOG_H
#define HOST_ESP_LOG_H
/* 主机测试不输出日志，但保留 tag 引用以免触发“未使用”警告 */
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#endif

/**
 * @file http_server.h
 * @brief HTTP Server - 负责启动HTTP服务器
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动HTTP服务器
 * @note 在调用此函数前应确保WiFi已连接
 * @return 服务器句柄，失败返回NULL
 */
httpd_handle_t http_server_start(void);

/**
 * @brief 停止HTTP服务器
 * @param server 服务器句柄
 */
void http_server_stop(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H

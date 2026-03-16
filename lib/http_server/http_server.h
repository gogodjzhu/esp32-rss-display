/**
 * @file http_server.h
 * @brief HTTP Server - 负责启动HTTP服务器和配网页面
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

httpd_handle_t http_server_start(void);

void http_server_stop(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H

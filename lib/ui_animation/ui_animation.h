#ifndef UI_ANIMATION_H
#define UI_ANIMATION_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 所有接口只允许应用/UI 所有者任务调用 */
void ui_animation_init(void);
void ui_animation_task(void);
void ui_animation_show_loading(void);
void ui_animation_show_error(void);
void ui_animation_show_no_network(const char *ap_ssid);
typedef struct {
    const char *firmware;
    const char *ssid;
    const char *ip;
    const char *backend_url;
} ui_connecting_info_t;
void ui_animation_show_connecting(const ui_connecting_info_t *info);
void ui_animation_prepare_image_mode(void);
void ui_animation_write_row(int x, int y, const uint16_t *pixels, int width);
void ui_animation_update_bottom_bar(int percent);
void ui_animation_show_rating_bar(int rating);
void ui_animation_show_submit_bar(bool success);
#ifdef __cplusplus
}
#endif
#endif

/**
 * @file main.cpp
 * @brief 主入口 - WiFi 连接后定时从后端拉取图片显示到 TFT 屏幕
 *
 * 流程：
 *   1. 初始化显示屏（ui_animation_init）
 *   2. 初始化 WiFi 并连接（wifi_manager_smart_connect）
 *   3. 无凭证 → 进入 AP 配网模式，启动 HTTP server 提供配网页面，等待配网
 *   4. 连接成功 → 启动 HTTP 管理服务器 + image_display_task
 *   5. image_display_task 每 30 秒：拉取图片 URL → 下载并显示 → 底部进度条倒计时
 *   6. HTTP 管理页面在两种模式下均可访问
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>

#include "ui_animation.h"
#include "wifi_manager.h"
#include "image_fetcher.h"
#include "http_server.h"

static const char *TAG = "MAIN";

/* 图片 URL 缓冲区最大长度 */
#define IMAGE_URL_BUF_LEN 512

/* 每次图片展示后等待的时间（秒） */
#define DISPLAY_INTERVAL_SEC 60

/* 进度条更新间隔（毫秒），越小越平滑 */
#define PROGRESS_UPDATE_MS   100

/* ---------- 按钮（GPIO9）相关 ---------- */

/* 图片显示任务句柄，供 ISR 发送通知 */
static TaskHandle_t s_display_task_handle = NULL;

/* 按钮事件队列，元素为 int64_t 时间戳（微秒） */
static QueueHandle_t s_button_queue = NULL;

/**
 * @brief GPIO9 中断服务函数（IRAM，下降沿触发）
 *
 * 带 50ms 软件防抖；触发后将当前时间戳（微秒）发送到 s_button_queue，
 * 由 image_display_task 在任务上下文完成双击检测和手势判断。
 */
static void IRAM_ATTR gpio_button_isr(void *arg)
{
    /* 防抖：距上次触发不足 50ms 则忽略 */
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 50000LL) return;
    last_us = now;

    /* 将时间戳入队，供任务上下文做双击判断 */
    BaseType_t woken = pdFALSE;
    if (s_button_queue != NULL) {
        xQueueSendFromISR(s_button_queue, &now, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief 初始化 GPIO9 为输入（内部上拉，下降沿中断）
 */
static void button_init(void)
{
    /* 创建按钮事件队列（深度 4，元素为 int64_t 时间戳） */
    s_button_queue = xQueueCreate(4, sizeof(int64_t));

    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << GPIO_NUM_9),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_9, gpio_button_isr, NULL);
    ESP_LOGI(TAG, "按钮 GPIO9 已初始化");
}

/* ---------- 图片显示任务 ---------- */

/**
 * @brief 提交评分并显示结果状态栏
 *
 * 调用 image_fetcher_submit_rating，根据结果在底部 2px 显示绿色（成功）或
 * 红色（失败）状态栏 1 秒，随后清空按钮队列（丢弃操作期间积压的事件）。
 */
static void do_submit_and_show(int sel)
{
    esp_err_t err = image_fetcher_submit_rating(sel);
    bool success  = (err == ESP_OK);
    ui_animation_show_submit_bar(success);
    ESP_LOGI(TAG, "评价提交%s：%d 分", success ? "成功" : "失败", sel);
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* 清空评价及状态显示期间积压的按钮事件 */
    { int64_t dummy; while (xQueueReceive(s_button_queue, &dummy, 0) == pdTRUE) {} }
}

static void image_display_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "图片显示任务启动");

    /* 保存任务句柄（保留，便于未来扩展） */
    s_display_task_handle = xTaskGetCurrentTaskHandle();

    /* 清空启动时可能残留的按钮事件 */
    {
        int64_t dummy;
        while (xQueueReceive(s_button_queue, &dummy, 0) == pdTRUE) {}
    }

    char url_buf[IMAGE_URL_BUF_LEN];

    /* 评价状态机 */
    typedef enum { DISPLAY_NORMAL, DISPLAY_RATING } display_state_t;
    display_state_t state              = DISPLAY_NORMAL;
    int             sel                = 0;           /* 当前评分 0=无, 1-5=分值 */
    bool            waiting_second     = false;       /* 是否正在等待第二次点击（双击检测） */
    int64_t         first_click_us     = 0;           /* 第一次点击的时间戳 */
    int64_t         last_action_us     = 0;           /* 最后一次操作时间戳（用于 5 秒超时） */
    int64_t         last_rating_act_us = 0;           /* 评价模式最后一次评分操作时间戳（防抖） */
    int             t_saved            = 0;           /* 进入评价模式时保存的倒计时步数 */

    while (true) {
        /* 获取下一条图片 URL */
        esp_err_t err = image_fetcher_get_next_url(url_buf, sizeof(url_buf));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "获取图片 URL 失败，5 秒后重试...");
            ui_animation_show_error();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 切换到图片模式：清除 spinner 等 LVGL 组件，预建底部进度条 */
        ui_animation_prepare_image_mode();

        /* 下载并显示图片 */
        err = image_fetcher_download_and_show(url_buf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "图片下载/显示失败，5 秒后重试...");
            ui_animation_show_error();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 重置状态（新图片展示时退出残留的评价模式，重置双击检测） */
        state              = DISPLAY_NORMAL;
        sel                = 0;
        waiting_second     = false;
        last_rating_act_us = 0;

        /* 底部进度条倒计时；评价模式下暂停推进，5 秒超时后继续 */
        int total_steps = DISPLAY_INTERVAL_SEC * (1000 / PROGRESS_UPDATE_MS);
        for (int t = 0; t <= total_steps; ) {
            /* 仅在正常模式下更新进度条 */
            if (state == DISPLAY_NORMAL) {
                ui_animation_update_bottom_bar(t * 100 / total_steps);
            }

            int64_t ts;
            if (xQueueReceive(s_button_queue, &ts, pdMS_TO_TICKS(PROGRESS_UPDATE_MS)) == pdTRUE) {
                last_action_us = ts;

                if (state == DISPLAY_NORMAL) {
                    if (!waiting_second) {
                        /* 第一次点击：记录时间，等待第二次点击以判断是否为双击 */
                        waiting_second = true;
                        first_click_us = ts;
                    } else {
                        int64_t interval = ts - first_click_us;
                        if (interval < 400000LL) {
                            /* 双击确认：进入评价模式，暂停倒计时 */
                            waiting_second     = false;
                            t_saved            = t;
                            state              = DISPLAY_RATING;
                            sel                = 0;
                            last_rating_act_us = ts;
                            ui_animation_show_rating_bar(sel);
                            ESP_LOGI(TAG, "进入评价模式，倒计时暂停于步数 %d", t_saved);
                        } else {
                            /* 间隔过长，将本次点击视为新的第一次点击，重置窗口 */
                            first_click_us = ts;
                            /* waiting_second 保持 true，继续等待下一次点击 */
                        }
                    }
                } else {
                    /* 评价模式下单击：200ms 防抖，防止连击跳多档 */
                    if (ts - last_rating_act_us >= 200000LL) {
                        last_rating_act_us = ts;
                        sel = (sel + 1) % 6;  /* 循环 0→1→2→3→4→5→0 */
                        ui_animation_show_rating_bar(sel);
                        ESP_LOGI(TAG, "评价切换：%d 分", sel);
                    }
                }
            } else {
                /* 无按键到来（PROGRESS_UPDATE_MS 超时） */
                int64_t now = esp_timer_get_time();

                /* 正常模式：检查是否需要将等待中的单击超时判定为「下一张」 */
                if (state == DISPLAY_NORMAL && waiting_second) {
                    if (now - first_click_us >= 400000LL) {
                        /* 单击确认（双击窗口已过）：跳到下一张图片 */
                        waiting_second = false;
                        ESP_LOGI(TAG, "单击确认：跳到下一张图片");
                        break;  /* 跳出倒计时循环，加载下一张 */
                    }
                }

                if (state == DISPLAY_RATING) {
                    if (now - last_action_us >= 5000000LL) {
                        /* 5 秒无操作：提交评价并显示结果状态栏，退出评价模式 */
                        do_submit_and_show(sel);
                        state = DISPLAY_NORMAL;
                        t     = t_saved;
                        /* 立即恢复进度条显示 */
                        ui_animation_update_bottom_bar(t * 100 / total_steps);
                        ESP_LOGI(TAG, "评价完成，从步数 %d 继续倒计时", t_saved);
                    }
                    /* 评价模式下不推进 t */
                } else {
                    t++;  /* 正常推进倒计时 */
                }
            }
        }
    }
}

/* ---------- 程序入口 ---------- */

extern "C" void app_main()
{
    ESP_LOGI(TAG, "RSS 图片显示固件启动");

    /* 初始化显示屏和 LVGL */
    ui_animation_init();
    ui_animation_show_loading();

    /* 初始化 WiFi 管理器 */
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败");
        ui_animation_show_error();
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* 尝试连接 WiFi（使用 NVS 中保存的凭证，或进入 AP 配网模式） */
    err = wifi_manager_smart_connect();
    if (err != ESP_OK) {
        /* 无凭证，已进入 AP 配网模式（192.168.4.1）
         * 启动 HTTP server 提供配网页面，然后驱动 LVGL 等待用户配网 */
        ESP_LOGI(TAG, "进入 AP 配网模式，启动 HTTP 配网服务器");
        ui_animation_show_no_network(wifi_manager_get_info()->ap_ssid);
        http_server_start();
        while (true) {
            ui_animation_task();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "WiFi 已连接，启动 HTTP 管理服务器");

    /* 初始化图片拉取模块（从 NVS 加载 backend URL），显示连接画面 */
    image_fetcher_init();
    ui_animation_show_connecting(image_fetcher_get_backend_url());

    /* 初始化按钮 GPIO9，按下后立即刷新图片 */
    button_init();

    /* 启动 HTTP 管理服务器（提供 /、/api/status、/reset 等页面） */
    httpd_handle_t http_server = http_server_start();
    if (http_server == NULL) {
        ESP_LOGW(TAG, "HTTP 服务器启动失败，继续运行图片显示");
    }

    /* 启动图片显示任务（栈 8KB，优先级 5） */
    xTaskCreate(image_display_task, "img_display", 8192, NULL, 5, NULL);

    /* app_main 退出，FreeRTOS 调度器接管 */
    vTaskDelete(NULL);
}

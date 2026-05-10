# Tasks: startup-screen-improvements

## Task 1: Kconfig — 删除 BACKEND_URL 配置项

文件：`src/Kconfig`

- [x] 删除以下整块（共 5 行）：
  ```
  config BACKEND_URL
      string "Backend server URL"
      default "http://192.168.3.112:8080"
      help
          后端服务器地址，用于获取图片 URL。
  ```

文件：`sdkconfig.airm2m_core_esp32c3`

- [x] 删除 `CONFIG_BACKEND_URL="http://192.168.3.112:8080"` 行

## Task 2: image_fetcher — 改用硬编码 fallback，新增 getter

文件：`lib/image_fetcher/image_fetcher.c`

- [x] 将 `CONFIG_BACKEND_URL` 替换为硬编码宏：
  ```c
  #define DEFAULT_BACKEND_URL "http://192.168.3.112:8080"
  ```
- [x] 将 `image_fetcher_init()` 中的 `CONFIG_BACKEND_URL` 替换为 `DEFAULT_BACKEND_URL`
- [x] 确认 `#include "sdkconfig.h"` 是否仍被其他宏使用（`CONFIG_DEVICE_ID` 等），若仍需保留则保留，否则删除
- [x] 在文件末尾新增 getter 函数：
  ```c
  const char *image_fetcher_get_backend_url(void)
  {
      return s_backend_url;
  }
  ```

文件：`lib/image_fetcher/image_fetcher.h`

- [x] 新增函数声明：
  ```c
  const char *image_fetcher_get_backend_url(void);
  ```

## Task 3: ui_animation — show_no_network 添加文字说明

文件：`lib/ui_animation/ui_animation.h`

- [x] 将 `ui_animation_show_no_network` 签名从 `void f(void)` 改为：
  ```c
  void ui_animation_show_no_network(const char *ap_ssid);
  ```

文件：`lib/ui_animation/ui_animation.c`

- [x] 更新 `ui_animation_show_no_network` 函数签名，接受 `const char *ap_ssid` 参数
- [x] 在现有黄色感叹号圆圈下方追加三个 LVGL label：
  - label 1（白色）："Connect to WiFi hotspot:"
  - label 2（黄色 `0xf39c12`）：`lv_label_set_text_fmt(l, "SSID: %s", ap_ssid)`
  - label 3（灰色 `0x888888`）："Open: 192.168.4.1"
  - 三个 label 依次 `LV_ALIGN_CENTER` + y offset（+55, +80, +105）排列
  - font 使用 `LV_FONT_DEFAULT`

## Task 4: ui_animation — 新增 show_connecting 函数

文件：`lib/ui_animation/ui_animation.h`

- [x] 新增声明：
  ```c
  void ui_animation_show_connecting(const char *backend_url);
  ```

文件：`lib/ui_animation/ui_animation.c`

- [x] 实现 `ui_animation_show_connecting`：
  - 调用 `ui_reset_screen()` 清屏
  - 黑色背景
  - 居中 spinner（60x60，y offset -30）
  - label 1（白色，y offset +40）："Connecting to server..."
  - label 2（灰色 `0x888888`，y offset +65）：`lv_label_set_text(l, backend_url)`
  - 调用 `ui_animation_task()` 驱动渲染一帧

## Task 5: main.cpp — 更新调用点

文件：`src/main.cpp`

- [x] AP 模式调用点：将 `ui_animation_show_no_network()` 改为：
  ```c
  wifi_info_t *info = wifi_manager_get_info();
  ui_animation_show_no_network(info->ap_ssid);
  ```
- [x] 连接成功后：将 `image_fetcher_init()` 从 `image_display_task` 开头移至 `app_main` 中 `button_init()` 之前，随后调用：
  ```c
  image_fetcher_init();
  ui_animation_show_connecting(image_fetcher_get_backend_url());
  vTaskDelay(pdMS_TO_TICKS(100));
  ```
- [x] 删除 `image_display_task` 开头原有的 `image_fetcher_init()` 调用

## Task 6: 编译验证

- [x] 运行 `pio run` 确认编译通过，无 `CONFIG_BACKEND_URL` 相关警告或错误

## Task 7: 烧录并目视验证

- [x] 运行 `pio run --target upload` 烧录成功
- [x] 重置设备（或首次上电）：确认 AP 配网画面显示热点名称和 IP
- [x] 正常启动（已有 WiFi 凭证）：确认出现 backend URL 过渡画面后进入图片显示

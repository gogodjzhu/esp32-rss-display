# Tasks: integrate-lvgl-ui

## Task 1: 添加 LVGL 依赖到主项目

- 在 `platformio.ini` 的 `lib_deps` 中添加 `lvgl/lvgl @ ^9.5.0`
- 将验证项目的 `exclude_lvgl_arm_files.py` 复制到主项目根目录
- 在 `platformio.ini` 中添加 `extra_scripts = pre:exclude_lvgl_arm_files.py`
- 添加 `build_unflags = -fuse-cxa-atexit`（若尚未存在）

## Task 2: 添加 lv_conf.h

- 将验证项目的 `src/lv_conf.h` 复制到主项目 `src/lv_conf.h`
- 确认配置：`LV_COLOR_DEPTH 16`，`LV_COLOR_FORMAT_RGB565_SWAPPED`
- 不启用任何字体（UI 不显示文字，无需 Montserrat 或 CJK 字体）

## Task 3: 创建 lib/ui_animation/ 库

- 创建目录 `lib/ui_animation/`
- 创建 `library.json`（库名 `ui_animation`）
- 创建 `ui_animation.h`，声明公开 API：
  - `ui_animation_init(void)`
  - `ui_animation_show_loading(void)`
  - `ui_animation_show_progress(int percent)`
  - `ui_animation_show_error(void)`
  - `ui_animation_show_ok(void)`
  - `ui_animation_task(void)`
- 创建 `ui_animation.c`，实现：
  - ST7789 SPI 初始化（从验证项目 `main.c` 迁移）
  - LVGL 初始化（display、buffer、flush callback）
  - 各 UI 状态函数（loading/progress/error/ok），全部使用纯图形组件（lv_obj/lv_arc/lv_bar/lv_line/lv_spinner），不使用文字
  - `ui_animation_task()` 驱动 `lv_timer_handler()` 和 `lv_tick_inc()`

## Task 4: 集成到 main.cpp

- 在 `main.cpp` 中 `#include "ui_animation.h"`
- 在 `app_main()` 启动序列中调用 `ui_animation_init()`
- 根据 WiFi 状态调用对应 UI 函数
- WiFi 连接成功（`WIFI_STATUS_CONNECTED`）时调用 `ui_animation_show_ok()`
- 在主循环中定期调用 `ui_animation_task()`

## Task 5: 编译验证

- 运行 `pio run` 确认编译通过，无错误

## Task 6: 烧录并拍照验证

- 运行 `pio run --target upload` 烧录到设备
- 等待启动（约 3 秒）后拍照
- 确认屏幕显示正确动画，无花条纹、无颜色错误

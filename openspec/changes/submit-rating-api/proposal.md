# Proposal: submit-rating-api

## 问题

`submit_rating()` 当前只打印日志（占位实现），用户的 1-5 分评价数据从未真正提交到后端。

## 目标

将评价提交从占位日志替换为真实 HTTP 调用，并在底部状态栏显示提交结果（成功/失败），完成评分闭环。

## 范围

### 包含

- 固件 `image_fetcher` 模块：解析 `/next` 响应中的 `item_id` 字段并缓存；新增 `image_fetcher_submit_rating()` 函数，向后端发送 `POST /v1/item/{item_id}/rating`
- 固件 `ui_animation` 模块：新增 `ui_animation_show_submit_bar(bool success)` 函数，全绿（成功）或全红（失败）覆盖底部 2px 状态栏
- 固件 `main.cpp`：`submit_rating()` 改为调用 `image_fetcher_submit_rating()`，根据返回值显示提交状态栏，延迟 1 秒后恢复进度条并退出评价模式

### 不包含

- 后端 rating 接口实现（已完成）
- 评分失败后的重试机制
- 本地评分队列/离线缓冲

## 约束

- 不新增 FreeRTOS 任务；HTTP 请求在 `image_display_task` 上下文同步执行
- `item_id == 0` 时跳过 HTTP 请求，直接显示失败状态栏（防止污染后端数据）
- HTTP 超时设为 5000ms，避免弱网下长时间冻屏
- 评分提交期间（网络请求 + 1 秒状态显示）不响应按钮输入（队列中事件在恢复正常模式前清空）
- 代码注释保持中文

## 成功标准

- 用户完成 1-5 分评分后，后端 `/v1/item/{id}/rating` 收到正确请求
- 底部状态栏在提交后显示 1 秒绿色（成功）或红色（失败），随后恢复蓝色进度条
- `item_id` 未获取到时不发送请求，状态栏显示红色

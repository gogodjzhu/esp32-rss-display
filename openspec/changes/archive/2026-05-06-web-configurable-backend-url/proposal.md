# Proposal: web-configurable-backend-url

## 问题

`BACKEND_URL` 目前硬编码在 Kconfig / sdkconfig 中，每次修改后端地址（如切换服务器或端口）都需要重新编译并烧录固件，流程繁琐，不适合部署后的现场调整。

## 目标

允许用户通过设备内置的 Web 页面配置 `BACKEND_URL`，修改后持久化到 NVS，重启生效，无需重新烧录固件。

## 范围

### 包含

- 新增 `/settings` Web 页面（`data/www/settings.html`）：展示当前 `BACKEND_URL`，提供输入框和"保存并重启"按钮
- `status.html` 新增入口链接，跳转到 `/settings`
- HTTP 服务器新增三个 URI handler：
  - `GET /settings` → 返回 `settings.html`
  - `GET /api/settings` → 返回当前 `backend_url`（JSON）
  - `POST /api/settings` → 保存到 NVS，延迟重启
- `image_fetcher` 初始化时优先从 NVS 读取 `backend_url`，NVS 无值时 fallback 到 `CONFIG_BACKEND_URL`

### 不包含

- 其他配置项（`DEVICE_ID`、`RSS_URL` 等）的 Web 配置
- 热更新（修改后立即生效，不重启）
- 输入合法性深度校验（如 URL 格式检查）

## 约束

- NVS 键名使用 `"backend_url"`，namespace 沿用 `"app_config"`，不与已有键冲突
- 保存后行为与 WiFi 配置保存一致：延迟 2s 后 `esp_restart()`
- `CONFIG_BACKEND_URL`（Kconfig 默认值）保留为编译时 fallback，不删除
- 代码注释保持中文

## 成功标准

- 设备在 STA 模式下，访问 `http://<device-ip>/settings` 可看到当前 backend URL
- 修改并保存后，设备重启，新地址生效（串口日志确认 `image_fetcher` 使用新地址）
- 重启后再次访问 `/settings`，显示已保存的新地址（NVS 持久化验证）

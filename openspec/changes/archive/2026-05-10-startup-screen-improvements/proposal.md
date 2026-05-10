# Proposal: startup-screen-improvements

## Problem

The device startup screen provides no useful information to the user:

1. **AP mode (no WiFi configured)**: Shows only a yellow exclamation icon. The user has no idea what hotspot to connect to or what IP to open.
2. **STA mode (WiFi connected)**: Jumps directly to image display with no transition. The user cannot tell what backend server is being used or whether the device is actually working.

Additionally, `BACKEND_URL` is still defined as a Kconfig menu item despite now being configurable via the web settings page, making the Kconfig menu misleading.

## Goals

1. Remove `config BACKEND_URL` from Kconfig; replace with a hardcoded fallback `#define DEFAULT_BACKEND_URL` in C code.
2. Improve the AP mode screen to show actionable connection instructions (hotspot name + IP).
3. Add a connecting screen after WiFi connects, showing the backend URL being used before the first image fetch.

## Scope

### Included

- `src/Kconfig`: Remove `config BACKEND_URL` block
- `lib/image_fetcher/image_fetcher.c`: Replace `CONFIG_BACKEND_URL` with `DEFAULT_BACKEND_URL` hardcoded fallback; add `image_fetcher_get_backend_url()` getter
- `lib/image_fetcher/image_fetcher.h`: Declare `image_fetcher_get_backend_url()`
- `lib/ui_animation/ui_animation.c` + `.h`:
  - `ui_animation_show_no_network(const char *ap_ssid)`: add AP SSID + IP text to existing screen
  - New `ui_animation_show_connecting(const char *backend_url)`: spinner + backend URL text
- `src/main.cpp`: Pass AP SSID to `show_no_network()`; call `show_connecting()` after WiFi connects

### Not Included

- Chinese font support (all display text in English)
- OTA or remote config changes
- Changes to WiFi connection logic

## Constraints

- No Chinese font available; all screen text must be ASCII/English
- LVGL built-in font (`lv_font_montserrat_14` or `LV_FONT_DEFAULT`) used for labels
- `show_no_network` signature changes from `void f(void)` to `void f(const char *ap_ssid)` — update all call sites
- Code comments remain in Chinese
- `DEFAULT_BACKEND_URL` hardcoded as `"http://192.168.3.112:8080"`

## Success Criteria

- Fresh device (no WiFi credentials) shows hotspot name and `192.168.4.1` on screen
- After WiFi connects, screen shows spinner + backend URL before first image appears
- `pio run` compiles cleanly with no references to `CONFIG_BACKEND_URL`
- Kconfig no longer has a `BACKEND_URL` entry

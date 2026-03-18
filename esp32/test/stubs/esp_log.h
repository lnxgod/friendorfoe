#pragma once

/**
 * Stub esp_log.h for native unit tests.
 * Provides no-op logging macros so scanner source files compile on the host.
 */

#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGD(tag, fmt, ...)
#define ESP_LOGV(tag, fmt, ...)

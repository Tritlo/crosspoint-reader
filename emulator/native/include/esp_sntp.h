#pragma once

enum sntp_sync_status_t { SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_IN_PROGRESS, SNTP_SYNC_STATUS_COMPLETED };
constexpr int ESP_SNTP_OPMODE_POLL = 0;

inline bool esp_sntp_enabled() { return false; }
inline void esp_sntp_stop() {}
inline void esp_sntp_setoperatingmode(int) {}
inline void esp_sntp_setservername(int, const char*) {}
inline void esp_sntp_init() {}
inline sntp_sync_status_t sntp_get_sync_status() { return SNTP_SYNC_STATUS_RESET; }

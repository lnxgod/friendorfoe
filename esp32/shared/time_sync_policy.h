#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_TIME_SYNC_STALE_AFTER_MS             30000LL
#define FOF_TIME_SYNC_LOCAL_FRESHNESS_MS         60000LL
#define FOF_TIME_SYNC_BACKEND_RESTEER_THRESHOLD_MS 250LL

bool fof_time_epoch_is_valid(int64_t epoch_ms);
bool fof_time_message_is_valid(bool has_ok, bool ok, int64_t epoch_ms);
bool fof_time_should_apply_backend_epoch(bool sntp_synced,
                                         bool have_local_epoch,
                                         int64_t local_epoch_ms,
                                         int64_t backend_epoch_ms,
                                         int64_t threshold_ms);
bool fof_time_offset_is_stale(int64_t last_valid_local_ms,
                              int64_t now_local_ms,
                              int64_t stale_after_ms);
const char *fof_time_sync_state_label(uint32_t valid_count,
                                      int64_t offset_ms,
                                      int64_t last_valid_local_ms,
                                      int64_t now_local_ms,
                                      int64_t stale_after_ms);

#ifdef __cplusplus
}
#endif

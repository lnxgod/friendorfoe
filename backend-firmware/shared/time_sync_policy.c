#include "time_sync_policy.h"

static int64_t abs64(int64_t value)
{
    return value < 0 ? -value : value;
}

bool fof_time_epoch_is_valid(int64_t epoch_ms)
{
    return epoch_ms > 1700000000000LL;
}

bool fof_time_message_is_valid(bool has_ok, bool ok, int64_t epoch_ms)
{
    if (!fof_time_epoch_is_valid(epoch_ms)) {
        return false;
    }
    if (has_ok) {
        return ok;
    }
    return true;
}

bool fof_time_should_apply_backend_epoch(bool sntp_synced,
                                         bool have_local_epoch,
                                         int64_t local_epoch_ms,
                                         int64_t backend_epoch_ms,
                                         int64_t threshold_ms)
{
    if (sntp_synced || !fof_time_epoch_is_valid(backend_epoch_ms)) {
        return false;
    }
    if (!have_local_epoch || !fof_time_epoch_is_valid(local_epoch_ms)) {
        return true;
    }
    return abs64(local_epoch_ms - backend_epoch_ms) > threshold_ms;
}

bool fof_time_offset_is_stale(int64_t last_valid_local_ms,
                              int64_t now_local_ms,
                              int64_t stale_after_ms)
{
    if (last_valid_local_ms <= 0 || now_local_ms <= last_valid_local_ms) {
        return false;
    }
    return (now_local_ms - last_valid_local_ms) > stale_after_ms;
}

const char *fof_time_sync_state_label(uint32_t valid_count,
                                      int64_t offset_ms,
                                      int64_t last_valid_local_ms,
                                      int64_t now_local_ms,
                                      int64_t stale_after_ms)
{
    if (valid_count == 0) {
        return "waiting";
    }
    if (offset_ms == 0 ||
        fof_time_offset_is_stale(last_valid_local_ms, now_local_ms, stale_after_ms)) {
        return "stale";
    }
    return "fresh";
}

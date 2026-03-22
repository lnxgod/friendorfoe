/**
 * Friend or Foe -- Bayesian Sensor Fusion Engine
 *
 * Log-odds Bayesian evidence combination ported from BayesianFusionEngine.kt.
 *
 * Log-odds representation:
 *   lo = ln(p / (1 - p))
 *   p  = exp(lo) / (1 + exp(lo))
 *
 * Update rule:
 *   lo_new = lo_old + ln(likelihood_ratio * confidence)
 *
 * Time decay:
 *   Exponential decay toward the prior with configurable half-life.
 *   decay_factor = exp(-0.693 * elapsed_sec / HALF_LIFE)
 *   lo = prior_lo + (lo - prior_lo) * decay_factor
 */

#include "bayesian_fusion.h"
#include "constants.h"
#include "detection_types.h"

#include "esp_log.h"

#include <string.h>
#include <math.h>
#include <stdbool.h>

/* ── Constants ─────────────────────────────────────────────────────────────── */

static const char *TAG = "bayes_fuse";

/** ln(2) used for half-life decay computation */
#define LN2     0.693147180559945

/* ── Belief state per candidate ────────────────────────────────────────────── */

typedef struct {
    char    candidate_id[64];   /* Unique ID string */
    bool    in_use;             /* Slot is occupied */
    double  log_odds;           /* Current log-odds of being a real drone */
    int64_t last_update_ms;     /* Timestamp of last evidence update */
} belief_state_t;

/* ── Module state ──────────────────────────────────────────────────────────── */

static belief_state_t s_states[MAX_TRACKED_DRONES];

/* ── Log-odds/probability conversions ──────────────────────────────────────── */

/**
 * Convert probability to log-odds.
 * Clamps input to [0.001, 0.999] to prevent inf/NaN.
 */
static double probability_to_log_odds(double p)
{
    if (p < 0.001) p = 0.001;
    if (p > 0.999) p = 0.999;
    return log(p / (1.0 - p));
}

/**
 * Convert log-odds to probability.
 *   p = exp(lo) / (1 + exp(lo))
 *     = 1 / (1 + exp(-lo))     (numerically stable form)
 */
static double log_odds_to_probability(double lo)
{
    if (lo > MAX_LOG_ODDS) lo = MAX_LOG_ODDS;
    if (lo < MIN_LOG_ODDS) lo = MIN_LOG_ODDS;
    double e = exp(lo);
    return e / (1.0 + e);
}

/* ── Sensor likelihood ratios ──────────────────────────────────────────────── */

/**
 * Get the base likelihood ratio for a detection source.
 *
 * Likelihood ratio = P(detection | drone) / P(detection | not drone)
 * Higher = more reliable sensor.
 */
static double get_base_lr(uint8_t source)
{
    switch (source) {
    case DETECTION_SRC_BLE_RID:     return LR_BLE_RID;
    case DETECTION_SRC_WIFI_BEACON: return LR_WIFI_BEACON;
    case DETECTION_SRC_WIFI_DJI_IE: return LR_WIFI_DJI_IE;
    case DETECTION_SRC_WIFI_SSID:   return LR_WIFI_SSID;
    case DETECTION_SRC_WIFI_OUI:    return LR_WIFI_OUI;
    default:                        return 2.0;
    }
}

/* ── Time decay ────────────────────────────────────────────────────────────── */

/**
 * Apply exponential time decay to log-odds, pulling it toward the prior.
 *
 * decay_factor = exp(-ln(2) * elapsed_sec / HALF_LIFE)
 * lo_decayed = prior_lo + (lo - prior_lo) * decay_factor
 */
static void apply_time_decay(belief_state_t *state, int64_t now_ms)
{
    double elapsed_sec = (double)(now_ms - state->last_update_ms) / 1000.0;
    if (elapsed_sec <= 0.0) {
        return;
    }

    double prior_log_odds = log(PRIOR_PROBABILITY / (1.0 - PRIOR_PROBABILITY));
    double decay_factor = exp(-LN2 * elapsed_sec / EVIDENCE_HALF_LIFE_SEC);

    state->log_odds = prior_log_odds +
                      (state->log_odds - prior_log_odds) * decay_factor;
}

/* ── Slot management ───────────────────────────────────────────────────────── */

/**
 * Find a belief state by candidate ID.
 * Returns NULL if not found.
 */
static belief_state_t *find_state(const char *candidate_id)
{
    for (int i = 0; i < MAX_TRACKED_DRONES; i++) {
        if (s_states[i].in_use &&
            strcmp(s_states[i].candidate_id, candidate_id) == 0) {
            return &s_states[i];
        }
    }
    return NULL;
}

/**
 * Allocate a new belief state slot. If the table is full,
 * evict the oldest entry (LRU).
 */
static belief_state_t *alloc_state(const char *candidate_id, int64_t now_ms)
{
    /* Find a free slot */
    for (int i = 0; i < MAX_TRACKED_DRONES; i++) {
        if (!s_states[i].in_use) {
            belief_state_t *s = &s_states[i];
            memset(s, 0, sizeof(*s));
            s->in_use = true;
            strncpy(s->candidate_id, candidate_id,
                    sizeof(s->candidate_id) - 1);
            s->log_odds = probability_to_log_odds(PRIOR_PROBABILITY);
            s->last_update_ms = now_ms;
            return s;
        }
    }

    /* Table full: evict oldest */
    int oldest_idx = 0;
    int64_t oldest_time = INT64_MAX;
    for (int i = 0; i < MAX_TRACKED_DRONES; i++) {
        if (s_states[i].last_update_ms < oldest_time) {
            oldest_time = s_states[i].last_update_ms;
            oldest_idx = i;
        }
    }

    ESP_LOGD(TAG, "Evicting oldest candidate \"%s\" to make room",
             s_states[oldest_idx].candidate_id);

    belief_state_t *s = &s_states[oldest_idx];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    strncpy(s->candidate_id, candidate_id, sizeof(s->candidate_id) - 1);
    s->log_odds = probability_to_log_odds(PRIOR_PROBABILITY);
    s->last_update_ms = now_ms;
    return s;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void bayesian_fusion_init(void)
{
    memset(s_states, 0, sizeof(s_states));
    ESP_LOGI(TAG, "Bayesian fusion engine initialized (max=%d, half_life=%.0fs)",
             MAX_TRACKED_DRONES, EVIDENCE_HALF_LIFE_SEC);
}

float bayesian_fusion_update(const char *candidate_id, uint8_t source,
                             float sensor_confidence, int64_t now_ms_val)
{
    if (!candidate_id || candidate_id[0] == '\0') {
        return (float)PRIOR_PROBABILITY;
    }

    /* Find existing or allocate new */
    belief_state_t *state = find_state(candidate_id);
    if (!state) {
        state = alloc_state(candidate_id, now_ms_val);
    }

    /* Apply time decay to existing evidence */
    apply_time_decay(state, now_ms_val);

    /* Compute scaled likelihood ratio:
     *   At confidence=1.0, use full base LR
     *   At confidence=0.0, LR=1.0 (neutral evidence)
     */
    double base_lr = get_base_lr(source);
    double scaled_lr = 1.0 + (base_lr - 1.0) * (double)sensor_confidence;

    /* Update log-odds with new evidence */
    double log_lr = log(scaled_lr);
    state->log_odds += log_lr;

    /* Clamp to prevent overflow/underflow */
    if (state->log_odds > MAX_LOG_ODDS) state->log_odds = MAX_LOG_ODDS;
    if (state->log_odds < MIN_LOG_ODDS) state->log_odds = MIN_LOG_ODDS;

    state->last_update_ms = now_ms_val;

    double fused_prob = log_odds_to_probability(state->log_odds);

    ESP_LOGD(TAG, "Update %s: src=%d conf=%.2f -> lo=%.2f prob=%.3f",
             candidate_id, source, sensor_confidence,
             state->log_odds, fused_prob);

    return (float)fused_prob;
}

float bayesian_fusion_get_probability(const char *candidate_id, int64_t now_ms_val)
{
    if (!candidate_id || candidate_id[0] == '\0') {
        return (float)PRIOR_PROBABILITY;
    }

    belief_state_t *state = find_state(candidate_id);
    if (!state) {
        return (float)PRIOR_PROBABILITY;
    }

    apply_time_decay(state, now_ms_val);
    return (float)log_odds_to_probability(state->log_odds);
}

void bayesian_fusion_prune(int64_t now_ms_val)
{
    int pruned = 0;

    for (int i = 0; i < MAX_TRACKED_DRONES; i++) {
        if (s_states[i].in_use) {
            int64_t age_ms = now_ms_val - s_states[i].last_update_ms;
            if (age_ms > DETECTION_STALE_MS) {
                ESP_LOGD(TAG, "Pruning stale candidate \"%s\" (age=%llds)",
                         s_states[i].candidate_id,
                         (long long)(age_ms / 1000));
                s_states[i].in_use = false;
                pruned++;
            }
        }
    }

    if (pruned > 0) {
        ESP_LOGD(TAG, "Pruned %d stale candidates", pruned);
    }
}

int bayesian_fusion_get_active_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TRACKED_DRONES; i++) {
        if (s_states[i].in_use) {
            count++;
        }
    }
    return count;
}

void bayesian_fusion_reset(void)
{
    memset(s_states, 0, sizeof(s_states));
    ESP_LOGI(TAG, "All belief states reset");
}
